#include "comm/comm_hub.h"
#include "comm/link.h"
#include "comm/tcp_link.h"
#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "designer/property_editor.h"
#include "runtime/hmi_widget.h"
#include "runtime/modbus_poller.h"
#include "runtime/runtime_context.h"
#include "runtime/runtime_surface.h"
#include "studio/export_service.h"

#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Slider.h>
#include <FluentQtWidgets/Widgets/SwitchButton.h>

#include <QtCore/QDirIterator>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTextEdit>

using namespace fhmi;

namespace {

// A link that is connected until told otherwise, records what is sent, and injects received bytes.
class FakeLink : public Link
{
  public:
    using Link::Link;
    QString description() const override { return QStringLiteral("fake"); }
    bool isConnected() const override { return m_connected; }
    bool canSend() const override { return m_connected && m_sendable; }
    void close() override {}
    void setConnected(bool connected)
    {
        if (connected != m_connected) {
            m_connected = connected;
            emit connectedChanged(connected);
        }
    }
    // Off: connected without a device to send to, as a listening server before one connects.
    void setSendable(bool sendable)
    {
        if (sendable != m_sendable) {
            m_sendable = sendable;
            emit canSendChanged();
        }
    }
    void inject(const QByteArray &bytes) { deliver(defaultParser(), bytes); }
    QList<QByteArray> sent;

  protected:
    qint64 writeBytes(const QByteArray &bytes) override
    {
        sent.append(bytes);
        return bytes.size();
    }

  private:
    bool m_connected = true;
    bool m_sendable = true;
};

// A PLC behind Modbus TCP with the given holding registers: it answers FC 1 and FC 3 reads (coil 0 on)
// and echoes writes; `exception` answers instead with that exception code.
QByteArray plcReply(const QByteArray &adu, const QHash<int, quint16> &hr, int exception = 0)
{
    const int address = quint8(adu.at(8)) << 8 | quint8(adu.at(9));
    const int count = quint8(adu.at(10)) << 8 | quint8(adu.at(11));
    QByteArray pdu = adu.mid(7, 5);
    if (exception) {
        pdu = QByteArray(1, char(adu.at(7) | 0x80)) + char(exception);
    } else if (adu.at(7) == 3) {
        pdu = QByteArray::fromHex("03") + char(2 * count);
        for (int i = 0; i < count; ++i) {
            pdu.append(char(hr.value(address + i) >> 8)).append(char(hr.value(address + i)));
        }
    } else if (adu.at(7) == 1) {
        pdu = QByteArray::fromHex("01") + char((count + 7) / 8) + QByteArray((count + 7) / 8 - 1, 0);
        pdu.insert(2, char(1));
    }
    QByteArray reply = adu.left(4);
    reply.append(char(0)).append(char(pdu.size() + 1)).append(adu.at(6));
    return reply + pdu;
}

// The PDUs (function code on) of the Modbus TCP requests sent.
QList<QByteArray> pdus(const QList<QByteArray> &adus)
{
    QList<QByteArray> out;
    for (const QByteArray &adu : adus) {
        out.append(adu.mid(7));
    }
    return out;
}

// Emits the Modbus poller's timer timeout, as if its interval had passed.
void tickPoller(RuntimeContext *context)
{
    auto *timer = context->modbusPoller()->findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
    QVERIFY(timer && timer->isActive());
    QMetaObject::invokeMethod(timer, "timeout");
}

// The grid of a link card's field rows.
QGridLayout *fieldGrid(QWidget *card)
{
    for (int i = 0; i < card->layout()->count(); ++i) {
        if (auto *grid = qobject_cast<QGridLayout *>(card->layout()->itemAt(i)->layout())) {
            return grid;
        }
    }
    return nullptr;
}

QJsonObject modbusDisplay(const char *id, int address, int period, int tab)
{
    return QJsonObject{{"id", id},
                       {"type", "NumericDisplay"},
                       {"tabIndex", tab},
                       {"geometry", QJsonObject{{"x", 10}, {"y", 10 + 100 * (address % 2)}, {"w", 160}, {"h", 90}}},
                       {"binding", QJsonObject{{"parseMode", "Modbus"}, {"mbArea", "保持寄存器"},
                                               {"mbRegIndex", address}, {"mbPeriod", period}}}};
}

QStringList exampleFiles()
{
    QStringList files;
    QDirIterator it(QStringLiteral(FHMI_EXAMPLES_DIR), {QStringLiteral("*.uidp")}, QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        files.append(it.next());
    }
    return files;
}

// What a save writes for a widget tree, with the optional keys a writer may omit.
QJsonArray normalized(const QJsonArray &widgets)
{
    QJsonArray out;
    for (const QJsonValue &value : widgets) {
        QJsonObject widget = value.toObject();
        if (widget.value(QStringLiteral("tabIndex")).toInt(0) == 0) {
            widget.remove(QStringLiteral("tabIndex"));
        }
        if (widget.value(QStringLiteral("binding")).toObject().isEmpty()) {
            widget.remove(QStringLiteral("binding"));
        }
        const QJsonArray children = normalized(widget.value(QStringLiteral("children")).toArray());
        if (children.isEmpty()) {
            widget.remove(QStringLiteral("children"));
        } else {
            widget.insert(QStringLiteral("children"), children);
        }
        widget.insert(QStringLiteral("properties"),
                      normalizedRangeProperties(widget.value(QStringLiteral("type")).toString(),
                                                widget.value(QStringLiteral("properties")).toObject()));
        out.append(widget);
    }
    return out;
}

QJsonObject jsonObject(const char *text) { return QJsonDocument::fromJson(text).object(); }

// The frame format of the parser spec's example A: AA 55 | LEN | CMD | temp (LE) | hum | SUM.
const char *const kHexConfigA = R"json({"header": "AA55", "footer": "", "lengthOffset": 2, "checksum": "和校验",
    "fields": [["cmd", 1, 1, "uint8"], ["temp", 2, 2, "int16", "小端(LE)"], ["hum", 4, 1, "uint8"]]})json";
// Example C: a header, and a silence to end the frame.
const char *const kHexConfigIdle = R"json({"header": "7E", "footer": "", "idleTimeout": 200,
    "fields": [["a", 0, 1, "uint8"]]})json";
const QByteArray kFrameA = QByteArray::fromHex("AA550401F6FF3C35");
const QByteArray kFrameA2 = QByteArray::fromHex("AA550402100028"
                                                "3D");

QList<QByteArray> rawOf(const QSignalSpy &spy)
{
    QList<QByteArray> out;
    for (const QList<QVariant> &args : spy) {
        out.append(args.first().toByteArray());
    }
    return out;
}

qlonglong messageValue(const QSignalSpy &spy, int index, const char *key)
{
    return spy.at(index).first().value<Message>().value(QLatin1String(key)).toLongLong();
}

// The editor next to a form label; the first match anywhere under `root`.
QWidget *fieldFor(QWidget *root, const QString &label)
{
    for (QFormLayout *form : root->findChildren<QFormLayout *>()) {
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem *caption = form->itemAt(row, QFormLayout::LabelRole);
            auto *text = caption ? qobject_cast<QLabel *>(caption->widget()) : nullptr;
            if (text && text->text() == label) {
                QLayoutItem *field = form->itemAt(row, QFormLayout::FieldRole);
                return field ? field->widget() : nullptr;
            }
        }
    }
    return nullptr;
}

bool labelShown(QWidget *root, const QString &text)
{
    for (QLabel *label : root->findChildren<QLabel *>()) {
        if (label->text() == text && label->isVisible()) {
            return true;
        }
    }
    return false;
}

QStringList labelTexts(QWidget *root)
{
    QStringList texts;
    for (QLabel *label : root->findChildren<QLabel *>()) {
        texts.append(label->text());
    }
    return texts;
}

QPushButton *buttonFor(QWidget *root, const QString &text)
{
    for (QPushButton *button : root->findChildren<QPushButton *>()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

} // namespace

class TestRuntime : public QObject
{
    Q_OBJECT

  private slots:
    void initTestCase()
    {
        Q_INIT_RESOURCE(fluentqtwidgets);
    }

    void everyTypeHasAWidget()
    {
        for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
            std::unique_ptr<HmiWidget> widget(createHmiWidget(meta.typeName, {}));
            QVERIFY2(widget, qPrintable(meta.typeName));
            QCOMPARE(widget->typeName(), meta.typeName);
        }
    }

    void examplesBuildAndRun_data()
    {
        QTest::addColumn<QString>("path");
        for (const QString &file : exampleFiles()) {
            QTest::newRow(qPrintable(QFileInfo(file).fileName())) << file;
        }
    }

    void examplesBuildAndRun()
    {
        QFETCH(QString, path);
        Project project;
        QString error;
        QVERIFY2(Project::load(path, &project, &error), qPrintable(error));
        RuntimeSurface surface;
        surface.build(project);
        int expected = 0;
        visitWidgets(project.widgets, [&expected](const QJsonObject &) { return ++expected > 0; });
        QCOMPARE(surface.widgets().size(), expected);
        QCOMPARE(surface.size(), QSize(project.canvasWidth, project.canvasHeight));
        surface.clear();
        QVERIFY(surface.widgets().isEmpty());
    }

    void designerRoundTripsExamples_data()
    {
        examplesBuildAndRun_data();
    }

    void designerRoundTripsExamples()
    {
        QFETCH(QString, path);
        Project project;
        QString error;
        QVERIFY2(Project::load(path, &project, &error), qPrintable(error));
        DesignScene scene;
        QVERIFY2(scene.loadWidgets(project.widgets, &error), qPrintable(error));
        QCOMPARE(normalized(scene.widgetsJson()), normalized(project.widgets));
    }

    void bindingUpdatesDisplayAndButtonSends()
    {
        Project project;
        project.widgets = QJsonArray{
            QJsonObject{{"id", "temp"},
                        {"type", "NumericDisplay"},
                        {"geometry", QJsonObject{{"x", 10}, {"y", 10}, {"w", 160}, {"h", 90}}},
                        {"properties", QJsonObject{{"decimals", "1"}}},
                        {"binding", QJsonObject{{"parseMode", "键值匹配"}, {"key", "t"}}}},
            QJsonObject{{"id", "run"},
                        {"type", "PushButton"},
                        {"geometry", QJsonObject{{"x", 10}, {"y", 120}, {"w", 120}, {"h", 36}}},
                        {"properties", QJsonObject{{"command", "RUN"}, {"commandNewline", "\\n"}}}},
        };
        RuntimeSurface surface;
        surface.build(project);
        FakeLink link;
        surface.context()->hub()->addLink(&link);

        link.inject("t=21.46\n");
        bool shown = false;
        for (QLabel *label : surface.findChildren<QLabel *>()) {
            shown = shown || label->text() == QStringLiteral("21.5");
        }
        QVERIFY(shown);

        auto *button = surface.findChild<FluentQt::PushButton *>();
        QVERIFY(button);
        button->click();
        QCOMPARE(link.sent, QList<QByteArray>{QByteArray("RUN\n")});
        surface.context()->hub()->removeLink(&link);
    }

    // The receive log shows text the way the link that received it spells it.
    void receiveLogDecodesWithTheLinksEncoding()
    {
        Project project;
        project.widgets = QJsonArray{QJsonObject{{"id", "log"},
                                                 {"type", "DataRecvCard"},
                                                 {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 600}, {"h", 300}}}}};
        RuntimeSurface surface;
        surface.build(project);
        FakeLink gbkLink;
        QString error;
        QVERIFY2(gbkLink.setParser(QStringLiteral("line"), QJsonObject{{"encoding", "gbk"}}, &error), qPrintable(error));
        FakeLink utf8Link;
        CommHub *hub = surface.context()->hub();
        hub->addLink(&gbkLink);
        hub->addLink(&utf8Link);
        const QByteArray gbk = QByteArray::fromHex("cec2b6c8") + ":25.3;"; // 温度:25.3;
        gbkLink.inject(gbk);
        QCOMPARE(hub->rawEncoding(), QStringLiteral("gbk"));
        QCOMPARE(hub->decodeRaw(gbk), QStringLiteral("温度:25.3;"));
        auto *log = surface.findChild<QTextEdit *>();
        QVERIFY(log);
        QVERIFY(log->toPlainText().contains(QStringLiteral("温度:25.3;")));
        utf8Link.inject(QByteArray::fromStdString("湿度=60\n"));
        QCOMPARE(hub->rawEncoding(), QStringLiteral("utf-8"));
        QVERIFY(log->toPlainText().contains(QStringLiteral("湿度=60")));
        hub->removeLink(&gbkLink);
        hub->removeLink(&utf8Link);
    }

    // A drag sends one command when the mouse lets go, whether it starts on the handle or on the
    // groove; arrow keys send each step; device feedback moving the handle sends nothing.
    void sliderSendsOnRelease()
    {
        Project project;
        project.widgets = QJsonArray{QJsonObject{
            {"id", "speed"},
            {"type", "Slider"},
            {"geometry", QJsonObject{{"x", 10}, {"y", 10}, {"w", 320}, {"h", 36}}},
            {"properties", QJsonObject{{"minimum", 0}, {"maximum", 3000}, {"value", 0}, {"sendOnChange", true},
                                       {"commandTemplate", "SPEED={value}"}}},
            {"binding", QJsonObject{{"parseMode", "键值匹配"}, {"key", "rpm"}}}}};
        RuntimeSurface surface;
        surface.build(project);
        surface.resize(400, 120);
        surface.show();
        QVERIFY(QTest::qWaitForWindowExposed(&surface));
        FakeLink link;
        surface.context()->hub()->addLink(&link);
        auto *slider = surface.findChild<FluentQt::Slider *>();
        QVERIFY(slider);
        const int y = slider->height() / 2;

        // from the handle (at value 0, the left end)
        QTest::mousePress(slider->handle(), Qt::LeftButton, {}, slider->handle()->rect().center());
        for (int x = 40; x <= 240; x += 20) {
            QTest::mouseMove(slider, QPoint(x, y));
        }
        QVERIFY(slider->value() > 1000);
        QVERIFY(link.sent.isEmpty());
        QTest::mouseRelease(slider->handle(), Qt::LeftButton, {}, slider->handle()->rect().center());
        QCOMPARE(link.sent.size(), 1);
        QVERIFY(link.sent.first().startsWith("SPEED=" + QByteArray::number(slider->value())));

        // from the groove: the press moves the value, the release sends it
        link.sent.clear();
        QTest::mousePress(slider, Qt::LeftButton, {}, QPoint(slider->width() - 30, y));
        QTest::mouseMove(slider, QPoint(slider->width() - 60, y));
        QVERIFY(link.sent.isEmpty());
        QTest::mouseRelease(slider, Qt::LeftButton, {}, QPoint(slider->width() - 60, y));
        QCOMPARE(link.sent.size(), 1);

        link.sent.clear();
        slider->setFocus();
        QTest::keyClick(slider, Qt::Key_Right);
        QTest::keyClick(slider, Qt::Key_Right);
        QCOMPARE(link.sent.size(), 2);

        link.sent.clear();
        link.inject("rpm=1500\n");
        QCOMPARE(slider->value(), 1500);
        QVERIFY(link.sent.isEmpty());
        surface.context()->hub()->removeLink(&link);
    }

    // The PLC tutorial: once its Modbus TCP link connects, the poller reads the bindings in three blocks,
    // one reply at a time, with no click; displays show the registers by address; the setpoint goes out as
    // FC16 float32 between two reads, the switch as FC05. The card shows the poller's status, its tooltip
    // the blocks; a block with an exception or without a reply is named. A link that goes down leaves the
    // poller waiting, and it starts again on connecting.
    void plcTutorialPollsOnConnecting()
    {
        Project project;
        QString error;
        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/10-PLC恒压供水.uidp"), &project, &error),
                 qPrintable(error));
        QJsonArray widgets = project.widgets;
        for (qsizetype i = 0; i < widgets.size(); ++i) {
            QJsonObject widget = widgets.at(i).toObject();
            if (widget.value("type").toString() == QLatin1String("ModbusConfigCard")) {
                QJsonObject props = widget.value("properties").toObject();
                props.insert("timeout", 50); // a missing reply shows within the test's patience
                widget.insert("properties", props);
                widgets[i] = widget;
            }
        }
        project.widgets = widgets;
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus_tcp"), {}));
        link.setConnected(false);
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        qint64 now = 0;
        surface.context()->setClock([&now]() { return now; });
        surface.context()->hub()->addLink(&link);
        QWidget *card = surface.findChild<QWidget *>(QStringLiteral("ModbusConfigCard"));
        QVERIFY(card);
        QVERIFY(labelShown(card, QStringLiteral("等待连接")));
        QVERIFY(buttonFor(card, QStringLiteral("停止"))); // polling, waiting for a link
        QVERIFY(link.sent.isEmpty());

        // A PLC with the register map in the PyQt UI Designer repo's examples/tutorials/README.md:
        // it answers reads, coil 0 (running) on, and echoes writes as Modbus does.
        const QHash<int, quint16> hr{{100, 0x3ED7}, {101, 0x0A3D}, {102, 0x4212}, {103, 0x0000},
                                     {104, 452},    {105, 76},     {200, 0x3ECC}, {201, 0xCCCD}};
        int answered = 0;
        int exceptionAt = -1; // reads from this address get exception 02
        bool coilsSilent = false;
        const auto serve = [&]() {
            while (answered < link.sent.size()) {
                const QByteArray adu = link.sent.at(answered++);
                const int address = quint8(adu.at(8)) << 8 | quint8(adu.at(9));
                if (!(coilsSilent && adu.at(7) == 1)) {
                    link.inject(plcReply(adu, hr, address == exceptionAt ? 2 : 0));
                }
            }
        };
        const auto status = [card](const QString &text) -> QLabel * {
            for (QLabel *label : card->findChildren<QLabel *>()) {
                if (label->text() == text) {
                    return label;
                }
            }
            return nullptr;
        };
        const QByteArray coils = QByteArray::fromHex("0100000004");
        const QByteArray live = QByteArray::fromHex("0300640006");
        const QByteArray setting = QByteArray::fromHex("0300c80002");

        link.setConnected(true);
        QCOMPARE(pdus(link.sent), QList<QByteArray>{coils});
        QVERIFY(status(QStringLiteral("3 块 · 轮询中")));
        serve();
        QCOMPARE(pdus(link.sent), (QList<QByteArray>{coils, live, setting}));
        QLabel *normal = status(QStringLiteral("3 块 · 正常"));
        QVERIFY(normal);
        QCOMPARE(normal->toolTip(), QStringLiteral("从站 1 · 功能码 1 · 地址 0–3 · 周期 500 ms\n"
                                                   "从站 1 · 功能码 3 · 地址 100–105 · 周期 500 ms\n"
                                                   "从站 1 · 功能码 3 · 地址 200–201 · 周期 500 ms"));
        const QStringList texts = labelTexts(&surface);
        for (const char *text : {"0.42", "36.5", "0.40"}) {
            QVERIFY2(texts.contains(QString::fromUtf8(text)), text);
        }
        bool level = false; // the ring paints its percentage, so read its value
        for (QProgressBar *ring : surface.findChildren<QProgressBar *>()) {
            level = level || ring->value() == 76;
        }
        QVERIFY(level);

        now = 500;
        tickPoller(surface.context()); // the next round starts with the coils
        QCOMPARE(link.sent.size(), 4);
        QLineEdit *setpoint = nullptr;
        for (auto *edit : surface.findChildren<QLineEdit *>()) {
            if (edit->placeholderText().startsWith(QStringLiteral("单位 MPa"))) {
                setpoint = edit;
            }
        }
        QVERIFY(setpoint);
        setpoint->setText(QStringLiteral("0.45"));
        QTest::keyClick(setpoint, Qt::Key_Return);
        QCOMPARE(link.sent.size(), 4); // the write waits for the read on the line
        serve();
        const QByteArray write = QByteArray::fromHex("1000c80002 04 3EE66666"); // 0.45 as float32
        QCOMPARE(pdus(link.sent).mid(4), (QList<QByteArray>{write, live, setting}));
        auto *runSwitch = surface.findChild<FluentQt::SwitchButton *>();
        QVERIFY(runSwitch);
        runSwitch->setChecked(true);
        serve();
        QCOMPARE(pdus(link.sent).last(), QByteArray::fromHex("050003ff00"));

        now = 1000;
        exceptionAt = 200;
        tickPoller(surface.context());
        serve();
        QVERIFY(status(QStringLiteral("第 3 块异常 02 非法数据地址")));
        exceptionAt = -1;

        now = 1500;
        coilsSilent = true;
        tickPoller(surface.context());
        QTRY_VERIFY((serve(), status(QStringLiteral("第 1 块无响应（从站 1 功能码 1 地址 0）")) != nullptr));
        coilsSilent = false;

        link.setConnected(false);
        QVERIFY(status(QStringLiteral("等待连接")));
        QVERIFY(buttonFor(card, QStringLiteral("停止"))); // still polling, not paused
        const qsizetype before = link.sent.size();
        link.setConnected(true); // a fresh plan, read at once
        QCOMPARE(link.sent.size(), before + 1);
        QCOMPARE(pdus(link.sent).last(), coils);
        surface.context()->hub()->removeLink(&link);
    }

    // Without a Modbus card the poller reads all the same, on its defaults (500 ms, 1000 ms, slave 1): the
    // PLC tutorial without its card reads its three blocks once a listening link gets a device, and shows
    // nothing. The tutorial with the temperature controller plans its four bindings and its chart's two
    // curves (holding register 4 as float32, and 1) into one block.
    void pollerNeedsNoCard()
    {
        Project project;
        QString error;
        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/10-PLC恒压供水.uidp"), &project, &error),
                 qPrintable(error));
        QJsonArray widgets;
        for (const QJsonValue &widget : std::as_const(project.widgets)) {
            if (widget.toObject().value("type").toString() != QLatin1String("ModbusConfigCard")) {
                widgets.append(widget);
            }
        }
        project.widgets = widgets;
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus_tcp"), {}));
        link.setSendable(false); // listening, no device yet
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        QVERIFY(!surface.findChild<QWidget *>(QStringLiteral("ModbusConfigCard")));
        ModbusPoller *poller = surface.context()->modbusPoller();
        QCOMPARE(poller->settings(), ModbusPoller::Settings{});
        QCOMPARE(poller->settings().defaultPeriod, 500);
        QCOMPARE(poller->settings().timeoutMs, 1000);
        QCOMPARE(poller->settings().fallbackSlave, 1);
        surface.context()->hub()->addLink(&link);
        QCOMPARE(poller->status(), QStringLiteral("等待连接"));
        QVERIFY(link.sent.isEmpty());
        link.setSendable(true);
        QCOMPARE(pdus(link.sent), QList<QByteArray>{QByteArray::fromHex("0100000004")});
        int answered = 0;
        while (answered < link.sent.size()) {
            link.inject(plcReply(link.sent.at(answered++), {}));
        }
        QCOMPARE(pdus(link.sent), (QList<QByteArray>{QByteArray::fromHex("0100000004"),
                                                     QByteArray::fromHex("0300640006"),
                                                     QByteArray::fromHex("0300c80002")}));
        QCOMPARE(poller->blocks(),
                 (QList<ModbusBlock>{{1, 1, 0, 4, 500}, {1, 3, 100, 6, 500}, {1, 3, 200, 2, 500}}));
        QCOMPARE(poller->status(), QStringLiteral("3 块 · 正常"));
        auto *timer = poller->findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(timer && timer->isActive());
        QCOMPARE(timer->interval(), 500);
        surface.context()->hub()->removeLink(&link);

        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/02-温控仪监控.uidp"), &project, &error),
                 qPrintable(error));
        FakeLink serial;
        QVERIFY(serial.setParser(QStringLiteral("modbus"), {}));
        RuntimeSurface controller;
        controller.build(project);
        controller.show();
        controller.context()->hub()->addLink(&serial);
        QCOMPARE(controller.context()->modbusPoller()->blocks(), (QList<ModbusBlock>{{1, 3, 0, 6, 200}}));
        QCOMPARE(serial.sent, QList<QByteArray>{QByteArray::fromHex("010300000006c5c8")});
        controller.context()->hub()->removeLink(&serial);
    }

    // The first Modbus card in the document sets the fallback slave, the default period (its 轮询ms field,
    // from the next keystroke on, 0 for 500 ms) and the timeout; a second card only shows the same status,
    // so one set of reads goes out. Either card's button pauses and resumes the poller for both: pausing
    // takes back the read not yet sent, resuming reads afresh.
    void firstCardSetsUpThePoller()
    {
        const auto rtu = [](const char *hex) {
            const QByteArray pdu = QByteArray::fromHex(hex);
            return pdu + modbusCrc(pdu);
        };
        const auto card = [](const char *id, int y, const QJsonObject &props) {
            return QJsonObject{{"id", id},
                               {"type", "ModbusConfigCard"},
                               {"geometry", QJsonObject{{"x", 200}, {"y", y}, {"w", 380}, {"h", 180}}},
                               {"properties", props}};
        };
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus"), {}));
        Project project;
        project.widgets = QJsonArray{
            modbusDisplay("near", 10, 0, 0), modbusDisplay("far", 101, 0, 0),
            card("first", 0, {{"defaultSlave", 7}, {"defaultPoll", 300}, {"timeout", 50}}),
            card("second", 200, {{"defaultSlave", 9}, {"defaultPoll", 100}, {"timeout", 2000}})};
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        qint64 now = 0;
        surface.context()->setClock([&now]() { return now; });
        ModbusPoller *poller = surface.context()->modbusPoller();
        QCOMPARE(poller->settings().fallbackSlave, 7);
        QCOMPARE(poller->settings().defaultPeriod, 300);
        QCOMPARE(poller->settings().timeoutMs, 50);
        const QList<QWidget *> cards = surface.findChildren<QWidget *>(QStringLiteral("ModbusConfigCard"));
        QCOMPARE(cards.size(), 2);
        const auto bothShow = [&cards](const QString &text) {
            return labelShown(cards.at(0), text) && labelShown(cards.at(1), text);
        };
        surface.context()->hub()->addLink(&link);
        QCOMPARE(link.sent, QList<QByteArray>{rtu("0703000a0001")}); // one read, with two cards
        QCOMPARE(poller->blocks(), (QList<ModbusBlock>{{7, 3, 10, 1, 300}, {7, 3, 101, 1, 300}}));
        QVERIFY(bothShow(QStringLiteral("2 块 · 轮询中")));
        QTRY_COMPARE(link.sent.size(), 2); // no reply within the first card's 50 ms
        QCOMPARE(link.sent.last(), rtu("070300650001"));
        link.inject(rtu("0703020001"));
        QVERIFY(bothShow(QStringLiteral("第 1 块无响应（从站 7 功能码 3 地址 10）")));

        // 发送 on the first card queues a write; the next read waits behind it, and pausing takes it back.
        QLineEdit *fieldsPoll = nullptr;
        for (auto *edit : cards.at(0)->findChildren<QLineEdit *>()) {
            if (edit->placeholderText() == QStringLiteral("默认 500")) {
                fieldsPoll = edit;
            }
        }
        QVERIFY(fieldsPoll);
        QCOMPARE(fieldsPoll->text(), QStringLiteral("300"));
        now = 300;
        tickPoller(surface.context());
        QCOMPARE(link.sent.last(), rtu("0703000a0001"));
        QPushButton *send = buttonFor(cards.at(0), QStringLiteral("发送"));
        QVERIFY(send);
        send->click(); // the fields' request: FC03 from 0, one register, slave 7
        link.inject(rtu("0703020001")); // the read: the write goes out, the next read waits behind it
        QCOMPARE(link.sent.last(), rtu("070300000001"));
        QPushButton *stop = buttonFor(cards.at(1), QStringLiteral("停止"));
        QVERIFY(stop);
        stop->click();
        QVERIFY(poller->isPaused());
        QVERIFY(bothShow(QStringLiteral("已暂停")));
        QVERIFY(buttonFor(cards.at(0), QStringLiteral("轮询")) && buttonFor(cards.at(1), QStringLiteral("轮询")));
        const qsizetype sent = link.sent.size();
        link.inject(rtu("0703020002")); // the write's reply; the read behind it was taken back
        QCOMPARE(link.sent.size(), sent);
        QVERIFY(labelShown(cards.at(0), QStringLiteral("设备已响应"))); // paused: the result on its status line
        QVERIFY(labelShown(cards.at(1), QStringLiteral("已暂停")));
        now = 900;
        poller->poll(); // nothing while paused, links change or not
        emit surface.context()->hub()->linksChanged();
        QCOMPARE(link.sent.size(), sent);

        buttonFor(cards.at(0), QStringLiteral("轮询"))->click();
        QVERIFY(!poller->isPaused());
        QVERIFY(buttonFor(cards.at(0), QStringLiteral("停止")) && buttonFor(cards.at(1), QStringLiteral("停止")));
        QVERIFY(bothShow(QStringLiteral("2 块 · 轮询中")));
        QCOMPARE(link.sent.size(), sent + 1); // a fresh plan: its first block at once
        QCOMPARE(link.sent.last(), rtu("0703000a0001"));
        link.inject(rtu("0703020001"));
        link.inject(rtu("0703020001"));
        QVERIFY(bothShow(QStringLiteral("2 块 · 正常")));

        auto *timer = poller->findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(timer && timer->isActive());
        QCOMPARE(timer->interval(), 300);
        fieldsPoll->setText(QStringLiteral("0")); // 500 ms from now on
        QCOMPARE(poller->settings().defaultPeriod, 500);
        QCOMPARE(timer->interval(), 500);
        QCOMPARE(poller->blocks(), (QList<ModbusBlock>{{7, 3, 10, 1, 500}, {7, 3, 101, 1, 500}}));
        QVERIFY(bothShow(QStringLiteral("2 块 · 轮询中")));
        for (auto *edit : cards.at(1)->findChildren<QLineEdit *>()) {
            if (edit->placeholderText() == QStringLiteral("默认 500")) {
                edit->setText(QStringLiteral("50")); // the second card's field changes nothing
            }
        }
        QCOMPARE(poller->settings().defaultPeriod, 500);
        QCOMPARE(timer->interval(), 500);
        surface.context()->hub()->removeLink(&link);
        QVERIFY(bothShow(QStringLiteral("等待连接")));
    }

    // Each block is read at its own period, on a clock the test steps; a block of a tab page in the
    // background is not read, and one that comes into view is read at once. A binding with no period
    // of its own is read at the default 500 ms.
    void pollerReadsEachBlockAtItsPeriod()
    {
        Project project;
        project.widgets = QJsonArray{
            QJsonObject{{"id", "tabs"},
                        {"type", "TabWidget"},
                        {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 400}, {"h", 300}}},
                        {"properties", QJsonObject{{"tabNames", "A\nB\nC"}}},
                        {"children", QJsonArray{modbusDisplay("fast", 0, 100, 0), modbusDisplay("slow", 50, 300, 0),
                                                modbusDisplay("other", 200, 0, 1)}}}};
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus_tcp"), {}));
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        qint64 now = 0;
        surface.context()->setClock([&now]() { return now; });
        ModbusPoller *poller = surface.context()->modbusPoller();
        QSignalSpy statuses(poller, &ModbusPoller::statusChanged);

        QHash<int, int> reads; // by start address
        int answered = 0;
        const auto serve = [&]() {
            while (answered < link.sent.size()) {
                const QByteArray adu = link.sent.at(answered++);
                ++reads[quint8(adu.at(8)) << 8 | quint8(adu.at(9))];
                link.inject(plcReply(adu, {}));
            }
        };
        const auto run = [&](qint64 until) {
            for (; now <= until; now += 100) {
                tickPoller(surface.context());
                serve();
            }
        };
        surface.context()->hub()->addLink(&link);
        QCOMPARE(statuses.size(), 1);
        QCOMPARE(statuses.at(0).at(0).toString(), QStringLiteral("2 块 · 轮询中"));
        serve();
        QCOMPARE(poller->status(), QStringLiteral("2 块 · 正常"));
        auto *timer = poller->findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(timer);
        QCOMPARE(timer->interval(), 100);
        now = 100;
        run(1200);
        QCOMPARE(reads.value(0), 13); // every 100 ms from 0 to 1200
        QCOMPARE(reads.value(50), 5); // every 300 ms
        QCOMPARE(reads.value(200), 0);

        auto *tabs = surface.findChild<QTabWidget *>();
        QVERIFY(tabs);
        tabs->setCurrentIndex(1);
        reads.clear();
        run(2500);
        QCOMPARE(reads, (QHash<int, int>{{200, 3}})); // at 1300, 1800 and 2300, and nothing from page A
        QCOMPARE(poller->status(), QStringLiteral("1 块 · 正常"));
        QCOMPARE(poller->toolTip(), QStringLiteral("从站 1 · 功能码 3 · 地址 200–200 · 周期 500 ms"));
        tabs->setCurrentIndex(2);
        const qsizetype sent = link.sent.size();
        run(2800);
        QCOMPARE(link.sent.size(), sent);
        QCOMPARE(poller->status(), QStringLiteral("没有 Modbus 绑定"));
        QCOMPARE(poller->toolTip(), QStringLiteral("没有 Modbus 绑定"));
        surface.context()->hub()->removeLink(&link);
        QCOMPARE(poller->status(), QStringLiteral("等待连接"));
    }

    // 发送 queues the fields' request as a write. While the poller reads, its result comes as a notification
    // and the status line keeps the poller's status; paused, the status line shows 已发送 and the result.
    // Without a link to take it, the status line says so.
    void modbusSendGoesBesideThePolling()
    {
        const auto rtu = [](const char *hex) {
            const QByteArray pdu = QByteArray::fromHex(hex);
            return pdu + modbusCrc(pdu);
        };
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus"), {}));
        Project project;
        project.widgets = QJsonArray{
            modbusDisplay("pv", 100, 0, 0),
            QJsonObject{{"id", "bus"},
                        {"type", "ModbusConfigCard"},
                        {"geometry", QJsonObject{{"x", 200}, {"y", 0}, {"w", 380}, {"h", 180}}},
                        {"properties", QJsonObject{{"defaultPoll", 60000}, {"timeout", 50}}}}};
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        QSignalSpy notes(surface.context(), &RuntimeContext::notificationRequested);
        surface.context()->hub()->addLink(&link);
        QCOMPARE(link.sent, QList<QByteArray>{rtu("010300640001")});
        QWidget *card = surface.findChild<QWidget *>(QStringLiteral("ModbusConfigCard"));
        QVERIFY(card);
        QPushButton *stop = buttonFor(card, QStringLiteral("停止"));
        QPushButton *send = buttonFor(card, QStringLiteral("发送"));
        QVERIFY(stop && send);
        QVERIFY(send->isEnabled());
        send->click(); // FC03 from 0, one register: queued behind the read on the line
        QCOMPARE(link.sent.size(), 1);
        QVERIFY(labelShown(card, QStringLiteral("1 块 · 轮询中")));
        link.inject(rtu("010302002a"));
        QCOMPARE(link.sent.last(), rtu("010300000001"));
        QVERIFY(labelShown(card, QStringLiteral("1 块 · 正常")));
        link.inject(rtu("0103020007"));
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.at(0), (QList<QVariant>{QStringLiteral("Modbus"), QStringLiteral("设备已响应"),
                                               NotifyType::Info, 2000}));
        QVERIFY(labelShown(card, QStringLiteral("1 块 · 正常")));
        send->click();
        QTRY_COMPARE(notes.size(), 2);
        QCOMPARE(notes.at(1), (QList<QVariant>{QStringLiteral("Modbus"), QStringLiteral("无响应（50 ms 超时）"),
                                               NotifyType::Warning, 2000}));
        QVERIFY(labelShown(card, QStringLiteral("1 块 · 正常")));

        stop->click(); // paused: the status line has the result
        QVERIFY(labelShown(card, QStringLiteral("已暂停")));
        send->click();
        QVERIFY(labelShown(card, QStringLiteral("已发送: 01 03 00 00 00 01 84 0A")));
        link.inject(rtu("0103020007"));
        QVERIFY(labelShown(card, QStringLiteral("设备已响应")));
        QCOMPARE(notes.size(), 2);

        surface.context()->hub()->removeLink(&link);
        send->click();
        QVERIFY(labelShown(card, QStringLiteral("发送失败，请检查连接")));
        QCOMPARE(notes.size(), 3); // and the context's 未连接，命令未发送
    }

    void linkCardsFitTheirRows_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("mode");
        for (const char *mode : {"完整", "简洁", "单行"}) {
            QTest::addRow("serial %s", mode) << QStringLiteral("SerialConfigCard") << QString::fromUtf8(mode);
        }
        for (const char *type : {"TcpConfigCard", "MqttConfigCard"}) {
            for (const char *mode : {"完整", "简洁"}) {
                QTest::addRow("%s %s", type, mode) << QString::fromLatin1(type) << QString::fromUtf8(mode);
            }
        }
    }

    // A link card has one button, text only and of fixed width, at the right end of its last visible row
    // (or of the single-row strip). The card is never shorter than its rows: their height fits the default
    // size, and asked for 60 px the card takes that height and its rows still do not overlap.
    void linkCardsFitTheirRows()
    {
        QFETCH(QString, type);
        QFETCH(QString, mode);
        std::unique_ptr<HmiWidget> card(createHmiWidget(type, QJsonObject{{"displayMode", mode}}));
        QVERIFY(card);
        card->show();
        const int minimum = card->minimumSizeHint().height();
        qInfo().noquote() << type << mode << "minimumSizeHint" << card->minimumSizeHint().width() << "x" << minimum;
        QVERIFY2(minimum <= card->meta().defaultHeight, qPrintable(QString::number(minimum)));
        if (mode == QStringLiteral("单行")) {
            QVERIFY2(minimum <= 48, qPrintable(QString::number(minimum))); // the strips of the examples: 48 and 50 px
        }
        QCOMPARE(card->minimumHeight(), minimum);

        QList<QPushButton *> buttons;
        for (QPushButton *button : card->findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("连接")) {
                buttons.append(button);
            }
        }
        QCOMPARE(buttons.size(), 1);
        QPushButton *button = buttons.first();
        QVERIFY(button->icon().isNull());
        QCOMPARE(button->minimumWidth(), button->maximumWidth());
        QGridLayout *grid = fieldGrid(card.get());
        QVERIFY(grid);
        QLayout *owner = nullptr;
        int ownerRow = -1;
        int lastRow = -1;
        for (int i = 0; i < grid->count(); ++i) {
            int row = 0, column = 0, rows = 0, columns = 0;
            grid->getItemPosition(i, &row, &column, &rows, &columns);
            lastRow = std::max(lastRow, row);
            QLayoutItem *item = grid->itemAt(i);
            if (item->widget() == button) {
                owner = grid;
                ownerRow = row;
            } else if (item->layout() && item->layout()->indexOf(button) >= 0) {
                owner = item->layout();
                ownerRow = row;
            }
        }
        QVERIFY(owner);
        QCOMPARE(ownerRow, lastRow);
        if (mode == QStringLiteral("单行")) {
            QCOMPARE(owner, grid);
        } else {
            auto *row = qobject_cast<QHBoxLayout *>(owner); // the last field, stretching, then the button
            QVERIFY(row);
            QCOMPARE(row->count(), 2);
            QCOMPARE(row->indexOf(button), 1);
            QCOMPARE(row->stretch(0), 1);
            QCOMPARE(row->stretch(1), 0);
        }

        card->resize(card->meta().defaultWidth, 60);
        QCOMPARE(card->height(), std::max(60, minimum));
        card->layout()->activate();
        QMap<int, QRect> rows;
        for (int i = 0; i < grid->count(); ++i) {
            int row = 0, column = 0, rowSpan = 0, columnSpan = 0;
            grid->getItemPosition(i, &row, &column, &rowSpan, &columnSpan);
            rows[row] |= grid->itemAt(i)->geometry();
        }
        QLayoutItem *header = card->layout()->itemAt(0);
        QRect previous = header->isEmpty() ? QRect() : header->geometry();
        for (const QRect &rect : std::as_const(rows)) {
            QVERIFY2(!previous.isValid() || rect.top() > previous.bottom(),
                     qPrintable(QStringLiteral("a row at y %1 overlaps the one ending at %2")
                                    .arg(rect.top())
                                    .arg(previous.bottom())));
            previous = rect;
        }
        QVERIFY(previous.bottom() < card->height());
    }

    // The designer keeps a link card at least as tall as its rows: a smaller height takes the minimum,
    // fewer rows (简洁) lower the minimum and keep the height, more rows raise it, and a card saved shorter
    // grows on opening. The single-row serial strip of the examples keeps its 50 px.
    void designerKeepsLinkCardsTallEnough()
    {
        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets(
                     QJsonArray{QJsonObject{{"id", "tcp"},
                                            {"type", "TcpConfigCard"},
                                            {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 360}, {"h", 250}}}},
                                QJsonObject{{"id", "old"},
                                            {"type", "TcpConfigCard"},
                                            {"geometry", QJsonObject{{"x", 0}, {"y", 300}, {"w", 360}, {"h", 120}}}}},
                     &error),
                 qPrintable(error));
        CanvasItem *tcp = scene.findItem(QStringLiteral("tcp"));
        QVERIFY(tcp);
        const qreal full = tcp->minimumHeight();
        QVERIFY(full > 100 && full <= 250);
        QCOMPARE(scene.findItem(QStringLiteral("old"))->size().height(), full);
        QCOMPARE(scene.widgetsJson().at(1).toObject().value("geometry").toObject().value("h").toDouble(), full);

        PropertyEditor editor;
        editor.setScene(&scene);
        editor.resize(360, 1400);
        editor.showItem(tcp);
        editor.show();
        QApplication::processEvents();
        auto *height = qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("高度")));
        QVERIFY(height);
        QCOMPARE(height->minimum(), int(std::ceil(full)));
        height->setValue(100);
        QCOMPARE(tcp->size().height(), full);
        tcp->setSize(QSizeF(360, 100)); // as the resize handles do
        QCOMPARE(tcp->size().height(), full);

        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("显示模式")));
        QVERIFY(mode);
        mode->setCurrentText(QStringLiteral("简洁"));
        const qreal brief = tcp->minimumHeight();
        QVERIFY(brief < full);
        QCOMPARE(tcp->size().height(), full);
        QCOMPARE(height->minimum(), int(std::ceil(brief)));
        tcp->setSize(QSizeF(360, brief));
        mode->setCurrentText(QStringLiteral("完整"));
        QCOMPARE(tcp->size().height(), full);
        QCOMPARE(height->value(), int(std::ceil(full)));

        Project project;
        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/04-环境监测站.uidp"), &project, &error),
                 qPrintable(error));
        DesignScene example;
        QVERIFY2(example.loadWidgets(project.widgets, &error), qPrintable(error));
        bool found = false;
        for (CanvasItem *item : example.topLevelItems()) {
            if (item->typeName() == QLatin1String("SerialConfigCard")) {
                QCOMPARE(item->size().height(), 50.0);
                found = true;
            }
        }
        QVERIFY(found);
    }

    // A link reports each whole frame of a hex parser as one raw unit, followed by its
    // messages, and a frame ended by silence the same way; line and Modbus parsers buffer by
    // themselves, so their units are the chunks as they came.
    void linkReportsWholeFrames()
    {
        FakeLink link;
        QString error;
        QVERIFY2(link.setParser(QStringLiteral("hex"), jsonObject(kHexConfigA), &error), qPrintable(error));
        QSignalSpy raw(&link, &Link::rawReceived);
        QSignalSpy messages(&link, &Link::messageReceived);
        const QByteArray stream = QByteArray::fromHex("1234") + kFrameA + kFrameA2;
        link.inject(stream.left(5));
        QVERIFY(raw.isEmpty());
        link.inject(stream.mid(5));
        QCOMPARE(rawOf(raw), (QList<QByteArray>{kFrameA, kFrameA2}));
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messageValue(messages, 0, "temp"), -10);
        QCOMPARE(messageValue(messages, 1, "hum"), 40);

        raw.clear();
        messages.clear();
        QVERIFY(link.setParser(QStringLiteral("hex"), jsonObject(kHexConfigIdle)));
        link.inject(QByteArray::fromHex("7E01"));
        link.inject(QByteArray::fromHex("02"));
        QVERIFY(raw.isEmpty());
        QTRY_COMPARE(rawOf(raw), QList<QByteArray>{QByteArray::fromHex("7E0102")});
        QCOMPARE(messageValue(messages, 0, "a"), 1);

        for (const char *type : {"line", "modbus"}) {
            raw.clear();
            QVERIFY(link.setParser(QLatin1String(type), {}));
            link.inject("a=1\nb=");
            QCOMPARE(rawOf(raw), QList<QByteArray>{QByteArray("a=1\nb=")});
        }
    }

    // Over TCP: a client reports whole frames; a server keeps a parser and an idle timer per
    // peer, so two devices' bytes never mix.
    void tcpLinksReportWholeFrames()
    {
        QTcpServer device;
        QVERIFY(device.listen(QHostAddress::LocalHost, 0));
        TcpClientLink client;
        QString error;
        QVERIFY2(client.setParser(QStringLiteral("hex"), jsonObject(kHexConfigA), &error), qPrintable(error));
        QSignalSpy raw(&client, &Link::rawReceived);
        QSignalSpy messages(&client, &Link::messageReceived);
        client.connectTo(QStringLiteral("127.0.0.1"), device.serverPort());
        QVERIFY(device.waitForNewConnection(5000));
        QTcpSocket *socket = device.nextPendingConnection();
        QTRY_VERIFY(client.isConnected());
        socket->write(QByteArray::fromHex("00") + kFrameA + kFrameA2);
        QTRY_COMPARE(rawOf(raw), (QList<QByteArray>{kFrameA, kFrameA2}));
        QCOMPARE(messageValue(messages, 1, "cmd"), 2);

        TcpServerLink server; // parses lines
        QVERIFY2(server.listen(QStringLiteral("127.0.0.1"), 0, &error), qPrintable(error));
        const quint16 port = quint16(server.description().section(QLatin1Char(':'), -1).toUInt());
        QSignalSpy serverRaw(&server, &Link::rawReceived);
        QSignalSpy serverMessages(&server, &Link::messageReceived);
        QTcpSocket first;
        QTcpSocket second;
        first.connectToHost(QHostAddress::LocalHost, port);
        second.connectToHost(QHostAddress::LocalHost, port);
        QTRY_COMPARE(server.clientCount(), 2);
        first.write("a=1");
        QTRY_COMPARE(serverRaw.size(), 1); // the server holds the first peer's unfinished record
        second.write("b=2\n");
        first.write("0\n");
        QTRY_COMPARE(serverMessages.size(), 2);
        const QList<Message> records{serverMessages.at(0).first().value<Message>(),
                                     serverMessages.at(1).first().value<Message>()};
        QVERIFY(records.contains(Message{{"a", 10LL}}));
        QVERIFY(records.contains(Message{{"b", 2LL}}));
        // Peers that connect after a change of parser frame on silence, each with its own timer.
        serverRaw.clear();
        serverMessages.clear();
        QVERIFY(server.setParser(QStringLiteral("hex"), jsonObject(kHexConfigIdle)));
        QTcpSocket third;
        QTcpSocket fourth;
        third.connectToHost(QHostAddress::LocalHost, port);
        fourth.connectToHost(QHostAddress::LocalHost, port);
        QTRY_COMPARE(server.clientCount(), 4);
        third.write(QByteArray::fromHex("7E03"));
        fourth.write(QByteArray::fromHex("7E04"));
        QTRY_COMPARE(serverMessages.size(), 2);
        QList<QByteArray> units = rawOf(serverRaw);
        std::sort(units.begin(), units.end());
        QCOMPARE(units, (QList<QByteArray>{QByteArray::fromHex("7E03"), QByteArray::fromHex("7E04")}));
        server.close();
    }

    // A HEX byte binding on a hex link gets whole frames: no header or footer to fill in, and
    // byteOffset counts from the frame's first byte.
    void hexBindingReadsWholeFrames()
    {
        Project project;
        project.widgets = QJsonArray{QJsonObject{{"id", "temp"},
                                                 {"type", "NumericDisplay"},
                                                 {"geometry", QJsonObject{{"x", 10}, {"y", 10}, {"w", 160}, {"h", 90}}},
                                                 {"properties", QJsonObject{{"decimals", "0"}}},
                                                 {"binding", QJsonObject{{"parseMode", "HEX字节提取"},
                                                                         {"byteOffset", 4},
                                                                         {"dataType", "int16"},
                                                                         {"byteOrder", "小端(LE)"}}}}};
        RuntimeSurface surface;
        surface.build(project);
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("hex"), jsonObject(kHexConfigA)));
        surface.context()->hub()->addLink(&link);
        for (const char c : QByteArray::fromHex("55") + kFrameA) {
            link.inject(QByteArray(1, c));
        }
        QVERIFY(labelTexts(&surface).contains(QStringLiteral("-10")));
        surface.context()->hub()->removeLink(&link);
    }

    // A card with its own mode frames with its own parserConfig; left empty, it takes the
    // project's when the modes match.
    void linkCardsUseTheirOwnFrameFormat_data()
    {
        QTest::addColumn<QByteArray>("projectParser");
        QTest::addColumn<QByteArray>("cardConfig");
        QTest::addColumn<QByteArray>("frame");
        QTest::addColumn<bool>("withFields");
        QTest::newRow("own") << QByteArray("line") << QByteArray(kHexConfigA) << kFrameA << true;
        QTest::newRow("project's") << QByteArray("hex") << QByteArray("{}") << kFrameA << true;
        QTest::newRow("defaults") << QByteArray("line") << QByteArray("{}") << QByteArray::fromHex("AA550155AA")
                                  << false;
    }

    void linkCardsUseTheirOwnFrameFormat()
    {
        QFETCH(QByteArray, projectParser);
        QFETCH(QByteArray, cardConfig);
        QFETCH(QByteArray, frame);
        QFETCH(bool, withFields);
        QTcpServer device;
        QVERIFY(device.listen(QHostAddress::LocalHost, 0));
        Project project;
        project.serial.insert(QStringLiteral("parser"), QString::fromLatin1(projectParser));
        if (projectParser == "hex") {
            project.serial.insert(QStringLiteral("parserConfig"), jsonObject(kHexConfigA));
        }
        project.widgets =
            QJsonArray{QJsonObject{{"id", "tcp"},
                                   {"type", "TcpConfigCard"},
                                   {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 360}, {"h", 290}}},
                                   {"properties", QJsonObject{{"defaultHost", "127.0.0.1"},
                                                              {"defaultPort", device.serverPort()},
                                                              {"defaultParser", "hex"},
                                                              {"parserConfig", jsonObject(cardConfig.constData())}}}}};
        RuntimeSurface surface;
        surface.build(project);
        QSignalSpy raw(surface.context()->hub(), &CommHub::rawReceived);
        QSignalSpy messages(surface.context()->hub(), &CommHub::messageReceived);
        QPushButton *connectButton = buttonFor(&surface, QStringLiteral("连接"));
        QVERIFY(connectButton);
        connectButton->click();
        QVERIFY(device.waitForNewConnection(5000));
        QTcpSocket *socket = device.nextPendingConnection();
        socket->write(QByteArray::fromHex("00") + frame);
        QTRY_COMPARE(raw.size(), 1);
        QCOMPARE(raw.first().first().toByteArray(), frame); // framed as hex, not passed on by chunk
        if (withFields) {
            QCOMPARE(messageValue(messages, 0, "temp"), -10);
        }
        surface.context()->hub()->disconnectAll();
    }

    // The project's 默认帧格式 shows the rows of its mode. Line rows, in order: separator,
    // terminator (picked or typed as escapes, stored as the characters), key-value separator and
    // encoding; hex rows write lengths, checksums and the field table, whose bad lines are named.
    void editorEditsTheProjectFrameFormat()
    {
        Project project;
        PropertyEditor editor;
        editor.setProject(&project);
        editor.resize(360, 1600);
        editor.show();
        QApplication::processEvents();
        QSignalSpy changed(&editor, &PropertyEditor::projectSettingsChanged);
        QFormLayout *form = nullptr;
        for (QFormLayout *candidate : editor.findChildren<QFormLayout *>()) {
            if (fieldFor(candidate->parentWidget(), QStringLiteral("分隔符"))) {
                form = candidate;
            }
        }
        QVERIFY(form);
        QStringList labels;
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem *caption = form->itemAt(row, QFormLayout::LabelRole);
            auto *text = caption ? qobject_cast<QLabel *>(caption->widget()) : nullptr;
            if (text && text->isVisible()) {
                labels << text->text();
            }
        }
        QCOMPARE(labels, (QStringList{QStringLiteral("解析模式"), QStringLiteral("分隔符"), QStringLiteral("行结束符"),
                                      QStringLiteral("键值分隔符"), QStringLiteral("编码")}));

        auto *terminator = qobject_cast<FluentQt::EditableComboBox *>(fieldFor(&editor, QStringLiteral("行结束符")));
        QVERIFY(terminator);
        QStringList items;
        for (int i = 0; i < terminator->count(); ++i) {
            items << terminator->itemText(i);
        }
        QCOMPARE(items, (QStringList{QStringLiteral("\\n"), QStringLiteral("\\r\\n"), QStringLiteral("\\r"),
                                     QStringLiteral(";")}));
        QVERIFY(!project.parserConfig().contains("terminator")); // showing a row writes nothing
        terminator->setCurrentIndex(1);
        QCOMPARE(project.parserConfig().value("terminator").toString(), QStringLiteral("\r\n"));
        terminator->setText(QStringLiteral("#\\t\\\\\\x"));
        QCOMPARE(project.parserConfig().value("terminator").toString(), QStringLiteral("#\t\\\\x"));
        terminator->setText(QString()); // never stored: the parser would refuse it
        QCOMPARE(project.parserConfig().value("terminator").toString(), QStringLiteral("#\t\\\\x"));
        qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("键值分隔符")))->setCurrentText(":");
        auto *encoding = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("编码")));
        QCOMPARE(encoding->count(), 3);
        encoding->setCurrentText(QStringLiteral("gbk"));
        QCOMPARE(project.parserConfig().value("kvSeparator").toString(), QStringLiteral(":"));
        QCOMPARE(project.parserConfig().value("encoding").toString(), QStringLiteral("gbk"));
        QString error;
        QVERIFY2(createParser(QStringLiteral("line"), project.parserConfig(), &error), qPrintable(error));
        // Shown again, the terminator is in escapes; a file's "GBK" shows as gbk.
        Project lineProject;
        lineProject.serial.insert(QStringLiteral("parserConfig"),
                                  QJsonObject{{"terminator", "#\t\\\\x"}, {"encoding", "GBK"}});
        PropertyEditor lineEditor;
        lineEditor.setProject(&lineProject);
        QCOMPARE(qobject_cast<QLineEdit *>(fieldFor(&lineEditor, QStringLiteral("行结束符")))->text(),
                 QStringLiteral("#\\t\\\\\\\\x"));
        QCOMPARE(qobject_cast<FluentQt::ComboBox *>(fieldFor(&lineEditor, QStringLiteral("编码")))->currentText(),
                 QStringLiteral("gbk"));

        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("解析模式")));
        mode->setCurrentText(QStringLiteral("hex"));
        QApplication::processEvents();
        QCOMPARE(project.parserType(), QStringLiteral("hex"));
        for (const char *row : {"帧头(HEX)", "帧尾(HEX)", "长度字段偏移", "长度字节数", "长度字节序", "长度含义",
                                "固定帧长", "校验", "校验起点", "校验字节序", "空闲超时(ms)", "字段表"}) {
            QVERIFY2(labelShown(&editor, QString::fromUtf8(row)), row);
        }
        for (const char *row : {"分隔符", "行结束符", "键值分隔符", "编码"}) {
            QVERIFY2(!labelShown(&editor, QString::fromUtf8(row)), row);
        }
        auto *offset = qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("长度字段偏移")));
        QCOMPARE(offset->minimum(), -1);
        offset->setValue(2);
        qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("校验")))->setCurrentText("CRC16-Modbus");
        qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("长度字节数")))->setCurrentText("2");
        QCOMPARE(project.parserConfig().value("lengthOffset"), QJsonValue(2));
        QCOMPARE(project.parserConfig().value("checksum").toString(), QStringLiteral("CRC16-Modbus"));
        QCOMPARE(project.parserConfig().value("lengthSize"), QJsonValue(2));
        auto *table = fieldFor(&editor, QStringLiteral("字段表"))->findChild<QPlainTextEdit *>();
        QVERIFY(table);
        table->setPlainText(QStringLiteral("# 温度\ntemp，0，2，int16,小端(LE)\n hum, 2, 1, uint8 "));
        const QJsonArray fields = project.parserConfig().value("fields").toArray();
        QCOMPARE(fields.size(), 2);
        QCOMPARE(fields.at(0).toArray(), (QJsonArray{"temp", 0, 2, "int16", "小端(LE)"}));
        QCOMPARE(fields.at(1).toArray(), (QJsonArray{"hum", 2, 1, "uint8"}));
        // A line that is not a field is pointed out, and the stored table stays as it was.
        table->setPlainText(QStringLiteral("temp,0,2,int16\n\nx,0,3,int16"));
        QVERIFY(labelShown(&editor, QStringLiteral("第 3 行格式错误")));
        QCOMPARE(project.parserConfig().value("fields").toArray(), fields);
        table->setPlainText(QStringLiteral("temp,0,2,int16"));
        QVERIFY(!labelShown(&editor, QStringLiteral("第 3 行格式错误")));
        QVERIFY(changed.size() >= 8);

        // What the rows wrote survives a save.
        QTemporaryDir dir;
        QVERIFY2(project.save(dir.filePath("p.uidp"), &error), qPrintable(error));
        Project reopened;
        QVERIFY2(Project::load(dir.filePath("p.uidp"), &reopened, &error), qPrintable(error));
        QCOMPARE(reopened.parserConfig(), project.parserConfig());
        PropertyEditor again;
        again.setProject(&reopened);
        QCOMPARE(qobject_cast<QSpinBox *>(fieldFor(&again, QStringLiteral("长度字段偏移")))->value(), 2);
        QCOMPARE(fieldFor(&again, QStringLiteral("字段表"))->findChild<QPlainTextEdit *>()->toPlainText(),
                 QStringLiteral("temp,0,2,int16"));

        mode->setCurrentText(QStringLiteral("modbus_ascii")); // a mode without frame rows
        QApplication::processEvents();
        QCOMPARE(project.parserType(), QStringLiteral("modbus_ascii"));
        QVERIFY(!labelShown(&editor, QStringLiteral("行结束符")));
        QVERIFY(!labelShown(&editor, QStringLiteral("帧头(HEX)")));
    }

    // An item's rows follow its mode. A link card shows its own 帧格式 for the mode it picks,
    // starting from the project's when the modes match; a Modbus-mode control keeps its Modbus
    // rows in view with the send toggle off, since it sends either way.
    void editorShowsAnItemsRowsByItsMode()
    {
        DesignScene scene;
        QString error;
        QVERIFY2(
            scene.loadWidgets(
                QJsonArray{QJsonObject{{"id", "uart"},
                                       {"type", "SerialConfigCard"},
                                       {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 360}, {"h", 340}}}},
                           QJsonObject{{"id", "setpoint"},
                                       {"type", "LineEdit"},
                                       {"geometry", QJsonObject{{"x", 0}, {"y", 400}, {"w", 200}, {"h", 33}}},
                                       {"properties", QJsonObject{{"commandMode", "Modbus"}, {"sendOnEnter", false}}}},
                           QJsonObject{{"id", "bus"},
                                       {"type", "ModbusConfigCard"},
                                       {"geometry", QJsonObject{{"x", 400}, {"y", 0}, {"w", 380}, {"h", 180}}}},
                           modbusDisplay("pv", 100, 0, 0)},
                &error),
            qPrintable(error));
        Project project;
        project.serial.insert(QStringLiteral("parser"), QStringLiteral("hex"));
        project.serial.insert(QStringLiteral("parserConfig"), jsonObject(kHexConfigA));
        PropertyEditor editor;
        editor.setProject(&project);
        editor.setScene(&scene);
        editor.resize(360, 1800);
        editor.showItem(scene.findItem(QStringLiteral("uart")));
        editor.show();
        QApplication::processEvents();
        QVERIFY(!labelShown(&editor, QStringLiteral("帧格式")));
        const auto pick = [&editor](const QString &parser) {
            auto *combo = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("默认解析模式")));
            QVERIFY(combo);
            combo->setCurrentText(parser);
            QApplication::processEvents(); // the form is rebuilt for the new mode
            QApplication::processEvents();
        };
        pick(QStringLiteral("hex"));
        QVERIFY(labelShown(&editor, QStringLiteral("帧格式")));
        QVERIFY(labelShown(&editor, QStringLiteral("固定帧长")));
        QVERIFY(!labelShown(&editor, QStringLiteral("分隔符")));
        // The card has no parserConfig yet and parses as the project does: the project's rows.
        QCOMPARE(qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("长度字段偏移")))->value(), 2);
        fieldFor(&editor, QStringLiteral("字段表"))
            ->findChild<QPlainTextEdit *>()
            ->setPlainText(QStringLiteral("v,0,2,uint16"));
        CanvasItem *item = scene.findItem(QStringLiteral("uart"));
        QJsonObject stored = item->propertyValue(QStringLiteral("parserConfig")).toObject();
        QCOMPARE(stored.value("lengthOffset"), QJsonValue(2));
        QCOMPARE(stored.value("fields").toArray().at(0).toArray(), (QJsonArray{"v", 0, 2, "uint16"}));
        QCOMPARE(project.parserConfig(), jsonObject(kHexConfigA)); // the project's is left alone
        // Saved and opened again, the card shows the same frame format.
        const QJsonArray saved = scene.widgetsJson();
        QCOMPARE(saved.at(0).toObject().value("properties").toObject().value("parserConfig").toObject(), stored);
        DesignScene reopened;
        QVERIFY2(reopened.loadWidgets(saved, &error), qPrintable(error));
        PropertyEditor again;
        again.setScene(&reopened);
        again.showItem(reopened.findItem(QStringLiteral("uart")));
        QCOMPARE(fieldFor(&again, QStringLiteral("字段表"))->findChild<QPlainTextEdit *>()->toPlainText(),
                 QStringLiteral("v,0,2,uint16"));

        pick(QStringLiteral("line"));
        QVERIFY(labelShown(&editor, QStringLiteral("分隔符")));
        QVERIFY(!labelShown(&editor, QStringLiteral("固定帧长")));
        qobject_cast<FluentQt::EditableComboBox *>(fieldFor(&editor, QStringLiteral("行结束符")))
            ->setCurrentText(QStringLiteral(";"));
        qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("编码")))
            ->setCurrentText(QStringLiteral("ascii"));
        stored = item->propertyValue(QStringLiteral("parserConfig")).toObject();
        QCOMPARE(stored.value("terminator").toString(), QStringLiteral(";"));
        QCOMPARE(stored.value("encoding").toString(), QStringLiteral("ascii"));
        QVERIFY(!project.parserConfig().contains("terminator"));
        pick(QStringLiteral("modbus_ascii"));
        QVERIFY(!labelShown(&editor, QStringLiteral("行结束符")));
        QVERIFY(!labelShown(&editor, QStringLiteral("帧头(HEX)")));
        pick(QStringLiteral("项目设置"));
        QVERIFY(!labelShown(&editor, QStringLiteral("帧格式")));

        editor.showItem(scene.findItem(QStringLiteral("setpoint")));
        QApplication::processEvents();
        QVERIFY(labelShown(&editor, QStringLiteral("发送模式")));
        QVERIFY(labelShown(&editor, QStringLiteral("功能码")));
        FluentQt::ComboBox *commandMode = nullptr;
        for (auto *combo : editor.findChildren<FluentQt::ComboBox *>()) {
            if (combo->currentText() == QStringLiteral("Modbus") && combo->findText(QStringLiteral("文本")) >= 0) {
                commandMode = combo;
            }
        }
        QVERIFY(commandMode);
        commandMode->setCurrentText(QStringLiteral("文本")); // text mode: the toggle off hides them
        QApplication::processEvents();
        QVERIFY(!labelShown(&editor, QStringLiteral("功能码")));
        QVERIFY(!labelShown(&editor, QStringLiteral("发送模式")));

        // A Modbus card has no poll table rows; a Modbus binding names a register table and an address, and
        // has its 采集周期(ms).
        editor.showItem(scene.findItem(QStringLiteral("bus")));
        QApplication::processEvents();
        QVERIFY(labelShown(&editor, QStringLiteral("默认轮询(ms)")));
        for (const char *gone : {"轮询方式", "连接后自动轮询", "轮询表"}) {
            QVERIFY2(!labelTexts(&editor).contains(QString::fromUtf8(gone)), gone);
        }
        editor.showItem(scene.findItem(QStringLiteral("pv")));
        QApplication::processEvents();
        auto *area = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("寄存器区")));
        QVERIFY(area);
        QCOMPARE(area->count(), 4);
        QCOMPARE(area->itemText(0), QStringLiteral("保持寄存器"));
        QCOMPARE(area->findText(QStringLiteral("响应内序号")), -1);
        QVERIFY(labelShown(&editor, QStringLiteral("寄存器地址")));
        QVERIFY(!labelTexts(&editor).contains(QStringLiteral("寄存器索引")));
        QVERIFY(labelShown(&editor, QStringLiteral("采集周期(ms)")));
        auto *period = qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("采集周期(ms)")));
        QVERIFY(period);
        QCOMPARE(period->maximum(), 60000);
        QCOMPARE(period->toolTip(), QStringLiteral("0 = 用默认周期（500 ms，或 Modbus 卡片的轮询 ms）"));
        period->setValue(250);
        QCOMPARE(scene.findItem(QStringLiteral("pv"))->binding().value("mbPeriod").toInt(), 250);
    }

    void exportCarriesProjectAndImages()
    {
        QTemporaryDir source;
        QTemporaryDir output;
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(source.filePath("logo.png")));

        Project project;
        project.appName = QStringLiteral("温控/面板");
        project.filePath = source.filePath("panel.uidp");
        project.widgets = QJsonArray{QJsonObject{{"id", "img"},
                                                 {"type", "ImageDisplay"},
                                                 {"properties", QJsonObject{{"imagePath", "logo.png"}}}}};
        QString error;
        const QString created = ExportService::exportApplication(project, output.path(), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo(created).fileName().startsWith(QStringLiteral("温控_面板")));
#ifdef Q_OS_MACOS
        const QDir resources(created + "/Contents/Resources");
        QVERIFY(QFileInfo::exists(created + "/Contents/MacOS/FluentHMIRuntime"));
#else
        const QDir resources(created);
#endif
        Project exported;
        QVERIFY2(Project::load(resources.filePath("project.uidp"), &exported, &error), qPrintable(error));
        QCOMPARE(exported.appName, project.appName);
        const QString bundled = exported.widgets.at(0).toObject().value("properties").toObject().value("imagePath").toString();
        QCOMPARE(bundled, QStringLiteral("assets/logo.png"));
        QVERIFY(QFileInfo::exists(resources.filePath(bundled)));

        // A second export to the same place refuses rather than overwriting.
        QVERIFY(ExportService::exportApplication(project, output.path(), &error).isEmpty());
        QVERIFY(!error.isEmpty());

        // A project without images still gets its project file.
        Project plain;
        plain.appName = QStringLiteral("plain");
        QVERIFY2(!ExportService::exportApplication(plain, output.path(), &error).isEmpty(), qPrintable(error));
    }

    void containerNestingInDesigner()
    {
        DesignScene scene;
        CanvasItem *group = scene.createItem("VBoxLayout", QPointF(100, 100));
        QVERIFY(group);
        group->setSize(QSizeF(300, 300));
        CanvasItem *child = scene.createItem("PushButton", QPointF(150, 200));
        QCOMPARE(child->containerParent(), group);
        QCOMPARE(child->pos().x(), 8.0); // laid out by the vertical box
        const QJsonArray json = scene.widgetsJson();
        QCOMPARE(json.size(), 1);
        QCOMPARE(json.at(0).toObject().value("children").toArray().size(), 1);
    }
};

QTEST_MAIN(TestRuntime)
#include "tst_runtime.moc"
