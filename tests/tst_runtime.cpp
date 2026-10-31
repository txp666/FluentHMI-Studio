#include "comm/comm_hub.h"
#include "comm/link.h"
#include "comm/tcp_link.h"
#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "designer/property_editor.h"
#include "runtime/hmi_widget.h"
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
#include <QtGui/QImage>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTextEdit>

using namespace fhmi;

namespace {

// A link that is always connected, records what is sent, and injects received bytes.
class FakeLink : public Link
{
  public:
    using Link::Link;
    QString description() const override { return QStringLiteral("fake"); }
    bool isConnected() const override { return true; }
    void close() override {}
    void inject(const QByteArray &bytes) { deliver(defaultParser(), bytes); }
    QList<QByteArray> sent;

  protected:
    qint64 writeBytes(const QByteArray &bytes) override
    {
        sent.append(bytes);
        return bytes.size();
    }
};

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
const char *const kHexConfigIdle = R"json({"header": "7E", "footer": "", "idleTimeout": 30,
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

    // The PLC tutorial example: the Modbus card polls three address blocks one reply at a time,
    // displays read registers by address, the setpoint goes out as FC16 float32, the switch as FC05.
    void plcExamplePollsBindsAndWrites()
    {
        Project project;
        QString error;
        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/10-PLC恒压供水.uidp"), &project, &error),
                 qPrintable(error));
        RuntimeSurface surface;
        surface.build(project);
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus_tcp"), {}));
        surface.context()->hub()->addLink(&link);

        // A PLC with the register map in the PyQt UI Designer repo's examples/tutorials/README.md.
        QHash<int, quint16> hr{{100, 0x3ED7}, {101, 0x0A3D}, {102, 0x4212}, {103, 0x0000},
                               {104, 452},    {105, 76},     {200, 0x3ECC}, {201, 0xCCCD}};
        QHash<int, int> coils{{0, 1}, {1, 0}, {2, 0}, {3, 0}};
        QList<QByteArray> requests;
        const auto answer = [&](const QByteArray &adu) {
            const int fc = quint8(adu.at(7));
            const int address = quint8(adu.at(8)) << 8 | quint8(adu.at(9));
            const int count = quint8(adu.at(10)) << 8 | quint8(adu.at(11));
            requests.append(adu.mid(7, 5));
            QByteArray pdu(1, char(fc));
            if (fc == 3) {
                pdu.append(char(2 * count));
                for (int i = 0; i < count; ++i) {
                    pdu.append(char(hr.value(address + i) >> 8)).append(char(hr.value(address + i)));
                }
            } else if (fc == 1) {
                int bits = 0;
                for (int i = 0; i < count; ++i) {
                    bits |= coils.value(address + i) << i;
                }
                pdu.append(char(1)).append(char(bits));
            } else if (fc == 5) {
                coils[address] = adu.mid(10, 2) == QByteArray::fromHex("ff00") ? 1 : 0;
                pdu = adu.mid(7, 5);
            } else if (fc == 16) {
                for (int i = 0; i < count; ++i) {
                    hr[address + i] = quint16(quint8(adu.at(13 + 2 * i)) << 8 | quint8(adu.at(14 + 2 * i)));
                }
                pdu = adu.mid(7, 5);
            }
            QByteArray reply = adu.left(4);
            reply.append(char(0)).append(char(pdu.size() + 1)).append(adu.at(6));
            return reply + pdu;
        };
        int answered = 0;
        const auto serve = [&]() {
            while (answered < link.sent.size()) {
                link.inject(answer(link.sent.at(answered++))); // a reply lets the card send the next read
            }
        };
        const auto shows = [&surface](const QString &text) {
            for (QLabel *label : surface.findChildren<QLabel *>()) {
                if (label->text() == text) {
                    return true;
                }
            }
            return false;
        };
        const auto button = [&surface](const QString &text) {
            for (auto *b : surface.findChildren<FluentQt::PushButton *>()) {
                if (b->text() == text) {
                    return b;
                }
            }
            return static_cast<FluentQt::PushButton *>(nullptr);
        };

        QVERIFY(button(QStringLiteral("轮询")));
        button(QStringLiteral("轮询"))->click();
        QCOMPARE(link.sent.size(), 1); // the first read waits for its reply
        serve();
        QCOMPARE(requests, (QList<QByteArray>{QByteArray::fromHex("030064 0006"), QByteArray::fromHex("0300c80002"),
                                              QByteArray::fromHex("0100000004")}));
        QVERIFY(shows(QStringLiteral("轮询正常 · 3/3")));
        for (const char *text : {"0.42", "36.5", "0.40"}) {
            QVERIFY2(shows(QString::fromUtf8(text)), text);
        }
        bool level = false; // the ring paints its percentage, so read its value
        for (QProgressBar *ring : surface.findChildren<QProgressBar *>()) {
            level = level || ring->value() == 76;
        }
        QVERIFY(level);

        QLineEdit *setpoint = nullptr;
        for (auto *edit : surface.findChildren<QLineEdit *>()) {
            if (edit->placeholderText().startsWith(QStringLiteral("单位 MPa"))) {
                setpoint = edit;
            }
        }
        QVERIFY(setpoint);
        setpoint->setText(QStringLiteral("0.45"));
        QTest::keyClick(setpoint, Qt::Key_Return);
        serve();
        QCOMPARE(requests.last(), QByteArray::fromHex("1000c80002"));
        QCOMPARE((QList<quint16>{hr.value(200), hr.value(201)}), (QList<quint16>{0x3EE6, 0x6666}));
        auto *runSwitch = surface.findChild<FluentQt::SwitchButton *>();
        QVERIFY(runSwitch);
        runSwitch->setChecked(true);
        serve();
        QCOMPARE(coils.value(3), 1);
        surface.context()->hub()->removeLink(&link);
    }

    // A card waits for each reply: no reply is a timeout, an exception code is shown by name.
    void modbusCardReportsTimeoutsAndExceptions()
    {
        Project project;
        project.widgets = QJsonArray{QJsonObject{
            {"id", "bus"},
            {"type", "ModbusConfigCard"},
            {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 380}, {"h", 180}}},
            {"properties", QJsonObject{{"defaultPoll", 60000}, {"timeout", 50}, {"pollList", "1,3,0,2\n1,1,0,3"}}}}};
        RuntimeSurface surface;
        surface.build(project);
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus"), {}));
        surface.context()->hub()->addLink(&link);
        const auto status = [&surface]() {
            QStringList texts;
            for (QLabel *label : surface.findChildren<QLabel *>()) {
                texts.append(label->text());
            }
            return texts;
        };
        const auto rtu = [](const char *hex) {
            const QByteArray pdu = QByteArray::fromHex(hex);
            return pdu + modbusCrc(pdu);
        };
        FluentQt::PushButton *poll = nullptr;
        FluentQt::PushButton *send = nullptr;
        for (auto *b : surface.findChildren<FluentQt::PushButton *>()) {
            (b->text() == QStringLiteral("轮询") ? poll : send) = b;
        }
        QVERIFY(poll && send);

        poll->click();
        QCOMPARE(link.sent, QList<QByteArray>{rtu("010300000002")});
        QTRY_COMPARE(link.sent.size(), 2); // no reply within 50 ms: the round moves on to the coils
        QCOMPARE(link.sent.last(), rtu("010100000003"));
        link.inject(rtu("01010201ff")); // two bytes of coils: an answer to some other read
        QCOMPARE(link.sent.size(), 2);
        QVERIFY(status().contains(QStringLiteral("轮询中 · 2 条")));
        link.inject(rtu("018102"));
        QVERIFY(status().contains(QStringLiteral("第 1 条无响应 · 0/2")));
        poll->click(); // stop

        send->click(); // the fields' request: FC03 from 0, one register
        link.inject(rtu("010302002a"));
        QVERIFY(status().contains(QStringLiteral("设备已响应")));
        send->click();
        QTRY_VERIFY(status().contains(QStringLiteral("无响应（50 ms 超时）")));
        surface.context()->hub()->removeLink(&link);
    }

    // With no link parsing Modbus no reply can be recognised: each tick sends the next request.
    void modbusCardSendsEachTickWithoutAModbusParser()
    {
        Project project;
        project.widgets =
            QJsonArray{QJsonObject{{"id", "bus"},
                                   {"type", "ModbusConfigCard"},
                                   {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 380}, {"h", 180}}},
                                   {"properties", QJsonObject{{"defaultPoll", 20}, {"pollList", "1,3,0,2\n1,1,0,3"}}}}};
        RuntimeSurface surface;
        surface.build(project);
        FakeLink link; // parses lines
        surface.context()->hub()->addLink(&link);
        FluentQt::PushButton *poll = nullptr;
        for (auto *b : surface.findChildren<FluentQt::PushButton *>()) {
            if (b->text() == QStringLiteral("轮询")) {
                poll = b;
            }
        }
        QVERIFY(poll);
        poll->click();
        QTRY_VERIFY(link.sent.size() >= 3);
        QCOMPARE(link.sent.at(0), link.sent.at(2));
        QVERIFY(link.sent.at(0) != link.sent.at(1));
        poll->click();
        surface.context()->hub()->removeLink(&link);
    }

    // On a link parsing Modbus ASCII the card sends ASCII frames and waits for each ASCII reply.
    void modbusCardWaitsForAsciiReplies()
    {
        Project project;
        project.widgets = QJsonArray{QJsonObject{
            {"id", "bus"},
            {"type", "ModbusConfigCard"},
            {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 380}, {"h", 180}}},
            {"properties", QJsonObject{{"defaultPoll", 60000}, {"timeout", 5000}, {"pollList", "1,3,0,2\n1,1,0,3"}}}}};
        RuntimeSurface surface;
        surface.build(project);
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus_ascii"), {}));
        surface.context()->hub()->addLink(&link);
        QPushButton *poll = nullptr;
        for (QPushButton *b : surface.findChildren<QPushButton *>()) {
            if (b->text() == QStringLiteral("轮询")) {
                poll = b;
            }
        }
        QVERIFY(poll);
        poll->click();
        QCOMPARE(link.sent, QList<QByteArray>{":010300000002FA\r\n"}); // upper-case hex and LRC
        const QByteArray reply = ":0103040001FFFFF9\r\n";
        link.inject(reply.left(7));
        QCOMPARE(link.sent.size(), 1); // half a reply: still waiting
        link.inject(reply.mid(7));
        QCOMPARE(link.sent, (QList<QByteArray>{":010300000002FA\r\n", ":010100000003FB\r\n"}));
        link.inject(modbusAsciiFrame(QByteArray::fromHex("01010105")));
        bool shown = false;
        for (QLabel *label : surface.findChildren<QLabel *>()) {
            shown = shown || label->text() == QStringLiteral("轮询正常 · 2/2");
        }
        QVERIFY(shown);
        poll->click();
        surface.context()->hub()->removeLink(&link);
    }

    // A Modbus-mode control sends whether or not its send toggle is on, so the editor keeps its
    // Modbus settings in view; in text mode they still hide while the toggle is off.
    void editorKeepsModbusSettingsInViewWithTheToggleOff()
    {
        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets(QJsonArray{QJsonObject{
                                       {"id", "setpoint"},
                                       {"type", "LineEdit"},
                                       {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 200}, {"h", 33}}},
                                       {"properties", QJsonObject{{"commandMode", "Modbus"}, {"sendOnEnter", false}}}}},
                                   &error),
                 qPrintable(error));
        PropertyEditor editor;
        editor.setScene(&scene);
        editor.resize(360, 1400);
        editor.showItem(scene.findItem(QStringLiteral("setpoint")));
        editor.show();
        QApplication::processEvents();
        const auto shown = [&editor](const QString &text) {
            for (QLabel *label : editor.findChildren<QLabel *>()) {
                if (label->text() == text && label->isVisible()) {
                    return true;
                }
            }
            return false;
        };
        QVERIFY(shown(QStringLiteral("发送模式")));
        QVERIFY(shown(QStringLiteral("功能码")));
        FluentQt::ComboBox *mode = nullptr;
        for (auto *combo : editor.findChildren<FluentQt::ComboBox *>()) {
            if (combo->currentText() == QStringLiteral("Modbus") && combo->findText(QStringLiteral("文本")) >= 0) {
                mode = combo;
            }
        }
        QVERIFY(mode);
        mode->setCurrentText(QStringLiteral("文本"));
        QApplication::processEvents();
        QVERIFY(!shown(QStringLiteral("功能码")));
        QVERIFY(!shown(QStringLiteral("发送模式")));
    }

    // The serial link's read path: each complete frame of a hex link is one raw unit, followed
    // by its messages; a frame ended by silence goes out the same way; line mode stays by chunk.
    void linkReportsWholeFramesAndIdleFlushes()
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
        QTRY_COMPARE(raw.size(), 1);
        QCOMPARE(raw.first().first().toByteArray(), QByteArray::fromHex("7E0102"));
        QCOMPARE(messageValue(messages, 0, "a"), 1);

        raw.clear();
        messages.clear();
        QVERIFY(link.setParser(QStringLiteral("line"), {}));
        link.inject("a=1\nb=");
        QCOMPARE(rawOf(raw), QList<QByteArray>{QByteArray("a=1\nb=")});
        QCOMPARE(messages.size(), 1);
    }

    void tcpClientReportsWholeFrames()
    {
        QTcpServer device;
        QVERIFY(device.listen(QHostAddress::LocalHost, 0));
        TcpClientLink link;
        QString error;
        QVERIFY2(link.setParser(QStringLiteral("hex"), jsonObject(kHexConfigA), &error), qPrintable(error));
        QSignalSpy raw(&link, &Link::rawReceived);
        QSignalSpy messages(&link, &Link::messageReceived);
        link.connectTo(QStringLiteral("127.0.0.1"), device.serverPort());
        QTRY_VERIFY(device.hasPendingConnections());
        QTcpSocket *socket = device.nextPendingConnection();
        QTRY_VERIFY(link.isConnected());
        socket->write(QByteArray::fromHex("00") + kFrameA + kFrameA2.left(3));
        socket->flush();
        QTRY_COMPARE(raw.size(), 1);
        socket->write(kFrameA2.mid(3));
        QTRY_COMPARE(raw.size(), 2);
        QCOMPARE(rawOf(raw), (QList<QByteArray>{kFrameA, kFrameA2}));
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messageValue(messages, 1, "cmd"), 2);

        raw.clear();
        messages.clear();
        QVERIFY(link.setParser(QStringLiteral("hex"), jsonObject(kHexConfigIdle)));
        socket->write(QByteArray::fromHex("7E09"));
        QTRY_COMPARE(messages.size(), 1);
        QCOMPARE(rawOf(raw), QList<QByteArray>{QByteArray::fromHex("7E09")});
        QCOMPARE(messageValue(messages, 0, "a"), 9);

        raw.clear();
        QVERIFY(link.setParser(QStringLiteral("line"), {}));
        socket->write("t=1\nu=");
        QTRY_COMPARE(rawOf(raw).join(), QByteArray("t=1\nu=")); // chunks, so the unfinished line too
    }

    // Each device on a server has its own parser and idle timer, so their frames never mix.
    void tcpServerFramesEachPeerApart()
    {
        TcpServerLink link;
        QString error;
        QVERIFY2(link.setParser(QStringLiteral("hex"), jsonObject(kHexConfigA), &error), qPrintable(error));
        QVERIFY2(link.listen(QStringLiteral("127.0.0.1"), 0, &error), qPrintable(error));
        const quint16 port = quint16(link.description().section(QLatin1Char(':'), -1).toUInt());
        QSignalSpy raw(&link, &Link::rawReceived);
        QSignalSpy messages(&link, &Link::messageReceived);
        QTcpSocket first;
        QTcpSocket second;
        first.connectToHost(QHostAddress::LocalHost, port);
        second.connectToHost(QHostAddress::LocalHost, port);
        QTRY_COMPARE(link.clientCount(), 2);
        first.write(kFrameA.left(4));
        first.flush();
        QTest::qWait(50);
        second.write(kFrameA2);
        QTRY_COMPARE(raw.size(), 1);
        first.write(kFrameA.mid(4));
        QTRY_COMPARE(raw.size(), 2);
        QCOMPARE(rawOf(raw), (QList<QByteArray>{kFrameA2, kFrameA}));
        QCOMPARE(messageValue(messages, 0, "cmd"), 2);
        QCOMPARE(messageValue(messages, 1, "cmd"), 1);
        link.close();

        QVERIFY(link.setParser(QStringLiteral("hex"), jsonObject(kHexConfigIdle)));
        QVERIFY2(link.listen(QStringLiteral("127.0.0.1"), 0, &error), qPrintable(error));
        const quint16 idlePort = quint16(link.description().section(QLatin1Char(':'), -1).toUInt());
        raw.clear();
        messages.clear();
        QTcpSocket third;
        QTcpSocket fourth;
        third.connectToHost(QHostAddress::LocalHost, idlePort);
        fourth.connectToHost(QHostAddress::LocalHost, idlePort);
        QTRY_COMPARE(link.clientCount(), 2);
        third.write(QByteArray::fromHex("7E03"));
        fourth.write(QByteArray::fromHex("7E04"));
        QTRY_COMPARE(messages.size(), 2);
        QList<QByteArray> units = rawOf(raw);
        std::sort(units.begin(), units.end());
        QCOMPARE(units, (QList<QByteArray>{QByteArray::fromHex("7E03"), QByteArray::fromHex("7E04")}));
        link.close();
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
        bool shown = false;
        for (QLabel *label : surface.findChildren<QLabel *>()) {
            shown = shown || label->text() == QStringLiteral("-10");
        }
        QVERIFY(shown);
        surface.context()->hub()->removeLink(&link);
    }

    // A card with its own mode frames with its own parserConfig; left empty, it takes the
    // project's when the modes match.
    void linkCardsUseTheirOwnFrameFormat_data()
    {
        QTest::addColumn<QByteArray>("projectParser");
        QTest::addColumn<QByteArray>("cardConfig");
        QTest::addColumn<QByteArray>("frame");
        QTest::addColumn<QString>("key");
        QTest::newRow("own") << QByteArray("line") << QByteArray(kHexConfigA) << kFrameA << QStringLiteral("temp");
        QTest::newRow("project's") << QByteArray("hex") << QByteArray("{}") << kFrameA << QStringLiteral("temp");
        QTest::newRow("defaults") << QByteArray("line") << QByteArray("{}") << QByteArray::fromHex("AA550155AA")
                                  << QString();
    }

    void linkCardsUseTheirOwnFrameFormat()
    {
        QFETCH(QByteArray, projectParser);
        QFETCH(QByteArray, cardConfig);
        QFETCH(QByteArray, frame);
        QFETCH(QString, key);
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
        QPushButton *connectButton = nullptr;
        for (QPushButton *button : surface.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("连接")) {
                connectButton = button;
            }
        }
        QVERIFY(connectButton);
        connectButton->click();
        QTRY_VERIFY(device.hasPendingConnections());
        QTcpSocket *socket = device.nextPendingConnection();
        socket->write(QByteArray::fromHex("00") + frame);
        QTRY_COMPARE(raw.size(), 1);
        QCOMPARE(raw.first().first().toByteArray(), frame); // framed as hex, not passed on by chunk
        if (!key.isEmpty()) {
            QCOMPARE(messageValue(messages, 0, "temp"), -10);
        }
        surface.context()->hub()->disconnectAll();
    }

    void editorShowsTheProjectFrameFormatByMode()
    {
        Project project;
        PropertyEditor editor;
        editor.setProject(&project);
        editor.resize(360, 1600);
        editor.show();
        QApplication::processEvents();
        QSignalSpy changed(&editor, &PropertyEditor::projectSettingsChanged);
        QVERIFY(labelShown(&editor, QStringLiteral("分隔符")));
        QVERIFY(!labelShown(&editor, QStringLiteral("长度字段偏移")));
        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("解析模式")));
        QVERIFY(mode);
        mode->setCurrentText(QStringLiteral("hex"));
        QApplication::processEvents();
        QCOMPARE(project.parserType(), QStringLiteral("hex"));
        for (const char *row : {"帧头(HEX)", "帧尾(HEX)", "长度字段偏移", "长度字节数", "长度字节序", "长度含义",
                                "固定帧长", "校验", "校验起点", "校验字节序", "空闲超时(ms)", "字段表"}) {
            QVERIFY2(labelShown(&editor, QString::fromUtf8(row)), row);
        }
        QVERIFY(!labelShown(&editor, QStringLiteral("分隔符")));

        auto *offset = qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("长度字段偏移")));
        QVERIFY(offset);
        QCOMPARE(offset->minimum(), -1);
        offset->setValue(2);
        QCOMPARE(project.parserConfig().value("lengthOffset"), QJsonValue(2));
        auto *checksum = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("校验")));
        checksum->setCurrentText(QStringLiteral("CRC16-Modbus"));
        auto *size = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("长度字节数")));
        size->setCurrentText(QStringLiteral("2"));
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
        table->setPlainText(QStringLiteral("temp,0,2"));
        QVERIFY(labelShown(&editor, QStringLiteral("第 1 行格式错误")));
        table->setPlainText(QStringLiteral("temp,0,2,int16"));
        QVERIFY(!labelShown(&editor, QStringLiteral("第 1 行格式错误")));
        QVERIFY(changed.size() >= 6);

        // What the rows wrote survives a save and builds a parser.
        QTemporaryDir dir;
        QString error;
        QVERIFY2(project.save(dir.filePath("p.uidp"), &error), qPrintable(error));
        Project reopened;
        QVERIFY2(Project::load(dir.filePath("p.uidp"), &reopened, &error), qPrintable(error));
        QCOMPARE(reopened.parserConfig(), project.parserConfig());
        PropertyEditor again;
        again.setProject(&reopened);
        QCOMPARE(qobject_cast<QSpinBox *>(fieldFor(&again, QStringLiteral("长度字段偏移")))->value(), 2);
        QCOMPARE(fieldFor(&again, QStringLiteral("字段表"))->findChild<QPlainTextEdit *>()->toPlainText(),
                 QStringLiteral("temp,0,2,int16"));
    }

    // Line mode's rows, in order: separator, terminator (picked or typed as escapes, stored as
    // the characters), key-value separator and encoding.
    void editorEditsTheLineFrameRows()
    {
        Project project;
        PropertyEditor editor;
        editor.setProject(&project);
        editor.resize(360, 1600);
        editor.show();
        QApplication::processEvents();
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
        QCOMPARE(terminator->currentText(), QStringLiteral("\\n"));
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
        auto *kv = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("键值分隔符")));
        QCOMPARE(kv->count(), 2);
        kv->setCurrentText(QStringLiteral(":"));
        auto *encoding = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("编码")));
        QCOMPARE(encoding->count(), 3);
        encoding->setCurrentText(QStringLiteral("gbk"));
        QCOMPARE(project.parserConfig().value("kvSeparator").toString(), QStringLiteral(":"));
        QCOMPARE(project.parserConfig().value("encoding").toString(), QStringLiteral("gbk"));
        QString error;
        QVERIFY2(createParser(QStringLiteral("line"), project.parserConfig(), &error), qPrintable(error));

        // Shown again, the terminator is in escapes; a file's "GBK" shows as gbk.
        project.serial.insert(QStringLiteral("parserConfig"),
                              QJsonObject{{"terminator", "#\t\\\\x"}, {"encoding", "GBK"}});
        PropertyEditor again;
        again.setProject(&project);
        QCOMPARE(qobject_cast<QLineEdit *>(fieldFor(&again, QStringLiteral("行结束符")))->text(),
                 QStringLiteral("#\\t\\\\\\\\x"));
        QCOMPARE(qobject_cast<FluentQt::ComboBox *>(fieldFor(&again, QStringLiteral("编码")))->currentText(),
                 QStringLiteral("gbk"));

        // Other modes hide them; modbus_ascii is one of the modes.
        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("解析模式")));
        mode->setCurrentText(QStringLiteral("hex"));
        QApplication::processEvents();
        for (const char *row : {"分隔符", "行结束符", "键值分隔符", "编码"}) {
            QVERIFY2(!labelShown(&editor, QString::fromUtf8(row)), row);
        }
        mode->setCurrentText(QStringLiteral("modbus_ascii"));
        QApplication::processEvents();
        QCOMPARE(project.parserType(), QStringLiteral("modbus_ascii"));
        QVERIFY(!labelShown(&editor, QStringLiteral("行结束符")));
        QVERIFY(!labelShown(&editor, QStringLiteral("帧头(HEX)")));
    }

    void editorEditsALinkCardsOwnFrameFormat()
    {
        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets(
                     QJsonArray{QJsonObject{{"id", "uart"},
                                            {"type", "SerialConfigCard"},
                                            {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 360}, {"h", 340}}}}},
                     &error),
                 qPrintable(error));
        Project project; // line mode
        PropertyEditor editor;
        editor.setProject(&project);
        editor.setScene(&scene);
        editor.resize(360, 1800);
        editor.showItem(scene.findItem(QStringLiteral("uart")));
        editor.show();
        QApplication::processEvents();
        QVERIFY(!labelShown(&editor, QStringLiteral("帧格式")));
        QVERIFY(!labelShown(&editor, QStringLiteral("帧头(HEX)")));
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
        auto *fixed = qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("固定帧长")));
        QVERIFY(fixed);
        fixed->setValue(8);
        fieldFor(&editor, QStringLiteral("字段表"))
            ->findChild<QPlainTextEdit *>()
            ->setPlainText(QStringLiteral("v,0,2,uint16"));
        CanvasItem *item = scene.findItem(QStringLiteral("uart"));
        QJsonObject stored = item->propertyValue(QStringLiteral("parserConfig")).toObject();
        QCOMPARE(stored.value("fixedLength"), QJsonValue(8));
        QCOMPARE(stored.value("fields").toArray().size(), 1);
        QVERIFY(!project.parserConfig().contains("fixedLength")); // the project's is left alone

        // Saved and opened again, the card shows the same frame format.
        const QJsonArray saved = scene.widgetsJson();
        QCOMPARE(saved.at(0).toObject().value("properties").toObject().value("parserConfig").toObject(), stored);
        DesignScene reopened;
        QVERIFY2(reopened.loadWidgets(saved, &error), qPrintable(error));
        PropertyEditor again;
        again.setProject(&project);
        again.setScene(&reopened);
        again.showItem(reopened.findItem(QStringLiteral("uart")));
        QCOMPARE(qobject_cast<QSpinBox *>(fieldFor(&again, QStringLiteral("固定帧长")))->value(), 8);

        pick(QStringLiteral("line"));
        QVERIFY(labelShown(&editor, QStringLiteral("分隔符")));
        QVERIFY(!labelShown(&editor, QStringLiteral("固定帧长")));
        // The card's line rows write its own parserConfig, as the hex ones do.
        QVERIFY(labelShown(&editor, QStringLiteral("行结束符")));
        qobject_cast<FluentQt::EditableComboBox *>(fieldFor(&editor, QStringLiteral("行结束符")))
            ->setCurrentText(QStringLiteral(";"));
        qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("编码")))
            ->setCurrentText(QStringLiteral("ascii"));
        stored = item->propertyValue(QStringLiteral("parserConfig")).toObject();
        QCOMPARE(stored.value("terminator").toString(), QStringLiteral(";"));
        QCOMPARE(stored.value("encoding").toString(), QStringLiteral("ascii"));
        QVERIFY(!project.parserConfig().contains("terminator"));
        auto *modes = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("默认解析模式")));
        QVERIFY(modes->findText(QStringLiteral("modbus_ascii")) >= 0);
        pick(QStringLiteral("项目设置"));
        QVERIFY(!labelShown(&editor, QStringLiteral("帧格式")));
        QVERIFY(!labelShown(&editor, QStringLiteral("分隔符")));
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
