#include "comm/can/can_polling_backend.h"
#include "comm/can/can_session.h"
#include "comm/comm_hub.h"
#include "comm/link.h"
#include "comm/modbus_slave.h"
#include "comm/tcp_link.h"
#include "core/bit_profile.h"
#include "core/i18n.h"
#include "designer/canvas_item.h"
#include "designer/design_scene.h"
#include "designer/property_editor.h"
#include "player/player_window.h"
#include "runtime/appearance.h"
#include "runtime/can_recorder.h"
#include "runtime/date_time_input.h"
#include "runtime/hmi_widget.h"
#include "runtime/modbus_poller.h"
#include "runtime/project_data.h"
#include "runtime/runtime_context.h"
#include "runtime/runtime_surface.h"
#include "runtime/widgets/can_timing_dialog.h"
#include "runtime/widgets/can_widgets.h"
#include "runtime/widgets/container_widgets.h"
#include "runtime/widgets/data_widgets.h"
#include "runtime/widgets/display_widgets.h"
#include "runtime/widgets/industrial_widget.h"
#include "studio/designer_page.h"
#include "studio/export_service.h"
#include <QtCore/QThread>

#include <FluentQtWidgets/Config.h>
#include <FluentQtWidgets/DateTime/CalendarPicker.h>
#include <FluentQtWidgets/DateTime/PickerBase.h>
#include <FluentQtWidgets/Navigation/Pivot.h>
#include <FluentQtWidgets/Navigation/SegmentedWidget.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/InfoBadge.h>
#include <FluentQtWidgets/Widgets/Menu.h>
#include <FluentQtWidgets/Widgets/RealtimePlotWidget.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/Slider.h>
#include <FluentQtWidgets/Widgets/SwitchButton.h>
#include <FluentQtWidgets/Widgets/TabView.h>

#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QSemaphore>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QTimeZone>
#include <QtCore/QRegularExpression>
#include <QtGui/QImage>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QScreen>
#include <QtGui/QClipboard>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QAbstractItemModelTester>
#include <QtTest/QtTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QDateTimeEdit>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGraphicsView>
#include <QtWidgets/QGraphicsProxyWidget>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableView>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QToolButton>

#include <algorithm>
#include <tuple>

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

// A TCP link card, client unless the properties say otherwise.
QJsonObject tcpCard(const char *id, int y, const QJsonObject &props)
{
    return QJsonObject{{"id", id},
                       {"type", "TcpConfigCard"},
                       {"geometry", QJsonObject{{"x", 200}, {"y", y}, {"w", 380}, {"h", 250}}},
                       {"properties", props}};
}

// A device behind a TCP socket: the Modbus TCP requests it has received so far, whole ADUs only.
struct TcpDevice
{
    explicit TcpDevice(QTcpSocket *peer) : socket(peer) {}

    QTcpSocket *socket = nullptr;
    QByteArray pending;
    QList<QByteArray> received;

    const QList<QByteArray> &requests()
    {
        pending += socket->readAll();
        while (pending.size() >= 6) {
            const qsizetype size = 6 + (quint8(pending.at(4)) << 8 | quint8(pending.at(5)));
            if (pending.size() < size) {
                break;
            }
            received.append(pending.left(size));
            pending.remove(0, size);
        }
        return received;
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
                      normalizedWidgetProperties(widget.value(QStringLiteral("type")).toString(),
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
            if (!text && caption && caption->widget()) {
                text = caption->widget()->findChild<QLabel *>(); // a label with a help button beside it
            }
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

// The label showing `text` under `root`, for its tooltip.
QLabel *labelWith(QWidget *root, const QString &text)
{
    for (QLabel *label : root->findChildren<QLabel *>()) {
        if (label->text() == text && label->isVisible()) {
            return label;
        }
    }
    return nullptr;
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

    void dataPointDialogRenamesAtomicallyAndSupportsUndo()
    {
        QTemporaryDir directory;
        Project project;
        QString error;
        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/14-工程数据与报警.uidp"), &project, &error), qPrintable(error));
        auto points = project.logic.value("points").toArray();
        points.append(QJsonObject{{"name", "Standalone"}, {"binding", QJsonObject{{"key", "standalone"}}}});
        project.logic.insert("points", points);
        project.widgets.append(QJsonObject{{"type", "RealtimeChart"}, {"id", "trend"},
                                           {"properties", QJsonObject{{"_curve_0_parseMode", ParseMode::DataPoint},
                                                                       {"_curve_0_key", "L1"}}}});
        project.widgets.append(QJsonObject{{"type", "NumericDisplay"}, {"id", "standaloneDisplay"},
                                           {"binding", QJsonObject{{"parseMode", ParseMode::DataPoint},
                                                                   {"key", "Standalone"}}}});
        const QString path = directory.filePath("rename.uidp");
        QVERIFY2(project.save(path, &error), qPrintable(error));
        DesignerPage page;
        QVERIFY(page.openProject(path));
        page.resize(1200, 800);
        page.show();
        auto *editor = page.findChild<PropertyEditor *>();
        const auto before = page.snapshot().toJson();
        QTimer::singleShot(0, &page, [&] {
            auto *dialog = page.findChild<AppDialog *>(QStringLiteral("projectLogicDialog"));
            QVERIFY(dialog);
            const auto close = qScopeGuard([dialog] { dialog->reject(); });
            auto *table = dialog->findChild<QTableWidget *>("projectDataPoints");
            QVERIFY(table);
            table->setCurrentCell(table->rowCount() - 1, 0);
            for (auto *button : dialog->findChildren<QPushButton *>())
                if (button->isVisible() && button->text() == QStringLiteral("删除所选"))
                    button->click();
            dialog->yesButton()->click();
            const auto labels = dialog->findChildren<QLabel *>();
            QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](const QLabel *label) {
                return label->text().contains(QStringLiteral("standaloneDisplay"));
            }));
        });
        editor->editProjectLogic();
        QCOMPARE(page.snapshot().toJson(), before);
        const auto edit = [&](bool accept) {
            QTimer::singleShot(0, &page, [&, accept] {
                auto *dialog = page.findChild<AppDialog *>(QStringLiteral("projectLogicDialog"));
                QVERIFY(dialog);
                const auto close = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
                auto *points = dialog->findChild<QTableWidget *>("projectDataPoints");
                auto *alarms = dialog->findChild<QTableWidget *>("projectAlarms");
                QVERIFY(points && alarms);
                points->item(0, 0)->setText("L2");
                QCOMPARE(points->item(0, 0)->text(), QStringLiteral("L1")); // collision is rejected before rewriting references
                points->item(0, 0)->setText("设备A故障");
                QVERIFY(points->item(3, 2)->text().contains(QStringLiteral("T[\"设备A故障\"]")));
                points->item(3, 0)->setText("汇总故障");
                QVERIFY(alarms->item(0, 1)->text().contains(QStringLiteral("T[\"汇总故障\"]")));
                (accept ? dialog->yesButton() : dialog->cancelButton())->click();
            });
            editor->editProjectLogic();
        };
        edit(false);
        QCOMPARE(page.snapshot().toJson(), before);
        QVERIFY(!page.canUndo());
        edit(true);
        auto *scene = page.findChild<DesignScene *>();
        QCOMPARE(scene->findItem("status")->binding().value("key").toString(), QStringLiteral("汇总故障"));
        QCOMPARE(scene->findItem("trend")->propertyValue("_curve_0_key").toString(), QStringLiteral("设备A故障"));
        const auto after = page.snapshot().toJson();
        QVERIFY2(Project::validate(after, &error), qPrintable(error));
        QVERIFY(page.canUndo());
        page.undo();
        QCOMPARE(page.snapshot().toJson(), before);
        QVERIFY(!page.canUndo());
        page.redo();
        QCOMPARE(page.snapshot().toJson(), after);
    }

    void dataPointSelectorsShareChoicesAndKeepTypedBindings()
    {
        Project project;
        project.logic = {{"points", QJsonArray{QJsonObject{{"name", "temperature"}}, QJsonObject{{"name", "fault"}}}}};
        CanvasItem display("NumericDisplay", {}, "display");
        display.setBinding({{"parseMode", ParseMode::DataPoint}, {"key", "old-name"}});
        CanvasItem chart("RealtimeChart", {{"_curve_0_parseMode", ParseMode::DataPoint},
                                            {"_curve_0_key", "temperature"}}, "chart");
        PropertyEditor editor;
        editor.setProject(&project);
        editor.showItem(&display);
        auto *key = qobject_cast<FluentQt::EditableComboBox *>(fieldFor(&editor, QStringLiteral("绑定字段")));
        QVERIFY(key);
        QCOMPARE(key->text(), QStringLiteral("old-name")); // Opening the form must not replace old/unlisted keys.
        QCOMPARE(key->count(), 2);
        editor.resize(420, 900);
        editor.show();
        key->dropButton()->click();
        QVERIFY(key->dropMenu());
        QVERIFY(key->dropMenu()->isVisible());
        QCOMPARE(key->dropMenu()->menuActions().size(), 2);
        key->dropMenu()->close();
        key->setCurrentIndex(1);
        QCOMPARE(display.binding().value("key").toString(), QStringLiteral("fault"));
        key->setText("typed-name");
        QCOMPARE(display.binding().value("key").toString(), QStringLiteral("typed-name"));
        editor.showItem(&chart);
        key = qobject_cast<FluentQt::EditableComboBox *>(fieldFor(&editor, QStringLiteral("匹配键")));
        QVERIFY(key);
        QCOMPARE(key->count(), 2);
        QCOMPARE(key->text(), QStringLiteral("temperature"));
        key->setCurrentIndex(1);
        QCOMPARE(chart.propertyValue("_curve_0_key").toString(), QStringLiteral("fault"));
        QCOMPARE(display.binding().value("key").toString(), QStringLiteral("typed-name"));
    }

    void projectDataRoutesAggregateAndKeepAlarmStateAcrossDisconnect()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Project project;
        project.logic = {
            {"points",
             QJsonArray{QJsonObject{{"name", "L1"}, {"binding", QJsonObject{{"key", "fault"}, {"sourceCardId", "A"}}}},
                        QJsonObject{{"name", "L2"}, {"binding", QJsonObject{{"key", "fault"}, {"sourceCardId", "B"}}}},
                        QJsonObject{{"name", "Any"}, {"expression", "T[\"L1\"] or T[\"L2\"]"}}}},
            {"alarms", QJsonArray{QJsonObject{
                           {"id", "fault"}, {"condition", "T[\"Any\"] != 0"}, {"level", 3}, {"text", "总故障"}}}}};
        RuntimeContext context;
        context.configureData(project);
        context.projectData()->configure(project.logic, dir.filePath("history.json"));
        auto display = std::unique_ptr<HmiWidget>(
            createHmiWidget("NumericDisplay", {{"displayFormat", "枚举"}, {"textList", "0=正常\n1=故障|#d13438"}}));
        context.registerWidget("status", display.get());
        display->attach(&context);
        context.addBinding(
            *BindingEntry::fromWidget({{"id", "status"},
                                       {"type", "NumericDisplay"},
                                       {"binding", QJsonObject{{"parseMode", ParseMode::DataPoint}, {"key", "Any"}}}}));
        FakeLink a, b;
        context.hub()->addLink(&a, "A");
        context.hub()->addLink(&b, "B");
        context.setActive(true);
        QSignalSpy raised(context.projectData(), &ProjectData::alarmRaised);
        b.inject("fault=0\n");
        QVERIFY(!context.projectData()->values().value("Any").isValid());
        b.inject("fault=1\n");
        QCOMPARE(raised.size(), 1);
        QVERIFY(context.projectData()->alarms().first().known); // A is still unknown; B's known fault wins.
        QVERIFY(context.projectData()->audible());
        QVERIFY(context.projectData()->alarms().first().active);
        a.inject("fault=0\n");
        QCOMPARE(raised.size(), 1); // A must not overwrite B's fault.
        b.setConnected(false);
        QVERIFY(!context.projectData()->values().value("Any").isValid());
        QVERIFY(context.projectData()->alarms().first().active); // unknown is not a recovery
        context.projectData()->silence();
        QVERIFY(!context.projectData()->audible());
        b.setConnected(true);
        b.inject("fault=0\n");
        QVERIFY(!context.projectData()->alarms().first().active);
        QVERIFY(context.projectData()->alarms().first().outstanding());
        context.projectData()->acknowledge("fault");
        QVERIFY(!context.projectData()->alarms().first().outstanding());
        b.inject("fault=1\n");
        QCOMPARE(raised.size(), 2);
        QVERIFY(context.projectData()->audible());
        bool textShown = false;
        for (auto *label : display->findChildren<QLabel *>())
            textShown |= label->text() == QStringLiteral("故障");
        QVERIFY(textShown);
        auto badgeProps = display->properties();
        badgeProps.insert("displayStyle", "状态徽标");
        badgeProps.insert("unknownText", "未定义");
        display->setProperties(badgeProps);
        auto *badge = display->findChild<FluentQt::InfoBadge *>("numericDisplayBadge");
        QVERIFY(badge && !badge->isHidden());
        QCOMPARE(badge->text(), QStringLiteral("故障"));
        const QColor faultBackground = badge->lightBackgroundColor();
        display->applyBoundValue(0, {});
        QCOMPARE(badge->text(), QStringLiteral("正常"));
        QVERIFY(badge->lightBackgroundColor() != faultBackground);
        display->applyBoundValue(9, {});
        QCOMPARE(badge->text(), QStringLiteral("未定义"));
        display->applyBoundValue({}, {});
        QCOMPARE(badge->text(), QString::fromUtf8("—"));
        badgeProps.remove("displayStyle");
        display->setProperties(badgeProps);
        QVERIFY(badge->isHidden());
        context.projectData()->flushHistory();
        ProjectData restored;
        const QString historyPath = dir.filePath("history.json");
        restored.configure(project.logic, historyPath);
        QTRY_COMPARE(restored.history().size(), 5); // raise, silence, recovery, acknowledge, new raise
        QVERIFY(restored.history().first().toObject().value("time").toString().endsWith(QLatin1Char('Z')));

        const auto onDisk = [&] {
            QFile file(historyPath);
            return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).array() : QJsonArray();
        };
        QLockFile competingWriter(historyPath + QStringLiteral(".lock"));
        QVERIFY(competingWriter.tryLock(0));
        context.projectData()->acknowledge("fault");
        context.projectData()->flushHistory();
        ProjectData pending;
        pending.configure(project.logic, historyPath);
        QTRY_COMPARE(pending.history().size(), 6);
        auto external = onDisk();
        QCOMPARE(external.size(), 5);
        external.append(QJsonObject{{"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                                    {"id", "external event"}, {"level", 1}, {"event", "raised"}});
        QSaveFile writtenByOtherProcess(historyPath);
        QVERIFY(writtenByOtherProcess.open(QIODevice::WriteOnly));
        const auto bytes = QJsonDocument(external).toJson();
        QCOMPARE(writtenByOtherProcess.write(bytes), bytes.size());
        QVERIFY(writtenByOtherProcess.commit());
        competingWriter.unlock();
        QTRY_COMPARE(onDisk().size(), 7); // timed retry must merge the new file, not overwrite it
        const auto events = onDisk();
        QVERIFY(std::any_of(events.begin(), events.end(), [](const QJsonValue &event) {
            return event.toObject().value("id") == QStringLiteral("external event");
        }));
    }

    void projectDataCommitsSameFrameBeforeEvaluatingAlarms()
    {
        Project project;
        project.logic = {{"points", QJsonArray{QJsonObject{{"name", "a"}, {"binding", QJsonObject{{"key", "a"}}}},
                                               QJsonObject{{"name", "b"}, {"binding", QJsonObject{{"key", "b"}}}},
                                               QJsonObject{{"name", "sum"}, {"expression", "T[\"a\"] + T[\"b\"]"}}}},
                         {"alarms", QJsonArray{QJsonObject{{"id", "mismatch"}, {"condition", "T[\"sum\"] != 10"}}}}};
        RuntimeContext context;
        context.configureData(project);
        context.projectData()->configure(project.logic);
        FakeLink link;
        context.hub()->addLink(&link, "A");
        context.setActive(true);
        QSignalSpy raised(context.projectData(), &ProjectData::alarmRaised);
        link.inject("a=1,b=9\n");
        link.inject("a=9,b=1\n");
        QCOMPARE(context.projectData()->values().value("sum").toInt(), 10);
        QCOMPARE(raised.size(), 0);
        Project rawProject;
        rawProject.logic = {
            {"points", QJsonArray{QJsonObject{{"name", "fault"},
                                              {"binding", QJsonObject{{"parseMode", ParseMode::HexBytes},
                                                                      {"frameHeader", "AA"},
                                                                      {"frameTail", "55"},
                                                                      {"dataType", "uint8"}}}}}},
            {"alarms", QJsonArray{QJsonObject{{"id", "raw"}, {"condition", "T[\"fault\"] == 1"}}}}};
        RuntimeContext raw;
        raw.configureData(rawProject);
        raw.projectData()->configure(rawProject.logic);
        raw.setActive(true);
        QSignalSpy rawRaised(raw.projectData(), &ProjectData::alarmRaised);
        emit raw.hub()->rawReceived(QByteArray::fromHex("AA0155AA0055"), DataSource{"A", 1});
        QCOMPARE(rawRaised.size(), 1);
        QVERIFY(!raw.projectData()->alarms().first().active);
        QVERIFY(raw.projectData()->alarms().first().outstanding());
    }

    void canRecoveryIsPerChannelAndOptIn()
    {
        const QByteArray oldQueries = qgetenv("FHMI_CAN_STATUS_QUERIES");
        qputenv("FHMI_CAN_STATUS_QUERIES", "1");
        const auto restoreQueries = qScopeGuard([oldQueries] {
            if (oldQueries.isNull())
                qunsetenv("FHMI_CAN_STATUS_QUERIES");
            else
                qputenv("FHMI_CAN_STATUS_QUERIES", oldQueries);
        });
        struct Backend : CanPollingBackend
        {
            std::atomic<bool> off{true};
            std::atomic<int> recovered{0};
            std::atomic<int> written{0};
            QSemaphore allowRecovery;
            bool openState = false;
            ~Backend() override { close(); }
            bool open(const CanSettings &s, QString *) override
            {
                setAutoRecover(s.autoRecover);
                openState = true;
                startWorker();
                return true;
            }
            void close() override
            {
                stopWorker();
                openState = false;
            }
            bool isOpen() const override { return openState; }
            QString description() const override { return "recovery"; }
            bool readFrames(QList<CanFrame> &, QString *) override { return true; }
            bool writeFrame(const CanFrame &, QString *) override
            {
                ++written;
                return true;
            }
            CanBusState readBusState() override { return off ? CanBusState::BusOff : CanBusState::Normal; }
            bool recoverBus(QString *) override
            {
                ++recovered;
                if (!allowRecovery.tryAcquire(1, 3000))
                    return false;
                off = false;
                return true;
            }
        };
        Backend automatic, manual;
        CanSettings settings;
        settings.autoRecover = true;
        automatic.open(settings, nullptr);
        settings.autoRecover = false;
        manual.open(settings, nullptr);
        QTRY_COMPARE(automatic.recovered.load(), 1);
        QCOMPARE(manual.recovered.load(), 0);
        QVERIFY(!automatic.send({}, nullptr)); // a write during bus-off must not reach the driver after recovery
        automatic.allowRecovery.release();
        QTRY_VERIFY(!automatic.transmitState()->busOff);
        QVERIFY(automatic.send({}, nullptr));
        QTRY_COMPARE(automatic.written.load(), 1);
        QVERIFY(manual.isOpen());
        automatic.close();
        QVERIFY(manual.isOpen());
        manual.close();
    }
    void initTestCase()
    {
        Q_INIT_RESOURCE(fluentqtwidgets);
    }


    void routesKeepBindingsFramesAndCommandsSeparate()
    {
        RuntimeContext context;
        context.setActive(true);
        auto a = std::unique_ptr<HmiWidget>(createHmiWidget("LineEdit", {}));
        auto b = std::unique_ptr<HmiWidget>(createHmiWidget("LineEdit", {}));
        auto raw = std::unique_ptr<HmiWidget>(createHmiWidget("LineEdit", {}));
        context.registerWidget("a", a.get()); context.registerWidget("b", b.get()); context.registerWidget("raw", raw.get());
        auto binding = [](const QString &id, const QJsonObject &config) { return *BindingEntry::fromWidget({{"id",id},{"type","LineEdit"},{"binding",config}}); };
        context.addBinding(binding("a", {{"key","value"},{"sourceCardId","A"}}));
        context.addBinding(binding("b", {{"key","value"},{"sourceCardId","B"}}));
        context.addBinding(binding("raw", {{"parseMode",ParseMode::HexBytes},{"frameHeader","AA"},{"frameTail","55"},{"dataType","uint16"}}));
        FakeLink first, second;
        context.hub()->addLink(&first,"A"); context.hub()->addLink(&second,"B");
        first.inject("value=11\n"); second.inject("value=22\n");
        QCOMPARE(a->findChild<QLineEdit *>()->text(), QStringLiteral("11"));
        QCOMPARE(b->findChild<QLineEdit *>()->text(), QStringLiteral("22"));
        first.inject(QByteArray::fromHex("AA01"));
        second.inject(QByteArray::fromHex("0255"));
        QVERIFY(raw->findChild<QLineEdit *>()->text().isEmpty());
        second.setConnected(false); // must not discard A's half frame
        first.inject(QByteArray::fromHex("0255"));
        QCOMPARE(raw->findChild<QLineEdit *>()->text(), QStringLiteral("258"));
        auto command = CommandEncoder::encode({{"commandTargetId","A"}},"RUN");
        QVERIFY(context.send(command));
        QCOMPARE(first.sent.size(),1); QCOMPARE(second.sent.size(),0);
        first.setConnected(false); second.setConnected(true);
        QVERIFY(!context.send(command)); // never fall back to B
        QVERIFY(second.sent.isEmpty());
        QVERIFY(context.send(CommandEncoder::encode({},"LEGACY")));
        QCOMPARE(second.sent.size(),1);

        first.setConnected(true);
        first.setConnectionId(QStringLiteral("*"));
        QVERIFY(context.send(CommandEncoder::encode({{"commandTargetId", "*"}}, "Only literal ID")));
        QCOMPARE(first.sent.size(), 2);
        QCOMPARE(second.sent.size(), 1);
    }

    void slowModbusChannelDoesNotDelayAnotherChannel()
    {
        RuntimeContext context;
        FakeLink slow, fast;
        QVERIFY(slow.setParser("modbus_tcp",{})); QVERIFY(fast.setParser("modbus_tcp",{}));
        context.hub()->addLink(&slow,"slow"); context.hub()->addLink(&fast,"fast");
        auto *poller = context.modbusPoller();
        poller->setLinkCard(&slow,&slow,{500,5000,1}); poller->setLinkCard(&fast,&fast,{50,100,2});
        ModbusNeed need; need.table="hr"; need.address=100;
        context.setCurveNeeds(this,{need});
        qint64 now=0; context.setClock([&] { return now; }); context.setActive(true);
        QCOMPARE(slow.sent.size(),1); QCOMPARE(fast.sent.size(),1);
        QCOMPARE(quint8(slow.sent.first().at(6)),quint8(1));
        QCOMPARE(quint8(fast.sent.first().at(6)),quint8(2));
        fast.inject(plcReply(fast.sent.first(),{{100,42}}));
        now=50; poller->poll();
        QCOMPARE(fast.sent.size(),2); QCOMPARE(slow.sent.size(),1);
        QVERIFY(slow.modbusMaster()->busy());
    }

    void modbusFailureInvalidatesOnlyItsOwnProjectPoints()
    {
        const auto point = [](const char *name, const char *source, int address) {
            return QJsonObject{{"name", name}, {"binding", QJsonObject{{"parseMode", "Modbus"},
                {"sourceCardId", source}, {"mbRegIndex", address}}}};
        };
        Project project;
        project.logic = {{"points", QJsonArray{point("a", "A", 100), point("otherBlock", "A", 200),
                                                point("b", "B", 100), point("latest", "", 100)}}};
        RuntimeContext context;
        context.configureData(project);
        FakeLink a, b;
        QVERIFY(a.setParser("modbus_tcp", {}));
        QVERIFY(b.setParser("modbus_tcp", {}));
        context.hub()->addLink(&a, "A");
        context.hub()->addLink(&b, "B");
        qint64 now = 0;
        context.setClock([&] { return now; });
        context.setActive(true);
        QCOMPARE(a.sent.size(), 1);
        QCOMPARE(b.sent.size(), 1);
        a.inject(plcReply(a.sent.last(), {{100, 1}}));
        QCOMPARE(a.sent.size(), 2);
        a.inject(plcReply(a.sent.last(), {{200, 2}}));
        b.inject(plcReply(b.sent.last(), {{100, 3}}));
        QCOMPARE(context.projectData()->values().value("a").toInt(), 1);
        QCOMPARE(context.projectData()->values().value("latest").toInt(), 3);

        now = 500;
        context.modbusPoller()->poll();
        QCOMPARE(a.sent.size(), 3);
        const auto request = a.sent.last();
        QVERIFY(QMetaObject::invokeMethod(a.modbusMaster()->findChild<QTimer *>(), "timeout"));
        const auto values = context.projectData()->values();
        QVERIFY(!values.value("a").isValid());
        QCOMPARE(values.value("otherBlock").toInt(), 2);
        QCOMPARE(values.value("b").toInt(), 3);
        QCOMPARE(values.value("latest").toInt(), 3);
        a.inject(plcReply(request, {{100, 99}}));
        QCOMPARE(context.projectData()->values(), values); // A late TCP reply cannot revive stale values.
    }

    void switchingConnectionClearsItsOldEndpointWithoutBroadeningTarget()
    {
        Project project;
        project.widgets = {
            QJsonObject{{"id", "tcp"}, {"type", "TcpConfigCard"}},
            QJsonObject{{"id", "uart"}, {"type", "SerialConfigCard"}},
            QJsonObject{{"id", "bus"}, {"type", "CanConfigCard"}},
            QJsonObject{{"id", "switch"}, {"type", "ToggleSwitch"},
                        {"properties", QJsonObject{{"commandMode", "文本"}, {"onCommand", "01"},
                                                   {"commandTargetId", "tcp"}, {"commandEndpoint", "192.168.1.20:502"},
                                                   {"canId", "100"}}},
                        {"binding", QJsonObject{{"parseMode", "键值匹配"}, {"key", "state"}, {"sourceCardId", "tcp"},
                                                {"sourceEndpoint", "192.168.1.20:502"}}}},
        };
        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets(project.widgets, &error), qPrintable(error));
        auto *item = scene.findItem("switch");
        PropertyEditor editor;
        editor.setProject(&project);
        editor.setScene(&scene);
        editor.showItem(item);
        editor.show();
        auto *target = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("发送连接")));
        auto *destination = qobject_cast<QLineEdit *>(fieldFor(&editor, QStringLiteral("发送子通道")));
        auto *source = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("来源连接")));
        auto *endpoint = qobject_cast<QLineEdit *>(fieldFor(&editor, QStringLiteral("来源子通道")));
        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("发送模式")));
        QVERIFY(target && destination && source && endpoint && mode);
        QVERIFY(!destination->isHidden() && !endpoint->isHidden());
        target->setCurrentIndex(target->findData(QStringLiteral("uart")));
        source->setCurrentIndex(source->findData(QStringLiteral("uart")));
        QVERIFY(destination->isHidden() && endpoint->isHidden());
        QVERIFY(item->propertyValue("commandEndpoint").toString().isEmpty());
        QVERIFY(item->binding().value("sourceEndpoint").toString().isEmpty());
        project.widgets = scene.widgetsJson();
        QVERIFY2(Project::validate(project.toJson(), &error), qPrintable(error));

        target->setCurrentIndex(0);
        destination->setText(QStringLiteral("192.168.1.20:502")); // A route scoped only by endpoint.
        mode->setCurrentText(QStringLiteral("CAN"));
        QVERIFY(!destination->isHidden());
        QCOMPARE(item->propertyValue("commandEndpoint").toString(), QStringLiteral("192.168.1.20:502"));
        QVERIFY(!CommandEncoder::encode(item->properties(), QStringLiteral("01")).command.has_value());
        project.widgets = scene.widgetsJson();
        QVERIFY(!Project::validate(project.toJson(), &error));
        target->setCurrentIndex(target->findData(QStringLiteral("bus")));
        QVERIFY(destination->isHidden());
        project.widgets = scene.widgetsJson();
        QVERIFY2(Project::validate(project.toJson(), &error), qPrintable(error));
    }

    void explicitlyRoutedModbusControlDoesNotWriteItsLocalBinding()
    {
        RuntimeContext context;
        ModbusSlave local({1, 0, true}, ModbusSlaveStore::fromProperties({{"slaveHrCount", 10}}));
        context.hub()->addModbusSlave(&local, "local");
        FakeLink remote;
        context.hub()->addLink(&remote, "remote");
        const QJsonObject props{{"commandMode", "Modbus"}, {"commandTargetId", "remote"}, {"mbAddress", 2}};
        auto input = std::unique_ptr<HmiWidget>(createHmiWidget("SpinBox", props));
        context.registerWidget("control", input.get());
        context.addBinding(*BindingEntry::fromWidget({{"id", "control"}, {"type", "SpinBox"},
            {"binding", QJsonObject{{"parseMode", "Modbus"}, {"sourceCardId", "local"}, {"mbRegIndex", 2}}}}));
        input->attach(&context);
        auto *spin = input->findChild<QSpinBox *>();
        QVERIFY(spin);
        spin->setValue(7);
        QCOMPARE(remote.sent.size(), 1);
        QCOMPARE(local.store().value("hr", 2), quint16(0));

        auto localTarget = props;
        localTarget.insert("commandTargetId", "local");
        input->setProperties(localTarget);
        QSignalSpy changes(&local, &ModbusSlave::changed);
        spin->setValue(8);
        QCOMPARE(remote.sent.size(), 1);
        QCOMPARE(local.store().value("hr", 2), quint16(8));
        QCOMPARE(changes.size(), 1);

        const QJsonObject s7Target{{"commandMode", "S7"}, {"commandTargetId", "offline-plc"},
            {"s7Command", QJsonObject{{"address", "DB1.DBW0"}, {"dataType", "INT"}}}};
        input->setProperties(s7Target);
        spin->setValue(9);
        QCOMPARE(local.store().value("hr", 2), quint16(8)); // A named receive source is never an S7 destination.
        QCOMPARE(changes.size(), 1);
    }

    void canCaptureKeepsChannelsAndEvictedFramesAndFlushesTail()
    {
        QTemporaryDir directory;
        RuntimeContext context;
        auto widget=std::unique_ptr<HmiWidget>(createHmiWidget("CanMonitor",{{"maxMessages",8},{"autoSaveRows",3},{"autoSave",true},{"autoSaveDirectory",directory.path()}}));
        auto *monitor=dynamic_cast<CanMonitorWidget *>(widget.get());
        monitor->attach(&context);
        auto *recorder=monitor->findChild<CanRecorder *>();
        QSignalSpy finished(recorder,&CanRecorder::finished);
        CanFrame frame; frame.id=0x123; frame.data=QByteArray::fromHex("11"); frame.timestampMs=1700000000000LL;
        emit context.hub()->canFrames(nullptr,{frame,frame},false,{"A",11});
        frame.data=QByteArray::fromHex("22");
        emit context.hub()->canFrames(nullptr,{frame,frame,frame},false,{"B",22});
        auto *view=monitor->findChild<QTableView *>("canMessages");
        QTRY_COMPARE(view->model()->rowCount(),2); // identical IDs stay separate
        QHash<QString,QString> counts;
        for (int row=0;row<2;++row) counts.insert(view->model()->index(row,10).data().toString().section(" / ",0,0),view->model()->index(row,8).data().toString());
        QCOMPARE(counts.value("A"),QStringLiteral("2")); QCOMPARE(counts.value("B"),QStringLiteral("3"));
        monitor->findChild<QSpinBox *>("canMaxMessages")->setValue(1);
        QTRY_COMPARE(view->model()->rowCount(),1);
        emit context.hub()->canFrames(nullptr,{frame,frame,frame},false,{"B",22});
        monitor->detach();
        QTRY_COMPARE(finished.size(),1);
        const auto files=QDir(directory.path()).entryList({"*.csv"},QDir::Files,QDir::Name);
        QCOMPARE(files.size(),3);
        QStringList records;
        QList<int> fileRows;
        for(const auto &name:files) {
            QFile file(QDir(directory.path()).filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly));
            const auto lines=QString::fromUtf8(file.readAll()).split("\r\n",Qt::SkipEmptyParts);
            QVERIFY(lines.first().contains("connection_id")); records+=lines.mid(1);
            fileRows.append(lines.size()-1);
        }
        QCOMPARE(fileRows,QList<int>({3,3,2})); // changing display rows must neither rotate nor resize capture files
        QCOMPARE(records.size(),8);
        QVERIFY(records.at(0).contains("\"A\"")); QVERIFY(records.at(4).contains("\"B\""));
        QFile blocked(directory.filePath("not-a-directory")); QVERIFY(blocked.open(QIODevice::WriteOnly)); blocked.close();
        QSignalSpy failed(recorder,&CanRecorder::failed);
        recorder->start(blocked.fileName(),3); recorder->append({frame},false,{"A",11},"CAN0",nullptr);
        QTRY_COMPARE(failed.size(),1); QVERIFY(!recorder->isRecording());
        QVERIFY(failed.at(0).at(2).toBool());
        recorder->start(blocked.fileName(), 3);
        recorder->append({frame}, false, {"A", 11}, "CAN0", nullptr);
        recorder->start(directory.path(), 3);
        QTRY_COMPARE(failed.size(), 2);
        QVERIFY(!failed.at(1).at(2).toBool());
        QVERIFY(recorder->isRecording());
        recorder->stop();
    }

    void canSessionOpensClosesAndOutlivesItsCardWithoutBlockingUi()
    {
        struct DriverCalls
        {
            std::atomic<int> open{0};
            std::atomic<int> close{0};
            std::atomic<int> send{0};
            QSemaphore completeSend;
        };
        struct DelayedBackend : CanBackend
        {
            std::shared_ptr<DriverCalls> calls;
            bool up = false;

            explicit DelayedBackend(std::shared_ptr<DriverCalls> calls) : calls(std::move(calls)) {}
            ~DelayedBackend() override { close(); }
            bool open(const CanSettings &, QString *) override
            {
                ++calls->open;
                QThread::msleep(120);
                up = true;
                return true;
            }
            void close() override
            {
                if (up) {
                    QThread::msleep(120);
                    up = false;
                    ++calls->close;
                }
            }
            bool send(const CanFrame &frame, QString *) override
            {
                if (++calls->send == 1 && !calls->completeSend.tryAcquire(1, 3000))
                    return false;
                emit framesSent({frame});
                return true;
            }
            bool isOpen() const override { return up; }
            QString description() const override { return QStringLiteral("delayed"); }
        };
        auto calls = std::make_shared<DriverCalls>();
        const auto factory = [calls](const QString &, QString *) { return std::make_unique<DelayedBackend>(calls); };
        CanSession session(factory);
        QSignalSpy opened(&session, &CanSession::opened), stopped(&session, &CanSession::closed);
        int ticks = 0;
        QTimer heartbeat;
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start();
        QVERIFY(session.open({}, nullptr));
        QTRY_COMPARE(opened.size(), 1);
        QVERIFY(session.isOpen());
        QVERIFY(ticks >= 3);

        QVERIFY(session.send({}, nullptr));
        QVERIFY(session.send({}, nullptr));
        QTRY_COMPARE(calls->send.load(), 1);
        const int before = ticks;
        session.close();
        calls->completeSend.release();
        QTRY_COMPARE(stopped.size(), 1);
        QVERIFY(ticks - before >= 3);
        QCOMPARE(calls->close.load(), 1);
        QCOMPARE(calls->send.load(), 1); // only the already running driver call can finish

        auto detached = std::make_unique<CanSession>(factory);
        QVERIFY(detached->open({}, nullptr));
        QTRY_COMPARE(calls->open.load(), 2);
        detached.reset();
        QTRY_COMPARE(calls->close.load(), 2); // native cleanup outlives the card
    }

    void canLogRetainsNewestFramesAndCanStopFollowing()
    {
        RuntimeContext context;
        context.setActive(true);
        std::unique_ptr<HmiWidget> widget(
            createHmiWidget("CanMonitor", {{"displayMode", "滚动日志"}, {"maxMessages", 100}, {"autoScroll", true}}));
        auto *monitor = dynamic_cast<CanMonitorWidget *>(widget.get());
        QVERIFY(monitor);
        monitor->resize(720, 300);
        monitor->attach(&context);
        monitor->show();
        auto *view = monitor->findChild<QTableView *>("canMessages");
        auto *maximum = monitor->findChild<QSpinBox *>("canMaxMessages");
        auto *follow = monitor->findChild<FluentQt::CheckBox *>("canAutoScroll");
        auto *mode = monitor->findChild<FluentQt::ComboBox *>("canDisplayMode");
        QVERIFY(view && maximum && follow && mode);
        QVERIFY(view->isColumnHidden(7)); // period/count describe an aggregate, not an individual frame
        QVERIFY(view->isColumnHidden(8));
        QVERIFY(!view->isSortingEnabled());
        QVERIFY(follow->isVisible());
        QAbstractItemModelTester tester(view->model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        QList<CanFrame> frames;
        for (int i = 0; i < 150; ++i) {
            CanFrame frame;
            frame.id = 0x123;
            frame.data = QByteArray(1, char(i));
            frame.timestampMs = 1700000000000LL + i;
            frames.append(frame);
        }
        emit context.hub()->canFrames(nullptr, frames, false);
        QTRY_COMPARE(view->model()->rowCount(), 100);
        QCOMPARE(view->model()->index(0, 1).data(Qt::UserRole).toLongLong(), 1700000000050LL);
        QCOMPARE(view->model()->index(99, 6).data().toString(), QStringLiteral("95"));
        for (int row = 0; row < 100; ++row) {
            QCOMPARE(view->model()->index(row, 0).data().toString(), QString::number(row + 51));
            QCOMPARE(view->model()->index(row, 0).data(Qt::UserRole).toULongLong(), quint64(row + 51));
        }
        QTRY_VERIFY(view->verticalScrollBar()->maximum() > 0);
        QTRY_COMPARE(view->verticalScrollBar()->value(), view->verticalScrollBar()->maximum());
        follow->setChecked(false);
        view->verticalScrollBar()->setValue(0);
        CanFrame next = frames.last();
        next.data = QByteArray::fromHex("AA");
        next.timestampMs++;
        emit context.hub()->canFrames(nullptr, {next}, false);
        QTRY_COMPARE(view->model()->index(99, 6).data().toString(), QStringLiteral("AA"));
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("52"));
        QCOMPARE(view->model()->index(99, 0).data().toString(), QStringLiteral("151"));
        QCOMPARE(view->verticalScrollBar()->value(), 0);

        // Export flushes the pending batch too, before the next 100 ms repaint.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        next.data = QByteArray::fromHex("BB");
        next.timestampMs++;
        emit context.hub()->canFrames(nullptr, {next}, false);
        QString error;
        QVERIFY2(monitor->saveCsv(dir.filePath("frames.csv"), &error), qPrintable(error));
        QFile file(dir.filePath("frames.csv"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray csv = file.readAll();
        QVERIFY(csv.startsWith(QByteArray::fromHex("EFBBBF")));
        QVERIFY(csv.contains("\"BB\""));
        QVERIFY(!csv.contains("\"32\"")); // oldest retained payload was evicted
        const QStringList lines = QString::fromUtf8(csv).split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 101);
        QVERIFY(!lines.first().contains(QStringLiteral("\"period_ms\"")));
        QVERIFY(!lines.first().contains(QStringLiteral("\"count\"")));
        for (int row = 0; row < 100; ++row) {
            const QString first = lines.at(row + 1).section(QLatin1Char(','), 0, 0);
            QCOMPARE(first, QLatin1Char('"') + view->model()->index(row, 0).data().toString() + QLatin1Char('"'));
        }
        maximum->setValue(3);
        QCOMPARE(view->model()->rowCount(), 3);
        QCOMPARE(view->model()->index(0, 6).data().toString(), QStringLiteral("95"));
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("150"));
        mode->setCurrentIndex(0);
        QCOMPARE(view->model()->rowCount(), 1);
        QCOMPARE(view->model()->index(0, 8).data().toString(), QStringLiteral("3"));
        QCOMPARE(view->model()->index(0, 6).data().toString(), QStringLiteral("BB"));
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("152"));
        QVERIFY(!follow->isEnabled());
        QVERIFY(!follow->isVisible());
        QVERIFY(!view->isColumnHidden(7));
        QVERIFY(!view->isColumnHidden(8));
        QVERIFY(view->isSortingEnabled());
        QVERIFY2(monitor->saveCsv(dir.filePath("summary.csv"), &error), qPrintable(error));
        QFile summary(dir.filePath("summary.csv"));
        QVERIFY(summary.open(QIODevice::ReadOnly));
        const auto summaryCsv = summary.readAll();
        QVERIFY(summaryCsv.contains("\"period_ms\",\"count\""));

        mode->setCurrentIndex(1);
        QCOMPARE(view->model()->rowCount(), 1);
        QCOMPARE(view->model()->index(0, 6).data().toString(), QStringLiteral("BB"));
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("152"));
        QVERIFY(follow->isEnabled());
        QVERIFY(follow->isVisible());
        QVERIFY(!follow->isChecked()); // hiding it must keep the user's previous preference
        QVERIFY(view->isColumnHidden(7));
        QVERIFY(view->isColumnHidden(8));
        QVERIFY(!view->isSortingEnabled());

        emit context.hub()->canFrames(nullptr, {next}, false);
        QTRY_COMPARE(view->model()->rowCount(), 2);
        QCOMPARE(view->model()->index(1, 0).data().toString(), QStringLiteral("153"));

        // Exercise the actual Clear action, which resets the counter as well as the retained rows.
        emit view->customContextMenuRequested(QPoint(4, 4));
        auto *menu = monitor->findChild<QMenu *>();
        QVERIFY(menu && !menu->actions().isEmpty());
        menu->actions().first()->trigger();
        menu->hide();
        QCOMPARE(view->model()->rowCount(), 0);
        emit context.hub()->canFrames(nullptr, {next}, false);
        QTRY_COMPARE(view->model()->rowCount(), 1);
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("1"));

        emit context.hub()->canLinkConnected(nullptr); // the first connection follows the same clear path
        QCOMPARE(view->model()->rowCount(), 0);
        emit context.hub()->canFrames(nullptr, {next}, false);
        QTRY_COMPARE(view->model()->rowCount(), 1);
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("1"));
    }

    void canSummaryCapsIdsAndExportsFilteredFdAndRemoteFrames()
    {
        RuntimeContext context;
        context.setActive(true);
        std::unique_ptr<HmiWidget> widget(createHmiWidget("CanMonitor", {{"maxMessages", 3}}));
        auto *monitor = dynamic_cast<CanMonitorWidget *>(widget.get());
        QVERIFY(monitor);
        monitor->attach(&context);
        monitor->resize(960, 400);
        monitor->show();
        auto *view = monitor->findChild<QTableView *>("canMessages");
        auto *id = monitor->findChild<QLineEdit *>("canIdFilter");
        auto *type = monitor->findChild<FluentQt::ComboBox *>("canTypeFilter");
        QVERIFY(view && id && type);
        QAbstractItemModelTester tester(view->model(), QAbstractItemModelTester::FailureReportingMode::QtTest);
        CanFrame frame;
        frame.id = 0x102;
        frame.data = QByteArray::fromHex("01");
        frame.timestampMs = 1700000000000LL;
        CanFrame old = frame;
        old.id = 0x100;
        CanFrame newer = frame;
        newer.data = QByteArray::fromHex("02");
        newer.timestampMs += 20;
        CanFrame fd = newer;
        fd.extended = true;
        fd.fd = fd.brs = true;
        fd.data.clear();
        for (int i = 0; i < 64; ++i) {
            fd.data.append(char(i));
        }
        CanFrame remote = newer;
        remote.id = 0x103;
        remote.rtr = true;
        remote.data = QByteArray(8, char(0));
        emit context.hub()->canFrames(nullptr, {frame, old, newer, fd}, false);
        emit context.hub()->canFrames(nullptr, {remote}, true);
        QTRY_COMPARE(view->model()->rowCount(), 3);
        QCOMPARE(view->model()->index(0, 2).data().toString(), QStringLiteral("0x102"));
        QCOMPARE(view->model()->index(0, 8).data().toString(), QStringLiteral("2"));
        QCOMPARE(view->model()->index(0, 7).data().toString(), QStringLiteral("20.0"));
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("3"));
        type->setCurrentIndex(3);
        id->setText(QStringLiteral("00000102"));
        QCOMPARE(view->model()->rowCount(), 1);
        QTRY_VERIFY(view->rowHeight(0) > 2 * view->verticalHeader()->defaultSectionSize());
        QTemporaryDir dir;
        QString error;
        const auto readCsv = [&](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
        };
        QVERIFY2(monitor->saveCsv(dir.filePath("fd.csv"), &error), qPrintable(error));
        const QString csv = readCsv(dir.filePath("fd.csv"));
        QCOMPARE(csv.count(QStringLiteral("\r\n")), 2);
        QVERIFY(csv.contains(QStringLiteral("\"0x00000102\",\"RX\",\"FD BRS\",\"64\"")));
        QVERIFY(csv.contains(QString::fromLatin1(fd.data.toHex(' ').toUpper())));
        QVERIFY(!csv.contains(QLatin1String("0x103")));
        id->clear();
        type->setCurrentIndex(2);
        QCOMPARE(view->model()->rowCount(), 1);
        QVERIFY2(monitor->saveCsv(dir.filePath("remote.csv"), &error), qPrintable(error));
        QVERIFY(readCsv(dir.filePath("remote.csv")).contains(QStringLiteral("\"TX\",\"RTR\",\"8\",\"\"")));
        QVERIFY(!monitor->saveCsv(dir.filePath("missing/frames.csv"), &error));
        QVERIFY(!error.isEmpty());
    }

    void canMonitorFollowsLogWhenReturningToItsTab()
    {
        Project project;
        const QJsonObject monitorData{
            {"id", "messages"}, {"type", "CanMonitor"}, {"tabIndex", 1},
            {"geometry", QJsonObject{{"x", 8}, {"y", 40}, {"w", 720}, {"h", 260}}},
            {"properties", QJsonObject{{"displayMode", "滚动日志"}, {"autoScroll", true}}}};
        project.widgets = {QJsonObject{
            {"id", "pages"}, {"type", "TabWidget"},
            {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 750}, {"h", 320}}},
            {"properties", QJsonObject{{"tabNames", "Controls\nCAN"}, {"currentTab", 1}}},
            {"children", QJsonArray{monitorData}}}};
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        auto *tabs = surface.findChild<QTabWidget *>("hmiTabs");
        auto *view = surface.findChild<QTableView *>("canMessages");
        QVERIFY(tabs && view && view->isVisible());
        tabs->setCurrentIndex(0);
        QVERIFY(!view->isVisible());

        CanFrame classic;
        classic.id = 0x321;
        classic.data = QByteArray(8, '\0');
        CanFrame fd = classic;
        fd.fd = true;
        fd.data = QByteArray(64, '\1');
        QList<CanFrame> frames(64, classic);
        frames.append(fd);
        emit surface.context()->hub()->canFrames(nullptr, frames, false);
        QTRY_COMPARE(view->model()->rowCount(), int(frames.size()));
        QCOMPARE(view->verticalScrollBar()->value(), 0);

        QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(1).center());
        QVERIFY(view->isVisible());
        QTRY_VERIFY(view->verticalScrollBar()->maximum() > 0);
        QTRY_COMPARE(view->verticalScrollBar()->value(), view->verticalScrollBar()->maximum());
        QTRY_VERIFY(view->rowHeight(int(frames.size()) - 1) > view->verticalHeader()->defaultSectionSize());
        QCOMPARE(view->model()->index(int(frames.size()) - 1, 0).data().toInt(), int(frames.size()));
    }

    void canSequenceSurvivesFilteringSortingAndSummaryUpdates()
    {
        RuntimeContext context;
        context.setActive(true);
        std::unique_ptr<HmiWidget> widget(createHmiWidget("CanMonitor", {{"showSignals", true}}));
        auto *monitor = dynamic_cast<CanMonitorWidget *>(widget.get());
        QVERIFY(monitor);
        monitor->attach(&context);
        auto *view = monitor->findChild<QTableView *>("canMessages");
        auto *filter = monitor->findChild<QLineEdit *>("canIdFilter");
        auto *mode = monitor->findChild<FluentQt::ComboBox *>("canDisplayMode");
        QVERIFY(view && filter && mode);
        QList<CanFrame> frames;
        for (int i = 0; i < 12; ++i) {
            CanFrame frame;
            frame.id = quint32(0x100 + i);
            frame.data = QByteArray(1, char(i));
            frame.timestampMs = 1700000000000LL + i;
            frames.append(frame);
        }
        emit context.hub()->canFrames(nullptr, frames, false);
        QTRY_COMPARE(view->model()->rowCount(), 12);
        filter->setText(QStringLiteral("102-10B"));
        QCOMPARE(view->model()->rowCount(), 10);
        view->sortByColumn(0, Qt::DescendingOrder);
        // Numeric sequence ordering: 12, 11, 10, 9, ... rather than lexical 9, 8, ... .
        for (int row = 0; row < 10; ++row) {
            QCOMPARE(view->model()->index(row, 0).data().toString(), QString::number(12 - row));
        }

        QTemporaryDir dir;
        QString error;
        const QString path = dir.filePath("sequence.csv");
        QVERIFY2(monitor->saveCsv(path, &error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 11);
        QVERIFY(lines.first().endsWith(QStringLiteral("\"signals\",\"timestamp_ms\",\"connection_id\",\"channel\"")));
        for (int row = 0; row < 10; ++row) {
            const QStringList fields = lines.at(row + 1).split(QLatin1Char(','));
            QCOMPARE(fields.size(), 14); // includes both the optional signal column and the raw timestamp
            QCOMPARE(fields.first(), QStringLiteral("\"%1\"").arg(view->model()->index(row, 0).data().toString()));
            QCOMPARE(fields.at(2), QStringLiteral("\"%1\"").arg(view->model()->index(row, 2).data().toString()));
        }

        mode->setCurrentIndex(1); // switching to chronological log preserves each latest summary sequence
        QCOMPARE(view->model()->rowCount(), 10);
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("3"));
        QCOMPARE(view->model()->index(9, 0).data().toString(), QStringLiteral("12"));
        emit context.hub()->canFrames(nullptr, {frames.at(3)}, false);
        QTRY_COMPARE(view->model()->rowCount(), 11);
        QCOMPARE(view->model()->index(10, 0).data().toString(), QStringLiteral("13"));
        mode->setCurrentIndex(0);
        QCOMPARE(view->model()->rowCount(), 10);
        QCOMPARE(view->model()->index(1, 2).data().toString(), QStringLiteral("0x103"));
        QCOMPARE(view->model()->index(1, 0).data().toString(), QStringLiteral("13"));
        QCOMPARE(view->model()->index(1, 8).data().toString(), QStringLiteral("2"));

        emit context.hub()->canFrames(nullptr, {frames.at(2)}, false);
        QTRY_COMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("14"));
        view->sortByColumn(0, Qt::DescendingOrder);
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("14"));
        QCOMPARE(view->model()->index(1, 0).data().toString(), QStringLiteral("13"));
        QCOMPARE(view->model()->index(2, 0).data().toString(), QStringLiteral("12"));
    }

    void canCsvHasReadableMillisecondsAndRawTimestamp_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("summary") << QStringLiteral("按 ID 汇总");
        QTest::newRow("scrolling") << QStringLiteral("滚动日志");
    }

    void canCsvHasReadableMillisecondsAndRawTimestamp()
    {
        QFETCH(QString, mode);
        RuntimeContext context;
        context.setActive(true);
        std::unique_ptr<HmiWidget> widget(createHmiWidget("CanMonitor", {{"displayMode", mode}}));
        auto *monitor = dynamic_cast<CanMonitorWidget *>(widget.get());
        QVERIFY(monitor);
        monitor->attach(&context);
        const QDateTime beforeMidnight(QDate(2026, 11, 17), QTime(23, 59, 59, 999), QTimeZone::systemTimeZone());
        QList<CanFrame> frames;
        for (int i = 0; i < 2; ++i) {
            CanFrame frame;
            frame.id = quint32(0x18FF0A01 + i);
            frame.extended = true;
            frame.timestampMs = beforeMidnight.toMSecsSinceEpoch() + i * 2;
            frame.data = QByteArray::fromHex("1122334400000500");
            frames.append(frame);
        }
        emit context.hub()->canFrames(nullptr, frames, false);
        QTemporaryDir dir;
        const QString path = dir.filePath("can-time.csv");
        QString error;
        QVERIFY2(monitor->saveCsv(path, &error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(bytes.startsWith(QByteArray::fromHex("EFBBBF") + "\"sequence\",\"time\",\"id\","));
        const QStringList lines = QString::fromUtf8(bytes).split(QStringLiteral("\r\n"), Qt::SkipEmptyParts);
        QCOMPARE(lines.size(), 3);
        QVERIFY(lines.first().contains(QStringLiteral("\"timestamp_ms\"")));
        for (int i = 0; i < 2; ++i) {
            const QStringList fields = lines.at(i + 1).split(QLatin1Char(','));
            QCOMPARE(fields.size(), mode == QStringLiteral("滚动日志") ? 11 : 13);
            QCOMPARE(fields.first(), QStringLiteral("\"%1\"").arg(i + 1));
            const QString time = fields.at(1).mid(1, fields.at(1).size() - 2);
            const auto rawField = fields.at(fields.size() - 3);
            const QString raw = rawField.mid(1, rawField.size() - 2);
            QVERIFY(QRegularExpression(QStringLiteral("\\.\\d{3}(?:Z|[+-]\\d{2}:\\d{2})$")).match(time).hasMatch());
            QVERIFY(time.startsWith(i == 0 ? QStringLiteral("2026-11-17T23:59:59.999")
                                          : QStringLiteral("2026-11-18T00:00:00.001")));
            const QDateTime parsed = QDateTime::fromString(time, Qt::ISODateWithMs);
            QVERIFY(parsed.isValid());
            QCOMPARE(parsed.toMSecsSinceEpoch(), frames.at(i).timestampMs);
            QCOMPARE(raw, QString::number(frames.at(i).timestampMs));
        }
    }

    void zlgTimingFieldsFollowModelAndCustomValue()
    {
        std::unique_ptr<HmiWidget> widget(createHmiWidget(
            "CanConfigCard", {{"defaultAdapter", "ZLG"}, {"zlgModel", "USBCAN-II"}, {"zlgCustomTiming", "00,1C"}}));
        auto *card = dynamic_cast<CanConfigCardWidget *>(widget.get());
        QVERIFY(card);
        card->show();
        auto *adapter = card->findChild<FluentQt::ComboBox *>("canAdapter");
        auto *model = card->findChild<FluentQt::ComboBox *>("canZlgModel");
        auto *rate = card->findChild<FluentQt::ComboBox *>("canBitrate");
        auto *sample = card->findChild<FluentQt::ComboBox *>("canSamplePoint");
        auto *dataRate = card->findChild<FluentQt::ComboBox *>("canDataBitrate");
        auto *dataSample = card->findChild<FluentQt::ComboBox *>("canDataSamplePoint");
        auto *custom = card->findChild<FluentQt::LineEdit *>("canCustomTiming");
        auto *hint = card->findChild<QLabel *>("canTimingHint");
        auto *fd = card->findChild<QCheckBox *>("canFd");
        QVERIFY(adapter && model && rate && sample && dataRate && dataSample && custom && hint && fd);
        QVERIFY(custom->isVisible());
        QCOMPARE(custom->text(), QStringLiteral("00,1C"));
        QVERIFY(!sample->isVisible());
        QVERIFY(!rate->isVisible());
        QVERIFY(!fd->isVisible());
        QVERIFY(hint->text().contains(QStringLiteral("87.5%")));
        custom->clear();
        QVERIFY(sample->isVisible());
        QVERIFY(rate->isVisible());
        sample->setCurrentText(QStringLiteral("75%"));
        QVERIFY(hint->text().contains(QStringLiteral("75%")));
        QVERIFY(hint->text().contains(QStringLiteral("00,3A")));
        custom->setText(QStringLiteral("65,13,2,1"));
        QVERIFY(custom->isError());
        QCOMPARE(hint->text(), QStringLiteral("位时序无效"));
        QVERIFY(custom->toolTip().contains(QStringLiteral("BRP")));

        model->setCurrentText(QStringLiteral("USBCANFD-200U"));
        fd->setChecked(true);
        custom->setText(QStringLiteral("1Mbps(75%),5Mbps(75%),(60,0080040D,00800001)"));
        QVERIFY(!custom->isError());
        QVERIFY(!rate->isVisible());
        QVERIFY(!sample->isVisible());
        QVERIFY(!dataRate->isVisible());
        QVERIFY(!dataSample->isVisible());
        QVERIFY(hint->text().contains(QStringLiteral("5Mbps(75%)")));
        QVERIFY(custom->toolTip().contains(QStringLiteral("ZCANPRO")));
        custom->clear();
        QVERIFY(rate->isVisible());
        QVERIFY(dataRate->isVisible());
        QVERIFY(!sample->isVisible());
        QVERIFY(!dataSample->isVisible());

        model->setCurrentText(QStringLiteral("USBCAN-4E-U"));
        QVERIFY(!fd->isVisible());
        QVERIFY(!dataRate->isVisible());
        QVERIFY(fd->isChecked()); // The remembered FD configuration is inactive on a classic model.
        custom->setText(QStringLiteral("200000"));
        QVERIFY(!custom->isError());
        QVERIFY(hint->text().contains(QStringLiteral("200k")));
        QVERIFY(!sample->isVisible());
        adapter->setCurrentText(QStringLiteral("slcan"));
        QVERIFY(!custom->isVisible());
        QVERIFY(!hint->isVisible());
        adapter->setCurrentText(QStringLiteral("ZLG"));
        QVERIFY(custom->isVisible());
        QCOMPARE(custom->text(), QStringLiteral("200000"));
        QVERIFY(hint->isVisible());
        custom->clear();
        model->setCurrentText(QStringLiteral("USBCANFD-200U"));
        QVERIFY(fd->isVisible() && fd->isChecked());
        QVERIFY(dataRate->isVisible());
    }

    void canPropertyHelpCopiesExamplesWithoutChangingTheProject()
    {
        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets({QJsonObject{{"id", "can"}, {"type", "CanConfigCard"}},
                                    QJsonObject{{"id", "red"},
                                                {"type", "ToggleSwitch"},
                                                {"properties", QJsonObject{{"commandMode", "CAN"},
                                                                           {"canId", "HMI"},
                                                                           {"commandTemplate", "B4b4={value}"}}}}},
                                   &error),
                 qPrintable(error));
        PropertyEditor editor;
        editor.setScene(&scene);
        editor.resize(400, 720);
        editor.showItem(scene.findItem(QStringLiteral("can")));
        editor.show();
        const auto original = scene.widgetsJson();
        QSignalSpy changed(&editor, &PropertyEditor::modified);
        const QString clipboard = QGuiApplication::clipboard()->text();
        const auto restoreClipboard = qScopeGuard([clipboard]() { QGuiApplication::clipboard()->setText(clipboard); });
        auto *help = editor.findChild<QToolButton *>(QStringLiteral("propertyHelp_canTxInitialState"));
        QVERIFY(help && !help->isHidden());
        editor.ensureWidgetVisible(help);
        QTest::mouseClick(help, Qt::LeftButton);
        QPointer<QDialog> dialog = editor.findChild<QDialog *>(QStringLiteral("fieldHelpDialog"));
        QTRY_VERIFY(dialog && dialog->isVisible());
        QVERIFY(!dialog->isWindow()); // help stays in the host, including on native macOS
        auto *example = dialog->findChild<QPlainTextEdit *>(QStringLiteral("fieldHelpExample0"));
        QVERIFY(example && example->isReadOnly());
        QCOMPARE(example->toPlainText(), QStringLiteral("HMI=IO"));
        auto *copy = dialog->findChild<QPushButton *>(QStringLiteral("fieldHelpCopy0"));
        QVERIFY(copy);
        QTest::mouseClick(copy, Qt::LeftButton);
        QCOMPARE(QGuiApplication::clipboard()->text(), example->toPlainText());
        editor.resize(400, 360); // the overlay follows its host; long help keeps scrolling
        auto *scroll = dialog->findChild<QScrollArea *>(QStringLiteral("fieldHelpScroll"));
        QVERIFY(scroll);
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        auto *close = dialog->findChild<QPushButton *>(QStringLiteral("fieldHelpClose"));
        QVERIFY(close && close->isVisible());
        QTest::mouseClick(close, Qt::LeftButton);
        QTRY_VERIFY(dialog.isNull());
        editor.showItem(scene.findItem(QStringLiteral("red")));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        help = editor.findChild<QToolButton *>(QStringLiteral("propertyHelp_commandTemplate"));
        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("发送模式")));
        QVERIFY(help && mode);
        QVERIFY(!help->isHidden());
        editor.ensureWidgetVisible(help);
        QTest::mouseClick(help, Qt::LeftButton);
        dialog = editor.findChild<QDialog *>(QStringLiteral("fieldHelpDialog"));
        QVERIFY(dialog);
        QCOMPARE(dialog->findChild<QPlainTextEdit *>(QStringLiteral("fieldHelpExample0"))->toPlainText(),
                 QStringLiteral("B4b4={value}"));

        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(scene.widgetsJson(), original);
        QCOMPARE(changed.count(), 0);
        mode->setCurrentText(QStringLiteral("文本"));
        QVERIFY(help->isHidden());
        mode->setCurrentText(QStringLiteral("CAN"));
        QVERIFY(!help->isHidden());
    }

    void canRuntimeHelpIsTranslatedAndKeepsTimingUnchanged()
    {
        TableTranslator translator;
        QVERIFY(translator.loadTable(QString::fromLatin1(kEnglishTable)));
        QVERIFY(qApp->installTranslator(&translator));
        const auto removeTranslator = qScopeGuard([&]() { qApp->removeTranslator(&translator); });
        std::unique_ptr<HmiWidget> card(createHmiWidget(
            "CanConfigCard", {{"defaultAdapter", "ZLG"}, {"zlgModel", "USBCAN-II"}, {"zlgCustomTiming", "00,1C"}}));
        card->show();
        auto *help = card->findChild<QToolButton *>(QStringLiteral("canTimingHelp"));
        auto *custom = card->findChild<QLineEdit *>(QStringLiteral("canCustomTiming"));
        QVERIFY(help && custom && help->isVisible());
        QTest::mouseClick(help, Qt::LeftButton);
        QPointer<QDialog> dialog = card->findChild<QDialog *>(QStringLiteral("fieldHelpDialog"));
        QVERIFY(dialog);
        QCOMPARE(dialog->windowTitle(), QStringLiteral("Help: Sample point and custom bit timing"));
        QCOMPARE(dialog->findChild<QPlainTextEdit *>(QStringLiteral("fieldHelpExample0"))->toPlainText(),
                 QStringLiteral("00,1C"));
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(custom->text(), QStringLiteral("00,1C"));
        QVERIFY(labelShown(card.get(), QStringLiteral("Not connected")));
    }

    void zlgCalculatorAppliesSelectedTimingWithoutConnecting()
    {
        std::unique_ptr<HmiWidget> widget(createHmiWidget(
            "CanConfigCard", {{"defaultAdapter", "ZLG"}, {"zlgModel", "USBCAN-II"}, {"zlgSamplePoint", "87.5%"}}));
        auto *card = dynamic_cast<CanConfigCardWidget *>(widget.get());
        QVERIFY(card);
        card->resize(380, 340);
        card->show();
        auto *calculate = card->findChild<QPushButton *>("canTimingCalculatorButton");
        auto *custom = card->findChild<QLineEdit *>("canCustomTiming");
        auto *model = card->findChild<FluentQt::ComboBox *>("canZlgModel");
        QVERIFY(calculate && custom && model);
        QVERIFY(calculate->isEnabled());
        const auto openCalculator = [&]() -> CanTimingDialog * {
            calculate->click();
            for (auto *dialog : card->findChildren<CanTimingDialog *>()) {
                if (dialog->isVisible()) {
                    return dialog;
                }
            }
            return nullptr;
        };
        auto *dialog = openCalculator();
        QVERIFY(dialog);
        auto *table = dialog->findChild<QTableWidget *>("timingCalculatorResults");
        auto *sample = dialog->findChild<QDoubleSpinBox *>("timingCalculatorSample");
        auto *apply = dialog->findChild<QPushButton *>("timingCalculatorApply");
        QVERIFY(table && sample && apply);
        QVERIFY(dialog->widget()->rect().contains(
            QRect(apply->mapTo(dialog->widget(), QPoint()), apply->size())));
        QCOMPARE(table->item(0, 7)->text(), QStringLiteral("00,1C"));
        QCOMPARE(dialog->selectedTiming(), QStringLiteral("00,1C"));
        sample->setValue(75);
        QCOMPARE(dialog->selectedTiming(), QStringLiteral("00,3A"));
        apply->click();
        QTRY_COMPARE(custom->text(), QStringLiteral("00,3A"));
        QVERIFY(labelShown(card, QStringLiteral("未连接")));

        dialog = openCalculator();
        QVERIFY(dialog);
        auto *rate = dialog->findChild<QLineEdit *>("timingCalculatorRate");
        apply = dialog->findChild<QPushButton *>("timingCalculatorApply");
        QVERIFY(rate && apply);
        rate->setText(QStringLiteral("nan"));
        QVERIFY(dialog->selectedTiming().isEmpty());
        QVERIFY(!apply->isEnabled());
        QPointer<CanTimingDialog> closing = dialog;
        dialog->reject();
        QTRY_VERIFY(closing.isNull());
        QCOMPARE(custom->text(), QStringLiteral("00,3A"));

        custom->setText(QStringLiteral("00,9C"));
        dialog = openCalculator();
        QVERIFY(dialog);
        auto *triple = dialog->findChild<QCheckBox *>("timingCalculatorTriple");
        QVERIFY(triple && triple->isChecked());
        QCOMPARE(dialog->selectedTiming(), QStringLiteral("00,9C"));
        closing = dialog;
        dialog->accept();
        QTRY_VERIFY(closing.isNull());
        QCOMPARE(custom->text(), QStringLiteral("00,9C"));
        model->setCurrentText(QStringLiteral("USBCANFD-200U"));
        QVERIFY(!calculate->isEnabled());
    }

    void copiedS7GroupRetargetsConnections()
    {
        const QJsonObject bit{{"address", "DB1.DBX0.1"}, {"dataType", "BOOL"}};
        const QJsonArray group{
            QJsonObject{{"type", "S7ConfigCard"}, {"id", "plc"}},
            QJsonObject{{"type", "ToggleSwitch"}, {"id", "switch"},
                        {"binding", QJsonObject{{"parseMode", "S7"}, {"sourceCardId", "plc"}, {"s7", bit}}},
                        {"properties", QJsonObject{{"commandMode", "S7"}, {"commandTargetId", "plc"}, {"s7Command", bit}}}},
            QJsonObject{{"type", "RealtimeChart"}, {"id", "trend"},
                        {"properties", QJsonObject{{"_curve_0_parseMode", "S7"}, {"_curve_0_sourceCardId", "plc"}, {"_curve_0_s7", bit}}}}};
        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets(group, &error), qPrintable(error));
        const auto copied = scene.paste(group, nullptr, 1);
        QCOMPARE(copied.size(), 3);
        const QString newPlc = copied.at(0)->widgetId();
        QVERIFY(newPlc != QLatin1String("plc"));
        QCOMPARE(copied.at(1)->binding().value("sourceCardId").toString(), newPlc);
        QCOMPARE(copied.at(1)->propertyValue("commandTargetId").toString(), newPlc);
        QCOMPARE(copied.at(2)->propertyValue("_curve_0_sourceCardId").toString(), newPlc);
        const auto controlOnly = scene.paste({group.at(1)}, nullptr, 1);
        QCOMPARE(controlOnly.size(), 1);
        QCOMPARE(controlOnly.first()->propertyValue("commandTargetId").toString(), QStringLiteral("plc"));
        QVERIFY(scene.renameItem(scene.findItem("plc"), "renamed", &error));
        QCOMPARE(scene.findItem("switch")->binding().value("sourceCardId").toString(), QStringLiteral("renamed"));
        QCOMPARE(controlOnly.first()->propertyValue("commandTargetId").toString(), QStringLiteral("renamed"));
        QCOMPARE(copied.at(1)->propertyValue("commandTargetId").toString(), newPlc);
    }

    void chartExpressionPanelStoresCurrentCurve()
    {
        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets(
                     QJsonArray{QJsonObject{{"id", "trend"},
                                            {"type", "RealtimeChart"},
                                            {"properties", QJsonObject{{"curveCount", 2},
                                                                       {"_curve_0_expression", "value + 1"},
                                                                       {"_curve_1_parseMode", ParseMode::CanSignal},
                                                                       {"_curve_1_canId", "invalid"},
                                                                       {"_curve_1_expression", "value * 3"}}}}},
                     &error),
                 qPrintable(error));
        auto *item = scene.findItem("trend");
        QVERIFY(item);
        PropertyEditor editor;
        editor.setScene(&scene);
        editor.showItem(item);
        editor.show();
        auto *expression = qobject_cast<QLineEdit *>(fieldFor(&editor, QStringLiteral("转换表达式")));
        QVERIFY(expression);
        QCOMPARE(expression->text(), QStringLiteral("value + 1"));
        expression->setText("value + 2");
        QCOMPARE(item->properties().value("_curve_0_expression").toString(), QStringLiteral("value + 2"));
        auto *current = qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("当前编辑曲线")));
        QVERIFY(current);
        current->setValue(1);
        QApplication::processEvents();
        expression = qobject_cast<QLineEdit *>(fieldFor(&editor, QStringLiteral("转换表达式")));
        QVERIFY(expression);
        QCOMPARE(expression->text(), QStringLiteral("value * 3"));
        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("解析模式")));
        QVERIFY(mode);
        QCOMPARE(mode->currentText(), ParseMode::CanSignal);
        for (int index = 0; index < mode->count(); ++index) {
            mode->setCurrentIndex(index);
            QVERIFY(!expression->isHidden());
        }
        expression->setText("value * 4");
        const QJsonObject saved = scene.widgetsJson().first().toObject().value("properties").toObject();
        QCOMPARE(saved.value("_curve_0_expression").toString(), QStringLiteral("value + 2"));
        QCOMPARE(saved.value("_curve_1_expression").toString(), QStringLiteral("value * 4"));
        DesignScene reloaded;
        QVERIFY2(reloaded.loadWidgets(scene.widgetsJson(), &error), qPrintable(error));
        QCOMPARE(reloaded.findItem("trend")->properties().value("_curve_1_expression"),
                 saved.value("_curve_1_expression"));
    }

    void chartConstantExpressionSkipsInvalidSources()
    {
        RuntimeContext context;
        context.setActive(true);
        const QJsonObject props{{"curveCount", 2},
                                {"_curve_0_name", "parsed"},
                                {"_curve_0_key", "temperature"},
                                {"_curve_0_expression", "42"},
                                {"_curve_1_name", "raw"},
                                {"_curve_1_parseMode", ParseMode::HexBytes},
                                {"_curve_1_dataType", "float32"},
                                {"_curve_1_expression", "42"}};
        std::unique_ptr<HmiWidget> widget(createHmiWidget("RealtimeChart", props));
        auto *chart = dynamic_cast<RealtimeChartWidget *>(widget.get());
        QVERIFY(chart);
        chart->attach(&context);
        emit context.hub()->messageReceived({});
        for (const QString &value : {QStringLiteral("bad"), QStringLiteral("nan"), QStringLiteral("inf")}) {
            emit context.hub()->messageReceived({{"temperature", value}});
        }
        emit context.hub()->rawReceived(QByteArray::fromHex("7f800000")); // infinity
        emit context.hub()->rawReceived(QByteArray::fromHex("7fc00000")); // NaN
        emit context.hub()->messageReceived({{"temperature", QStringLiteral("12.5")}});
        emit context.hub()->rawReceived(QByteArray::fromHex("41480000")); // 12.5
        QTemporaryDir dir;
        const QString path = dir.filePath("history.json");
        chart->saveHistory(path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject recorded = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(recorded.value("parsed").toArray(), QJsonArray{42.0});
        QCOMPARE(recorded.value("raw").toArray(), QJsonArray{42.0});
    }

    void chartTransformsBeforeRecording()
    {
        RuntimeContext context;
        context.setActive(true);
        const QJsonObject props{{"curveCount", 3},
                                {"_curve_0_name", "parsed"},
                                {"_curve_0_key", "temperature"},
                                {"_curve_0_expression", "value * 2 + 1"},
                                {"_curve_1_name", "raw"},
                                {"_curve_1_parseMode", ParseMode::HexBytes},
                                {"_curve_1_dataType", "uint16"},
                                {"_curve_1_expression", "value * 2 + 1"},
                                {"_curve_2_name", "can"},
                                {"_curve_2_parseMode", ParseMode::CanSignal},
                                {"_curve_2_canId", "0x123"},
                                {"_curve_2_canFactor", 2},
                                {"_curve_2_canOffset", 3},
                                {"_curve_2_expression", "value * 2 + 1"}};
        std::unique_ptr<HmiWidget> widget(createHmiWidget("RealtimeChart", props));
        auto *chart = dynamic_cast<RealtimeChartWidget *>(widget.get());
        QVERIFY(chart);
        chart->attach(&context);
        emit context.hub()->messageReceived({{"temperature", 10LL}});
        emit context.hub()->rawReceived(QByteArray::fromHex("000a"));
        CanFrame frame;
        frame.id = 0x123;
        frame.data = QByteArray::fromHex("0a");
        emit context.hub()->canFrames(nullptr, {frame}, false);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("history.json");
        chart->saveHistory(path);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject recorded = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(recorded.value("parsed").toArray(), QJsonArray{21.0});
        QCOMPARE(recorded.value("raw").toArray(), QJsonArray{21.0});
        QCOMPARE(recorded.value("can").toArray(), QJsonArray{47.0});
    }

    void sharedBitProfilesEditSaveAndRuntimeState()
    {
        Project project;
        project.appName = "signals";
        project.bitProfiles = {
            {"machine",
             QJsonObject{{"width", 8}, {"bits", QJsonArray{QJsonObject{{"name", "READY"}, {"color", "#228866"}}}}}}};
        const auto device = [](const char *id, const char *type, const char *key) {
            return QJsonObject{{"id", id},
                               {"type", type},
                               {"properties", QJsonObject{{"bitProfile", "machine"}}},
                               {"binding", QJsonObject{{"key", key}}}};
        };
        project.widgets = {device("word", "StatusWordPanel", "word"), device("timing", "DigitalTimingChart", "word"),
                           device("encoder", "RotaryEncoder", "encoder"), device("cells", "BmsCellMatrix", "cells"),
                           device("supply", "DcPowerSupply", "supply")};
        QTemporaryDir dir;
        QString error;
        const QString path = dir.filePath("signals.uidp");
        QVERIFY2(project.save(path, &error), qPrintable(error));
        DesignerPage page;
        page.show();
        QVERIFY(page.openProject(path));
        auto *scene = page.findChild<DesignScene *>();
        QVERIFY(scene);
        scene->selectIds({"word"});
        QApplication::processEvents();
        auto *width = qobject_cast<FluentQt::ComboBox *>(fieldFor(&page, QStringLiteral("位宽")));
        QVERIFY(width);
        width->setCurrentText("16");
        QApplication::processEvents();
        auto *word = dynamic_cast<IndustrialWidget *>(scene->findItem("word")->widget());
        auto *timing = dynamic_cast<IndustrialWidget *>(scene->findItem("timing")->widget());
        QCOMPARE(word->bitProfile().value("width").toInt(), 16);
        QCOMPARE(timing->bitProfile().value("width").toInt(), 16);
        auto *row = fieldFor(&page, "B0");
        QVERIFY(row);
        auto edits = row->findChildren<QLineEdit *>();
        QCOMPARE(edits.size(), 2);
        edits.at(0)->setText("READY16 <&>{{x}}");
        edits.at(1)->setText("#ff8800");
        auto *name = qobject_cast<QLineEdit *>(fieldFor(&page, QStringLiteral("新配置名")));
        QVERIFY(name);
        name->setText("plc16");
        QPushButton *rename = nullptr;
        for (auto *button : page.findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("重命名配置"))
                rename = button;
        QVERIFY(rename);
        rename->click();
        QApplication::processEvents();
        QCOMPARE(scene->findItem("word")->propertyValue("bitProfile").toString(), QStringLiteral("plc16"));
        QCOMPARE(scene->findItem("timing")->propertyValue("bitProfile").toString(), QStringLiteral("plc16"));
        QVERIFY(page.save());
        Project loaded;
        QVERIFY2(Project::load(path, &loaded, &error), qPrintable(error));
        QVERIFY(!loaded.bitProfiles.contains("machine"));
        QCOMPARE(loaded.bitProfiles.size(), 1);
        QCOMPARE(loaded.bitProfiles.value("plc16").toObject().value("width").toInt(), 16);
        RuntimeSurface runtime;
        runtime.build(loaded);
        runtime.show();
        QApplication::processEvents();
        IndustrialWidget *counter = nullptr, *cells = nullptr, *supply = nullptr;
        word = nullptr;
        timing = nullptr;
        for (auto *widget : runtime.widgets()) {
            auto *item = dynamic_cast<IndustrialWidget *>(widget);
            if (widget->typeName() == "StatusWordPanel")
                word = item;
            if (widget->typeName() == "DigitalTimingChart")
                timing = item;
            if (widget->typeName() == "RotaryEncoder")
                counter = item;
            if (widget->typeName() == "BmsCellMatrix")
                cells = item;
            if (widget->typeName() == "DcPowerSupply")
                supply = item;
        }
        QVERIFY(word && timing && counter && cells && supply);
        QCOMPARE(timing->timingSamples().size(), 0);
        emit runtime.context()->hub()->messageReceived({{"word", 65535LL}});
        QCOMPARE(word->readingText(), QStringLiteral("0xFFFF"));
        QCOMPARE(timing->timingSamples().size(), 1);
        emit runtime.context()->hub()->messageReceived({{"word", QStringLiteral("bad")}});
        QCOMPARE(timing->timingSamples().size(), 2);
        QVERIFY(!timing->timingSamples().last());
        emit runtime.context()->hub()->messageReceived({{"word", 1LL}});
        QCOMPARE(timing->timingSamples().size(), 3);
        for (int i = 0; i < 40; ++i)
            emit runtime.context()->hub()->messageReceived({{"word", i}});
        QCOMPARE(timing->timingSamples().size(), 32);
        QCOMPARE(timing->timingSamples().first(), std::optional<quint32>(8));
        QCOMPARE(timing->timingSamples().last(), std::optional<quint32>(39));
        const auto count = [&](const QVariant &value) {
            emit runtime.context()->hub()->messageReceived({{"encoder", value}});
        };
        count(100LL);
        QVERIFY(!counter->encoderDirection());
        count(100LL);
        QCOMPARE(counter->encoderDirection(), std::optional<int>(0));
        count(99LL);
        QCOMPARE(counter->encoderDirection(), std::optional<int>(-1));
        count(110LL);
        QCOMPARE(counter->encoderDirection(), std::optional<int>(1));
        count(.5);
        QVERIFY(!counter->reading());
        count(111LL);
        QVERIFY(!counter->encoderDirection());
        count(true);
        QVERIFY(!counter->reading());
        emit runtime.context()->hub()->messageReceived(
            {{"cells", QVariantList{3.7, 3.8, 3.6}},
             {"supply", QVariantMap{{"voltage", 12.34}, {"current", -.002}, {"mode", " cc "}}}});
        QCOMPARE(cells->cellValues().size(), 3);
        QVERIFY(cells->readingText().contains("200.0 mV"));
        QVERIFY(supply->readingText().endsWith("CC"));
        emit runtime.context()->hub()->messageReceived(
            {{"cells", QVariantList{true}}, {"supply", QVariantMap{{"voltage", "12"}, {"current", 1}, {"mode", "CV"}}}});
        QVERIFY(!cells->reading());
        QVERIFY(!supply->reading());
        emit runtime.context()->hub()->messageReceived(
            {{"cells", QVariantList{-.1, 0.0}},
             {"supply", QVariantMap{{"voltage", 0.0}, {"current", 0.0}, {"mode", "cv"}}}});
        QVERIFY(cells->reading());
        QVERIFY(supply->reading());
        for (const auto &type : {QStringLiteral("BmsCellMatrix"), QStringLiteral("DcPowerSupply")}) {
            const auto key = type == "BmsCellMatrix" ? QStringLiteral("cells") : QStringLiteral("supply");
            runtime.context()->addBinding(*BindingEntry::fromWidget(
                QJsonObject{{"id", key},
                            {"type", type},
                            {"binding", QJsonObject{{"parseMode", ParseMode::JsonPath},
                                                    {"jsonPath", QStringLiteral("packet.") + key}}}}));
        }
        emit runtime.context()->hub()->messageReceived(
            {{"packet", QVariantMap{{"cells", QVariantList{4.1, 4.2}},
                                    {"supply", QVariantMap{{"voltage", 5.0}, {"current", .25}, {"mode", "CV"}}}}}});
        QCOMPARE(cells->cellValues(), QList<double>({4.1, 4.2}));
        QVERIFY(supply->readingText().contains("5.00 V"));
        runtime.context()->addBinding(*BindingEntry::fromWidget(
            QJsonObject{{"id", "timing"},
                        {"type", "DigitalTimingChart"},
                        {"binding", QJsonObject{{"parseMode", ParseMode::JsonPath}, {"jsonPath", "packet.word"}}}}));
        const int before = timing->timingSamples().size();
        emit runtime.context()->hub()->messageReceived({{"packet", QVariantMap{{"unrelated", 1}}}});
        QCOMPARE(timing->timingSamples().size(), before);
        emit runtime.context()->hub()->messageReceived({{"packet", QVariantMap{{"word", QVariant()}}}});
        QVERIFY(!timing->timingSamples().last());
    }

    void everyTypeHasAWidget()
    {
        for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
            std::unique_ptr<HmiWidget> widget(createHmiWidget(meta.typeName, {}));
            QVERIFY2(widget, qPrintable(meta.typeName));
            QCOMPARE(widget->typeName(), meta.typeName);
        }
    }

    void exportedWindowFitsCanvasAndKeepsInputWorking()
    {
        Project project;
        project.canvasWidth = 600;
        project.canvasHeight = 400;
        project.widgets = QJsonArray{
            QJsonObject{{"id", "run"}, {"type", "PushButton"},
                        {"geometry", QJsonObject{{"x", 20}, {"y", 20}, {"w", 120}, {"h", 36}}},
                        {"properties", QJsonObject{{"command", "RUN"}, {"commandNewline", "无"}}}},
            QJsonObject{{"id", "entry"}, {"type", "LineEdit"},
                        {"geometry", QJsonObject{{"x", 20}, {"y", 100}, {"w", 200}, {"h", 36}}},
                        {"properties", QJsonObject{{"sendOnEnter", true}, {"commandNewline", "无"}}}}};
        FakeLink link;
        PlayerWindow window(project);
        RuntimeSurface *surface = window.surface();
        RuntimeContext *context = surface->context();
        context->hub()->addLink(&link);
        auto *view = window.findChild<QGraphicsView *>(QStringLiteral("canvasView"));
        auto *button = surface->findChild<QPushButton *>();
        auto *entry = surface->findChild<QLineEdit *>();
        QVERIFY(view && button && entry);
        window.show();
        for (const QSize size : {QSize(360, 300), QSize(1280, 900), QSize(900, 360)}) {
            window.resize(size);
            QApplication::processEvents();
            QCOMPARE(window.surface(), surface);
            QCOMPARE(surface->context(), context);
            QCOMPARE(surface->size(), QSize(600, 400));
            QCOMPARE(view->transform().m11(), view->transform().m22());
            const QRect shown = view->mapFromScene(QRectF(surface->rect())).boundingRect();
            QVERIFY(shown.left() >= -1 && shown.top() >= -1);
            QVERIFY(shown.right() <= view->viewport()->width() + 1);
            QVERIFY(shown.bottom() <= view->viewport()->height() + 1);
            const auto point = [view, surface](QWidget *control) {
                return view->mapFromScene(QPointF(control->mapTo(surface, control->rect().center())));
            };
            link.sent.clear();
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point(button));
            QCOMPARE(link.sent, QList<QByteArray>{QByteArray("RUN")});
            entry->clear();
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point(entry));
            QTest::keyClicks(view, "42");
            QTest::keyClick(view, Qt::Key_Return);
            QCOMPARE(entry->text(), QStringLiteral("42"));
            QCOMPARE(link.sent, (QList<QByteArray>{QByteArray("RUN"), QByteArray("42")}));
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

        HmiWidget *command = surface.widgets().at(1);
        for (const QString &style : {QStringLiteral("标准"), QStringLiteral("强调"), QStringLiteral("轻量"),
                                     QStringLiteral("描边"), QString()}) {
            QJsonObject props = command->properties();
            if (style.isEmpty())
                props.remove("buttonStyle");
            else
                props.insert("buttonStyle", style);
            command->setProperties(props);
            const auto buttons = command->findChildren<FluentQt::PushButton *>();
            QCOMPARE(buttons.size(), 1);
            auto *button = buttons.first();
            QCOMPARE(dynamic_cast<FluentQt::PrimaryPushButton *>(button) != nullptr, style == QStringLiteral("强调"));
            QCOMPARE(dynamic_cast<FluentQt::TransparentPushButton *>(button) != nullptr,
                     style == QStringLiteral("轻量"));
            QCOMPARE(dynamic_cast<FluentQt::OutlinedPushButton *>(button) != nullptr, style == QStringLiteral("描边"));
            QVERIFY(!button->isCheckable());
            link.sent.clear();
            button->click();
            QCOMPARE(link.sent, QList<QByteArray>{QByteArray("RUN\n")});
            command->detach();
            button->click();
            QCOMPARE(link.sent.size(), 1);
            command->attach(surface.context());
            button->click();
            QCOMPARE(link.sent, (QList<QByteArray>{QByteArray("RUN\n"), QByteArray("RUN\n")}));
        }
        surface.context()->hub()->removeLink(&link);
    }

    void numericInputKeepsPrecisionAndLegacyBinding()
    {
        Project project;
        const QJsonObject props{{"numberMode", "小数"}, {"decimals", 6}, {"minimum", 0}, {"maximum", 100},
                                {"value", 0.123456}, {"singleStep", 0.000001}, {"sendOnChange", true},
                                {"commandNewline", "\\n"}};
        project.widgets = QJsonArray{QJsonObject{{"id", "setpoint"}, {"type", "SpinBox"}, {"properties", props},
                                               {"binding", QJsonObject{{"key", "setpoint"}}}}};
        RuntimeSurface surface;
        surface.build(project);
        FakeLink link;
        surface.context()->hub()->addLink(&link);
        auto *input = surface.widgets().first();
        auto *decimal = input->findChild<QDoubleSpinBox *>("hmiNumericInput");
        QVERIFY(decimal);
        QCOMPARE(decimal->value(), 0.123456);
        link.inject("setpoint=1.234567\n");
        QCOMPARE(decimal->value(), 1.234567);
        QVERIFY(link.sent.isEmpty());
        decimal->stepUp();
        QCOMPARE(link.sent, QList<QByteArray>{QByteArray("1.234568\n")});
        auto coarseProps = props;
        coarseProps.insert("decimals", 2);
        input->setProperties(coarseProps);
        QCOMPARE(decimal->singleStep(), 0.01);
        QCOMPARE(link.sent.size(), 1);
        auto integerProps = props;
        integerProps.remove("numberMode");
        integerProps.remove("singleStep");
        input->setProperties(integerProps);
        auto *integer = input->findChild<QSpinBox *>("hmiNumericInput");
        QVERIFY(integer);
        link.inject("setpoint=7\n");
        QCOMPARE(integer->value(), 7);
        QCOMPARE(link.sent.size(), 1);
        integer->stepUp();
        QCOMPARE(link.sent.last(), QByteArray("8\n"));
        input->detach();
        integer->stepUp();
        QCOMPARE(link.sent.size(), 2);
        input->attach(surface.context());
        integer->stepUp();
        QCOMPARE(link.sent.size(), 3);

        CanvasItem item("SpinBox", props, "setpoint");
        PropertyEditor editor;
        editor.setProject(&project);
        editor.showItem(&item);
        auto *value = qobject_cast<QDoubleSpinBox *>(fieldFor(&editor, QStringLiteral("默认值")));
        QVERIFY(value);
        QCOMPARE(value->decimals(), 6);
        value->setValue(0.654321);
        QCOMPARE(item.propertyValue("value").toDouble(), 0.654321);
        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("数值模式")));
        QVERIFY(mode);
        mode->setCurrentIndex(0);
        QTRY_VERIFY(qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("默认值"))));
        QVERIFY(fieldFor(&editor, QStringLiteral("小数位数"))->isHidden());
        surface.context()->hub()->removeLink(&link);
    }

    void selectionStylesShareCommandsAndFeedback()
    {
        Project project;
        const QJsonObject choices{{"items", "手动模式\n自动模式\n维护模式"}, {"commands", "0\n1\n2"},
                                  {"sendOnSelect", true}, {"commandNewline", "\\n"}};
        const QJsonObject toggles{{"onCondition", "等于指定值"}, {"onValue", "2"},
                                  {"onCommand", "ON"}, {"offCommand", "OFF"}, {"commandNewline", "\\n"}};
        project.widgets = QJsonArray{
            QJsonObject{{"id", "mode"}, {"type", "ComboBox"}, {"properties", choices},
                        {"binding", QJsonObject{{"key", "mode"}}}},
            QJsonObject{{"id", "enable"}, {"type", "ToggleSwitch"}, {"properties", toggles},
                        {"binding", QJsonObject{{"key", "enable"}}}}};
        RuntimeSurface surface;
        surface.build(project);
        FakeLink link;
        surface.context()->hub()->addLink(&link);
        auto *choice = surface.widgets().at(0);
        surface.show();
        choice->resize(160, 36);
        for (const QString &style : {QString(), QStringLiteral("分段"), QStringLiteral("单选组"), QStringLiteral("下拉框")}) {
            auto props = choices;
            if (!style.isEmpty())
                props.insert("selectionStyle", style);
            link.sent.clear();
            choice->setProperties(props);
            link.inject("mode=1\n");
            QVERIFY(link.sent.isEmpty());
            if (auto *combo = choice->findChild<FluentQt::ComboBox *>("hmiOptionCombo")) {
                QCOMPARE(combo->currentIndex(), 1);
                combo->setCurrentIndex(2);
            } else if (auto *segments = choice->findChild<FluentQt::SegmentedWidget *>("hmiOptionSegments")) {
                QCOMPARE(segments->currentItem(), QStringLiteral("1"));
                segments->item("2")->click();
            } else {
                auto *group = choice->findChild<QButtonGroup *>("hmiOptionGroup");
                QVERIFY(group);
                QCOMPARE(group->checkedId(), 1);
                auto other = std::unique_ptr<HmiWidget>(createHmiWidget("ComboBox", props));
                auto *independent = other->findChild<QButtonGroup *>("hmiOptionGroup");
                independent->button(1)->click();
                QCOMPARE(group->checkedId(), 1);
                group->button(2)->click();
                QCOMPARE(independent->checkedId(), 1);
            }
            QCOMPARE(link.sent, QList<QByteArray>{QByteArray("2\n")});
            if (auto *scroll = choice->findChild<QScrollArea *>("hmiOptionScroll")) {
                QTRY_VERIFY(scroll->horizontalScrollBar()->maximum() > 0);
                QTRY_VERIFY(scroll->horizontalScrollBar()->value() > 0);
                QVERIFY(scroll->viewport()->height() >= 32);
                QWidget *last = choice->findChild<FluentQt::SegmentedWidget *>("hmiOptionSegments")
                                    ? choice->findChild<FluentQt::SegmentedWidget *>("hmiOptionSegments")->item("2")
                                    : choice->findChild<QButtonGroup *>("hmiOptionGroup")->button(2);
                const int left = last->mapTo(scroll->viewport(), QPoint()).x();
                QVERIFY(left >= 0 && left < scroll->viewport()->width());
            }
            link.inject("mode=99\n");
            if (auto *combo = choice->findChild<FluentQt::ComboBox *>("hmiOptionCombo"))
                QCOMPARE(combo->currentIndex(), -1);
            else if (auto *segments = choice->findChild<FluentQt::SegmentedWidget *>("hmiOptionSegments"))
                QVERIFY(segments->currentItem().isEmpty());
            else
                QCOMPARE(choice->findChild<QButtonGroup *>("hmiOptionGroup")->checkedId(), -1);
            QCOMPARE(link.sent.size(), 1);
        }
        auto *toggle = surface.widgets().at(1);
        for (const QString &style : {QStringLiteral("复选框"), QStringLiteral("开关")}) {
            auto props = toggles;
            props.insert("switchStyle", style);
            link.sent.clear();
            toggle->setProperties(props);
            link.inject("enable=2\n");
            QVERIFY(link.sent.isEmpty());
            if (auto *check = toggle->findChild<FluentQt::CheckBox *>("hmiCheckBox")) {
                QVERIFY(check->isChecked());
                QVERIFY(!check->isTristate());
                check->click();
            } else {
                auto *sw = toggle->findChild<FluentQt::SwitchButton *>("hmiToggleSwitch");
                QVERIFY(sw->isChecked());
                sw->setChecked(false);
            }
            QCOMPARE(link.sent, QList<QByteArray>{QByteArray("OFF\n")});
        }
        surface.context()->hub()->removeLink(&link);
    }

    void alarmFiltersKeepSelectionAndHistorySemantics()
    {
        QTemporaryDir dir;
        Project project;
        project.logic = {
            {"points", QJsonArray{QJsonObject{{"name", "p"}, {"binding", QJsonObject{{"key", "p"}}}},
                                   QJsonObject{{"name", "q"}, {"binding", QJsonObject{{"key", "q"}}}}}},
            {"alarms", QJsonArray{QJsonObject{{"id", "heat"}, {"condition", "T['p'] > 0"}, {"level", 3}, {"text", "温度过高"}},
                                   QJsonObject{{"id", "motor"}, {"condition", "T['q'] > 0"}, {"level", 1}, {"text", "电机故障"}}}}};
        project.widgets = QJsonArray{QJsonObject{{"id", "alarms"}, {"type", "AlarmList"}}};
        RuntimeSurface surface;
        surface.build(project);
        surface.context()->projectData()->configure(project.logic, dir.filePath("history.json"));
        FakeLink link;
        surface.context()->hub()->addLink(&link);
        link.inject("p=1,q=1\n");
        auto *list = surface.widgets().first();
        auto *table = list->findChild<QTableWidget *>("alarmTable");
        auto *keyword = list->findChild<QLineEdit *>("alarmFilterKeyword");
        auto *level = list->findChild<FluentQt::ComboBox *>("alarmFilterLevel");
        auto *state = list->findChild<FluentQt::ComboBox *>("alarmFilterState");
        auto *mode = list->findChild<FluentQt::ComboBox *>("alarmMode");
        auto *event = list->findChild<FluentQt::ComboBox *>("alarmFilterEvent");
        auto *time = list->findChild<FluentQt::ComboBox *>("alarmFilterTimeRange");
        auto *from = list->findChild<DateTimeInput *>("alarmFilterFrom");
        auto *to = list->findChild<DateTimeInput *>("alarmFilterTo");
        auto *ack = list->findChild<QPushButton *>("alarmAcknowledgeSelected");
        auto *reset = list->findChild<QPushButton *>("alarmFilterReset");
        auto *filter = list->findChild<QPushButton *>("alarmFilterToggle");
        QVERIFY(table && keyword && level && state && mode && event && time && from && to && ack && reset && filter);
        QVERIFY(list->findChild<QWidget *>("alarmFilterPanel")->isHidden());
        QCOMPARE(table->rowCount(), 2);
        level->setCurrentIndex(3);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("heat"));
        table->selectRow(0);
        QVERIFY(ack->isEnabled());
        keyword->setText("motor");
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(!ack->isEnabled());
        reset->click();
        QCOMPARE(table->rowCount(), 2);
        QVERIFY(!ack->isEnabled()); // The previous row number must not confirm a different alarm.
        keyword->setText(QStringLiteral("电机"));
        table->selectRow(0);
        ack->click();
        link.inject("q=0\n");
        QCOMPARE(table->rowCount(), 0);
        reset->click();
        state->setCurrentIndex(2);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("heat"));
        filter->click();
        time->setCurrentIndex(4);
        const QDateTime now = QDateTime::currentDateTime();
        from->setDateTime(now.addDays(-1));
        to->setDateTime(now.addSecs(-3600));
        QCOMPARE(table->rowCount(), 0);
        from->setDateTime(now.addSecs(-60));
        to->setDateTime(now.addSecs(60));
        QCOMPARE(table->rowCount(), 1);
        to->setDateTime(now.addSecs(-120));
        QVERIFY(!list->findChild<QWidget *>("alarmFilterTimeProblem")->isHidden());
        QCOMPARE(table->rowCount(), 0);
        reset->click();
        mode->setCurrentIndex(1);
        QCOMPARE(table->rowCount(), 4); // Two raises, one confirmation and one recovery.
        QVERIFY(!ack->isEnabled());
        for (const char *name : {"alarmAcknowledgeSelected", "alarmAcknowledgeAll", "alarmSilence"}) {
            auto *action = list->findChild<QPushButton *>(QLatin1String(name));
            QVERIFY(action && action->isHidden() && !action->isEnabled());
        }
        QCOMPARE(table->horizontalHeaderItem(3)->text(), QStringLiteral("事件"));
        QVERIFY(state->isHidden());
        QVERIFY(!event->isHidden());
        event->setCurrentIndex(3);
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("motor"));
        keyword->setText("HEAT");
        QCOMPARE(table->rowCount(), 0);
        event->setCurrentIndex(1);
        time->setCurrentIndex(1);
        QCOMPARE(table->rowCount(), 1);
        filter->click();
        QVERIFY(list->findChild<QWidget *>("alarmFilterPanel")->isHidden());
        QVERIFY(filter->text().contains("3")); // Hidden filters remain explicit.
        QCOMPARE(surface.context()->projectData()->history().size(), 4);
        QCOMPARE(list->findChild<QPushButton *>("alarmExportAllHistory")->text(), QStringLiteral("导出全部历史"));
        QVERIFY(!list->findChild<QPushButton *>("alarmExportFiltered")->isHidden());

        // History can contain explicit offsets. An inclusive end of 12:00:00 includes all
        // fractions of that second, but does not include the following second.
        const QString timePath = dir.filePath("boundary.json");
        const QDateTime lastSecond(QDate(2025, 1, 15), QTime(12, 0, 0, 983), QTimeZone::fromSecondsAheadOfUtc(8 * 3600));
        const QJsonArray records{
            QJsonObject{{"time", lastSecond.toString(Qt::ISODateWithMs)}, {"id", "inside"}, {"event", "raised"}},
            QJsonObject{{"time", lastSecond.addMSecs(17).toString(Qt::ISODateWithMs)}, {"id", "outside"}, {"event", "raised"}}};
        QFile timeFile(timePath);
        QVERIFY(timeFile.open(QIODevice::WriteOnly));
        QVERIFY(timeFile.write(QJsonDocument(records).toJson()) > 0);
        timeFile.close();
        surface.context()->projectData()->configure(project.logic, timePath);
        reset->click();
        time->setCurrentIndex(4);
        from->setDateTime(lastSecond.addSecs(-60));
        to->setDateTime(lastSecond);
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 2)->text(), QStringLiteral("inside"));

        // Opening the same filter form on a compact canvas must preserve its authored geometry.
        surface.show();
        list->resize(480, 160);
        filter->click();
        QPointer<QDialog> compactFilter = surface.findChild<QDialog *>("alarmFilterDialog");
        QVERIFY(compactFilter && compactFilter->isVisible());
        QVERIFY(compactFilter->isAncestorOf(to));
        QCOMPARE(list->size(), QSize(480, 160));
        compactFilter->reject();
        QTRY_VERIFY(compactFilter.isNull());
        QVERIFY(!filter->isChecked());
        QCOMPARE(table->rowCount(), 1);
        surface.context()->hub()->removeLink(&link);
    }

    void dateTimeInputSharesKeyboardAndPickerValues()
    {
        DateTimeInput input;
        input.setDateTime(QDateTime(QDate(2026, 11, 17), QTime(12, 34, 56, 750)));
        QCOMPARE(input.dateTime().time(), QTime(12, 34, 56));
        input.show();
        QApplication::processEvents();
        auto *date = input.findChild<QDateEdit *>("dateEdit");
        auto *time = input.findChild<QTimeEdit *>("timeEdit");
        auto *calendarButton = input.findChild<QToolButton *>("calendarButton");
        auto *timeButton = input.findChild<QToolButton *>("timePickerButton");
        QVERIFY(date && time && calendarButton && timeButton);
        QSignalSpy changes(&input, &DateTimeInput::dateTimeChanged);
        date->setFocus();
        date->setSelectedSection(QDateTimeEdit::DaySection);
        QTest::keyClicks(date, "19");
        QTest::keyClick(date, Qt::Key_Return);
        QCOMPARE(input.dateTime(), QDateTime(QDate(2026, 11, 19), QTime(12, 34, 56)));
        QCOMPARE(changes.size(), 1);
        time->setFocus();
        time->setSelectedSection(QDateTimeEdit::SecondSection);
        QTest::keyClicks(time, "42");
        QTest::keyClick(time, Qt::Key_Return);
        QCOMPARE(input.dateTime().time(), QTime(12, 34, 42));

        calendarButton->click();
        auto *calendar = input.findChild<FluentQt::CalendarView *>("dateCalendar");
        QVERIFY(calendar);
        QCOMPARE(calendar->date(), date->date());
        for (auto *day : calendar->dayButtons()) {
            if (!day->property("outOfMonth").toBool() && day->text() == QLatin1String("20")) {
                day->click();
                break;
            }
        }
        QCOMPARE(input.dateTime(), QDateTime(QDate(2026, 11, 20), QTime(12, 34, 42)));
        timeButton->click();
        auto *panel = input.findChild<FluentQt::PickerPanel *>("timePickerPanel");
        QVERIFY(panel);
        QCOMPARE(panel->value(), QStringList({"12", "34", "42"}));
        panel->setValue({"23", "59", "58"});
        QCOMPARE(time->time(), QTime(12, 34, 42)); // Rolling is a draft until confirmed.
        panel->yesButton()->click();
        QCOMPARE(input.dateTime(), QDateTime(QDate(2026, 11, 20), QTime(23, 59, 58)));
        timeButton->click();
        panel = input.findChild<FluentQt::PickerPanel *>("timePickerPanel");
        panel->setValue({"01", "02", "03"});
        panel->cancelButton()->click();
        QCOMPARE(time->time(), QTime(23, 59, 58));

        for (QDateTimeEdit *edit : {static_cast<QDateTimeEdit *>(date), static_cast<QDateTimeEdit *>(time)}) {
            auto *line = edit->findChild<QLineEdit *>();
            QVERIFY(line);
            const int textWidth = line->fontMetrics().horizontalAdvance(edit->text());
            QVERIFY2(line->contentsRect().width() >= textWidth + 4, qPrintable(edit->text()));
        }
    }

    void navigationStylesPreservePages()
    {
        const auto child = [](const char *id, int page, int x, int y) {
            return QJsonObject{{"id", id},
                               {"type", "Label"},
                               {"tabIndex", page},
                               {"properties", QJsonObject{{"text", id}}},
                               {"geometry", QJsonObject{{"x", x}, {"y", y}, {"w", 100}, {"h", 30}}}};
        };
        const QJsonObject data{{"id", "pages"},
                               {"type", "TabWidget"},
                               {"geometry", QJsonObject{{"x", 10}, {"y", 10}, {"w", 480}, {"h", 300}}},
                               {"properties", QJsonObject{{"tabNames", "A\nB\nC"}, {"_tabLayout_1", "垂直"}}},
                               {"children", QJsonArray{child("freeA", 0, 23, 83), child("autoA", 1, 99, 155),
                                                       child("autoB", 1, 179, 205), child("freeC", 2, 41, 105)}}};
        std::unique_ptr<CanvasItem> item(CanvasItem::fromJson(data, QString()));
        QVERIFY(item);
        const auto children = item->containerChildren();
        const qreal firstOffset = children.at(0)->y() - item->widget()->contentTop();
        const qreal lastOffset = children.at(3)->y() - item->widget()->contentTop();
        QVERIFY(!children.at(1)->isVisible());
        for (const QString &style : {QStringLiteral("下划线导航"), QStringLiteral("分段导航"),
                                     QStringLiteral("文档页签"), QStringLiteral("经典选项卡"), QString()}) {
            item->setPropertyValue("navigationStyle",
                                   style.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(style));
            const int top = item->widget()->contentTop();
            QCOMPARE(children.at(0)->y() - top, firstOffset);
            QCOMPARE(children.at(3)->y() - top, lastOffset);
            QCOMPARE(children.at(0)->x(), 23.0);
            QCOMPARE(children.at(3)->x(), 41.0);
            // This page stays hidden while both rows are rearranged for the new navigation height.
            QVERIFY(!children.at(1)->isVisible());
            QCOMPARE(children.at(1)->pos(), QPointF(ContainerLayout::Padding, top));
            QCOMPARE(children.at(2)->pos(), QPointF(ContainerLayout::Padding, top + 38));
            QCOMPARE(children.at(1)->size().width(), 480.0 - 2 * ContainerLayout::Padding);

            const QJsonObject saved = QJsonDocument::fromJson(QJsonDocument(item->toJson()).toJson()).object();
            if (style.isEmpty())
                QVERIFY(!saved.value("properties").toObject().contains("navigationStyle"));
            Project project;
            project.widgets = {saved};
            RuntimeSurface runtime;
            runtime.build(project);
            runtime.show();
            QApplication::processEvents();
            QCOMPARE(runtime.widgets().size(), 5);
            auto *container = dynamic_cast<TabContainerWidget *>(runtime.widgets().first());
            QVERIFY(container);
            QCOMPARE(container->pageCount(), 3);
            auto *classic = container->findChild<QTabWidget *>("hmiTabs");
            QVERIFY(classic);
            QCOMPARE(classic->tabBar()->isVisible(), style.isEmpty() || style == QStringLiteral("经典选项卡"));
            const auto savedChildren = saved.value("children").toArray();
            for (int page : {2, 1, 0}) {
                if (style == QStringLiteral("文档页签")) {
                    auto *navigation = container->findChild<FluentQt::TabBar *>("hmiDocumentTabs");
                    QVERIFY(navigation);
                    QTest::mouseClick(navigation->tabItem(page), Qt::LeftButton);
                } else if (style == QStringLiteral("下划线导航") || style == QStringLiteral("分段导航")) {
                    auto *navigation = container->findChild<FluentQt::Pivot *>("hmiPageNavigation");
                    QVERIFY(navigation);
                    QCOMPARE(qobject_cast<FluentQt::SegmentedWidget *>(navigation) != nullptr,
                             style == QStringLiteral("分段导航"));
                    QTest::mouseClick(navigation->item(QString::number(page)), Qt::LeftButton);
                } else {
                    auto *tabs = container->findChild<QTabWidget *>("hmiTabs");
                    QVERIFY(tabs);
                    QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier,
                                      tabs->tabBar()->tabRect(page).center());
                }
                QApplication::processEvents();
                QCOMPARE(container->currentPage(), page);
                for (int i = 0; i < savedChildren.size(); ++i) {
                    const auto stored = savedChildren.at(i).toObject();
                    auto *widget = runtime.widgets().at(i + 1);
                    const int childPage = stored.value("tabIndex").toInt(0);
                    const auto geometry = stored.value("geometry").toObject();
                    QCOMPARE(widget->parentWidget(), container->childHost(childPage));
                    QTRY_COMPARE(widget->mapTo(container, QPoint()),
                                 QPoint(geometry.value("x").toInt(), geometry.value("y").toInt()));
                    QCOMPARE(widget->isVisible(), childPage == page);
                }
            }
            if (style == QStringLiteral("文档页签")) {
                container->resize(150, 300);
                classic->setCurrentIndex(2);
                auto *navigation = container->findChild<FluentQt::TabBar *>("hmiDocumentTabs");
                auto *scroll = navigation->findChild<QScrollArea *>();
                QVERIFY(scroll);
                QTRY_VERIFY(scroll->viewport()->rect().contains(
                    navigation->currentTab()->mapTo(scroll->viewport(), navigation->currentTab()->rect().center())));
                QCOMPARE(container->width(), 150);
            }
        }
    }

    void pageManagementKeepsContentAndUndo()
    {
        const auto child = [](const char *id, int page) {
            return QJsonObject{{"id", id},
                               {"type", "Label"},
                               {"tabIndex", page},
                               {"geometry", QJsonObject{{"x", 20}, {"y", 80}, {"w", 100}, {"h", 30}}}};
        };
        Project project;
        project.widgets = {QJsonObject{{"id", "pages"},
                                       {"type", "TabWidget"},
                                       {"geometry", QJsonObject{{"x", 10}, {"y", 10}, {"w", 480}, {"h", 300}}},
                                       {"properties", QJsonObject{{"tabNames", "A\nB\nC"},
                                                                  {"currentTab", 1},
                                                                  {"_tabLayout_0", "自由"},
                                                                  {"_tabLayout_1", "垂直"},
                                                                  {"_tabLayout_2", "水平"}}},
                                       {"children", QJsonArray{child("a", 0), child("b", 1), child("c", 2)}}}};
        QTemporaryDir directory;
        QString error;
        const QString path = directory.filePath("pages.uidp");
        QVERIFY2(project.save(path, &error), qPrintable(error));
        DesignerPage page;
        QVERIFY(page.openProject(path));
        page.resize(1200, 800);
        page.show();
        auto *scene = page.findChild<DesignScene *>();
        auto *editor = page.findChild<PropertyEditor *>();
        QVERIFY(scene && editor);
        scene->selectIds({"pages"});
        QApplication::processEvents();
        const auto before = page.snapshot().toJson();
        auto *editorPage = editor->findChild<FluentQt::ComboBox *>("tabPageSelector");
        QVERIFY(editorPage);
        editorPage->setCurrentIndex(2);
        QApplication::processEvents();
        QCOMPARE(scene->findItem("pages")->editorPage(), 2);
        QCOMPARE(page.snapshot().toJson(), before); // Browsing must not write startup or move hidden children.
        QVERIFY(!page.canUndo());
        auto *startupPage = editor->findChild<FluentQt::ComboBox *>("tabStartupPage");
        QVERIFY(startupPage);
        startupPage->setCurrentIndex(0);
        QCOMPARE(scene->findItem("pages")->propertyValue("currentTab").toInt(), 0);
        QCOMPARE(scene->findItem("pages")->editorPage(), 2);
        page.undo();
        QApplication::processEvents();
        QCOMPARE(page.snapshot().toJson(), before);
        QCOMPARE(scene->findItem("pages")->editorPage(), 2);
        editor->findChild<FluentQt::ComboBox *>("tabPageSelector")->setCurrentIndex(1);
        QApplication::processEvents();
        for (const char *action : {"removeTabPage", "moveTabPagePrevious", "moveTabPageNext"}) {
            auto *button = editor->findChild<QToolButton *>(QLatin1String(action));
            QVERIFY(button && button->isEnabled());
            button->click();
            QApplication::processEvents();
            const auto *pages = scene->findItem("pages");
            QCOMPARE(pages->containerChildren().size(), 3);
            const bool removed = QByteArray(action) == "removeTabPage";
            const bool earlier = QByteArray(action) == "moveTabPagePrevious";
            QCOMPARE(pages->propertyValue("tabNames").toString(), removed   ? QStringLiteral("A\nC")
                                                                  : earlier ? QStringLiteral("B\nA\nC")
                                                                            : QStringLiteral("A\nC\nB"));
            QCOMPARE(scene->findItem("a")->tabIndex(), earlier ? 1 : 0);
            QCOMPARE(scene->findItem("b")->tabIndex(), earlier ? 0 : removed ? 1 : 2);
            QCOMPARE(scene->findItem("c")->tabIndex(), removed || !earlier ? 1 : 2);
            QCOMPARE(pages->propertyValue("_tabLayout_0").toString(),
                     earlier ? QStringLiteral("垂直") : QStringLiteral("自由"));
            QCOMPARE(pages->propertyValue("_tabLayout_1").toString(),
                     earlier ? QStringLiteral("自由") : QStringLiteral("水平"));
            if (removed)
                QVERIFY(!pages->properties().contains("_tabLayout_2"));
            else
                QCOMPARE(pages->propertyValue("_tabLayout_2").toString(),
                         earlier ? QStringLiteral("水平") : QStringLiteral("垂直"));
            QVERIFY(page.canUndo());
            page.undo();
            QApplication::processEvents();
            QCOMPARE(page.snapshot().toJson(), before);
            QVERIFY(!page.canUndo());
        }
        for (int remaining : {2, 1}) {
            auto *remove = editor->findChild<QToolButton *>("removeTabPage");
            QVERIFY(remove && remove->isEnabled());
            remove->click();
            QApplication::processEvents();
            QCOMPARE(tabNames(scene->findItem("pages")->propertyValue("tabNames").toString()).size(), remaining);
            QCOMPARE(scene->findItem("pages")->containerChildren().size(), 3);
        }
        auto *remove = editor->findChild<QToolButton *>("removeTabPage");
        QVERIFY(remove && !remove->isEnabled());
        const auto lastPage = page.snapshot().toJson();
        remove->click();
        QCOMPARE(page.snapshot().toJson(), lastPage);
    }

    void buttonWritesSingleCoil()
    {
        FakeLink link;
        RuntimeContext context;
        context.hub()->addLink(&link);
        std::unique_ptr<HmiWidget> widget(createHmiWidget("PushButton", {}));
        widget->attach(&context);
        auto *button = widget->findChild<QPushButton *>();
        QVERIFY(button);
        auto click = [&](const QString &function, QJsonValue value = QJsonValue::Undefined) {
            QJsonObject props{{"commandMode", "Modbus"}, {"mbFunc", function}};
            props.insert("mbValue", value);
            widget->setProperties(props);
            link.sent.clear();
            button->click();
            return link.sent;
        };
        const QString coil = QStringLiteral("05 写单线圈");
        const QList<QByteArray> on{QByteArray::fromHex("01050000ff008c3a")};
        QCOMPARE(click(coil), on);
        QCOMPARE(click(coil, 1), on);
        QCOMPARE(click(coil, QStringLiteral("1")), on);
        QCOMPARE(click(coil, 65280), on);
        QCOMPARE(click(coil, 0), QList<QByteArray>{QByteArray::fromHex("010500000000cdca")});
        QVERIFY(click(coil, 2).isEmpty());
        QVERIFY(click(coil, 1.5).isEmpty());
        QCOMPARE(click(QStringLiteral("01 读线圈"), 1), QList<QByteArray>{QByteArray::fromHex("010100000001fdca")});
        QCOMPARE(click(QStringLiteral("06 写单寄存器"), 1), QList<QByteArray>{QByteArray::fromHex("010600000001480a")});
    }

    void toggleWritesCoilOrHoldingRegister_data()
    {
        QTest::addColumn<QString>("area");
        QTest::addColumn<QByteArray>("on");
        QTest::addColumn<QByteArray>("off");
        for (const QString &area : {QString(), QStringLiteral("线圈")}) {
            QTest::newRow(area.isEmpty() ? "legacy" : "coil")
                << area << QByteArray::fromHex("0705002aff00ad94") << QByteArray::fromHex("0705002a0000ec64");
        }
        QTest::newRow("holding-register") << ModbusArea::HoldingRegisters << QByteArray::fromHex("0706002a000169a4")
                                          << QByteArray::fromHex("0706002a0000a864");
    }

    void toggleWritesCoilOrHoldingRegister()
    {
        QFETCH(QString, area);
        QFETCH(QByteArray, on);
        QFETCH(QByteArray, off);
        FakeLink link;
        RuntimeContext context;
        context.hub()->addLink(&link);
        QJsonObject props{{"commandMode", "Modbus"}, {"mbSlave", 7}, {"mbAddress", 42}};
        if (!area.isEmpty()) {
            props.insert("mbArea", area);
        }
        std::unique_ptr<HmiWidget> widget(createHmiWidget("ToggleSwitch", props));
        widget->attach(&context);
        auto *toggle = widget->findChild<FluentQt::SwitchButton *>();
        QVERIFY(toggle);
        toggle->setChecked(true);
        QCOMPARE(link.sent, QList<QByteArray>{on});
        toggle->setChecked(false);
        QCOMPARE(link.sent, (QList<QByteArray>{on, off}));
    }

    void toggleReceiveCondition()
    {
        FakeLink link;
        RuntimeContext context;
        context.hub()->addLink(&link);
        std::unique_ptr<HmiWidget> widget(createHmiWidget("ToggleSwitch", {}));
        widget->attach(&context);
        auto *toggle = widget->findChild<FluentQt::SwitchButton *>();
        QVERIFY(toggle);
        QSignalSpy changes(toggle, &FluentQt::SwitchButton::checkedChanged);
        BindingEntry binding;
        for (const QString &target : {QString(), QStringLiteral("5"), QStringLiteral("0"), QStringLiteral("2.5")}) {
            QJsonObject props{{"commandMode", "Modbus"}};
            if (!target.isEmpty()) {
                props.insert("onCondition", QStringLiteral("等于指定值"));
                props.insert("onValue", target);
            }
            widget->setProperties(props);
            for (double value : {0.0, 1.0, 5.0, -1.0, 2.5}) {
                widget->applyBoundValue(value, binding);
                QCOMPARE(toggle->isChecked(), target.isEmpty() ? value != 0 : value == target.toDouble());
            }
        }
        widget->applyBoundValue(2.5, binding);
        widget->applyBoundValue(QStringLiteral("invalid"), binding);
        QVERIFY(toggle->isChecked());
        QVERIFY(changes.isEmpty());
        QVERIFY(link.sent.isEmpty());

        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets(QJsonArray{QJsonObject{
                                       {"id", "switch"},
                                       {"type", "ToggleSwitch"},
                                       {"properties", QJsonObject{{"onCondition", "等于指定值"}, {"onValue", "5"}}}}},
                                   &error),
                 qPrintable(error));
        PropertyEditor editor;
        editor.setScene(&scene);
        editor.showItem(scene.findItem(QStringLiteral("switch")));
        editor.show();
        QApplication::processEvents();
        auto *condition = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("开启判定")));
        auto *onValue = qobject_cast<QLineEdit *>(fieldFor(&editor, QStringLiteral("判定值")));
        QVERIFY(condition && onValue && onValue->isVisible());
        QCOMPARE(onValue->text(), QStringLiteral("5"));
        condition->setCurrentText(QStringLiteral("非零为开"));
        QApplication::processEvents();
        QVERIFY(!onValue->isVisible());
        condition->setCurrentText(QStringLiteral("等于指定值"));
        QApplication::processEvents();
        QVERIFY(onValue->isVisible());
        QCOMPARE(onValue->text(), QStringLiteral("5"));
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
        QCOMPARE(gbkLink.dataSource().encoding, QStringLiteral("gbk"));
        QCOMPARE(hub->decodeRaw(gbk, gbkLink.dataSource()), QStringLiteral("温度:25.3;"));
        auto *log = surface.findChild<QTextEdit *>();
        QVERIFY(log);
        QVERIFY(log->toPlainText().contains(QStringLiteral("温度:25.3;")));
        utf8Link.inject(QByteArray::fromStdString("湿度=60\n"));
        QCOMPARE(utf8Link.dataSource().encoding, QStringLiteral("utf-8"));
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

    // The PLC tutorial sets no polling settings on its link card: once a Modbus TCP link connects, the poller
    // reads the bindings in three blocks on its defaults (500 ms, slave 1), one reply at a time, with no click;
    // displays show the registers by address; the setpoint goes out as FC16 float32 between two reads, the
    // switch as FC05. The status names a block with an exception or without a reply (after the default
    // 1000 ms); a link that goes down leaves the poller waiting, and it starts again on connecting.
    void plcTutorialPollsOnConnecting()
    {
        Project project;
        QString error;
        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/10-PLC恒压供水.uidp"), &project, &error),
                 qPrintable(error));
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus_tcp"), {}));
        link.setConnected(false);
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        qint64 now = 0;
        surface.context()->setClock([&now]() { return now; });
        ModbusPoller *poller = surface.context()->modbusPoller();
        surface.context()->hub()->addLink(&link);
        QCOMPARE(poller->status(), QStringLiteral("等待连接"));
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
        const QByteArray coils = QByteArray::fromHex("0100000004");
        const QByteArray live = QByteArray::fromHex("0300640006");
        const QByteArray setting = QByteArray::fromHex("0300c80002");

        link.setConnected(true);
        QCOMPARE(pdus(link.sent), QList<QByteArray>{coils});
        QCOMPARE(poller->status(), QStringLiteral("3 块 · 轮询中"));
        serve();
        QCOMPARE(pdus(link.sent), (QList<QByteArray>{coils, live, setting}));
        QCOMPARE(poller->blocks(),
                 (QList<ModbusBlock>{{1, 1, 0, 4, 500}, {1, 3, 100, 6, 500}, {1, 3, 200, 2, 500}}));
        QCOMPARE(poller->status(), QStringLiteral("3 块 · 正常"));
        QCOMPARE(poller->toolTip(), QStringLiteral("从站 1 · 功能码 1 · 地址 0–3 · 周期 500 ms\n"
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
        QCOMPARE(poller->status(), QStringLiteral("第 3 块异常 02 非法数据地址"));
        exceptionAt = -1;

        now = 1500;
        coilsSilent = true;
        tickPoller(surface.context());
        QTRY_COMPARE_WITH_TIMEOUT((serve(), poller->status()), QStringLiteral("第 1 块无响应（从站 1 功能码 1 地址 0）"),
                                  5000);
        coilsSilent = false;

        link.setConnected(false);
        QCOMPARE(poller->status(), QStringLiteral("等待连接"));
        QVERIFY(poller->isWaiting());
        const qsizetype before = link.sent.size();
        link.setConnected(true); // a fresh plan, read at once
        QCOMPARE(link.sent.size(), before + 1);
        QCOMPARE(pdus(link.sent).last(), coils);
        surface.context()->hub()->removeLink(&link);
    }

    // Seen with the PLC tutorial: a reply quicker than the round before left the next block due a moment
    // later, and that block waited for the next tick, a whole period late.
    void pollerReadsABlockDueBetweenTicksWhenDue()
    {
        Project project;
        QString error;
        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/10-PLC恒压供水.uidp"), &project, &error),
                 qPrintable(error));
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus_tcp"), {}));
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        qint64 now = 0;
        surface.context()->setClock([&now]() { return now; });
        const auto reply = [&link](int index) { link.inject(plcReply(link.sent.at(index), {})); };
        const QByteArray coils = QByteArray::fromHex("0100000004");
        const QByteArray live = QByteArray::fromHex("0300640006");
        const QByteArray setting = QByteArray::fromHex("0300c80002");

        surface.context()->hub()->addLink(&link); // the coils at 0
        now = 30;
        reply(0); // the live block at 30
        now = 60;
        reply(1);
        reply(2);
        now = 500;
        tickPoller(surface.context());
        now = 515; // the live block is due at 530
        reply(3);
        QCOMPARE(pdus(link.sent), (QList<QByteArray>{coils, live, setting, coils}));
        QTimer *wake = nullptr;
        for (QTimer *timer : surface.context()->modbusPoller()->findChildren<QTimer *>()) {
            wake = timer->isSingleShot() ? timer : wake;
        }
        QVERIFY(wake && wake->isActive());
        QCOMPARE(wake->interval(), 15);
        now = 530;
        QMetaObject::invokeMethod(wake, "timeout");
        QCOMPARE(pdus(link.sent), (QList<QByteArray>{coils, live, setting, coils, live}));
        surface.context()->hub()->removeLink(&link);
    }

    // Without a link card whose link parses Modbus the poller runs on its defaults (500 ms, 1000 ms, slave 1)
    // and waits for a link that can send: a listening link without a device sends nothing until one connects.
    // The temperature controller tutorial plans its four bindings and its chart's two curves (holding
    // register 4 as float32, and 1) into one block.
    void pollerRunsOnItsDefaults()
    {
        Project project;
        QString error;
        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/10-PLC恒压供水.uidp"), &project, &error),
                 qPrintable(error));
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus_tcp"), {}));
        link.setSendable(false); // listening, no device yet
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
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
        QCOMPARE(poller->status(), QStringLiteral("3 块 · 轮询中"));
        auto *timer = poller->findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(timer && timer->isActive());
        QCOMPARE(timer->interval(), 500);
        link.setSendable(false); // the device left
        QCOMPARE(poller->status(), QStringLiteral("等待连接"));
        surface.context()->hub()->removeLink(&link);

        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/02-温控仪监控.uidp"), &project, &error),
                 qPrintable(error));
        FakeLink serial;
        QVERIFY(serial.setParser(QStringLiteral("modbus"), {}));
        RuntimeSurface controller;
        controller.build(project);
        controller.show();
        controller.context()->hub()->addLink(&serial);
        QCOMPARE(controller.context()->modbusPoller()->blocks(), (QList<ModbusBlock>{{1, 3, 0, 6, 500}}));
        QCOMPARE(serial.sent, QList<QByteArray>{QByteArray::fromHex("010300000006c5c8")});
        controller.context()->hub()->removeLink(&serial);
    }

    // A link card hands the poller its modbusPeriod, modbusTimeout and modbusSlave once its link is connected
    // and parses Modbus, and again as soon as they change: the slave of bindings without a filter, the
    // blocks' default period and how long a read waits. Its status line then reads the connection, then the
    // poller's status, with the blocks as its tooltip; disconnected, the defaults apply again.
    void linkCardSetsUpThePoller()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QJsonObject props{{"defaultHost", "127.0.0.1"},    {"defaultPort", server.serverPort()},
                                {"defaultParser", "modbus_tcp"}, {"modbusPeriod", 300},
                                {"modbusTimeout", 50},           {"modbusSlave", 7}};
        Project project;
        project.widgets =
            QJsonArray{modbusDisplay("near", 10, 0, 0), modbusDisplay("far", 101, 0, 0), tcpCard("plc", 0, props)};
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        qint64 now = 0; // stands still: after the first round nothing is due
        surface.context()->setClock([&now]() { return now; });
        ModbusPoller *poller = surface.context()->modbusPoller();
        QCOMPARE(poller->settings(), ModbusPoller::Settings{}); // the card's link is not connected yet
        auto *card = qobject_cast<HmiWidget *>(surface.findChild<QWidget *>(QStringLiteral("TcpConfigCard")));
        QVERIFY(card);
        QVERIFY(labelShown(card, QStringLiteral("未连接")));
        buttonFor(card, QStringLiteral("连接"))->click();
        QVERIFY(server.waitForNewConnection(5000));
        TcpDevice plc(server.nextPendingConnection());
        QTRY_VERIFY(!plc.requests().isEmpty()); // a slow machine may already have sent the second block
        QElapsedTimer waited;
        waited.start();
        QCOMPARE(poller->settings().defaultPeriod, 300);
        QCOMPARE(poller->settings().timeoutMs, 50);
        QCOMPARE(poller->settings().fallbackSlave, 7);
        QCOMPARE(plc.requests().at(0).mid(6), QByteArray::fromHex("0703000a0001")); // unit 7
        QCOMPARE(poller->blocks(), (QList<ModbusBlock>{{7, 3, 10, 1, 300}, {7, 3, 101, 1, 300}}));
        const QString connection = QStringLiteral("TCP 127.0.0.1:%1").arg(server.serverPort());
        const QString blockTip = QStringLiteral("从站 7 · 功能码 3 · 地址 10–10 · 周期 300 ms\n"
                                                "从站 7 · 功能码 3 · 地址 101–101 · 周期 300 ms");
        // The first round is still going, or already over without a reply: either way the card shows it.
        QLabel *status = nullptr;
        for (QLabel *label : card->findChildren<QLabel *>()) {
            if (label->text().startsWith(connection + QStringLiteral(" · "))) {
                status = label;
            }
        }
        QVERIFY2(status, qPrintable(labelTexts(card).join(QLatin1Char('|'))));
        QVERIFY2(status->text() == connection + QStringLiteral(" · 2 块 · 轮询中") ||
                     status->text() == connection + QStringLiteral(" · 第 1 块无响应（从站 7 功能码 3 地址 10）"),
                 qPrintable(status->text()));
        QCOMPARE(status->toolTip(), blockTip);

        QTRY_VERIFY(plc.requests().size() >= 2); // no reply within the card's 50 ms: the next block
        QVERIFY2(waited.elapsed() < 900, qPrintable(QString::number(waited.elapsed())));
        QCOMPARE(plc.requests().at(1).mid(6), QByteArray::fromHex("070300650001"));
        plc.socket->write(plcReply(plc.requests().at(1), {{101, 42}}));
        QTRY_COMPARE(poller->status(), QStringLiteral("第 1 块无响应（从站 7 功能码 3 地址 10）"));
        QCOMPARE(status->text(), connection + QStringLiteral(" · 第 1 块无响应（从站 7 功能码 3 地址 10）"));
        QCOMPARE(status->toolTip(), blockTip);
        auto *timer = poller->findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(timer && timer->isActive());
        QCOMPARE(timer->interval(), 300);

        QJsonObject faster = props;
        faster.insert(QStringLiteral("modbusPeriod"), 200);
        card->setProperties(faster); // at once
        QCOMPARE(poller->settings().defaultPeriod, 200);
        QCOMPARE(poller->blocks(), (QList<ModbusBlock>{{7, 3, 10, 1, 200}, {7, 3, 101, 1, 200}}));
        QCOMPARE(timer->interval(), 200);
        QCOMPARE(status->text(), connection + QStringLiteral(" · 2 块 · 轮询中"));

        buttonFor(card, QStringLiteral("断开"))->click();
        QCOMPARE(status->text(), QStringLiteral("未连接"));
        QCOMPARE(status->toolTip(), QString());
        QCOMPARE(poller->settings(), ModbusPoller::Settings{});
        QVERIFY(poller->isWaiting());
    }

    // Of the link cards whose links are connected and parse Modbus, the first in the document sets the
    // poller up. A card whose link is not connected does not count; one that parses lines neither counts nor
    // shows the poller's status.
    void modbusLinkCardsKeepTheirOwnPollingSettings()
    {
        QTcpServer first, second, lines;
        for (QTcpServer *server : {&first, &second, &lines}) {
            QVERIFY(server->listen(QHostAddress::LocalHost, 0));
        }
        const auto props = [](const QTcpServer &server, const char *parser, int period, int timeout, int slave) {
            return QJsonObject{{"defaultHost", "127.0.0.1"}, {"defaultPort", server.serverPort()},
                               {"defaultParser", parser},    {"modbusPeriod", period},
                               {"modbusTimeout", timeout},   {"modbusSlave", slave}};
        };
        Project project;
        project.widgets =
            QJsonArray{modbusDisplay("pv", 100, 0, 0), tcpCard("a", 0, props(first, "modbus_tcp", 300, 50, 7)),
                       tcpCard("b", 260, props(second, "modbus_tcp", 100, 2000, 9)),
                       tcpCard("c", 520, props(lines, "line", 50, 20, 3))};
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        qint64 now = 0;
        surface.context()->setClock([&now]() { return now; });
        ModbusPoller *poller = surface.context()->modbusPoller();
        const QList<QWidget *> cards = surface.findChildren<QWidget *>(QStringLiteral("TcpConfigCard"));
        QCOMPARE(cards.size(), 3);
        const auto connection = [](const QTcpServer &server) {
            return QStringLiteral("TCP 127.0.0.1:%1").arg(server.serverPort());
        };
        const auto click = [&cards](int card, const char *text) {
            QPushButton *button = buttonFor(cards.at(card), QString::fromUtf8(text));
            QVERIFY(button);
            button->click();
        };

        click(2, "连接");
        QTRY_VERIFY(labelShown(cards.at(2), connection(lines)));
        QCOMPARE(labelWith(cards.at(2), connection(lines))->toolTip(), QString());
        QCOMPARE(poller->settings(), ModbusPoller::Settings{});
        QVERIFY(poller->isWaiting());

        click(1, "连接");
        QTRY_COMPARE(poller->settings(), (ModbusPoller::Settings{100, 2000, 9}));
        QCOMPARE(poller->blocks(), (QList<ModbusBlock>{{9, 3, 100, 1, 100}}));
        QVERIFY(labelShown(cards.at(1), connection(second) + QStringLiteral(" · 1 块 · 轮询中")));
        QVERIFY(labelShown(cards.at(2), connection(lines))); // parses lines: as it was

        click(0, "连接");
        QTRY_COMPARE(poller->settings(), (ModbusPoller::Settings{300, 50, 7}));
        QCOMPARE(poller->blocks(), (QList<ModbusBlock>{{7, 3, 100, 1, 300}, {9, 3, 100, 1, 100}}));
        QVERIFY(labelShown(cards.at(0), connection(first) + QStringLiteral(" · 1 块 · 轮询中")));
        QVERIFY(labelShown(cards.at(1), connection(second) + QStringLiteral(" · 1 块 · 轮询中")));
        QVERIFY(labelShown(cards.at(2), connection(lines)));

        click(0, "断开");
        QCOMPARE(poller->settings(), (ModbusPoller::Settings{100, 2000, 9}));
        QVERIFY(labelShown(cards.at(0), QStringLiteral("未连接")));
        click(1, "断开");
        QCOMPARE(poller->settings(), ModbusPoller::Settings{});
        click(2, "断开");
    }

    // A listening card that parses Modbus cannot send while it has no device, so it is not polled and shows
    // only its own state: no 等待连接, and nothing of the poller's while another Modbus link keeps the poller
    // going. Once a device connects, the poller's status follows the device count (without Modbus bindings
    // 没有 Modbus 绑定, which is the tooltip too, there being no blocks); when it leaves, it goes again.
    void listeningLinkCardShowsThePolling()
    {
        quint16 port = 0;
        {
            QTcpServer probe; // a free port
            QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
            port = probe.serverPort();
        }
        Project project;
        project.widgets = QJsonArray{tcpCard("server", 0,
                                             QJsonObject{{"defaultMode", "服务端"},
                                                         {"defaultHost", "127.0.0.1"},
                                                         {"defaultPort", port},
                                                         {"defaultParser", "modbus_tcp"}})};
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        ModbusPoller *poller = surface.context()->modbusPoller();
        QWidget *card = surface.findChild<QWidget *>(QStringLiteral("TcpConfigCard"));
        QVERIFY(card);
        buttonFor(card, QStringLiteral("连接"))->click();
        const QString listening = QStringLiteral("TCP 服务端 :%1").arg(port);
        QVERIFY2(labelShown(card, listening), qPrintable(labelTexts(card).join(QLatin1Char('|'))));
        QCOMPARE(labelWith(card, listening)->toolTip(), QString());
        QVERIFY(poller->isWaiting());

        FakeLink other; // another Modbus link: the poller is no longer waiting
        QVERIFY(other.setParser(QStringLiteral("modbus_tcp"), {}));
        surface.context()->hub()->addLink(&other);
        QCOMPARE(poller->status(), QStringLiteral("没有 Modbus 绑定"));
        QVERIFY(labelShown(card, listening)); // no device: still nothing of the poller's
        QCOMPARE(labelWith(card, listening)->toolTip(), QString());

        QTcpSocket device;
        device.connectToHost(QHostAddress::LocalHost, port);
        const QString polling = listening + QStringLiteral(" · 1 台设备 · 没有 Modbus 绑定");
        QTRY_VERIFY(labelShown(card, polling));
        QCOMPARE(labelWith(card, polling)->toolTip(), polling);
        device.disconnectFromHost();
        const QString left = listening + QStringLiteral(" · 0 台设备");
        QTRY_VERIFY(labelShown(card, left));
        QCOMPARE(labelWith(card, left)->toolTip(), QString());
        QCOMPARE(poller->status(), QStringLiteral("没有 Modbus 绑定")); // the other link keeps it going
        surface.context()->hub()->removeLink(&other);
        buttonFor(card, QStringLiteral("断开"))->click();
        QVERIFY(labelShown(card, QStringLiteral("未连接")));
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

    void linkCardsFitTheirRows_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("mode");
        for (const char *mode : {"完整", "简洁", "单行"}) {
            QTest::addRow("serial %s", mode) << QStringLiteral("SerialConfigCard") << QString::fromUtf8(mode);
        }
        for (const char *type : {"TcpConfigCard", "MqttConfigCard", "UdpConfigCard", "CanConfigCard"}) {
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
        QCOMPARE(card->height(), std::max(card->meta().minHeight, minimum));
        CanvasItem fresh(type, {{"displayMode", mode}}, QString());
        QCOMPARE(fresh.size().height(), fresh.minimumHeight());

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
            if (grid->itemAt(i)->isEmpty()) {
                continue; // CAN hides model-specific rows without removing them from the grid.
            }
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
    // Closing Studio with a widget selected crashed: the scene, deleted with its page, emitted selectionChanged for
    // the item it took away, and the page rebuilt the property panel from a project already destroyed. A scene
    // going away tells nobody.
    void destroyedSceneEmitsNothing()
    {
        auto *scene = new DesignScene;
        QString error;
        QVERIFY2(scene->loadWidgets(QJsonArray{QJsonObject{
                                        {"id", "label"},
                                        {"type", "Label"},
                                        {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 120}, {"h", 30}}}}},
                                    &error),
                 qPrintable(error));
        scene->findItem(QStringLiteral("label"))->setSelected(true);
        QSignalSpy selection(scene, &QGraphicsScene::selectionChanged);
        delete scene;
        QCOMPARE(selection.count(), 0);
    }

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
        QCOMPARE(tcp->size().height(), brief);
        QCOMPARE(height->minimum(), int(std::ceil(brief)));
        tcp->setSize(QSizeF(360, brief));
        mode->setCurrentText(QStringLiteral("完整"));
        QCOMPARE(tcp->size().height(), full);
        QCOMPARE(height->value(), int(std::ceil(full)));

        tcp->setSize(QSizeF(360, 350));
        tcp->setPropertyValue(QStringLiteral("title"), QStringLiteral("PLC"));
        QCOMPARE(tcp->size().height(), 350.0); // ordinary edits keep the user's chosen height
        QCOMPARE(tcp->widget()->height(), 350);
        std::unique_ptr<CanvasItem> restored(CanvasItem::fromJson(tcp->toJson(), QString()));
        QCOMPARE(restored->size().height(), 350.0);

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

    void canConnectionErrorsUseOneFlyoutWithoutGrowingTheCard()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        RuntimeContext context;
        std::unique_ptr<HmiWidget> card(createHmiWidget(
            "CanConfigCard", {{"defaultAdapter", "ZLG"}, {"zlgModel", "USBCAN-II"},
                              {"dbcFile", directory.filePath(QString(120, QLatin1Char('x')) + ".dbc")}}));
        card->attach(&context);
        card->show();
        QApplication::processEvents();
        const int before = card->height();
        QSignalSpy notifications(&context, &RuntimeContext::notificationRequested);
        QPushButton *connectButton = nullptr;
        for (auto *button : card->findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("连接")) {
                connectButton = button;
            }
        }
        QVERIFY(connectButton);
        QTest::mouseClick(connectButton, Qt::LeftButton);
        QCOMPARE(notifications.size(), 1);
        const QString detail = notifications.first().at(1).toString();
        QVERIFY(detail.contains(directory.path()));
        bool failed = false;
        for (auto *label : card->findChildren<QLabel *>()) {
            QVERIFY(label->text() != detail);
            if (label->text() == QStringLiteral("连接失败")) {
                QCOMPARE(label->toolTip(), detail);
                failed = true;
            }
        }
        QVERIFY(failed);
        QCOMPARE(card->height(), before);
        QTest::mouseClick(connectButton, Qt::LeftButton);
        QCOMPARE(notifications.size(), 1);
        card->detach();
    }

    // A CAN card dropped since the project was last saved is on the page but not yet in the project's widget list:
    // a binding offers CAN信号 for it all the same.
    void bindingSeesUnsavedCanCard()
    {
        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets(
                     QJsonArray{QJsonObject{{"id", "can"},
                                            {"type", "CanConfigCard"},
                                            {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 380}, {"h", 270}}}},
                                QJsonObject{{"id", "volt"},
                                            {"type", "NumericDisplay"},
                                            {"geometry", QJsonObject{{"x", 400}, {"y", 0}, {"w", 170}, {"h", 90}}}}},
                     &error),
                 qPrintable(error));
        Project project; // saved before the card was there
        PropertyEditor editor;
        editor.setProject(&project);
        editor.setScene(&scene);
        editor.showItem(scene.findItem(QStringLiteral("volt")));
        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("解析模式")));
        QVERIFY(mode);
        QVERIFY(mode->findText(QStringLiteral("CAN信号")) >= 0);
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
        const auto sourceA = serverRaw.first().at(1).value<DataSource>();
        const auto source0 = serverMessages.at(0).at(1).value<DataSource>();
        const auto source1 = serverMessages.at(1).at(1).value<DataSource>();
        QVERIFY(source0.stream != source1.stream);
        QVERIFY(source0.endpoint != source1.endpoint);
        QVERIFY(server.send("ONLY_A", sourceA.endpoint));
        QTRY_COMPARE(first.bytesAvailable(), 6);
        QCOMPARE(first.readAll(), QByteArray("ONLY_A"));
        QCOMPARE(second.bytesAvailable(), 0);
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

        QVERIFY(server.setParser(QStringLiteral("modbus_tcp"), {}));
        RuntimeContext context;
        context.hub()->addLink(&server, QStringLiteral("Server"));
        ModbusNeed offlineNeed;
        offlineNeed.table = QStringLiteral("hr");
        offlineNeed.route = {QStringLiteral("Server"), sourceA.endpoint};
        context.setCurveNeeds(this, {offlineNeed});
        context.setActive(true);
        QSignalSpy errors(&server, &Link::errorOccurred);
        QVERIFY(server.listen(QStringLiteral("127.0.0.1"), 0, &error));
        context.modbusPoller()->poll();
        QVERIFY(errors.isEmpty());
        QVERIFY(context.modbusPoller()->isWaiting()); // the configured peer is gone, although TCP is listening.
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
            if (!text && caption && caption->widget()) {
                text = caption->widget()->findChild<QLabel *>(); // label with an adjacent help button
            }
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
    // starting from the project's when the modes match, and its polling rows for an effective Modbus mode;
    // a Modbus-mode control keeps its
    // Modbus rows in view with the send toggle off, since it sends either way.
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
        const QStringList pollingRows = {QStringLiteral("默认轮询(ms)"), QStringLiteral("响应超时(ms)"),
                                         QStringLiteral("默认从站")};
        const auto pollingShown = [&editor, &pollingRows](bool shown) {
            for (const QString &row : pollingRows) {
                if (labelShown(&editor, row) != shown) {
                    return false;
                }
            }
            return true;
        };
        QVERIFY(pollingShown(false));
        const auto pick = [&editor](const QString &parser) {
            auto *combo = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("默认解析模式")));
            QVERIFY(combo);
            const int index = combo->findData(parser);
            QVERIFY(index >= 0);
            combo->setCurrentIndex(index);
            QApplication::processEvents(); // the form is rebuilt for the new mode
            QApplication::processEvents();
        };
        pick(QStringLiteral("modbus_tcp"));
        QVERIFY(pollingShown(true));
        QVERIFY(!labelShown(&editor, QStringLiteral("帧格式")));
        for (const auto &[row, value, lo, hi] : {std::tuple{QStringLiteral("默认轮询(ms)"), 500, 10, 60000},
                                                 std::tuple{QStringLiteral("响应超时(ms)"), 1000, 10, 60000},
                                                 std::tuple{QStringLiteral("默认从站"), 1, 1, 247}}) {
            auto *spin = qobject_cast<QSpinBox *>(fieldFor(&editor, row));
            QVERIFY2(spin, qPrintable(row));
            QCOMPARE(spin->value(), value);
            QCOMPARE(spin->minimum(), lo);
            QCOMPARE(spin->maximum(), hi);
        }
        qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("默认轮询(ms)")))->setValue(250);
        CanvasItem *item = scene.findItem(QStringLiteral("uart"));
        QCOMPARE(item->propertyValue(QStringLiteral("modbusPeriod")).toInt(), 250);
        pick(QStringLiteral("hex"));
        QVERIFY(pollingShown(false));
        QVERIFY(labelShown(&editor, QStringLiteral("帧格式")));
        QVERIFY(labelShown(&editor, QStringLiteral("固定帧长")));
        QVERIFY(!labelShown(&editor, QStringLiteral("分隔符")));
        // The card has no parserConfig yet and parses as the project does: the project's rows.
        QCOMPARE(qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("长度字段偏移")))->value(), 2);
        fieldFor(&editor, QStringLiteral("字段表"))
            ->findChild<QPlainTextEdit *>()
            ->setPlainText(QStringLiteral("v,0,2,uint16"));
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
        QVERIFY(pollingShown(true));
        QCOMPARE(qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("默认轮询(ms)")))->value(), 250);
        pick(QStringLiteral("项目设置"));
        QVERIFY(!labelShown(&editor, QStringLiteral("帧格式")));
        QVERIFY(pollingShown(false));
        QCOMPARE(item->propertyValue(QStringLiteral("modbusPeriod")).toInt(), 250); // kept, and still applies
        project.serial.insert(QStringLiteral("parser"), QStringLiteral("modbus_tcp"));
        editor.showItem(nullptr);
        editor.showItem(item);
        QApplication::processEvents();
        QVERIFY(pollingShown(true));
        QVERIFY(!labelShown(&editor, QStringLiteral("帧格式")));
        QCOMPARE(qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("默认轮询(ms)")))->value(), 250);

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

        // A Modbus binding names a register table and an address, and has its 采集周期(ms).
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
        QCOMPARE(period->toolTip(), QStringLiteral("0 = 用默认周期（链路卡片的「默认轮询(ms)」，默认 500）"));
        period->setValue(250);
        QCOMPARE(scene.findItem(QStringLiteral("pv"))->binding().value("mbPeriod").toInt(), 250);
    }

    // tests/data/image_placement_vectors.json, shared with PyQt UI Designer: where an ImageDisplay draws the
    // corners of its picture for a box, a picture size, a turn, mirroring and a scale mode.
    void imagePlacementMatchesTheGoldenVectors()
    {
        QFile file(QStringLiteral(FHMI_TEST_DATA_DIR "/image_placement_vectors.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject document = QJsonDocument::fromJson(file.readAll()).object();
        const double tolerance = document.value("tolerance").toDouble();
        const QJsonArray vectors = document.value("vectors").toArray();
        QCOMPARE(vectors.size(), 17);
        QStringList failures;
        for (qsizetype i = 0; i < vectors.size(); ++i) {
            const QJsonObject vector = vectors.at(i).toObject();
            const QSizeF box(vector.value("box")[0].toDouble(), vector.value("box")[1].toDouble());
            const int w = vector.value("image")[0].toInt();
            const int h = vector.value("image")[1].toInt();
            const QTransform transform =
                imageTransform(box, QSize(w, h), vector.value("rotation").toInt(), vector.value("mirrorH").toBool(),
                               vector.value("mirrorV").toBool(), vector.value("scaleMode").toString());
            const QList<QPointF> corners{QPointF(0, 0), QPointF(w, 0), QPointF(w, h), QPointF(0, h)};
            const QJsonArray expected = vector.value("corners").toArray();
            for (int j = 0; j < 4; ++j) {
                const QPointF got = transform.map(corners.at(j));
                const QPointF want(expected.at(j)[0].toDouble(), expected.at(j)[1].toDouble());
                if (std::abs(got.x() - want.x()) > tolerance || std::abs(got.y() - want.y()) > tolerance) {
                    failures << QStringLiteral("vector %1, corner %2: (%3, %4), expected (%5, %6)")
                                    .arg(i)
                                    .arg(j)
                                    .arg(got.x(), 0, 'g', 12)
                                    .arg(got.y(), 0, 'g', 12)
                                    .arg(want.x(), 0, 'g', 12)
                                    .arg(want.y(), 0, 'g', 12);
                }
            }
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
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
        project.bitProfiles={{"default",effectiveBitProfile({},QStringLiteral("default"))}};
        project.filePath = source.filePath("panel.uidp");
        project.widgets = QJsonArray{QJsonObject{{"id", "img"},
                                                 {"type", "ImageDisplay"},
                                                 {"properties", QJsonObject{{"imagePath", "logo.png"}}}}};
        project.widgets.append(QJsonObject{{"id","word"},{"type","StatusWordPanel"},{"properties",QJsonObject{{"bitProfile","default"}}}});
        project.widgets.append(QJsonObject{{"id","timing"},{"type","DigitalTimingChart"},{"properties",QJsonObject{{"bitProfile","default"}}}});
        project.widgets.append(QJsonObject{{"id","plc"},{"type","S7ConfigCard"},{"properties",QJsonObject{{"port",1102}}}});
        project.widgets.append(QJsonObject{{"id","start"},{"type","PushButton"},{"properties",QJsonObject{
            {"commandMode","S7"},{"commandTargetId","plc"},{"s7Command",QJsonObject{{"address","DB1.DBX0.1"},{"dataType","BOOL"},{"value","1"}}}}}});
        // A studio set to orange once exported a cyan application.
        auto *config = FluentQt::FluentConfig::instance();
        auto *theme = FluentQt::ThemeManager::instance();
        const QColor studioColor = config->themeColor();
        const auto restore = qScopeGuard([config, theme, studioColor]() {
            config->setThemeColor(studioColor);
            theme->setAccentColor(studioColor);
        });
        config->setThemeColor(QColor(QStringLiteral("#e8590c")));
        QString error;
        const QString created = ExportService::exportApplication(project, output.path(), &error);
        QVERIFY2(!created.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo(created).fileName().startsWith(QStringLiteral("温控_面板")));
#ifdef Q_OS_MACOS
        const QDir resources(created + "/Contents/Resources");
        QVERIFY(QFileInfo::exists(created + "/Contents/MacOS/FluentHMIRuntime"));
        QVERIFY(QFileInfo::exists(created + "/Contents/Frameworks/libsnap7.1.dylib"));
#elif defined(Q_OS_LINUX)
        // A tar.gz of the application's folder, whose program and start.sh stay executable.
        QVERIFY(created.endsWith(QStringLiteral(".tar.gz")));
        QProcess tar;
        tar.start(QStringLiteral("tar"), {QStringLiteral("-xzf"), created, QStringLiteral("-C"), output.path()});
        QVERIFY(tar.waitForFinished(60000) && tar.exitCode() == 0);
        const QDir resources(created.chopped(7));
        QVERIFY(QFileInfo(resources.filePath(resources.dirName())).isExecutable());
        QVERIFY(QFileInfo(resources.filePath("start.sh")).isExecutable());
#else
        const QDir resources(created);
#endif
        Project exported;
        QVERIFY2(Project::load(resources.filePath("project.uidp"), &exported, &error), qPrintable(error));
        QCOMPARE(exported.appName, project.appName);
        QCOMPARE(exported.toJson().value(QStringLiteral("version")).toString(),QStringLiteral("1.1"));
        QCOMPARE(exported.widgets.at(3),project.widgets.at(3));
        QCOMPARE(exported.widgets.at(4),project.widgets.at(4));
        QVERIFY(QFileInfo::exists(resources.filePath("licenses/snap7/NOTICE.md")));
#ifdef Q_OS_WIN
        QVERIFY(QFileInfo::exists(resources.filePath("snap7.dll")));
#elif defined(Q_OS_LINUX)
        QVERIFY(QFileInfo::exists(resources.filePath("libsnap7.so.1")) || QFileInfo::exists(resources.filePath("lib/libsnap7.so.1")));
#endif
        QCOMPARE(exported.bitProfiles,project.bitProfiles);
        QCOMPARE(exported.widgets.at(1).toObject().value("properties").toObject().value("bitProfile").toString(),QStringLiteral("default"));
        QCOMPARE(exported.widgets.at(2).toObject().value("properties").toObject().value("bitProfile").toString(),QStringLiteral("default"));
        const QString bundled = exported.widgets.at(0).toObject().value("properties").toObject().value("imagePath").toString();
        QCOMPARE(bundled, QStringLiteral("assets/logo.png"));
        QVERIFY(QFileInfo::exists(resources.filePath(bundled)));
        // What the exported runtime applies at start-up.
        theme->setAccentColor(QColor(QStringLiteral("#009faa")));
        QVERIFY(applyAppearance(resources.path()));
        QCOMPARE(theme->accentColor(), QColor(QStringLiteral("#e8590c")));

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
    void scaledCanvasMenusStayNativeAndAcceptInput()
    {
        Project project;
        project.canvasWidth = 1000;
        project.canvasHeight = 650;
        project.widgets =
            QJsonArray{QJsonObject{{"id", "chart"},
                                   {"type", "RealtimeChart"},
                                   {"geometry", QJsonObject{{"x", 400}, {"y", 330}, {"w", 590}, {"h", 300}}}}};
        PlayerWindow window(project);
        auto *plot = window.surface()->findChild<FluentQt::RealtimePlotWidget *>();
        auto *view = window.findChild<QGraphicsView *>("canvasView");
        QVERIFY(plot && view);
        window.show();
        QSize menuSize;
        for (const QSize size : {QSize(720, 490), QSize(1100, 760)}) {
            window.resize(size);
            QApplication::processEvents();
            plot->setXRange(10, 20);
            plot->setYRange(20, 30);
            const QPoint point(plot->width() - 60, plot->height() - 50);
            QContextMenuEvent event(QContextMenuEvent::Mouse, point, plot->mapToGlobal(point));
            QApplication::sendEvent(plot, &event);
            QPointer<FluentQt::RoundMenu> menu;
            for (auto *candidate : plot->findChildren<FluentQt::RoundMenu *>()) {
                if (candidate->isVisible()) {
                    menu = candidate;
                    break;
                }
            }
            QVERIFY(menu);
            QVERIFY(!menu->graphicsProxyWidget());
            QVERIFY(menu->windowFlags().testFlag(Qt::BypassGraphicsProxyWidget));
            if (menuSize.isEmpty()) {
                menuSize = menu->size();
            } else {
                QCOMPARE(menu->size(), menuSize);
            }
            QTest::qWait(250); // finish the existing popup animation before capture and click
            auto *items = menu->view();
            QVERIFY(items->count() > 0);
            QTest::mouseClick(items->viewport(), Qt::LeftButton, Qt::NoModifier,
                              items->visualItemRect(items->item(0)).center());
            QTRY_VERIFY(!menu || !menu->isVisible());
            QCOMPARE(plot->xMinimum(), 0.0);
            QCOMPARE(plot->xMaximum(), 1.0);
            QCOMPARE(plot->yMinimum(), -1.0);
            QCOMPARE(plot->yMaximum(), 1.0);
        }
    }

    void s7SavedGeometryMatchesRuntime()
    {
        for (const QSize requested : {QSize(900, 100), QSize(900, 240), QSize(380, 100)}) {
            const QJsonObject data{{"id", "plc"}, {"type", "S7ConfigCard"},
                                   {"geometry", QJsonObject{{"x", 20}, {"y", 20},
                                                           {"w", requested.width()}, {"h", requested.height()}}}};
            std::unique_ptr<CanvasItem> item(CanvasItem::fromJson(data, QString()));
            if (requested.width() == 900) {
                QCOMPARE(item->size().toSize(), requested);
            }
            Project project;
            project.widgets = {item->toJson()};
            RuntimeSurface surface;
            surface.build(project);
            surface.show();
            QApplication::processEvents();
            const auto *card = surface.widgets().first();
            QCOMPARE(card->size(), item->size().toSize());
            QVERIFY(card->height() >= card->minimumSizeHint().height());
        }
    }

    void s7CardKeepsErrorsOutOfItsLayout()
    {
        RuntimeContext context;
        QSignalSpy notices(&context, &RuntimeContext::notificationRequested);
        std::unique_ptr<HmiWidget> card(createHmiWidget("S7ConfigCard", {{"host", "invalid-host"}}));
        context.registerWidget("plc", card.get());
        card->attach(&context);
        card->show();
        QApplication::processEvents();
        auto *status = card->findChild<QLabel *>("s7Status");
        QVERIFY(status);
        QCOMPARE(status->text(), QStringLiteral("连接失败"));
        QCOMPARE(notices.size(), 1);
        QCOMPARE(notices.first().at(2).toString(), NotifyType::Error);
        const int narrowHeight = card->minimumSizeHint().height();
        card->resize(800, narrowHeight);
        QApplication::processEvents();
        const int wideHeight = card->minimumSizeHint().height();
        QVERIFY(wideHeight < narrowHeight);
        card->resize(380, wideHeight);
        QApplication::processEvents();
        QVERIFY(card->height() >= card->minimumSizeHint().height());
        for (auto *spin : card->findChildren<QSpinBox *>()) {
            auto *edit = spin->findChild<QLineEdit *>();
            QVERIFY(edit);
            QVERIFY2(
                edit->width() >= edit->fontMetrics().horizontalAdvance(spin->text()),
                qPrintable(QStringLiteral("%1 has only %2 px after resizing").arg(spin->text()).arg(edit->width())));
        }
        const int height = card->height();
        const QString reason = QStringLiteral("S7 connection failed: ") + QString(300, QLatin1Char('x'));
        emit context.s7Client()->errorOccurred("other", reason);
        QCOMPARE(notices.size(), 1);
        emit context.s7Client()->errorOccurred("plc", reason);
        emit context.s7Client()->errorOccurred("plc", reason);
        QCOMPARE(notices.size(), 2);
        QCOMPARE(status->toolTip(), reason);
        QCOMPARE(card->height(), height);
        QCOMPARE(status->text(), QStringLiteral("连接失败"));
    }
};

QTEST_MAIN(TestRuntime)
#include "tst_runtime.moc"
