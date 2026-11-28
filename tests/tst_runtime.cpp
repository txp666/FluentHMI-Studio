#include "comm/can/can_polling_backend.h"
#include "comm/can/can_initial_state.h"
#include "comm/can/can_link.h"
#include "comm/can/can_session.h"
#include "comm/comm_hub.h"
#include "comm/link.h"
#include "comm/modbus_slave.h"
#include "comm/serial_link.h"
#include "comm/tcp_link.h"
#include "core/bit_profile.h"
#include "core/frame_templates.h"
#include "core/i18n.h"
#include "core/project_logic.h"
#include "designer/canvas_item.h"
#include "designer/can_signal_editor.h"
#include "designer/command_preview.h"
#include "designer/design_scene.h"
#include "runtime/frame_bytes_view.h"
#include "designer/frame_template_editor.h"
#include "designer/project_frame_templates.h"
#include "designer/project_can_signals.h"
#include "designer/property_editor.h"
#include "designer/project_logic_dialog.h"
#include "player/player_window.h"
#include "runtime/app_dialog.h"
#include "runtime/appearance.h"
#include "runtime/can_recorder.h"
#include "runtime/date_time_input.h"
#include "runtime/hmi_widget.h"
#include "runtime/modbus_poller.h"
#include "runtime/periodic_sender.h"
#include "runtime/project_data.h"
#include "runtime/receive_frame_preview.h"
#include "runtime/runtime_context.h"
#include "runtime/runtime_surface.h"
#include "runtime/ui_helpers.h"
#include "runtime/widgets/can_timing_dialog.h"
#include "runtime/widgets/can_widgets.h"
#include "runtime/widgets/container_widgets.h"
#include "runtime/widgets/data_widgets.h"
#include "runtime/widgets/digital_timing_widget.h"
#include "runtime/widgets/display_widgets.h"
#include "runtime/widgets/industrial_widget.h"
#include "studio/ai_assistant_panel.h"
#include "studio/ai_service.h"
#include "studio/designer_page.h"
#include "studio/export_service.h"
#include <FluentQtWidgets/Window/FluentTitleBar.h>
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
#include <FluentQtWidgets/Widgets/ScrollArea.h>
#include <FluentQtWidgets/Widgets/Selection.h>
#include <FluentQtWidgets/Widgets/Slider.h>
#include <FluentQtWidgets/Widgets/SwitchButton.h>
#include <FluentQtWidgets/Widgets/TabView.h>

#include <QtCore/QDirIterator>
#include <QtCore/QElapsedTimer>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QLockFile>
#include <QtCore/QMap>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtCore/QScopeGuard>
#include <QtCore/QSemaphore>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QTimeZone>
#include <QtCore/QRegularExpression>
#include <QtGui/QImage>
#include <QtGui/QContextMenuEvent>
#include <QtGui/QScreen>
#include <QtGui/QClipboard>
#include <QtGui/QTextCursor>
#include <QtGui/QTextDocument>
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
    bool hasPendingWrite(const QString &) const override { return busy; }
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
    bool busy = false;

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

bool addExistingCommandInput(AppDialog *dialog, const QString &id)
{
    const auto *menu = dialog->findChild<FluentQt::RoundMenu *>(QStringLiteral("existingCommandInputs"));
    if (!menu)
        return false;
    for (auto *action : menu->findChildren<QAction *>()) {
        if (action->data().toString() == id) {
            action->trigger();
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
        points.append(QJsonObject{{"name", "Standalone"}, {"binding", QJsonObject{{"key", "standalone"}, {"sourceCardId", "PLC_A"}}}});
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

    void pointFrameEditorStagesTheSharedParserAndBindingTogether()
    {
        Project project;
        const QJsonObject defaults{{"header", "AA 55"}, {"footer", "0D"}};
        project.serial.insert("parser", "hex");
        project.serial.insert("parserConfig", defaults);
        project.widgets = {QJsonObject{{"id", "A"}, {"type", "TcpConfigCard"}},
                           QJsonObject{{"id", "B"}, {"type", "TcpConfigCard"}}};
        QJsonObject point{{"name", "Speed"}, {"binding", QJsonObject{{"parseMode", ParseMode::HexBytes},
                                                                   {"sourceCardId", "A"}}}};
        QWidget host;
        host.resize(1440, 900);
        host.show();
        const auto edit = [&](bool apply) {
            QTimer::singleShot(0, &host, [&, apply] {
                auto *source = host.findChild<AppDialog *>("projectDataSourceDialog");
                QVERIFY(source);
                const auto closeSource = qScopeGuard([source] { if (source->isVisible()) source->reject(); });
                auto *open = source->findChild<QPushButton *>("editReceiveFrameFormat");
                QVERIFY(open && open->isVisible());
                QTimer::singleShot(0, source, [&] {
                    auto *format = host.findChild<AppDialog *>("receiveFrameFormatEditor");
                    QVERIFY(format);
                    const auto closeFormat = qScopeGuard([format] { if (format->isVisible()) format->reject(); });
                    auto *tabs = format->findChild<FluentQt::SegmentedWidget *>("receiveEditorTabs");
                    auto *sample = format->findChild<QPlainTextEdit *>("receiveFrameSample");
                    auto *offset = format->findChild<QSpinBox *>("receiveValueOffset");
                    auto *trim = format->findChild<QLineEdit *>("receiveValueHeader");
                    auto *expression = format->findChild<QLineEdit *>("receiveValueExpression");
                    QVERIFY(tabs && sample && offset && trim && expression);
                    QVERIFY(offset->isVisible());
                    offset->setValue(1);
                    trim->setText(QStringLiteral("AA 55"));
                    expression->setText(QStringLiteral("value / 2"));
                    sample->setPlainText(QStringLiteral("AA 55 08 01 02 0D"));
                    QVERIFY(labelShown(format, QStringLiteral("当前取值 · 整帧字节 3–4 · 样例值 258 → 129.0")));
                    tabs->setCurrentItem(QStringLiteral("format"));
                    auto *header = format->findChild<QLineEdit *>("receiveFrame_header");
                    QVERIFY(header && header->isVisible());
                    QCOMPARE(header->text(), QStringLiteral("AA 55"));
                    header->setText(QStringLiteral("CC 33"));
                    sample->setPlainText(QStringLiteral("CC 33 08 01 02 0D"));
                    auto *add = format->findChild<QPushButton *>("addReceiveFrameField");
                    QVERIFY(add);
                    QTimer::singleShot(0, format, [&] {
                        auto *field = host.findChild<AppDialog *>("receiveFieldEditor");
                        QVERIFY(field);
                        const auto closeField = qScopeGuard([field] { if (field->isVisible()) field->reject(); });
                        field->findChild<QLineEdit *>("receiveFieldName")->setText(QStringLiteral("rpm"));
                        field->findChild<QSpinBox *>("receiveFieldOffset")->setValue(1);
                        field->findChild<FluentQt::ComboBox *>("receiveFieldType")->setCurrentText(QStringLiteral("uint16"));
                        field->yesButton()->click();
                    });
                    add->click();
                    tabs->setCurrentItem(QStringLiteral("binding"));
                    auto *bind = format->findChild<QPushButton *>("applyReceiveField");
                    QVERIFY(bind && bind->isVisible() && bind->isEnabled());
                    bind->click();
                    QVERIFY(labelShown(format, QStringLiteral("当前取值 · rpm · 样例值 258 → 129.0")));
                    format->yesButton()->click();
                });
                open->click();
                (apply ? source->yesButton() : source->cancelButton())->click();
            });
            return editDevicePoint(&project, &point, &host, false);
        };

        const auto before = project.toJson();
        const auto oldPoint = point;
        QVERIFY(!edit(false));
        QCOMPARE(project.toJson(), before);
        QCOMPARE(point, oldPoint);
        QVERIFY(edit(true));
        QCOMPARE(project.parserConfig(), defaults);
        QCOMPARE(project.widgets.at(1), before.value("widgets").toArray().at(1));
        const auto props = project.widgets.first().toObject().value("properties").toObject();
        QCOMPARE(props.value("defaultParser").toString(), QStringLiteral("hex"));
        QCOMPARE(point.value("binding").toObject().value("parseMode").toString(), ParseMode::KeyValue);
        QCOMPARE(point.value("binding").toObject().value("key").toString(), QStringLiteral("rpm"));
        project.logic.insert("points", QJsonArray{point});
        QCOMPARE(project.toJson().value("version"), QJsonValue("1.3"));
        RuntimeContext runtime;
        runtime.configureData(project);
        FakeLink connection;
        QVERIFY(connection.setParser(QStringLiteral("hex"), props.value("parserConfig").toObject()));
        runtime.hub()->addLink(&connection, "A");
        runtime.setActive(true);
        connection.inject(QByteArray::fromHex("CC330801020D"));
        QCOMPARE(runtime.projectData()->values().value("Speed").toInt(), 129);
    }

    void projectFrameTemplateFeedsTheReceiverAndSender()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const QString id = QStringLiteral("speed-v1");
        FrameTemplates::Entry entry{QStringLiteral("Speed"),
            {QStringLiteral("hex"), QJsonObject{{"header", "AA55"}, {"footer", "0D"}, {"lengthOffset", 2},
                 {"checksum", "异或"}, {"checksumStart", 3},
                 {FrameTemplates::SendDefaults, QJsonObject{{"cmd", "1"}}},
                 {FrameTemplates::SendRequired, QJsonArray{"cmd", "rpm"}},
                 {"fields", QJsonArray{QJsonArray{"cmd", 1, 1, "uint8"},
                                       QJsonArray{"rpm", 2, 2, "uint16", "小端(LE)"}}}}}, {}};
        Project project;
        project.frameTemplates.insert(id, entry.toJson());
        project.widgets = {tcpCard("device", 60, {{"defaultHost", "127.0.0.1"}, {"defaultPort", server.serverPort()}}),
            QJsonObject{{"id", "send"}, {"type", "PushButton"},
                {"properties", QJsonObject{{"commandMode", "HEX"}, {"command", "01 02"}, {"text", "Send"},
                                           {"commandTargetId", "device"}, {"frameHeader", "99"},
                                           {FrameTemplates::SendId, id}, {FrameTemplates::SendFieldsEnabled, true},
                                           {FrameTemplates::SendBindings, QJsonArray{
                                               QJsonObject{{"field", "rpm"}, {"inputId", "rpmInput"}}}}}}},
            QJsonObject{{"id", "rpmInput"}, {"type", "LineEdit"},
                {"properties", QJsonObject{{"inputOnly", false}, {"commandMode", "HEX"}, {"commandTargetId", "device"},
                                           {FrameTemplates::SendId, id}, {FrameTemplates::SendValueField, "rpm"}}}},
            QJsonObject{{"id", "speedReadout"}, {"type", "Label"},
                {"binding", QJsonObject{{"parseMode", ParseMode::KeyValue}, {"sourceCardId", "device"}, {"key", "rpm"}}}},
            QJsonObject{{"id", "trace"}, {"type", "RealtimeChart"},
                {"properties", QJsonObject{{"curveCount", 2}, {"_curve_0_name", "RPM"}, {"_curve_0_key", "rpm"},
                                           {"_curve_0_expression", "value / 100"}, {"_curve_1_name", "CMD"}, {"_curve_1_key", "cmd"},
                                           {"_curve_7_parseMode", ParseMode::KeyValue}, {"_curve_7_key", "rpm"}}}},
            QJsonObject{{"id", "records"}, {"type", "DataTable"},
                {"properties", QJsonObject{{"sourceCardId", "device"}, {"recordMode", "逐行记录"}, {"recordSource", "messages"},
                                           {"recordColumns", QJsonArray{QJsonObject{{"field", "rpm"}, {"label", "RPM"}}}}}}}};
        QJsonObject point{{"name", "Speed"},
            {"binding", QJsonObject{{"parseMode", ParseMode::HexBytes}, {"sourceCardId", "device"},
                                   {"byteOffset", 2}, {"frameHeader", "AA55"}}}};
        QWidget host;
        host.resize(1200, 875);
        host.show();
        QTimer::singleShot(0, &host, [&] {
            auto *source = host.findChild<AppDialog *>("projectDataSourceDialog");
            QVERIFY(source);
            auto close = qScopeGuard([source] { if (source->isVisible()) source->reject(); });
            const auto *editor = source->findChild<PropertyEditor *>();
            QVERIFY(editor);
            auto *templates = source->findChild<QPushButton *>("editSourceReceiveTemplates");
            QVERIFY(templates && templates->isVisible());
            QTimer::singleShot(0, &host, [&] {
                auto *choice = host.findChild<AppDialog *>("receiveTemplateGroupDialog");
                QVERIFY(choice);
                auto closeChoice = qScopeGuard([choice] { choice->reject(); });
                auto *check = choice->findChild<QCheckBox *>(QStringLiteral("receiveTemplateCheck_") + id);
                QVERIFY(check);
                check->setChecked(true);
                QTimer::singleShot(0, &host, [&] {
                    auto *impact = host.findChild<AppDialog *>("frameTemplateImpact");
                    QVERIFY(impact);
                    impact->yesButton()->click();
                });
                choice->yesButton()->click();
                closeChoice.dismiss();
            });
            templates->click();
            auto *fields = source->findChild<FluentQt::ComboBox *>("bindingTemplateField");
            QVERIFY(fields && fields->isVisible());
            const int field = fields->findData(QStringLiteral("rpm"));
            QVERIFY(field > 0);
            QVERIFY(!labelShown(source, QStringLiteral("字节偏移")));
            QVERIFY(!labelShown(source, QStringLiteral("转换表达式")));
            fields->setCurrentIndex(field);
            const auto templateFormShown = [editor] {
                auto *content = editor->widget();
                const auto *picker = content->findChild<FluentQt::ComboBox *>("bindingTemplateField");
                return picker && picker->isVisible() && picker->currentData().toString() == QLatin1String("rpm") &&
                       !labelShown(content, QStringLiteral("字节偏移")) && !labelShown(content, QStringLiteral("绑定字段"));
            };
            QTRY_VERIFY(templateFormShown());
            source->yesButton()->click();
            close.dismiss();
        });
        QVERIFY(editDevicePoint(&project, &point, &host, false));
        QCOMPARE(point.value("binding").toObject().value("key").toString(), QStringLiteral("rpm"));
        QCOMPARE(point.value("binding").toObject().value("byteOffset").toInt(), 2);
        QCOMPARE(point.value("binding").toObject().value("frameHeader").toString(), QStringLiteral("AA55"));
        project.logic.insert("points", QJsonArray{point});
        const auto receiver = project.widgets.first().toObject().value("properties").toObject();
        QCOMPARE(receiver.value(FrameTemplates::ReceiveId).toString(), id);
        QVERIFY(receiver.value("parserConfig").isUndefined());

        QTimer::singleShot(0, &host, [&] {
            auto *source = host.findChild<AppDialog *>("projectDataSourceDialog");
            QVERIFY(source);
            auto closeSource = qScopeGuard([source] { source->reject(); });
            auto *openFrame = source->findChild<QPushButton *>("editReceiveFrameFormat");
            QVERIFY(openFrame && openFrame->isVisible());
            QTimer::singleShot(0, &host, [&] {
                auto *frame = host.findChild<AppDialog *>("receiveFrameFormatEditor");
                QVERIFY(frame);
                auto closeFrame = qScopeGuard([frame] { frame->reject(); });
                auto *fields = frame->findChild<QTableWidget *>("receiveFrameFields");
                auto *edit = buttonFor(frame, QStringLiteral("编辑所选"));
                QVERIFY(fields && edit);
                const auto rows = fields->findItems(QStringLiteral("rpm"), Qt::MatchExactly);
                QVERIFY(!rows.isEmpty());
                const int rpmRow = rows.first()->row();
                QCOMPARE(fields->currentRow(), rpmRow);
                QVERIFY(rows.first()->font().bold());
                auto *sample = frame->findChild<QPlainTextEdit *>("receiveFrameSample");
                QVERIFY(sample);
                sample->setPlainText(QStringLiteral("AA 55 03 01 DC 05 D8 0D"));
                QVERIFY(labelShown(frame, QStringLiteral("当前取值 · rpm · 样例值 1500")));
                const auto elsewhere = fields->findItems(QStringLiteral("cmd"), Qt::MatchExactly);
                QVERIFY(!elsewhere.isEmpty());
                fields->setCurrentCell(elsewhere.first()->row(), 0);
                QVERIFY(labelShown(frame, QStringLiteral("当前取值 · rpm · 样例值 1500")));
                auto *tabs = frame->findChild<FluentQt::SegmentedWidget *>("receiveEditorTabs");
                QVERIFY(tabs);
                tabs->setCurrentItem(QStringLiteral("format"));
                fields->setCurrentCell(rpmRow, 0);
                QTimer::singleShot(0, &host, [&] {
                    auto *field = host.findChild<AppDialog *>("receiveFieldEditor");
                    QVERIFY(field);
                    auto closeField = qScopeGuard([field] { field->reject(); });
                    auto *name = field->findChild<QLineEdit *>("receiveFieldName");
                    QVERIFY(name);
                    name->setText(QStringLiteral("speed"));
                    field->yesButton()->click();
                    closeField.dismiss();
                });
                edit->click();
                QTimer::singleShot(0, &host, [&] {
                    auto *impact = host.findChild<AppDialog *>("frameTemplateImpact");
                    QVERIFY(impact);
                    auto closeImpact = qScopeGuard([impact] { impact->reject(); });
                    auto *references = impact->findChild<QTableWidget *>("frameTemplateImpactReferences");
                    QVERIFY(references);
                    QVERIFY(!references->findItems(QStringLiteral("读取字段 rpm → speed"), Qt::MatchExactly).isEmpty());
                    QVERIFY(!references->findItems(QStringLiteral("写入字段 rpm → speed"), Qt::MatchExactly).isEmpty());
                    QVERIFY(!references->findItems(QStringLiteral("speedReadout"), Qt::MatchExactly).isEmpty());
                    QVERIFY(!references->findItems(QStringLiteral("trace · RPM (1)"), Qt::MatchExactly).isEmpty());
                    QVERIFY(!references->findItems(QStringLiteral("records"), Qt::MatchExactly).isEmpty());
                    QVERIFY(references->findItems(QStringLiteral("trace · CMD (2)"), Qt::MatchExactly).isEmpty());
                    impact->yesButton()->click();
                    closeImpact.dismiss();
                });
                frame->yesButton()->click();
                closeFrame.dismiss();
            });
            openFrame->click();
            source->yesButton()->click();
            closeSource.dismiss();
        });
        QVERIFY(editDevicePoint(&project, &point, &host, false));
        QCOMPARE(point.value("binding").toObject().value("key").toString(), QStringLiteral("speed"));
        QCOMPARE(project.logic.value("points").toArray().first().toObject().value("binding").toObject().value("key").toString(),
                 QStringLiteral("speed"));
        entry = *FrameTemplates::find(project.frameTemplates, id);
        QVERIFY(entry.sample.isEmpty());
        Project loaded;
        QString error;
        const auto saved = project.toJson();
        QCOMPARE(saved.value("version"), QJsonValue("1.3"));
        QVERIFY2(Project::fromJson(saved, &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.toJson(), saved);
        QCOMPARE(loaded.widgets.at(3).toObject().value("binding").toObject().value("key").toString(), QStringLiteral("speed"));
        const auto curves = loaded.widgets.at(4).toObject().value("properties").toObject();
        QCOMPARE(curves.value("_curve_0_key").toString(), QStringLiteral("speed"));
        QCOMPARE(curves.value("_curve_1_key").toString(), QStringLiteral("cmd"));
        QCOMPARE(curves.value("_curve_7_key").toString(), QStringLiteral("speed"));
        const auto recordColumns = loaded.widgets.at(5).toObject().value("properties").toObject().value("recordColumns");
        QCOMPARE(recordColumns.toArray().first().toObject(), (QJsonObject{{"field", "speed"}, {"label", "RPM"}}));

        for (const auto &header : {QStringLiteral("AA55"), QStringLiteral("CC33")}) {
            const bool first = header == QLatin1String("AA55");
            entry.format.settings.insert("header", header);
            entry.format.settings.insert(FrameTemplates::SendDefaults, QJsonObject{{"cmd", first ? "1" : "2"}});
            loaded.frameTemplates.insert(id, entry.toJson());
            const QByteArray expected = QByteArray::fromHex(header.toLatin1() + (first ? "0301DC05D80D" : "0302FC08F60D"));
            RuntimeSurface surface;
            surface.build(loaded);
            surface.show();
            auto *card = surface.findChild<QWidget *>("TcpConfigCard");
            QVERIFY(card);
            auto *connect = buttonFor(card, QStringLiteral("连接"));
            auto *send = buttonFor(&surface, QStringLiteral("Send"));
            QVERIFY(connect && send);
            auto *input = surface.findChild<QWidget *>("LineEdit");
            QVERIFY(input);
            auto *edit = input->findChild<QLineEdit *>();
            QVERIFY(edit);
            connect->click();
            QTRY_VERIFY(server.hasPendingConnections());
            auto peer = std::unique_ptr<QTcpSocket>(server.nextPendingConnection());
            QTRY_VERIFY(surface.context()->hub()->isConnected());
            QSignalSpy notices(surface.context(), &RuntimeContext::notificationRequested);
            edit->clear();
            send->click();
            QTest::keyClick(edit, Qt::Key_Return);
            QCOMPARE(notices.size(), 2);
            for (const auto &notice : notices)
                QVERIFY(notice.at(1).toString().contains(QStringLiteral("必填字段 speed")));
            QVERIFY(!peer->bytesAvailable());
            edit->setText(first ? QStringLiteral("1500") : QStringLiteral("2300"));
            send->click();
            QTRY_COMPARE(peer->bytesAvailable(), expected.size());
            QCOMPARE(peer->readAll(), expected);
            QCOMPARE(peer->write(expected), expected.size());
            QTRY_COMPARE(surface.context()->projectData()->values().value("Speed").toInt(), first ? 1500 : 2300);
            auto *records = surface.findChild<QWidget *>(QStringLiteral("DataTable"))->findChild<QTableWidget *>();
            QVERIFY(records && records->rowCount() == 1);
            QCOMPARE(records->item(0, 0)->text(), first ? QStringLiteral("1500") : QStringLiteral("2300"));
            const auto *display = surface.findChild<QWidget *>("Label");
            QVERIFY(display);
            const auto *readout = display->findChild<QLabel *>();
            QVERIFY(readout);
            QTRY_COMPARE(readout->text(), QString::number(first ? 1500 : 2300));
            auto *chart = dynamic_cast<RealtimeChartWidget *>(surface.findChild<HmiWidget *>("RealtimeChart"));
            QVERIFY(chart);
            QTemporaryDir history;
            QFile samples(history.filePath("curves.json"));
            chart->saveHistory(samples.fileName());
            QVERIFY(samples.open(QIODevice::ReadOnly));
            const auto recorded = QJsonDocument::fromJson(samples.readAll()).object();
            QCOMPARE(recorded.value("RPM").toArray(), QJsonArray{first ? 15.0 : 23.0});
            QCOMPARE(recorded.value("CMD").toArray(), QJsonArray{first ? 1.0 : 2.0});
            Q_EMIT edit->returnPressed();
            QTRY_COMPARE(peer->bytesAvailable(), expected.size());
            QCOMPARE(peer->readAll(), expected);
        }
    }

    void changingReceiveTemplatesKeepsSameNamedFieldsSeparate()
    {
        Project before;
        const auto entry = [](const QString &name, const QString &header) {
            const QJsonArray fields{QJsonValue(QJsonArray{"value", 0, 1, "uint8"})};
            return FrameTemplates::Entry{name, {QStringLiteral("hex"),
                {{"header", header}, {"fixedLength", 2}, {"fields", fields}}}, {}};
        };
        before.frameTemplates = {{"speed", entry("Speed", "AA").toJson()}, {"heat", entry("Heat", "BB").toJson()}};
        before.widgets = {tcpCard("device", 60, {{FrameTemplates::ReceiveIds, QJsonArray{"speed", "heat"}}})};
        const auto point = [](const QString &name, const QString &field) {
            return QJsonObject{{"name", name}, {"binding", QJsonObject{{"sourceCardId", "device"}, {"key", field}}}};
        };
        before.logic.insert("points", QJsonArray{point("Speed", "speed.value"), point("Heat", "heat.value")});
        const auto switchTo = [](Project project, const QString &id) {
            auto card = project.widgets.first().toObject();
            auto props = card.value("properties").toObject();
            props.remove(FrameTemplates::ReceiveIds);
            props.insert(FrameTemplates::ReceiveId, id);
            card.insert("properties", props);
            project.widgets[0] = card;
            return project;
        };
        const auto key = [](const Project &project, int row) {
            return project.logic.value("points").toArray().at(row).toObject().value("binding").toObject().value("key");
        };
        QWidget host;
        host.resize(980, 680);
        host.show();
        const auto respond = [&](bool accept) {
            QTimer::singleShot(0, &host, [&, accept] {
                auto *review = host.findChild<AppDialog *>("frameTemplateImpact");
                QVERIFY(review);
                (accept ? review->yesButton() : review->cancelButton())->click();
            });
        };
        auto speed = switchTo(before, "speed");
        respond(false);
        QVERIFY(!confirmReceiveTemplateSelection(before, speed, "device", &host));
        QCOMPARE(speed.logic, before.logic);
        respond(true);
        QVERIFY(confirmReceiveTemplateSelection(before, speed, "device", &host));
        QCOMPARE(key(speed, 0), QJsonValue("value"));
        QCOMPARE(key(speed, 1), QJsonValue("heat.value"));

        auto heat = switchTo(speed, "heat");
        respond(true);
        QVERIFY(confirmReceiveTemplateSelection(speed, heat, "device", &host));
        QCOMPARE(key(heat, 0), QJsonValue("speed.value"));
        QCOMPARE(key(heat, 1), QJsonValue("value"));
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
        emit raw.hub()->unitReceived(QByteArray::fromHex("AA0155AA0055"), {}, DataSource{"A", 1});
        QCOMPARE(rawRaised.size(), 1);
        QVERIFY(!raw.projectData()->alarms().first().active);
        QVERIFY(raw.projectData()->alarms().first().outstanding());

        project.logic = {
            {"points", QJsonArray{QJsonObject{{"name", "raw"},
                {"binding", QJsonObject{{"parseMode", ParseMode::HexBytes}, {"byteOffset", 2}, {"dataType", "uint8"}}}},
                QJsonObject{{"name", "field"}, {"binding", QJsonObject{{"key", "decoded"}}}}}},
            {"alarms", QJsonArray{QJsonObject{{"id", "mismatch"}, {"condition", "T[\"raw\"] != T[\"field\"]"}}}}};
        RuntimeContext mixed;
        FakeLink framed;
        mixed.configureData(project);
        mixed.projectData()->configure(project.logic);
        QVERIFY(framed.setParser("hex", {{"header", "AA55"}, {"footer", ""}, {"fixedLength", 4},
                                        {"fields", QJsonArray{QJsonValue(QJsonArray{"decoded", 1, 1, "uint8"})}}}));
        mixed.hub()->addLink(&framed, "A");
        mixed.setActive(true);
        QSignalSpy mixedRaised(mixed.projectData(), &ProjectData::alarmRaised);
        framed.inject(QByteArray::fromHex("AA550101AA550202"));
        QCOMPARE(mixed.projectData()->values().value("raw").toInt(), 2);
        QCOMPARE(mixed.projectData()->values().value("field").toInt(), 2);
        QCOMPARE(mixedRaised.size(), 0);

        project.logic.insert("points", QJsonArray{QJsonObject{{"name", "raw"},
            {"binding", QJsonObject{{"parseMode", ParseMode::HexBytes}, {"frameHeader", "763D"},
                {"frameTail", "0A"}, {"dataType", "uint8"}, {"expression", "value - 48"}}}},
            QJsonObject{{"name", "field"}, {"binding", QJsonObject{{"key", "v"}}}}});
        RuntimeContext lines;
        FakeLink lineLink;
        lines.configureData(project);
        lines.projectData()->configure(project.logic);
        lines.hub()->addLink(&lineLink, "A");
        lines.setActive(true);
        QSignalSpy lineAlarms(lines.projectData(), &ProjectData::alarmRaised);
        lineLink.inject("v=0\n");
        lineLink.inject("v=1\nv=2\nv=");
        lineLink.inject("3\nv=4\n");
        QCOMPARE(lines.projectData()->values().value("raw").toInt(), 4);
        QCOMPARE(lines.projectData()->values().value("field").toInt(), 4);
        QCOMPARE(lineAlarms.size(), 0);
    }

    void projectPointsKeepInvalidValuesOutOfRecordsAndCommands()
    {
        Project project;
        project.widgets = {QJsonObject{{"id", "A"}, {"type", "TcpConfigCard"}},
                           QJsonObject{{"id", "B"}, {"type", "TcpConfigCard"}},
                           QJsonObject{{"id", "PLC"}, {"type", "S7ConfigCard"}}};
        const auto receive = [](const char *key) {
            return QJsonObject{{"parseMode", "键值匹配"}, {"sourceCardId", "A"}, {"key", key}};
        };
        auto temperature = receive("temp");
        temperature.insert("expression", "value / (100 - value)");
        project.logic = {{"points", QJsonArray{QJsonObject{{"name", "rpm"}, {"binding", receive("rpm")}},
                                                QJsonObject{{"name", "temp"}, {"binding", temperature}},
                                                QJsonObject{{"name", "Kp"}, {"initialValue", 1.25}}}},
                         {"alarms", QJsonArray{QJsonObject{{"id", "high"}, {"condition", "T[\"temp\"] > 50"}}}}};
        RuntimeContext context;
        FakeLink a;
        context.configureData(project);
        context.projectData()->configure(project.logic);
        context.hub()->addLink(&a, "A");
        context.setActive(true);
        auto table = std::unique_ptr<HmiWidget>(createHmiWidget("DataTable", {{"recordMode", "逐行记录"},
            {"recordSource", "points"}, {"recordTriggerPoint", "rpm"},
            {"recordColumns", QJsonArray{QJsonObject{{"field", "temp"}}, QJsonObject{{"field", "Kp"}}}}}));
        table->attach(&context);
        DataSource first{"A", nextDataStream(), "192.0.2.1:1000"};
        first.device = "192.0.2.1";
        DataSource second{"A", nextDataStream(), "192.0.2.2:1000"};
        second.device = "192.0.2.2";
        DataSource survivor{"A", nextDataStream(), "192.0.2.1:1001"};
        survivor.device = first.device;
        const QString pointId = QStringLiteral("@data:temp");
        auto *display = table->findChild<QTableWidget *>();
        QVERIFY(display);
        auto *data = context.projectData();
        emit context.hub()->unitReceived({}, Message{{"rpm", 1500}, {"temp", 20}}, first);
        QCOMPARE(data->sample("temp").quality, DataQuality::Current);
        QCOMPARE(display->rowCount(), 1);
        QCOMPARE(display->item(0, 0)->text(), QStringLiteral("0.25"));
        QCOMPARE(display->item(0, 1)->text(), QStringLiteral("1.25"));
        QVERIFY(context.inputValue(pointId));
        emit context.hub()->unitReceived({}, Message{{"rpm", 1400}, {"temp", 100}}, first);
        QCOMPARE(data->sample("temp").quality, DataQuality::ConversionFailed);
        QCOMPARE(data->sample("temp").value.toDouble(), 0.25);
        QVERIFY(!data->alarms().first().known);
        QVERIFY(!context.inputValue(pointId));
        QCOMPARE(display->rowCount(), 2);
        QVERIFY(display->item(1, 0)->text().isEmpty());
        emit context.hub()->unitReceived({}, Message{{"rpm", 900}}, second);
        QCOMPARE(data->sample("rpm").quality, DataQuality::Ambiguous);
        QVERIFY(!data->values().value("rpm").isValid());
        QCOMPARE(display->rowCount(), 2);
        emit context.hub()->streamClosed(second);
        QCOMPARE(data->values().value("rpm").toInt(), 1400);
        emit context.hub()->unitReceived({}, Message{{"rpm", 900}}, second);
        emit context.hub()->streamClosed(first);
        QCOMPARE(data->sample("rpm").quality, DataQuality::Old);
        QCOMPARE(data->sample("rpm").value.toInt(), 1400);
        emit context.hub()->unitReceived({}, Message{{"rpm", 2100}}, second);
        QVERIFY(!context.inputValue(QStringLiteral("@data:rpm")));
        emit context.hub()->streamClosed(second);
        emit context.hub()->unitReceived({}, Message{{"rpm", 2200}}, survivor);
        QCOMPARE(data->sample("rpm").quality, DataQuality::Old);
        emit context.hub()->streamClosed(survivor);
        DataSource firstReconnected{"A", nextDataStream(), "192.0.2.1:2000"};
        firstReconnected.device = first.device;
        emit context.hub()->unitReceived({}, Message{{"rpm", 1300}}, firstReconnected);
        QCOMPARE(data->sample("rpm").quality, DataQuality::Current);
        QCOMPARE(data->sample("rpm").value.toInt(), 1300);
        auto input = std::unique_ptr<HmiWidget>(createHmiWidget("LineEdit", {{"text", "2.5"}, {"inputOnly", true}}));
        context.registerWidget("@data:Kp", input.get());
        QVERIFY(!context.inputValue(QStringLiteral("@data:Kp")));
        QCOMPARE(context.inputValue(pointInputId("Kp"))->value.toDouble(), 1.25);
        QCOMPARE(context.inputValue(widgetInputId("@data:Kp"))->text(), QStringLiteral("2.5"));
        auto command = CommandEncoder::modbusFromProperties({{"commandTargetId", "PLC"}, {"mbFunc", "06 写单寄存器"},
                                                              {"mbAddress", 100}, {"mbValue", "7"}});
        QVERIFY(command.command);
        QVERIFY(!context.send(command));
        QVERIFY(a.sent.isEmpty());
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
        QCoreApplication::setOrganizationName(QStringLiteral("FluentHMI Tests"));
        Q_INIT_RESOURCE(fluentqtwidgets);
    }


    void routesKeepBindingsFramesAndCommandsSeparate()
    {
        RuntimeContext context;
        context.setActive(true);
        auto a = std::unique_ptr<HmiWidget>(createHmiWidget("Label", {}));
        auto b = std::unique_ptr<HmiWidget>(createHmiWidget("Label", {}));
        auto raw = std::unique_ptr<HmiWidget>(createHmiWidget("Label", {{"text", ""}}));
        context.registerWidget("a", a.get()); context.registerWidget("b", b.get()); context.registerWidget("raw", raw.get());
        auto binding = [](const QString &id, const QJsonObject &config) {
            return *BindingEntry::fromWidget({{"id", id}, {"type", "Label"}, {"binding", config}});
        };
        context.addBinding(binding("a", {{"key","value"},{"sourceCardId","A"}}));
        context.addBinding(binding("b", {{"key","value"},{"sourceCardId","B"}}));
        context.addBinding(binding("raw", {{"parseMode",ParseMode::HexBytes},{"frameHeader","AA"},{"frameTail","55"},{"dataType","uint16"}}));
        FakeLink first, second;
        context.hub()->addLink(&first,"A"); context.hub()->addLink(&second,"B");
        QVERIFY(!context.send(CommandEncoder::encode({}, "AMBIGUOUS")));
        QVERIFY(first.sent.isEmpty() && second.sent.isEmpty());
        first.inject("value=11\n"); second.inject("value=22\n");
        QCOMPARE(a->findChild<QLabel *>()->text(), QStringLiteral("11"));
        QCOMPARE(b->findChild<QLabel *>()->text(), QStringLiteral("22"));
        first.inject(QByteArray::fromHex("AA01"));
        second.inject(QByteArray::fromHex("0255"));
        QVERIFY(raw->findChild<QLabel *>()->text().isEmpty());
        second.setConnected(false); // must not discard A's half frame
        first.inject(QByteArray::fromHex("0255"));
        QCOMPARE(raw->findChild<QLabel *>()->text(), QStringLiteral("258"));
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
        QVERIFY(context.send(CommandEncoder::encode({{"commandTargetId", DataRoute::Broadcast}}, "Broadcast")));
        QCOMPARE(first.sent.last(), QByteArray("Broadcast\r\n"));
        QCOMPARE(second.sent.last(), first.sent.last());
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
        QVERIFY(!context.projectData()->values().value("latest").isValid());
        QCOMPARE(context.projectData()->sample("latest").quality, DataQuality::Ambiguous);

        now = 500;
        context.modbusPoller()->poll();
        QCOMPARE(a.sent.size(), 3);
        const auto request = a.sent.last();
        QVERIFY(QMetaObject::invokeMethod(a.modbusMaster()->findChild<QTimer *>(), "timeout"));
        const auto values = context.projectData()->values();
        QVERIFY(!values.value("a").isValid());
        QCOMPARE(values.value("otherBlock").toInt(), 2);
        QCOMPARE(values.value("b").toInt(), 3);
        QVERIFY(!values.value("latest").isValid());
        a.inject(plcReply(request, {{100, 99}}));
        QCOMPARE(context.projectData()->values(), values); // A late TCP reply cannot revive stale values.
    }

    void switchingConnectionClearsItsOldEndpointWithoutBroadeningTarget()
    {
        Project project;
        project.widgets = {
            QJsonObject{{"id", "tcp"}, {"type", "TcpConfigCard"}},
            QJsonObject{{"id", "server"}, {"type", "TcpConfigCard"}, {"properties", QJsonObject{{"defaultMode", "服务端"}}}},
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
        target->setCurrentIndex(target->findData(QStringLiteral("tcp")));
        source->setCurrentIndex(source->findData(QStringLiteral("tcp")));
        QVERIFY(destination->isHidden() && endpoint->isHidden());
        target->setCurrentIndex(target->findData(QStringLiteral("server")));
        source->setCurrentIndex(source->findData(QStringLiteral("server")));
        QVERIFY(!destination->isHidden() && !endpoint->isHidden());
        target->setCurrentIndex(target->findData(QStringLiteral("uart")));
        source->setCurrentIndex(source->findData(QStringLiteral("uart")));
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

        auto *parseMode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("解析模式")));
        QVERIFY(parseMode);
        parseMode->setCurrentIndex(parseMode->findData(ParseMode::JsonPath));
        QVERIFY(item->binding().value("jsonPath").toString().isEmpty());
        QCOMPARE(item->binding().value("sourceCardId").toString(), QStringLiteral("uart"));
        QVERIFY(!BindingEntry::fromWidget(item->toJson()).has_value());
        editor.showItem(scene.findItem(QStringLiteral("uart")));
        editor.showItem(item);
        parseMode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("解析模式")));
        QCOMPARE(parseMode->currentData().toString(), ParseMode::JsonPath);
        parseMode->setCurrentIndex(parseMode->findData(ParseMode::KeyValue));
        QCOMPARE(BindingEntry::fromWidget(item->toJson())->key, QStringLiteral("state"));
    }

    void explicitlyRoutedModbusControlDoesNotWriteItsLocalBinding()
    {
        RuntimeContext context;
        ModbusSlave local({1, 0, true}, ModbusSlaveStore::fromProperties({{"slaveHrCount", 10}}));
        context.hub()->addModbusSlave(&local, "local");
        FakeLink remote;
        QVERIFY(remote.setParser(QStringLiteral("modbus"), {}));
        context.hub()->addLink(&remote, "remote");
        const QJsonObject props{{"commandMode", "Modbus"}, {"sendOnChange", true},
                                {"commandTargetId", "remote"}, {"mbAddress", 2}};
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

        const QJsonObject s7Target{{"commandMode", "S7"}, {"sendOnChange", true}, {"commandTargetId", "offline-plc"},
            {"s7Command", QJsonObject{{"address", "DB1.DBW0"}, {"dataType", "INT"}}}};
        input->setProperties(s7Target);
        spin->setValue(9);
        QCOMPARE(local.store().value("hr", 2), quint16(8)); // A named receive source is never an S7 destination.
        QCOMPARE(changes.size(), 1);

        Project project;
        const QJsonObject readProps{{"commandMode", "Modbus"}, {"mbFunc", "03 读保持寄存器"}, {"mbValue", "1"}};
        project.widgets = {tcpCard("remote", 60, {{"defaultParser", "modbus"}}),
            tcpCard("local", 280, {{"defaultParser", "modbus_tcp"}, {"modbusRole", "从站"}}),
            QJsonObject{{"id", "read"}, {"type", "PushButton"}, {"properties", readProps}}};
        project.logic = {{"points", QJsonArray{QJsonObject{{"name", "gain"},
            {"binding", QJsonObject{{"parseMode", "Modbus"}, {"sourceCardId", "local"}, {"mbRegIndex", 0}}}}}}};
        QString error;
        QVERIFY2(Project::validate(project.toJson(), &error), qPrintable(error));
        QCOMPARE(previewCommandTarget(readProps, "PushButton", project).route.connectionId, QStringLiteral("remote"));
        const auto otherSlave = resolveProjectRoute(project.widgets, project.parserType(), ModbusDestination{2, 0x06},
                                                    {}, RoutePurpose::Send);
        QVERIFY(otherSlave);
        QCOMPARE(otherSlave->connectionId, QStringLiteral("remote"));
        auto unbound = std::unique_ptr<HmiWidget>(createHmiWidget("SpinBox", {}));
        context.registerWidget(pointInputId("gain"), unbound.get());
        context.configureData(project);
        unbound->attach(&context);
        auto *unboundSpin = unbound->findChild<QSpinBox *>();
        QVERIFY(unboundSpin);
        unboundSpin->setValue(12);
        QCOMPARE(local.store().value("hr", 0), quint16(0));
        QCOMPARE(changes.size(), 1);

        auto propsForRoute = readProps;
        QVERIFY(remote.setParser(QStringLiteral("modbus"), {}));
        for (const auto &route : {QString(), DataRoute::Broadcast}) {
            propsForRoute.insert("commandTargetId", route);
            const auto read = CommandEncoder::modbusFromProperties(propsForRoute);
            QVERIFY(read.command);
            QVERIFY(context.send(read));
            QCOMPARE(remote.sent.last(), CommandEncoder::rtuFrame(read.command->modbus));
            const auto response = QByteArray::fromHex("0103020000");
            remote.inject(response + modbusCrc(response));
        }
        QCOMPARE(remote.sent.size(), 3);
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
            QSemaphore completeOpen;
            QSemaphore completeClose;
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
                if (++calls->open == 1 && !calls->completeOpen.tryAcquire(1, 3000))
                    return false;
                up = true;
                return true;
            }
            void close() override
            {
                if (up) {
                    if (calls->close.load() == 0)
                        calls->completeClose.tryAcquire(1, 3000);
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
        heartbeat.setTimerType(Qt::PreciseTimer);
        heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++ticks; });
        heartbeat.start();
        QVERIFY(session.open({}, nullptr));
        QTRY_COMPARE(calls->open.load(), 1);
        QTRY_VERIFY(ticks >= 3);
        QCOMPARE(opened.size(), 0); // UI ticked while the driver open call was still running
        calls->completeOpen.release();
        QTRY_COMPARE(opened.size(), 1);
        QVERIFY(session.isOpen());

        QVERIFY(session.send({}, nullptr));
        QVERIFY(session.send({}, nullptr));
        QTRY_COMPARE(calls->send.load(), 1);
        const int before = ticks;
        session.close();
        calls->completeSend.release();
        QTRY_VERIFY(ticks - before >= 3);
        QCOMPARE(stopped.size(), 0); // UI ticked while the driver close call was still running
        calls->completeClose.release();
        QTRY_COMPARE(stopped.size(), 1);
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
        QVERIFY(!view->model()->index(99, 6).data(Qt::ToolTipRole).isValid());
        for (int row = 0; row < 100; ++row) {
            QCOMPARE(view->model()->index(row, 0).data().toString(), QString::number(row + 51));
            QCOMPARE(view->model()->index(row, 0).data(Qt::UserRole).toULongLong(), quint64(row + 51));
        }
        auto *scroll = view->verticalScrollBar();
        QTRY_VERIFY(scroll->maximum() > 0);
        QTRY_COMPARE(scroll->value(), scroll->maximum());
        scroll->setValue(scroll->maximum() / 2);
        const auto visibleSequence = [&] { return view->model()->index(view->rowAt(0), 0).data().toString(); };
        const auto reading = visibleSequence();
        QVERIFY(!reading.isEmpty());
        CanFrame next = frames.last();
        next.data = QByteArray::fromHex("AA");
        next.timestampMs++;
        emit context.hub()->canFrames(nullptr, {next}, false);
        QTRY_COMPARE(view->model()->index(99, 6).data().toString(), QStringLiteral("AA"));
        QCOMPARE(view->model()->index(0, 0).data().toString(), QStringLiteral("52"));
        QCOMPARE(view->model()->index(99, 0).data().toString(), QStringLiteral("151"));
        QCOMPARE(visibleSequence(), reading);
        QVERIFY(follow->isChecked());
        scroll->setValue(scroll->maximum());

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
        QTRY_COMPARE(scroll->value(), scroll->maximum());
        follow->setChecked(false);
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
        QVERIFY(custom->toolTip().isEmpty());
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
                        {"properties", QJsonObject{{"_curve_0_parseMode", "S7"}, {"_curve_0_sourceCardId", "plc"}, {"_curve_0_s7", bit}}}},
            QJsonObject{{"type", "DigitalTimingChart"}, {"id", "timing"},
                        {"properties", QJsonObject{{"timingSource", "逐路绑定"}, {"curveCount", 32},
                                                  {"_curve_31_parseMode", "S7"}, {"_curve_31_sourceCardId", "plc"}, {"_curve_31_s7", bit}}}}};
        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets(group, &error), qPrintable(error));
        const auto copied = scene.paste(group, nullptr, 1);
        QCOMPARE(copied.size(), 4);
        const QString newPlc = copied.at(0)->widgetId();
        QVERIFY(newPlc != QLatin1String("plc"));
        QCOMPARE(copied.at(1)->binding().value("sourceCardId").toString(), newPlc);
        QCOMPARE(copied.at(1)->propertyValue("commandTargetId").toString(), newPlc);
        QCOMPARE(copied.at(2)->propertyValue("_curve_0_sourceCardId").toString(), newPlc);
        QCOMPARE(copied.at(3)->propertyValue("_curve_31_sourceCardId").toString(), newPlc);
        const auto controlOnly = scene.paste({group.at(1)}, nullptr, 1);
        QCOMPARE(controlOnly.size(), 1);
        QCOMPARE(controlOnly.first()->propertyValue("commandTargetId").toString(), QStringLiteral("plc"));
        QVERIFY(scene.renameItem(scene.findItem("plc"), "renamed", &error));
        QCOMPARE(scene.findItem("switch")->binding().value("sourceCardId").toString(), QStringLiteral("renamed"));
        QCOMPARE(controlOnly.first()->propertyValue("commandTargetId").toString(), QStringLiteral("renamed"));
        QCOMPARE(scene.findItem("timing")->propertyValue("_curve_31_sourceCardId").toString(), QStringLiteral("renamed"));
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
        auto *current = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("当前编辑曲线")));
        QVERIFY(current);
        QCOMPARE(current->count(), 2);
        current->setCurrentIndex(1);
        QApplication::processEvents();
        expression = qobject_cast<QLineEdit *>(fieldFor(&editor, QStringLiteral("转换表达式")));
        QVERIFY(expression);
        QCOMPARE(expression->text(), QStringLiteral("value * 3"));
        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("解析模式")));
        QVERIFY(mode);
        QCOMPARE(mode->currentData().toString(), ParseMode::CanSignal);
        for (int index = 0; index < mode->count(); ++index) {
            mode->setCurrentIndex(index);
            QCOMPARE(expression->isHidden(), mode->currentData().toString() == ParseMode::None);
        }
        mode->setCurrentIndex(mode->findData(ParseMode::KeyValue));
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
        auto *timing = dynamic_cast<DigitalTimingChartWidget *>(scene->findItem("timing")->widget());
        QVERIFY(word && timing);
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
                timing = dynamic_cast<DigitalTimingChartWidget *>(widget);
            if (widget->typeName() == "RotaryEncoder")
                counter = item;
            if (widget->typeName() == "BmsCellMatrix")
                cells = item;
            if (widget->typeName() == "DcPowerSupply")
                supply = item;
        }
        QVERIFY(word && timing && counter && cells && supply);
        auto *readout = timing->findChild<QLabel *>(QStringLiteral("timingReadout"));
        auto *follow = timing->findChild<QPushButton *>(QStringLiteral("timingFollow"));
        auto *plot = timing->findChild<FluentQt::RealtimePlotWidget *>(QStringLiteral("digitalTimingPlot"));
        auto *window = timing->findChild<FluentQt::ComboBox *>(QStringLiteral("timingWindow"));
        QVERIFY(readout && follow && plot && window);
        QCOMPARE(readout->text(), QStringLiteral("待数据"));
        QCOMPARE(plot->xMaximum() - plot->xMinimum(), 10.0);
        window->setCurrentIndex(window->findData(5));
        QCOMPARE(plot->xMaximum() - plot->xMinimum(), 5.0);
        emit runtime.context()->hub()->unitReceived({}, Message{{"word", 65535LL}});
        QCOMPARE(word->readingText(), QStringLiteral("0xFFFF"));
        QTRY_COMPARE(readout->text(), QStringLiteral("0xFFFF"));
        emit runtime.context()->hub()->unitReceived({}, Message{{"word", QStringLiteral("bad")}});
        QTRY_COMPARE(readout->text(), QStringLiteral("数据异常"));
        emit runtime.context()->hub()->unitReceived({}, Message{{"word", 1LL}});
        QTRY_COMPARE(readout->text(), QStringLiteral("0x0001"));
        follow->click();
        QCOMPARE(follow->text(), QStringLiteral("回到实时"));
        for (int i = 0; i < 40; ++i)
            emit runtime.context()->hub()->unitReceived({}, Message{{"word", i}});
        QTest::qWait(60);
        QCOMPARE(readout->text(), QStringLiteral("0x0001"));
        follow->click();
        QTRY_COMPARE(readout->text(), QStringLiteral("0x0027"));
        const auto count = [&](const QVariant &value) {
            emit runtime.context()->hub()->unitReceived({}, Message{{"encoder", value}});
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
        emit runtime.context()->hub()->unitReceived({}, Message{
            {"cells", QVariantList{3.7, 3.8, 3.6}},
            {"supply", QVariantMap{{"voltage", 12.34}, {"current", -.002}, {"mode", " cc "}}}});
        QCOMPARE(cells->cellValues().size(), 3);
        QVERIFY(cells->readingText().contains("200.0 mV"));
        QVERIFY(supply->readingText().endsWith("CC"));
        emit runtime.context()->hub()->unitReceived({}, Message{
            {"cells", QVariantList{true}}, {"supply", QVariantMap{{"voltage", "12"}, {"current", 1}, {"mode", "CV"}}}});
        QVERIFY(!cells->reading());
        QVERIFY(!supply->reading());
        emit runtime.context()->hub()->unitReceived({}, Message{
            {"cells", QVariantList{-.1, 0.0}},
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
        emit runtime.context()->hub()->unitReceived({}, Message{
            {"packet", QVariantMap{{"cells", QVariantList{4.1, 4.2}},
                                   {"supply", QVariantMap{{"voltage", 5.0}, {"current", .25}, {"mode", "CV"}}}}}});
        QCOMPARE(cells->cellValues(), QList<double>({4.1, 4.2}));
        QVERIFY(supply->readingText().contains("5.00 V"));
        runtime.context()->addBinding(*BindingEntry::fromWidget(
            QJsonObject{{"id", "timing"},
                        {"type", "DigitalTimingChart"},
                        {"binding", QJsonObject{{"parseMode", ParseMode::JsonPath}, {"jsonPath", "packet.word"}}}}));
        emit runtime.context()->hub()->unitReceived({}, Message{{"packet", QVariantMap{{"unrelated", 1}}}});
        QTest::qWait(60);
        QCOMPARE(readout->text(), QStringLiteral("0x0027"));
        emit runtime.context()->hub()->unitReceived({}, Message{{"packet", QVariantMap{{"word", QVariant()}}}});
        QTRY_COMPARE(readout->text(), QStringLiteral("数据异常"));
    }

    void timingChannelsKeepIndependentLevelsAndExportGaps_data()
    {
        QTest::addColumn<int>("channelCount");
        QTest::newRow("three-channels") << 3;
        QTest::newRow("thirty-two-channels") << 32;
    }

    void timingChannelsKeepIndependentLevelsAndExportGaps()
    {
        QFETCH(int, channelCount);
        Project project;
        const auto card = [](const char *id) {
            return QJsonObject{{"id", id}, {"type", "CanConfigCard"}};
        };
        const QString lastChannel = QStringLiteral("_curve_%1_").arg(channelCount - 1);
        CanvasItem item("DigitalTimingChart", {{TimingSource::Property, TimingSource::Channels}, {"curveCount", 1},
                    {"_curve_0_name", "Ready"}, {"_curve_0_parseMode", ParseMode::CanField},
                    {"_curve_0_key", "0x201.status"}, {"_curve_0_sourceCardId", "a"},
                    {"_curve_1_name", "Raw"}, {"_curve_1_parseMode", ParseMode::CanSignal},
                    {"_curve_1_canId", "201"}, {"_curve_1_canBitLength", 1}, {"_curve_1_sourceCardId", "a"},
                    {lastChannel + "parseMode", ParseMode::DataPoint}, {lastChannel + "key", "Point"}}, "timing");
        {
            PropertyEditor editor;
            editor.showItem(&item);
            auto *count = qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("通道数量")));
            QVERIFY(count);
            QCOMPARE(count->maximum(), 32);
            count->setValue(channelCount);
            QApplication::processEvents();
            auto *picker = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("当前编辑通道")));
            QVERIFY(picker);
            QCOMPARE(picker->count(), channelCount);
            picker->setCurrentIndex(channelCount - 1);
            QApplication::processEvents();
            auto *name = qobject_cast<QLineEdit *>(fieldFor(&editor, QStringLiteral("通道名称")));
            QVERIFY(name);
            name->setText(QStringLiteral("Point"));
            QCOMPARE(item.propertyValue(lastChannel + "name").toString(), QStringLiteral("Point"));
        }
        project.widgets = {
            card("a"), card("b"),
            QJsonObject{{"id", "timing"}, {"type", "DigitalTimingChart"},
                {"properties", item.properties()},
                {"binding", QJsonObject{{"key", "legacy"}}}}
        };
        project.logic = {{"points", QJsonArray{QJsonObject{{"name", "Point"},
            {"binding", QJsonObject{{"parseMode", ParseMode::CanSignal}, {"sourceCardId", "b"},
                                    {"canId", "202"}, {"canBitLength", 1}}}}}}};
        QTemporaryDir dir;
        const QString path = dir.filePath("timing.uidp");
        QString error;
        QCOMPARE(project.toJson().value("version").toString(), QStringLiteral("1.3"));
        QVERIFY2(project.save(path, &error), qPrintable(error));
        QVERIFY2(Project::load(path, &project, &error), qPrintable(error));

        QStringList names{"Ready", "Raw"};
        QStringList missing;
        for (int i = 2; i < channelCount - 1; ++i) {
            names.append(QStringLiteral("通道 %1").arg(i + 1));
            missing.append(names.last() + QStringLiteral(" — · "));
        }
        names.append(QStringLiteral("Point"));
        const auto expectedReadout = [&](const QString &ready, const QString &raw) {
            return QStringLiteral("Ready %1 · Raw %2 · %3Point 1").arg(ready, raw, missing.join(QString()));
        };
        const QByteArray csvHeader = "\xef\xbb\xbftime_s," + names.join(',').toUtf8() + "\r\n";
        const QByteArray emptyColumns(channelCount - 3, ',');
        const QByteArray missingReadyRow = ",,1," + emptyColumns + "1\r\n";

        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        auto *a = surface.widgets().at(0)->findChild<CanLink *>();
        auto *b = surface.widgets().at(1)->findChild<CanLink *>();
        auto *timing = dynamic_cast<DigitalTimingChartWidget *>(surface.widgets().at(2));
        QVERIFY(a && b && timing);
        const auto group = surface.context()->widgetPreferencesGroup(*timing);
        QVERIFY(!group.isEmpty());
        QSettings options;
        options.beginGroup(group);
        const auto clearPreferences = qScopeGuard([&options] { options.remove(QString()); });
        options.setValue("timingWindow", 37);
        options.setValue("timingExportDirectory", dir.path());
        options.setValue("timingAutoExport", true);
        options.sync();
        QCOMPARE(options.status(), QSettings::NoError);
        timing->detach();
        timing->attach(surface.context());
        auto *window = timing->findChild<FluentQt::ComboBox *>("timingWindow");
        auto *plot = timing->findChild<FluentQt::RealtimePlotWidget *>("digitalTimingPlot");
        QVERIFY(window && plot);
        if (channelCount == 32) {
            auto *scroll = plot->findChild<FluentQt::ScrollBar *>();
            QVERIFY(scroll && scroll->isVisible());
            QCOMPARE(scroll->maximum() + scroll->pageStep(), channelCount);
            scroll->setValue(scroll->maximum());
        }
        QCOMPARE(window->currentData().toInt(), 37);
        window->setCurrentIndex(window->findData(0));
        QTimer::singleShot(0, &surface, [&] {
            auto *dialog = surface.findChild<AppDialog *>();
            QVERIFY(dialog);
            auto *seconds = dialog->findChild<QSpinBox *>("timingWindowSeconds");
            if (!seconds)
                dialog->reject();
            QVERIFY(seconds);
            seconds->setValue(41);
            dialog->accept();
        });
        QTRY_COMPARE(window->currentData().toInt(), 41);
        QCOMPARE(plot->xMaximum() - plot->xMinimum(), 41.0);
        CanSettings settings;
        settings.adapter = CanAdapter::Virtual;
        QVERIFY2(a->open(settings, {}, "201 status : 8|1@1+", {}, &error), qPrintable(error));
        QVERIFY2(b->open(settings, {}, {}, {}, &error), qPrintable(error));
        const auto send = [&](CanLink *link, quint32 id, const QByteArray &payload) {
            CanFrame frame;
            frame.id = id;
            frame.data = QByteArray::fromHex(payload);
            return link->sendFrame(frame, &error);
        };
        QVERIFY2(send(a, 0x201, "0001"), qPrintable(error));
        QVERIFY2(send(b, 0x202, "01"), qPrintable(error));
        QTRY_VERIFY(timing->accessibleDescription().endsWith(expectedReadout("1", "0")));
        QVERIFY2(send(a, 0x201, "01"), qPrintable(error));
        QVERIFY2(send(b, 0x201, "0001"), qPrintable(error)); // Same ID on a different connection.
        QTRY_VERIFY(timing->accessibleDescription().endsWith(expectedReadout("—", "1")));

        const QString csvPath = dir.filePath("timing.csv");
        QVERIFY2(timing->saveHistory(csvPath, &error), qPrintable(error));
        QFile csv(csvPath);
        QVERIFY(csv.open(QIODevice::ReadOnly));
        const auto content = csv.readAll();
        QVERIFY(content.startsWith(csvHeader));
        QVERIFY(content.contains(missingReadyRow));

        QVERIFY2(timing->loadHistory(csvPath, &error), qPrintable(error));
        auto *follow = timing->findChild<QPushButton *>("timingFollow");
        QVERIFY(follow);
        QCOMPARE(follow->text(), QStringLiteral("回到实时"));
        QVERIFY(timing->accessibleDescription().contains(QStringLiteral("timing.csv")));
        QVERIFY(timing->accessibleDescription().endsWith(expectedReadout("—", "1")));
        a->close();
        QVERIFY(timing->accessibleDescription().endsWith(expectedReadout("—", "1")));
        follow->click();
        QTRY_VERIFY(timing->accessibleDescription().endsWith(expectedReadout("—", "—")));
        const QString jsonPath = dir.filePath("timing.json");
        QVERIFY2(timing->saveHistory(jsonPath, &error), qPrintable(error));
        QFile json(jsonPath);
        QVERIFY(json.open(QIODevice::ReadOnly));
        const auto rows = QJsonDocument::fromJson(json.readAll()).object().value("rows").toArray();
        QVERIFY(!rows.isEmpty());
        const auto last = rows.last().toArray();
        QVERIFY(last.at(0).toDouble() > rows.first().toArray().first().toDouble());
        QVERIFY(last.at(1).isNull() && last.at(2).isNull());
        QCOMPARE(last.size(), channelCount + 1);
        QCOMPARE(last.last().toInt(), 1);
        QVERIFY2(timing->loadHistory(jsonPath, &error), qPrintable(error));
        QVERIFY(timing->accessibleDescription().contains(QStringLiteral("timing.json")));
        QVERIFY(timing->accessibleDescription().endsWith(expectedReadout("—", "—")));
        follow->click();
        surface.context()->setActive(false);
        const QDir records(dir.path());
        const auto automatic = records.entryList({QStringLiteral("timing-*.csv")}, QDir::Files);
        QCOMPARE(automatic.size(), 1);
        QFile recorded(records.filePath(automatic.first()));
        QVERIFY(recorded.open(QIODevice::ReadOnly));
        const auto autoCsv = recorded.readAll();
        QVERIFY(autoCsv.startsWith(csvHeader));
        QVERIFY(autoCsv.contains(missingReadyRow));
        QVERIFY(autoCsv.endsWith(",,," + emptyColumns + "1\r\n"));
        const auto replay = readTimingRecording(autoCsv, ChartFileFormat::Csv, &error);
        QVERIFY2(replay, qPrintable(error));
        QCOMPARE(replay->names, names);
        QVERIFY(replay->samples.back().seconds > replay->samples.front().seconds);
        QCOMPARE(replay->samples.back().levels.valid, TimingMask{1} << (channelCount - 1));
        QVERIFY(!readTimingRecording("time_s,A\n2,1\n1,0\n", ChartFileFormat::Csv, &error));
    }

    void everyTypeHasAWidget()
    {
        for (const WidgetMeta &meta : WidgetRegistry::instance().all()) {
            std::unique_ptr<HmiWidget> widget(createHmiWidget(meta.typeName, {}));
            QVERIFY2(widget, qPrintable(meta.typeName));
            QCOMPARE(widget->typeName(), meta.typeName);
        }
    }

    void commandConfirmationHoldsAndReleasesTheOriginalAction()
    {
        Project project;
        project.canvasWidth = 600;
        project.canvasHeight = 400;
        project.widgets = QJsonArray{
            QJsonObject{{"id", "entry"}, {"type", "LineEdit"},
                        {"geometry", QJsonObject{{"x", 20}, {"y", 20}, {"w", 200}, {"h", 36}}},
                        {"properties", QJsonObject{{"inputOnly", false}, {"commandNewline", "无"}, {"commandTargetId", "device"},
                                                   {"confirmBeforeSend", true}, {"showSendConfirmationDetails", true}}}},
            QJsonObject{{"id", "switch"}, {"type", "ToggleSwitch"},
                        {"geometry", QJsonObject{{"x", 20}, {"y", 84}, {"w", 160}, {"h", 36}}},
                        {"properties", QJsonObject{{"onCommand", "ON"}, {"commandNewline", "无"},
                                                   {"commandTargetId", "device"}, {"confirmBeforeSend", true}}}}};
        QCOMPARE(project.toJson().value(QStringLiteral("version")).toString(), QStringLiteral("1.3"));
        FakeLink link;
        FakeLink replacement;
        link.setConnectionId(QStringLiteral("device"));
        replacement.setConnectionId(link.connectionId());
        PlayerWindow window(project);
        auto *surface = window.surface();
        auto *hub = surface->context()->hub();
        hub->addLink(&link);
        auto *view = window.findChild<QGraphicsView *>(QStringLiteral("canvasView"));
        auto *entry = surface->findChild<QLineEdit *>();
        auto *toggle = surface->findChild<FluentQt::SwitchButton *>();
        QVERIFY(view && entry && toggle);
        window.show();
        QApplication::processEvents();
        const auto confirmation = [&window] {
            return QPointer(window.findChild<AppDialog *>(QStringLiteral("sendConfirmationDialog")));
        };
        const QPoint entryPoint = view->mapFromScene(QPointF(entry->mapTo(surface, entry->rect().center())));
        const auto submitEntry = [&] {
            QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, entryPoint);
            QTest::keyClick(view, Qt::Key_Return);
        };

        entry->setText(QStringLiteral("RUN"));
        submitEntry();
        auto dialog = confirmation();
        QVERIFY(dialog && dialog->isVisible());
        QCOMPARE(dialog->parentWidget(), &window);
        auto *details = dialog->findChild<QPlainTextEdit *>(QStringLiteral("sendConfirmationDetails"));
        QVERIFY(details && details->toPlainText().contains(QStringLiteral("device")));
        QVERIFY(details->toPlainText().contains(QStringLiteral("RUN")));
        QVERIFY(link.sent.isEmpty());
        entry->setText(QStringLiteral("STOP"));
        QCOMPARE(window.focusWidget(), dialog->cancelButton());
        QTest::keyClick(window.focusWidget(), Qt::Key_Return);
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(link.sent.isEmpty());

        entry->setText(QStringLiteral("RUN"));
        submitEntry();
        dialog = confirmation();
        QVERIFY(dialog);
        entry->setText(QStringLiteral("STOP"));
        QTest::keyClick(entry, Qt::Key_Return); // An extra submit during the prompt cannot replace the action.
        QCOMPARE(confirmation(), dialog);
        dialog->yesButton()->click();
        dialog->yesButton()->click();
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(link.sent, QList<QByteArray>{QByteArray("RUN")});

        submitEntry();
        dialog = confirmation();
        QVERIFY(dialog);
        link.setConnected(false);
        link.setConnected(true);
        dialog->yesButton()->click();
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(link.sent, QList<QByteArray>{QByteArray("RUN")});

        toggle->toggleChecked();
        dialog = confirmation();
        QVERIFY(dialog);
        QVERIFY(!dialog->findChild<QPlainTextEdit *>(QStringLiteral("sendConfirmationDetails")));
        QVERIFY(!toggle->isChecked());
        dialog->cancelButton()->click();
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(!toggle->isChecked());
        QCOMPARE(link.sent.size(), 1);

        toggle->toggleChecked();
        dialog = confirmation();
        QVERIFY(dialog);
        hub->removeLink(&link);
        hub->addLink(&replacement);
        dialog->yesButton()->click();
        QTRY_VERIFY(dialog.isNull());
        QVERIFY(replacement.sent.isEmpty());
        QVERIFY(!toggle->isChecked());

        toggle->toggleChecked();
        dialog = confirmation();
        QVERIFY(dialog);
        dialog->yesButton()->click();
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(replacement.sent, QList<QByteArray>{QByteArray("ON")});
        QVERIFY(toggle->isChecked());
    }

    void oldInputsKeepReturnSendingDisabled()
    {
        const QList<QJsonObject> oldFlags{{}, {{"sendOnEnter", false}}, {{"readOnly", true}, {"sendOnEnter", true}}};
        for (auto properties : oldFlags) {
            properties.insert("text", "STOP");
            Project project;
            QVERIFY(Project::fromJson({{"version", "1.2"}, {"widgets", QJsonArray{QJsonObject{
                {"id", "input"}, {"type", "LineEdit"}, {"properties", properties},
                {"binding", QJsonObject{{"key", "rpm"}}}}}}}, &project, nullptr));
            RuntimeSurface surface;
            FakeLink link;
            surface.build(project, true);
            surface.context()->hub()->addLink(&link, "device");
            auto *input = surface.findChild<HmiWidget *>(QStringLiteral("LineEdit"));
            QVERIFY(input);
            auto *line = input->findChild<QLineEdit *>();
            QVERIFY(line);
            QCOMPARE(line->isReadOnly(), properties.value("readOnly").toBool());
            QVERIFY(input->properties().value("inputOnly").toBool());
            link.inject("rpm=77\n");
            QCOMPARE(line->text(), line->isReadOnly() ? QStringLiteral("77") : QStringLiteral("STOP"));
            QVERIFY(QMetaObject::invokeMethod(line, "returnPressed", Qt::DirectConnection));
            QVERIFY(link.sent.isEmpty());
        }
    }

    void virtualSerialPortsPairWithinOneProjectAndDropOldWrites()
    {
        CommHub project;
        CommHub separateProject;
        SerialLink a(nullptr, project.virtualSerialPair());
        SerialLink b(nullptr, project.virtualSerialPair());
        SerialLink unpaired(nullptr, separateProject.virtualSerialPair());
        SerialSettings left;
        left.portName = QStringLiteral("virtual:A");
        SerialSettings right;
        right.portName = QStringLiteral("virtual:B");
        QVERIFY(a.open(left));
        QVERIFY(unpaired.open(right));
        QVERIFY(!a.canSend());
        QVERIFY(!b.open(left));
        QVERIFY(b.open(right));
        QSignalSpy receives(&b, &Link::rawReceived);
        QVERIFY(a.send("rpm=1800\n"));
        QTRY_COMPARE(receives.size(), 1);
        QVERIFY(receives.first().first().toByteArray().contains("rpm=1800"));
        QVERIFY(a.send("old\n"));
        const auto epoch = a.sendSession({});
        a.close();
        QVERIFY(!b.canSend());
        QVERIFY(a.open(left));
        QVERIFY(a.sendSession({}) != epoch);
        QCOMPARE(a.sendSession({}), b.sendSession({}));
        QVERIFY(a.send("rpm=2300\n"));
        QTRY_COMPARE(receives.size(), 2);
        QVERIFY(receives.last().first().toByteArray().contains("rpm=2300"));
    }

    void periodicSendUsesTheConfirmedTemplateAndStopsOnClickOrDisconnect()
    {
        Project project;
        const QJsonObject config{{"header", "A5"}, {"footer", "0D"}, {"checksum", "异或"},
                                 {"fields", QJsonArray{QJsonValue(QJsonArray{"rpm", 0, 2, "uint16", "小端(LE)"})}}};
        project.frameTemplates.insert("speed", FrameTemplates::Entry{"Speed", {"hex", config}, {}}.toJson());
        project.frameTemplates.insert("line", FrameTemplates::Entry{"Line", {"line",
            {{"encoding", "gbk"}, {"terminator", "\r"}}}, {}}.toJson());
        const QJsonObject send{{"commandMode", "HEX"}, {"commandTargetId", "device"},
                               {"sendIntervalMs", 40}, {FrameTemplates::SendId, "speed"}};
        const QJsonObject manual{{"commandMode", "文本"}, {"commandTargetId", "device"},
                                 {"sendIntervalMs", 60}, {FrameTemplates::SendId, "line"}};
        auto buttonProps = send;
        buttonProps.insert("text", "Start RPM");
        buttonProps.insert("confirmBeforeSend", true);
        buttonProps.insert(FrameTemplates::SendFieldsEnabled, true);
        buttonProps.insert(FrameTemplates::SendBindings, QJsonArray{QJsonObject{{"field", "rpm"}, {"inputId", "rpm"}}});
        project.widgets = QJsonArray{
            QJsonObject{{"id", "rpm"}, {"type", "LineEdit"}, {"properties", QJsonObject{{"inputOnly", true}}}},
            QJsonObject{{"id", "start"}, {"type", "PushButton"}, {"properties", buttonProps}},
            QJsonObject{{"id", "card"}, {"type", "DataSendCard"}, {"properties", manual}}};
        QCOMPARE(project.toJson().value("version").toString(), QStringLiteral("1.3"));

        FakeLink device;
        RuntimeSurface surface;
        surface.build(project);
        surface.context()->hub()->addLink(&device, QStringLiteral("device"));
        const auto controls = surface.widgets();
        auto *input = controls.at(0)->findChild<QLineEdit *>();
        auto *button = controls.at(1)->findChild<QPushButton *>();
        auto *cardInput = controls.at(2)->findChild<QLineEdit *>();
        auto *cardButton = controls.at(2)->findChild<QPushButton *>();
        const auto confirmation = [&surface] {
            return surface.findChild<AppDialog *>(QStringLiteral("sendConfirmationDialog"));
        };
        input->setText(QStringLiteral("1500"));
        QTest::qWait(80);
        QVERIFY(device.sent.isEmpty());
        button->click();
        QVERIFY(confirmation());
        QVERIFY(device.sent.isEmpty());
        input->setText(QStringLiteral("2300"));
        confirmation()->yesButton()->click();
        QTRY_VERIFY(device.sent.size() >= 2);
        const auto firstFrame = QByteArray::fromHex("A5DC057C0D");
        QCOMPARE(device.sent.front(), firstFrame);
        QCOMPARE(device.sent.back(), QByteArray::fromHex("A5FC08510D"));
        const auto lastValidCount = device.sent.size();
        auto *sender = surface.context()->periodicSender();
        input->clear();
        QTRY_VERIFY(!sender->tasks().first().inputError.isEmpty());
        QCOMPARE(device.sent.size(), lastValidCount);
        input->setText(QStringLiteral("1500"));
        QTRY_VERIFY(device.sent.size() > lastValidCount);
        QCOMPARE(device.sent.back(), firstFrame);
        QVERIFY(sender->tasks().first().inputError.isEmpty());
        QCOMPARE(button->text(), QStringLiteral("停止周期发送"));
        button->click();
        const auto sent = device.sent.size();
        QTest::qWait(90);
        QCOMPARE(device.sent.size(), sent);
        QCOMPARE(button->text(), QStringLiteral("Start RPM"));

        cardInput->setText(QStringLiteral("开"));
        device.busy = true;
        cardButton->click();
        QCOMPARE(cardButton->text(), QStringLiteral("取消启动"));
        QTest::qWait(80);
        QCOMPARE(device.sent.size(), sent);
        device.busy = false;
        QTRY_VERIFY(device.sent.size() >= sent + 2);
        QCOMPARE(device.sent.last(), QByteArray::fromHex("BFAA0D"));
        QVERIFY(cardInput->isEnabled());
        cardInput->setText(QStringLiteral("关"));
        QVERIFY(QMetaObject::invokeMethod(cardInput, "returnPressed"));
        QVERIFY(sender->running(QStringLiteral("card")));
        QTRY_COMPARE(device.sent.last(), QByteArray::fromHex("B9D80D"));
        device.setConnected(false);
        QCOMPARE(cardButton->text(), QStringLiteral("开始周期发送"));
        QVERIFY(cardInput->isEnabled());
        const auto disconnected = device.sent.size();
        device.setConnected(true);
        QTest::qWait(90);
        QCOMPARE(device.sent.size(), disconnected);

        cardButton->click();
        QTRY_VERIFY(device.sent.size() > disconnected);
        surface.context()->setActive(false);
        const auto stopped = device.sent.size();
        QTest::qWait(90);
        QCOMPARE(device.sent.size(), stopped);
    }

    void modbusWriteListUsesCurrentInputsInOneConfirmedFrame()
    {
        const QStringList ids{QStringLiteral("Kp"), QStringLiteral("Ki"), QStringLiteral("Kd")};
        const QStringList values{QStringLiteral("1.5"), QStringLiteral("0.25"), QStringLiteral("0.0625")};
        Project project;
        project.canvasWidth = 720;
        project.canvasHeight = 430;
        for (qsizetype i = 0; i < ids.size(); ++i) {
            const QString title = i == 0 ? QStringLiteral("比例系数") : QString();
            project.widgets.append(QJsonObject{{"id", ids.at(i)}, {"type", "LineEdit"},
                {"geometry", QJsonObject{{"x", 20}, {"y", 20 + int(i) * 44}, {"w", 160}, {"h", 36}}},
                {"properties", QJsonObject{{"text", "0"}, {"title", title}, {"inputOnly", true}}}});
        }
        project.widgets.append(QJsonObject{{"id", "send"}, {"type", "PushButton"},
            {"geometry", QJsonObject{{"x", 20}, {"y", 160}, {"w", 160}, {"h", 36}}},
            {"properties", QJsonObject{{"commandMode", "Modbus"}, {"mbFunc", "16 写多个寄存器"},
                                       {"mbAddress", 100}, {"commandTargetId", "serial"}, {"confirmBeforeSend", true}}}});
        project.widgets.append(QJsonObject{{"id", "serial"}, {"type", "SerialConfigCard"},
            {"geometry", QJsonObject{{"x", 205}, {"y", 20}, {"w", 350}, {"h", 240}}},
            {"properties", QJsonObject{{"defaultParser", "modbus"}}}});

        QString error;
        DesignScene scene;
        QVERIFY2(scene.loadWidgets(project.widgets, &error), qPrintable(error));
        PropertyEditor editor;
        editor.setScene(&scene);
        editor.resize(680, 640);
        editor.showItem(scene.findItem(QStringLiteral("send")));
        editor.show();
        auto *open = qobject_cast<QPushButton *>(fieldFor(&editor, QStringLiteral("发送配置")));
        QVERIFY(open && open->isVisible());
        open->click();
        auto listDialog = QPointer(editor.findChild<AppDialog *>());
        QVERIFY(listDialog);
        auto *table = listDialog->findChild<QTableWidget *>(QStringLiteral("commandInputsTable"));
        QVERIFY(table);
        for (qsizetype i = 0; i < ids.size(); ++i) {
            QVERIFY(addExistingCommandInput(listDialog, ids.at(i)));
            auto *type = table->cellWidget(i, 1)->findChild<FluentQt::ComboBox *>();
            QVERIFY(type);
            const int typeIndex = type->findData(QStringLiteral("float32"));
            QVERIFY(typeIndex >= 0);
            type->setCurrentIndex(typeIndex);
        }
        listDialog->yesButton()->click();
        QTRY_VERIFY(listDialog.isNull());
        project.widgets = scene.widgetsJson();
        QVERIFY2(Project::fromJson(project.toJson(), &project, &error), qPrintable(error));

        auto *preview = editor.findChild<QPushButton *>(QStringLiteral("sendPreview"));
        QVERIFY(preview);
        preview->click();
        auto previewDialog = QPointer(editor.findChild<AppDialog *>(QStringLiteral("commandPreviewDialog")));
        QVERIFY(previewDialog);
        auto *output = previewDialog->findChild<QPlainTextEdit *>(QStringLiteral("commandPreviewOutput"));
        QVERIFY(output && output->toPlainText().contains(QStringLiteral("Kd = 0 (float32 · 地址 104)")));
        previewDialog->yesButton()->click();
        QTRY_VERIFY(previewDialog.isNull());
        editor.close();

        FakeLink link;
        link.setConnectionId(QStringLiteral("serial"));
        QVERIFY(link.setParser(QStringLiteral("modbus"), {}));
        RuntimeSurface runtime;
        runtime.build(project);
        runtime.context()->hub()->addLink(&link);
        runtime.show();
        QApplication::processEvents();
        QMap<QString, QLineEdit *> inputs;
        QPushButton *send = nullptr;
        for (auto *widget : runtime.widgets()) {
            const QString id = widget->property("fhmiWidgetId").toString();
            if (id == QLatin1String("send"))
                send = widget->findChild<QPushButton *>();
            else if (ids.contains(id)) {
                auto *input = widget->findChild<QLineEdit *>();
                auto *title = widget->findChild<QLabel *>(QStringLiteral("hmiInputTitle"));
                QVERIFY(input && title);
                const bool hasTitle = id == QLatin1String("Kp");
                QCOMPARE(title->isVisible(), hasTitle);
                if (hasTitle) {
                    QCOMPARE(title->text(), QStringLiteral("比例系数"));
                    QVERIFY(title->geometry().right() < input->geometry().left());
                }
                inputs.insert(id, input);
            }
        }
        QVERIFY(send);
        for (qsizetype i = 0; i < ids.size(); ++i) {
            auto *input = inputs.value(ids.at(i));
            QVERIFY(input);
            input->setText(values.at(i));
            QTest::keyClick(input, Qt::Key_Return);
        }
        QVERIFY(link.sent.isEmpty());

        QSignalSpy notices(runtime.context(), &RuntimeContext::notificationRequested);
        auto *integral = inputs.value(QStringLiteral("Ki"));
        integral->setText(QStringLiteral("invalid"));
        send->click();
        QVERIFY(link.sent.isEmpty());
        QCOMPARE(notices.size(), 1);
        QVERIFY(notices.first().at(1).toString().contains(QStringLiteral("Ki")));
        integral->setText(values.at(1));
        send->click();
        auto confirmation = QPointer(runtime.findChild<AppDialog *>(QStringLiteral("sendConfirmationDialog")));
        QVERIFY(confirmation && link.sent.isEmpty());
        auto *summary = confirmation->findChild<QPlainTextEdit *>(QStringLiteral("sendConfirmationDetails"));
        QVERIFY(summary);
        QCOMPARE(summary->toPlainText(), QStringLiteral("比例系数 = 1.5\nKi = 0.25\nKd = 0.0625"));
        integral->setText(QStringLiteral("99")); // Confirm the values shown, even if an input changes.
        confirmation->yesButton()->click();
        QTRY_VERIFY(confirmation.isNull());
        const auto payload = QByteArray::fromHex("0110006400060c3fc000003e8000003d800000");
        QCOMPARE(link.sent, QList<QByteArray>{payload + modbusCrc(payload)});
    }

    void commandInputListCreatesInputsOnlyWhenApplied()
    {
        const QJsonObject button{{"id", "send"}, {"type", "PushButton"}, {"tabIndex", 1},
            {"geometry", QJsonObject{{"x", 20}, {"y", 70}, {"w", 160}, {"h", 36}}},
            {"properties", QJsonObject{{"commandMode", "文本"}, {"confirmBeforeSend", true}}}};
        const QJsonObject existing{{"id", "existing"}, {"type", "LineEdit"}, {"tabIndex", 1},
            {"geometry", QJsonObject{{"x", 20}, {"y", 116}, {"w", 200}, {"h", 36}}},
            {"properties", QJsonObject{{"inputOnly", true}, {"text", "258"}}}};
        Project project;
        project.widgets = {QJsonObject{{"id", "tabs"}, {"type", "TabWidget"},
            {"geometry", QJsonObject{{"x", 10}, {"y", 10}, {"w", 600}, {"h", 340}}},
            {"properties", QJsonObject{{"tabNames", "A\nB"}, {"currentTab", 1}}},
            {"children", QJsonArray{button, existing}}}};
        QTemporaryDir directory;
        QString error;
        const QString path = directory.filePath(QStringLiteral("write-list.uidp"));
        QVERIFY2(project.save(path, &error), qPrintable(error));
        DesignerPage page;
        QVERIFY(page.openProject(path));
        auto *scene = page.findChild<DesignScene *>();
        auto *editor = page.findChild<PropertyEditor *>();
        QVERIFY(scene && editor);
        page.resize(1200, 800);
        page.show();
        scene->selectIds({QStringLiteral("send")});
        QApplication::processEvents();
        const auto before = page.snapshot().toJson();
        const auto editSend = [&] {
            QPushButton *button = nullptr;
            if (QTest::qWaitFor([&] {
                for (auto *edit : editor->findChildren<QPushButton *>(QStringLiteral("editFrameTemplate"))) {
                    if (edit->isVisible()) {
                        button = edit;
                        return true;
                    }
                }
                return false;
            }))
                button->click();
            return QPointer(page.findChild<AppDialog *>(QStringLiteral("frameTemplateEditor")));
        };
        const auto click = [](AppDialog *dialog, const QString &label) {
            for (auto *button : dialog->findChildren<QAbstractButton *>()) {
                if (button->text() == label || button->accessibleName() == label) {
                    button->click();
                    return true;
                }
            }
            return false;
        };
        auto dialog = editSend();
        QVERIFY(dialog && click(dialog, QStringLiteral("新建输入框")));
        dialog->cancelButton()->click();
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(page.snapshot().toJson(), before);
        QVERIFY(!page.canUndo());

        dialog = editSend();
        QVERIFY(dialog && addExistingCommandInput(dialog, QStringLiteral("existing")));
        QVERIFY(click(dialog, QStringLiteral("新建输入框")));
        auto *table = dialog->findChild<QTableWidget *>(QStringLiteral("commandInputsTable"));
        auto *expression = dialog->findChild<QPlainTextEdit *>(QStringLiteral("frameTemplateExpression"));
        auto *preview = dialog->findChild<QPlainTextEdit *>(QStringLiteral("frameTemplatePreview"));
        QVERIFY(table && expression && preview);
        QCOMPARE(table->rowCount(), 2);
        QCOMPARE(table->columnCount(), 2);
        QCOMPARE(expression->toPlainText(), QStringLiteral("p1={p1},p2={p2}"));
        expression->setPlainText(QStringLiteral("KP={p1}\nKI="));
        expression->moveCursor(QTextCursor::End);
        auto *insert = dialog->findChild<FluentQt::DropDownPushButton *>(QStringLiteral("insertFrameVariable"));
        QVERIFY(insert && insert->menu());
        auto *token = insert->menu()->findChildren<QAction *>().last();
        QCOMPARE(token->data().toString(), QStringLiteral("{p2}"));
        token->trigger();
        QCOMPARE(expression->toPlainText(), QStringLiteral("KP={p1}\nKI={p2}"));
        QCOMPARE(preview->toPlainText(), QStringLiteral("KP=258\\nKI=2\\r\\n"));
        QVERIFY(click(dialog, QStringLiteral("自动生成")));
        QCOMPARE(page.snapshot().toJson(), before);
        dialog->yesButton()->click();
        QTRY_VERIFY(dialog.isNull());
        const QString templateKey = QStringLiteral("commandTemplate");
        const QString inputsKey = QStringLiteral("commandInputs");
        const auto selected = [&] { return scene->findItem(QStringLiteral("send")); };
        QCOMPARE(selected()->propertyValue(templateKey).toString(), QStringLiteral("p1={p1},p2={p2}"));
        const auto rows = selected()->propertyValue(inputsKey).toArray();
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).toObject().value(QStringLiteral("inputId")).toString(), QStringLiteral("existing"));
        const auto *created = scene->findItem(rows.at(1).toObject().value(QStringLiteral("inputId")).toString());
        QVERIFY(created && created->propertyValue(QStringLiteral("inputOnly")).toBool());
        QCOMPARE(created->containerParent(), selected()->containerParent());
        QCOMPARE(created->tabIndex(), selected()->tabIndex());
        QCOMPARE(created->propertyValue(QStringLiteral("title")).toString(), QStringLiteral("参数 2"));
        const auto after = page.snapshot().toJson();
        page.undo();
        QCOMPARE(page.snapshot().toJson(), before);
        QVERIFY(!page.canUndo());
        page.redo();
        QCOMPARE(page.snapshot().toJson(), after);

        scene->selectIds({QStringLiteral("send")});
        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(editor, QStringLiteral("发送模式")));
        QVERIFY(mode);
        mode->setCurrentIndex(mode->findData(QStringLiteral("HEX")));
        QCOMPARE(selected()->propertyValue(templateKey).toString(), QStringLiteral("{p1} {p2}"));
        const auto editBinary = [&](AppDialog *format) {
            auto *inputs = format->findChild<QTableWidget *>(QStringLiteral("commandInputsTable"));
            auto *fields = format->findChild<QTableWidget *>(QStringLiteral("frameTemplateTable"));
            QVERIFY(inputs && fields);
            QCOMPARE(inputs->columnCount(), 3);
            auto *type = inputs->cellWidget(0, 2)->findChild<FluentQt::ComboBox *>();
            type->setCurrentIndex(type->findData(QStringLiteral("uint32")));
            QVERIFY(click(format, QStringLiteral("添加固定段")));
            QCOMPARE(fields->rowCount(), 3);
            const int contentColumn = fields->columnCount() - 1;
            auto *literal = fields->cellWidget(2, contentColumn)->findChild<QLineEdit *>();
            QVERIFY(literal);
            literal->setText(QStringLiteral("AB"));
            fields->setCurrentCell(2, contentColumn);
            QVERIFY(click(format, QStringLiteral("上移")));
            auto *header = format->findChild<QLineEdit *>(QStringLiteral("frameTemplate_frameHeader"));
            QVERIFY(header);
            header->setText(QStringLiteral("AA"));
            QVERIFY(click(format, QStringLiteral("长度与校验")));
            auto *lengthMode = format->findChild<FluentQt::ComboBox *>(QStringLiteral("txFrame_lengthMode"));
            auto *algorithm = format->findChild<FluentQt::ComboBox *>(QStringLiteral("txFrame_checksum"));
            auto *offset = format->findChild<QSpinBox *>(QStringLiteral("txFrame_lengthOffset"));
            auto *start = format->findChild<QSpinBox *>(QStringLiteral("txFrame_checksumStart"));
            QVERIFY(lengthMode && algorithm && offset && start && lengthMode->isVisible());
            lengthMode->setCurrentIndex(2);
            offset->setValue(1);
            algorithm->setCurrentIndex(algorithm->findData(QStringLiteral("异或")));
            start->setValue(2);
            auto *output = format->findChild<QPlainTextEdit *>(QStringLiteral("frameTemplatePreview"));
            QCOMPARE(output->toPlainText(), QStringLiteral("AA 07 00 00 01 02 AB 00 00 A8"));
            auto *bytes = format->findChild<FrameBytesView *>();
            QVERIFY(bytes && bytes->isVisible());
            QCOMPARE(bytes->bytes(), QByteArray::fromHex("AA0700000102AB0000A8"));
            QVERIFY(labelShown(format, QStringLiteral("长度覆盖 · 整帧字节 2–8 · 00 00 01 02 AB 00 00")));
            QVERIFY(labelShown(format, QStringLiteral("参与校验 · 整帧字节 2–8 · 00 00 01 02 AB 00 00")));
            QVERIFY(labelShown(format, QStringLiteral("异或 · 写入校验值 A8 · 整帧字节 9")));
            QTest::mouseClick(bytes->viewport(), Qt::LeftButton, {}, QPoint(8, 8));
            QVERIFY(header->isVisible());
        };
        const auto beforeFormat = page.snapshot().toJson();
        dialog = editSend();
        QVERIFY(dialog);
        editBinary(dialog);
        dialog->cancelButton()->click();
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(page.snapshot().toJson(), beforeFormat);
        dialog = editSend();
        QVERIFY(dialog);
        editBinary(dialog);
        dialog->yesButton()->click();
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(selected()->propertyValue(templateKey).toString(), QStringLiteral("{p1} AB {p2}"));
        QCOMPARE(selected()->propertyValue(inputsKey).toArray().at(0).toObject().value(QStringLiteral("dataType")).toString(),
                 QStringLiteral("uint32"));
        QCOMPARE(selected()->propertyValue(QStringLiteral("txFrame")).toObject().value(QStringLiteral("lengthOffset")).toInt(), 1);
        const auto custom = page.snapshot().toJson();
        page.undo();
        QCOMPARE(page.snapshot().toJson(), beforeFormat);
        page.redo();
        QCOMPARE(page.snapshot().toJson(), custom);
    }

    void frameTemplateMatchesCanSignalsByName()
    {
        const auto definitions = CanSignalSet::build({}, QStringLiteral("123 Kp : 0|16@1+\n123 Ki : 16|16@1+"));
        QVERIFY(definitions);
        const QList<CanPreviewSource> sources{{QStringLiteral("can"), definitions, {}}};
        const QMap<QString, CommandEncoder::InputValue> inputs{
            {QStringLiteral("first"), {QStringLiteral("258"), QStringLiteral("Kp")}},
            {QStringLiteral("second"), {QStringLiteral("3"), {}}}};
        QJsonArray rows{QJsonObject{{"inputId", "first"}, {"variable", "p1"}},
                        QJsonObject{{"inputId", "second"}, {"variable", "p2"}}};
        QJsonObject properties{{"commandMode", "CAN"}, {"canId", "0x123"}, {"commandInputs", rows}};
        const QString unfinished = defaultFrameTemplate(properties, sources, inputs);
        QCOMPARE(unfinished, QStringLiteral("Kp={p1},{p2}"));
        properties.insert("commandTemplate", unfinished);
        const auto read = [&inputs](const QString &id) { return std::optional(inputs.value(id)); };
        auto result = CommandEncoder::buttonFromProperties(properties, read);
        QVERIFY(result.command);
        QVERIFY(!definitions->preparePayload(result.command->can));

        rows[1] = QJsonObject{{"inputId", "second"}, {"variable", "Ki"}};
        properties.insert("commandInputs", rows);
        const QString generated = defaultFrameTemplate(properties, sources, inputs);
        QCOMPARE(generated, QStringLiteral("Kp={p1},Ki={Ki}"));
        properties.insert("commandTemplate", generated);
        result = CommandEncoder::buttonFromProperties(properties, read);
        QVERIFY(result.command);
        const auto payload = definitions->preparePayload(result.command->can);
        QVERIFY(payload);
        QCOMPARE(payload->frame.data.left(4), QByteArray::fromHex("02010300"));
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
                        {"properties", QJsonObject{{"inputOnly", false}, {"commandNewline", "无"}}},
                        {"binding", QJsonObject{{"parseMode", "Modbus"}, {"mbRegIndex", 100}}}}};
        FakeLink link;
        PlayerWindow window(project);
        RuntimeSurface *surface = window.surface();
        RuntimeContext *context = surface->context();
        QVERIFY(context->modbusNeeds().isEmpty());
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
            QCOMPARE(link.sent, QList<QByteArray>{QByteArray("RUN")});
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
        const QJsonObject choices{{"options", QJsonArray{QJsonObject{{"label", "手动模式"}, {"value", "0"}},
                                                        QJsonObject{{"label", "自动模式"}, {"value", "1"}},
                                                        QJsonObject{{"label", "维护模式"}, {"value", "2"}}}},
                                  {"sendOnSelect", true}, {"commandNewline", "\\n"}};
        const QJsonObject toggles{{"onCondition", "等于指定值"}, {"onValue", "1"}, {"offFeedbackValue", "2"},
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
            auto *check = toggle->findChild<FluentQt::CheckBox *>("hmiCheckBox");
            auto *sw = toggle->findChild<FluentQt::SwitchButton *>("hmiToggleSwitch");
            QVERIFY(check || sw);
            const auto checked = [check, sw] { return check ? check->isChecked() : sw->isChecked(); };
            link.inject("enable=1\n");
            QVERIFY(checked());
            link.inject("enable=3\n");
            QVERIFY(checked());
            link.inject("enable=2\n");
            QVERIFY(!checked());
            link.inject("enable=3\n");
            QVERIFY(!checked());
            link.inject("enable=1\n");
            QVERIFY(link.sent.isEmpty());
            if (check) {
                QVERIFY(check->isChecked());
                QVERIFY(!check->isTristate());
                check->click();
            } else {
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
        QVERIFY(link.setParser(QStringLiteral("modbus"), {}));
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
            if (!link.sent.isEmpty()) {
                const auto request = link.sent.first();
                const auto coils = QByteArray::fromHex("01010101");
                link.inject(request.at(1) == '\x01' ? coils + modbusCrc(coils) : request);
            }
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
        QVERIFY(link.setParser(QStringLiteral("modbus"), {}));
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
        link.inject(on);
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
        Project project;
        project.widgets = {
            QJsonObject{{"id", "switch"}, {"type", "ToggleSwitch"},
                        {"properties", QJsonObject{{"onCondition", "等于指定值"}, {"onValue", "5"}}}},
            QJsonObject{{"id", "serial"}, {"type", "SerialConfigCard"},
                        {"properties", QJsonObject{{"defaultParser", "modbus"}}}}};
        QVERIFY2(scene.loadWidgets(project.widgets, &error),
                 qPrintable(error));
        PropertyEditor editor;
        editor.setProject(&project);
        editor.setScene(&scene);
        editor.showItem(scene.findItem(QStringLiteral("switch")));
        editor.show();
        QApplication::processEvents();
        auto *condition = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("状态判定")));
        auto *onValue = qobject_cast<QLineEdit *>(fieldFor(&editor, QStringLiteral("开启判定值")));
        auto *offValue = qobject_cast<QLineEdit *>(fieldFor(&editor, QStringLiteral("关闭判定值")));
        QVERIFY(condition && onValue && offValue && onValue->isVisible() && offValue->isVisible());
        QCOMPARE(onValue->text(), QStringLiteral("5"));
        QVERIFY(offValue->text().isEmpty());
        offValue->setText(QStringLiteral("2"));
        const auto saved = scene.findItem(QStringLiteral("switch"))->toJson().value("properties").toObject();
        QCOMPARE(saved.value("offFeedbackValue").toString(), QStringLiteral("2"));
        condition->setCurrentText(QStringLiteral("非零为开"));
        QApplication::processEvents();
        QVERIFY(!onValue->isVisible());
        QVERIFY(!offValue->isVisible());
        condition->setCurrentText(QStringLiteral("等于指定值"));
        QApplication::processEvents();
        QVERIFY(onValue->isVisible() && offValue->isVisible());
        QCOMPARE(onValue->text(), QStringLiteral("5"));
        QCOMPARE(offValue->text(), QStringLiteral("2"));

        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("发送模式")));
        auto *area = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("写入区")));
        auto *summary = qobject_cast<QLabel *>(fieldFor(&editor, QStringLiteral("拨动时写入")));
        auto *preview = editor.findChild<QPushButton *>(QStringLiteral("sendPreview"));
        auto *onCommand = qobject_cast<QLineEdit *>(fieldFor(&editor, QStringLiteral("开启命令")));
        QVERIFY(mode && area && summary && preview && onCommand);
        QCOMPARE(mode->parentWidget(), preview->parentWidget());
        QCOMPARE(condition->parentWidget(), fieldFor(&editor, QStringLiteral("解析模式"))->parentWidget());
        QVERIFY(labelShown(mode->parentWidget(), QStringLiteral("操作与发送")));
        QVERIFY(labelShown(condition->parentWidget(), QStringLiteral("接收与绑定")));
        QVERIFY(onCommand->isVisible());
        onCommand->setText(QStringLiteral("ON"));
        mode->setCurrentText(QStringLiteral("Modbus"));
        QVERIFY(!onCommand->isVisible());
        QVERIFY(summary->isVisible());
        QVERIFY(summary->text().contains(QStringLiteral("开 0xFF00 · 关 0x0000")));
        area->setCurrentText(ModbusArea::HoldingRegisters);
        QVERIFY(summary->text().contains(QStringLiteral("开 1 · 关 0")));

        preview->click();
        auto *dialog = editor.findChild<QDialog *>(QStringLiteral("commandPreviewDialog"));
        QVERIFY(dialog);
        auto *output = dialog->findChild<QPlainTextEdit *>(QStringLiteral("commandPreviewOutput"));
        QVERIFY(output && output->isReadOnly());
        QVERIFY(output->toPlainText().contains(QStringLiteral("写入值 1")));
        QVERIFY(output->toPlainText().contains(QStringLiteral("写入值 0")));
        QVERIFY(link.sent.isEmpty());
        dialog->reject();
        mode->setCurrentText(QStringLiteral("文本"));
        QVERIFY(!summary->isVisible());
        QCOMPARE(onCommand->text(), QStringLiteral("ON"));
    }

    // The receive log shows text the way the link that received it spells it.
    void receiveLogDecodesWithTheLinksEncoding()
    {
        Project project;
        project.widgets = QJsonArray{QJsonObject{{"id", "log"},
                                                 {"type", "DataRecvCard"},
                                                 {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 600}, {"h", 300}}},
                                                 {"properties", QJsonObject{{"maxLines", 50}}}}};
        RuntimeSurface surface;
        surface.build(project);
        surface.show();
        FakeLink gbkLink;
        QString error;
        QVERIFY2(gbkLink.setParser(QStringLiteral("line"), QJsonObject{{"encoding", "gbk"}}, &error), qPrintable(error));
        FakeLink utf8Link;
        CommHub *hub = surface.context()->hub();
        hub->addLink(&gbkLink);
        hub->addLink(&utf8Link);
        auto *log = surface.findChild<QTextEdit *>(QStringLiteral("receivePacketLog"));
        QVERIFY(log);
        QSignalSpy edits(log->document(), &QTextDocument::contentsChanged);
        const QByteArray gbk = QByteArray::fromHex("cec2b6c8") + ":25.3;"; // 温度:25.3;
        gbkLink.inject(gbk);
        QCOMPARE(gbkLink.dataSource().encoding, QStringLiteral("gbk"));
        QCOMPARE(hub->decodeRaw(gbk, gbkLink.dataSource()), QStringLiteral("温度:25.3;"));
        utf8Link.inject(QByteArray::fromStdString("湿度=60\n"));
        QCOMPARE(utf8Link.dataSource().encoding, QStringLiteral("utf-8"));
        QVERIFY(log->document()->isEmpty());
        QTRY_COMPARE(log->document()->blockCount(), 2);
        QCOMPARE(edits.size(), 1);
        QVERIFY(log->toPlainText().contains(QStringLiteral("温度:25.3;")));
        QVERIFY(log->toPlainText().contains(QStringLiteral("湿度=60")));
        QCOMPARE(log->lineWrapMode(), QTextEdit::NoWrap);
        auto *format = surface.findChild<FluentQt::ComboBox *>(QStringLiteral("receiveLogFormat"));
        QVERIFY(format);
        format->setCurrentIndex(format->findData(QStringLiteral("hex")));
        QVERIFY(log->toPlainText().contains(QStringLiteral("CE C2 B6 C8 3A 32 35 2E 33 3B")));
        QVERIFY(log->toPlainText().contains(QStringLiteral("E6 B9 BF E5 BA A6 3D 36 30 0A")));
        format->setCurrentIndex(format->findData(QStringLiteral("text")));
        QVERIFY(log->toPlainText().contains(QStringLiteral("温度:25.3;")));
        QVERIFY(log->toPlainText().contains(QStringLiteral("湿度=60\\n")));
        format->setCurrentIndex(format->findData(QStringLiteral("auto")));
        FakeLink binaryLink;
        QVERIFY(binaryLink.setParser(QStringLiteral("hex"), {}));
        hub->addLink(&binaryLink, QStringLiteral("binary"));
        binaryLink.inject(QByteArray::fromHex("AA55010055AA"));
        QTRY_VERIFY(log->toPlainText().contains(QStringLiteral("AA 55 01 00 55 AA  [binary]")));

        auto *clear = surface.findChild<QPushButton *>(QStringLiteral("clearReceiveLog"));
        QVERIFY(clear);
        utf8Link.inject("previous=1\n");
        clear->click();
        utf8Link.inject("current=2\n");
        QTRY_VERIFY(log->toPlainText().contains(QStringLiteral("current=2")));
        QCOMPARE(log->document()->blockCount(), 1);
        QVERIFY(labelShown(&surface, QStringLiteral("收 1 条")));

        auto *sample = surface.findChild<QPushButton *>(QStringLiteral("inspectReceivedSample"));
        QVERIFY(sample && sample->isEnabled());
        log->setFocus();
        QTRY_VERIFY(log->hasFocus());
        auto selection = log->textCursor();
        selection.movePosition(QTextCursor::Start);
        selection.movePosition(QTextCursor::Right);
        log->setTextCursor(selection);
        QCOMPARE(sample->text(), QStringLiteral("试解析所选"));
        for (int i = 0; i < 60; ++i)
            utf8Link.inject("value=" + QByteArray::number(i) + '\n');
        QTRY_COMPARE(log->document()->blockCount(), 50);
        QVERIFY(log->document()->firstBlock().text().contains(QStringLiteral("value=10")));
        QCOMPARE(sample->text(), QStringLiteral("试解析"));
        QVERIFY(labelShown(&surface, QStringLiteral("收 61 条")));
        hub->removeLink(&binaryLink);
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
        surface.context()->hub()->addLink(&link, QStringLiteral("plc_link"));
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

        surface.context()->hub()->addLink(&link, QStringLiteral("plc_link")); // the coils at 0
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
        surface.context()->hub()->addLink(&link, QStringLiteral("plc_link"));
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
        controller.context()->hub()->addLink(&serial, QStringLiteral("SerialConfigCard_0b8ae5"));
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
        auto display = modbusDisplay("pv", 100, 0, 0);
        auto binding = display.value("binding").toObject();
        binding.insert("sourceCardId", DataRoute::Broadcast);
        display.insert("binding", binding);
        project.widgets =
            QJsonArray{display, tcpCard("a", 0, props(first, "modbus_tcp", 300, 50, 7)),
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
        QVERIFY(labelWith(card, polling)->toolTip().isEmpty());
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

    void canSignalRenameMigratesReferencesAndPreservesFeedbackPairs()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile dbc(directory.filePath(QStringLiteral("feedback.dbc")));
        QVERIFY(dbc.open(QIODevice::WriteOnly));
        const QByteArray definition = "BO_ 256 HMI: 8 Node\n SG_ ready : 16|1@1+ (1,0) [0|1] \"\" Node\n"
                                      "BO_ 257 IO: 8 Node\n SG_ rpm : 0|16@1+ (1,0) [0|8000] \"rpm\" Node\n"
                                      " SG_ ready : 16|1@1+ (1,0) [0|1] \"\" Node\n";
        QCOMPARE(dbc.write(definition), definition.size());
        dbc.close();
        const QString original = QStringLiteral("100 rpm : 0|16@1+ (1,0) [0|8000] \"rpm\"");
        const QString renamed = QStringLiteral("100 shaftRpm : 0|16@1+ (1,0) [0|8000] \"rpm\"");
        const QJsonObject binding{{"parseMode", ParseMode::CanField}, {"sourceCardId", "can"}, {"key", "0x100.rpm"}};
        const QJsonObject send{{"commandMode", "CAN"}, {"canId", "HMI"}, {"commandTargetId", "can"},
                               {"commandTemplate", "rpm={value}; ready=1"}, {"command", "1500"}};
        const auto widget = [](const QString &id, const QString &type, const QJsonObject &props = {},
                               const QJsonObject &binding = {}) {
            return QJsonObject{{"id", id}, {"type", type}, {"properties", props}, {"binding", binding}};
        };
        auto broadcast = send;
        broadcast.insert("commandTargetId", DataRoute::Broadcast);
        Project draft;
        draft.widgets = {widget("can", "CanConfigCard", {{"dbcFile", dbc.fileName()}, {"canSignals", original},
                                                          {"canTxInitialState", "HMI=IO"}}),
                         widget("other", "CanConfigCard"),
                         widget("display", "NumericDisplay", {}, binding),
                         widget("timing", "DigitalTimingChart", {{"timingSource", TimingSource::Channels},
                             {"_curve_0_parseMode", ParseMode::CanField}, {"_curve_0_sourceCardId", "can"},
                             {"_curve_0_key", "0x100.rpm"}}),
                         widget("send", "PushButton", send),
                         widget("broadcast", "PushButton", broadcast)};
        draft.logic.insert("points", QJsonArray{QJsonObject{{"name", "Speed"}, {"binding", binding}}});

        const auto rows = stageCanSignalEdit(draft, QStringLiteral("can"), {renamed, {{"0x100.rpm", "0x100.shaftRpm"}}});
        QCOMPARE(rows.size(), 6);
        const auto props = [&](int row) { return draft.widgets.at(row).toObject().value("properties").toObject(); };
        const auto point = draft.logic.value("points").toArray().first().toObject();
        QCOMPARE(point.value("binding").toObject().value("key").toString(), QStringLiteral("0x100.shaftRpm"));
        QCOMPARE(draft.widgets.at(2).toObject().value("binding").toObject().value("key").toString(),
                 QStringLiteral("0x100.shaftRpm"));
        QCOMPARE(props(3).value("_curve_0_key").toString(), QStringLiteral("0x100.shaftRpm"));
        QCOMPARE(props(4).value("commandTemplate").toString(), QStringLiteral("shaftRpm={value}; ready=1"));
        QCOMPARE(props(5).value("commandTemplate"), send.value("commandTemplate"));
        const auto mapping = props(0).value("canTxInitialState").toString().split(u'\n');
        QVERIFY(mapping.contains(QStringLiteral("HMI.shaftRpm=IO.rpm")));
        QVERIFY(mapping.contains(QStringLiteral("HMI.ready=IO.ready")));
        const auto set = CanSignalSet::build(designerCanDatabase(&draft, dbc.fileName()), renamed);
        QVERIFY(set);
        QVERIFY(CanInitialState::build(*set, mapping.join(u'\n'), nullptr));
        const auto cmd = CommandEncoder::buttonFromProperties(props(4), {});
        QVERIFY(cmd.command);
        const auto prepared = set->preparePayload(cmd.command->can);
        QVERIFY(prepared);
        QCOMPARE(prepared->frame.data.left(3), QByteArray::fromHex("DC0501"));
    }

    // Bindings and previews see CAN definitions in the current scene, before the project is saved.
    void bindingSeesUnsavedCanCard()
    {
        DesignScene scene;
        QString error;
        QVERIFY2(scene.loadWidgets(
                     QJsonArray{QJsonObject{{"id", "can"},
                                            {"type", "CanConfigCard"},
                                            {"properties", QJsonObject{{"canSignals", "123 LED_RED : 4|1@1+ (1,0) [0|1]"}}},
                                            {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 380}, {"h", 270}}}},
                                QJsonObject{{"id", "volt"},
                                            {"type", "NumericDisplay"},
                                            {"geometry", QJsonObject{{"x", 400}, {"y", 0}, {"w", 170}, {"h", 90}}}},
                                QJsonObject{{"id", "command"}, {"type", "PushButton"},
                                            {"properties", QJsonObject{{"commandMode", "CAN"}, {"commandTargetId", "can"},
                                                                       {"canId", "123"}, {"command", "1"},
                                                                       {"commandTemplate", "LED_REDD={value}"}}}}},
                     &error),
                 qPrintable(error));
        Project project; // saved before the card was there
        PropertyEditor editor;
        editor.setProject(&project);
        editor.setScene(&scene);
        editor.showItem(scene.findItem(QStringLiteral("volt")));
        auto *mode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("解析模式")));
        QVERIFY(mode);
        QVERIFY(mode->findData(ParseMode::CanSignal) >= 0);

        auto *command = scene.findItem(QStringLiteral("command"));
        editor.showItem(command);
        editor.show();
        auto *preview = editor.findChild<QPushButton *>(QStringLiteral("sendPreview"));
        QVERIFY(preview);
        const auto previewText = [&]() {
            preview->click();
            QPointer<QDialog> dialog = editor.findChild<QDialog *>(QStringLiteral("commandPreviewDialog"));
            auto *output = dialog ? dialog->findChild<QPlainTextEdit *>(QStringLiteral("commandPreviewOutput")) : nullptr;
            const QString text = output ? output->toPlainText() : QString();
            if (dialog) {
                dialog->reject();
                if (!QTest::qWaitFor([&] { return dialog.isNull(); }))
                    return QString();
            }
            return text;
        };
        QVERIFY(previewText().contains(QStringLiteral("没有信号 LED_REDD")));
        command->setPropertyValue(QStringLiteral("commandTemplate"), QStringLiteral("LED_RED={value}"));
        QVERIFY(previewText().contains(QStringLiteral("信号赋值 LED_RED=1")));
        command->setPropertyValue(QStringLiteral("command"), QStringLiteral("2"));
        QVERIFY(previewText().contains(QStringLiteral("超出")));
    }

    // HEX and Modbus publish complete frames. Line parsing retains the incoming raw units.
    void linkReportsWholeFrames()
    {
        FakeLink link;
        QString error;
        QVERIFY2(link.setParser(QStringLiteral("hex"), jsonObject(kHexConfigA), &error), qPrintable(error));
        QSignalSpy raw(&link, &Link::rawReceived);
        QSignalSpy wire(&link, &Link::wireReceived);
        QSignalSpy messages(&link, &Link::messageReceived);
        QByteArray bad = kFrameA;
        bad[bad.size() - 1] = char(bad.back() ^ 1);
        link.inject(bad);
        QCOMPARE(rawOf(wire), QList<QByteArray>{bad});
        QVERIFY(raw.isEmpty());
        QVERIFY(messages.isEmpty());
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

        raw.clear();
        QVERIFY(link.setParser(QStringLiteral("line"), {}));
        link.inject("a=1\nb=");
        QCOMPARE(rawOf(raw), QList<QByteArray>{QByteArray("a=1\nb=")});

        raw.clear();
        wire.clear();
        QVERIFY(link.setParser(QStringLiteral("modbus"), {}));
        const auto pdu = QByteArray::fromHex("010304000A0014");
        const auto response = pdu + modbusCrc(pdu);
        link.inject(response.left(4));
        QVERIFY(raw.isEmpty());
        link.inject(response.mid(4));
        QCOMPARE(rawOf(raw), QList<QByteArray>{response});
        QCOMPARE(rawOf(wire), (QList<QByteArray>{response.left(4), response.mid(4)}));
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
        QCOMPARE(labels, (QStringList{QStringLiteral("接收帧模板"), QStringLiteral("解析模式"),
                                      QStringLiteral("分隔符"), QStringLiteral("行结束符"),
                                      QStringLiteral("键值分隔符"), QStringLiteral("编码"), QStringLiteral("示例报文")}));

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
        for (const char *row : {"帧头(HEX)", "帧尾(HEX)", "帧长方式", "校验算法", "最大帧长(字节)", "组帧超时(ms)", "字段表"}) {
            QVERIFY2(labelShown(&editor, QString::fromUtf8(row)), row);
        }
        for (const char *row : {"分隔符", "行结束符", "键值分隔符", "编码"}) {
            QVERIFY2(!labelShown(&editor, QString::fromUtf8(row)), row);
        }
        QVERIFY(!labelShown(&editor, QStringLiteral("固定帧长")));
        QVERIFY(!labelShown(&editor, QStringLiteral("长度字段偏移")));
        auto *lengthMode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("帧长方式")));
        QVERIFY(lengthMode);
        lengthMode->setCurrentIndex(1);
        QVERIFY(labelShown(&editor, QStringLiteral("固定帧长")));
        QVERIFY(!labelShown(&editor, QStringLiteral("长度字段偏移")));
        lengthMode->setCurrentIndex(2);
        QVERIFY(!labelShown(&editor, QStringLiteral("固定帧长")));
        QCOMPARE(project.parserConfig().value("fixedLength").toInt(), 0);
        auto *offset = qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("长度字段偏移")));
        QCOMPARE(offset->minimum(), 0);
        offset->setValue(2);
        qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("校验算法")))->setCurrentText("CRC16-Modbus");
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

    void receiveLengthAliasSharesOneRowAndKeepsItsBindingKey()
    {
        const auto settings = jsonObject(R"json({"header":"AA55","footer":"","lengthOffset":2,
            "checksum":"CRC8","checksumStart":2,"fields":[["length",0,1,"uint8"],["cmd",1,1,"uint8"]]})json");
        QWidget host;
        host.resize(1100, 800);
        host.show();
        QString savedKey;
        openReceiveFramePreview(settings, &host, QByteArray::fromHex("AA550310DC05C5"),
                                [&savedKey](const QString &key, bool) { savedKey = key; });
        auto *dialog = host.findChild<AppDialog *>(QStringLiteral("receiveFramePreview"));
        QVERIFY(dialog);
        auto *preview = dialog->findChild<ReceivePreview *>();
        auto *table = dialog->findChild<QTableWidget *>(QStringLiteral("receiveFrameFields"));
        auto *sample = dialog->findChild<QPlainTextEdit *>(QStringLiteral("receiveFrameSample"));
        auto *save = dialog->findChild<QPushButton *>(QStringLiteral("saveReceivedField"));
        auto *bytes = dialog->findChild<FrameBytesView *>();
        QVERIFY(preview && table && sample && save && bytes);
        QCOMPARE(table->rowCount(), 4);
        const auto payloadCoverage = QStringLiteral("长度覆盖 · 整帧字节 3–5 · 10 DC 05");
        QVERIFY(labelShown(dialog, payloadCoverage));
        QVERIFY(labelShown(dialog, QStringLiteral("参与校验 · 整帧字节 2–5 · 03 10 DC 05")));
        QTest::mouseClick(bytes->viewport(), Qt::LeftButton, {}, QPoint(150, 40));
        QVERIFY(table->currentRow() >= 0);
        QCOMPARE(table->item(table->currentRow(), 0)->text(), QStringLiteral("长度（length）"));
        QCOMPARE(table->item(table->currentRow(), 4)->text(), QStringLiteral("3 字节"));
        QCOMPARE(preview->selectedHexField(), 0);
        QVERIFY(save->isEnabled());
        save->click();
        QCOMPARE(savedKey, QStringLiteral("length"));

        sample->setPlainText(QStringLiteral("AA 55 03 10 DC 05 C4"));
        preview->selectHexField(0);
        QCOMPARE(table->rowCount(), 4);
        QVERIFY(!save->isEnabled());
        QVERIFY(labelShown(dialog, payloadCoverage));
        auto *coverage = dialog->findChild<QLabel *>(QStringLiteral("frameLengthCoverage"));
        QVERIFY(coverage);
        sample->setPlainText(QStringLiteral("AA 55 04 10 DC 05 C5"));
        QVERIFY(!coverage->isVisible());

        preview->loadFrameSample(jsonObject(R"json({"header":"BEEF","footer":"","lengthOffset":2,
            "lengthSize":2,"lengthEndian":"小端(LE)","lengthCovers":"整帧",
            "fields":[["frameBytes",0,2,"uint16","小端(LE)"],["otherOrder",0,2,"uint16","大端(BE)"]]})json"),
            QStringLiteral("BE EF 06 00 12 34"));
        QCOMPARE(table->rowCount(), 3);
        QCOMPARE(table->item(1, 0)->text(), QStringLiteral("长度（frameBytes）"));
        QCOMPARE(table->item(1, 4)->text(), QStringLiteral("6 字节"));
        QCOMPARE(table->item(2, 0)->text(), QStringLiteral("otherOrder"));
        QCOMPARE(table->item(2, 4)->text(), QStringLiteral("1536"));
        QVERIFY(labelShown(dialog, QStringLiteral("长度覆盖 · 整帧字节 0–5 · BE EF 06 00 12 34")));

        preview->loadFrameSample(jsonObject(R"json({"header":"AA55","footer":"0D","lengthOffset":2,
            "lengthCovers":"长度字段之后","checksum":"异或","checksumStart":3})json"),
            QStringLiteral("AA 55 04 10 DC CC 0D"));
        QVERIFY(labelShown(dialog, QStringLiteral("长度覆盖 · 整帧字节 3–6 · 10 DC CC 0D")));
        QVERIFY(labelShown(dialog, QStringLiteral("参与校验 · 整帧字节 3–4 · 10 DC")));
    }

    // An item's rows follow its mode. A link card shows its own 帧格式 for the mode it picks,
    // starting from the project's when the modes match. A command input shows its write settings only.
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
                                       {"properties", QJsonObject{{"commandMode", "Modbus"}}}},
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
        auto *openPreview = editor.findChild<QPushButton *>(QStringLiteral("previewReceiveFrame"));
        QVERIFY(openPreview && openPreview->isVisible()); // inherits the project's HEX parser
        openPreview->click();
        auto receive = QPointer(editor.findChild<AppDialog *>(QStringLiteral("receiveFramePreview")));
        QVERIFY(receive);
        auto *receivedSample = receive->findChild<QPlainTextEdit *>(QStringLiteral("receiveFrameSample"));
        auto *decoded = receive->findChild<QTableWidget *>(QStringLiteral("receiveFrameFields"));
        auto *packet = receive->findChild<FrameBytesView *>();
        QVERIFY(receivedSample && decoded && packet);
        receivedSample->setPlainText(QStringLiteral("AA 55 04 01 F6 FF 3C 35"));
        QCOMPARE(packet->bytes(), kFrameA);
        QVERIFY(labelShown(receive, QStringLiteral("参与校验 · 整帧字节 0–6 · AA 55 04 01 F6 FF 3C")));
        QVERIFY(labelShown(receive, QStringLiteral("和校验 · 报文值 35 · 计算值 35 · 校验通过")));
        const auto temperatures = decoded->findItems(QStringLiteral("temp"), Qt::MatchExactly);
        QCOMPARE(temperatures.size(), 1);
        const int decodedRow = temperatures.first()->row();
        QCOMPARE(decoded->item(decodedRow, 2)->text(), QStringLiteral("4–5"));
        QCOMPARE(decoded->item(decodedRow, 4)->text(), QStringLiteral("-10"));
        QTest::mouseClick(packet->viewport(), Qt::LeftButton, {}, QPoint(8, 8));
        QCOMPARE(decoded->currentRow(), 0);
        receivedSample->setPlainText(QStringLiteral("AA 55 04 01 F6 FF 3C 34"));
        QVERIFY(labelShown(receive, QStringLiteral("和校验 · 报文值 34 · 计算值 35 · 校验值不匹配")));
        QVERIFY(decoded->currentItem());
        QCOMPARE(decoded->item(decoded->currentRow(), 4)->text(), QStringLiteral("校验值不匹配"));
        const auto invalidTemperatures = decoded->findItems(QStringLiteral("temp"), Qt::MatchExactly);
        QCOMPARE(invalidTemperatures.size(), 1);
        QVERIFY(decoded->item(invalidTemperatures.first()->row(), 4)->text().isEmpty());
        receive->yesButton()->click();
        QTRY_VERIFY(receive.isNull());
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
        QVERIFY(labelShown(&editor, QStringLiteral("示例报文")));
        QVERIFY(labelShown(&editor, QStringLiteral("帧长方式")));
        QVERIFY(!labelShown(&editor, QStringLiteral("固定帧长")));
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
        QVERIFY(!fieldFor(&editor, QStringLiteral("只读")));
        QVERIFY(!fieldFor(&editor, QStringLiteral("回车发送")));
        QVERIFY(!labelShown(&editor, QStringLiteral("接收与绑定")));

        const auto *setpoint = scene.findItem(QStringLiteral("setpoint"));
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus"), {}));
        RuntimeContext runtime;
        runtime.hub()->addLink(&link);
        auto input = std::unique_ptr<HmiWidget>(createHmiWidget(QStringLiteral("LineEdit"), setpoint->properties()));
        input->attach(&runtime);
        auto *inputField = input->findChild<QLineEdit *>();
        QVERIFY(inputField);
        inputField->setText(QStringLiteral("12"));
        QTest::keyClick(inputField, Qt::Key_Return);
        QVERIFY(link.sent.size() == 1 && link.sent.front().startsWith(QByteArray::fromHex("01060000000c")));
        QVERIFY(labelShown(&editor, QStringLiteral("操作与发送")));
        QVERIFY(labelShown(&editor, QStringLiteral("发送模式")));
        QVERIFY(labelShown(&editor, QStringLiteral("功能码")));
        auto *commandMode = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("发送模式")));
        QVERIFY(commandMode);
        QCOMPARE(commandMode->currentText(), QStringLiteral("Modbus"));
        QVERIFY(!labelShown(&editor, QStringLiteral("寄存器序")));
        auto *preview = editor.findChild<QPushButton *>(QStringLiteral("sendPreview"));
        QVERIFY(preview && preview->isVisible());
        preview->click();
        auto *dialog = editor.findChild<QDialog *>(QStringLiteral("commandPreviewDialog"));
        QVERIFY(dialog);
        auto *sample = qobject_cast<QLineEdit *>(fieldFor(dialog, QStringLiteral("示例值")));
        auto *output = dialog->findChild<QPlainTextEdit *>(QStringLiteral("commandPreviewOutput"));
        QVERIFY(sample && output);
        QCOMPARE(sample->accessibleName(), QStringLiteral("示例值"));
        sample->setText(QStringLiteral("12"));
        QVERIFY(output->toPlainText().contains(QStringLiteral("写入值 12")));
        dialog->reject();
        QCOMPARE(link.sent.size(), 1);
        runtime.hub()->removeLink(&link);

        // A Modbus binding names a register table and an address, and has its 采集周期(ms).
        editor.showItem(scene.findItem(QStringLiteral("pv")));
        QApplication::processEvents();
        auto *area = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("读取区")));
        QVERIFY(area);
        QCOMPARE(area->count(), 4);
        QCOMPARE(area->itemText(0), QStringLiteral("保持寄存器"));
        QCOMPARE(area->findText(QStringLiteral("响应内序号")), -1);
        QVERIFY(labelShown(&editor, QStringLiteral("读取地址")));
        QVERIFY(!labelTexts(&editor).contains(QStringLiteral("寄存器索引")));
        FluentQt::ComboBox *dataType = nullptr;
        for (auto *combo : editor.findChildren<FluentQt::ComboBox *>()) {
            if (combo->isVisible() && combo->findText(QStringLiteral("uint32")) >= 0)
                dataType = combo;
        }
        auto *regOrder = fieldFor(&editor, QStringLiteral("寄存器序"));
        QVERIFY(dataType && dataType->isVisible() && regOrder && regOrder->isHidden());
        dataType->setCurrentText(QStringLiteral("uint32"));
        QVERIFY(regOrder->isVisible());
        area->setCurrentText(QStringLiteral("线圈"));
        QVERIFY(dataType->isHidden() && regOrder->isHidden());
        area->setCurrentText(ModbusArea::HoldingRegisters);
        QCOMPARE(dataType->currentText(), QStringLiteral("uint32"));
        QVERIFY(regOrder->isVisible());
        QVERIFY(labelShown(&editor, QStringLiteral("采集周期(ms)")));
        auto *period = qobject_cast<QSpinBox *>(fieldFor(&editor, QStringLiteral("采集周期(ms)")));
        QVERIFY(period);
        QCOMPARE(period->maximum(), 60000);
        QCOMPARE(period->value(), 0);
        QCOMPARE(period->specialValueText(), QStringLiteral("默认周期"));
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

    // Holding-register bit data point shared as both read binding and write target for ToggleSwitch and PushButton,
    // plus PropertyEditor visibility for bit fields.
    void modbusRegisterBitReadWriteAndEditor()
    {
        Project project;
        project.widgets = QJsonArray{
            QJsonObject{{"id", "sim"}, {"type", "TcpConfigCard"},
                        {"properties", QJsonObject{{"defaultParser", "modbus_tcp"}, {"modbusRole", "从站"},
                                                   {"modbusSlave", 1}, {"slaveHrCount", 20},
                                                   {"slaveInit", "hr10=4"}}}},
            QJsonObject{{"id", "sw0"}, {"type", "ToggleSwitch"},
                        {"properties", QJsonObject{{"commandMode", "Modbus"}, {"mbWritePoint", "Relay0"}}},
                        {"binding", QJsonObject{{"parseMode", "工程数据点"}, {"key", "Relay0"}}}},
            QJsonObject{{"id", "btn1"}, {"type", "PushButton"},
                        {"properties", QJsonObject{{"commandMode", "Modbus"}, {"mbFunc", "06 写单寄存器"},
                                                   {"mbWritePoint", "Relay1"}, {"mbBitAction", "取反"}}}},
            QJsonObject{{"id", "led1"}, {"type", "LEDIndicator"},
                        {"binding", QJsonObject{{"parseMode", "工程数据点"}, {"key", "Relay1"}}}},
            QJsonObject{{"id", "led2"}, {"type", "LEDIndicator"},
                        {"binding", QJsonObject{{"parseMode", "Modbus"}, {"sourceCardId", "sim"},
                                                {"mbArea", "保持寄存器"}, {"mbRegIndex", 10}, {"mbBit", 2}}}}};
        project.logic = QJsonObject{{"points", QJsonArray{
            QJsonObject{{"name", "Relay0"}, {"binding", QJsonObject{
                {"parseMode", "Modbus"}, {"sourceCardId", "sim"}, {"mbArea", "保持寄存器"},
                {"mbRegIndex", 10}, {"mbBit", 0}}}},
            QJsonObject{{"name", "Relay1"}, {"binding", QJsonObject{
                {"parseMode", "Modbus"}, {"sourceCardId", "sim"}, {"mbArea", "保持寄存器"},
                {"mbRegIndex", 10}, {"mbBit", 1}}}}}}};

        RuntimeSurface surface;
        surface.build(project);
        QCOMPARE(surface.context()->hub()->modbusSlaves().size(), 1);
        ModbusSlave *slave = surface.context()->hub()->modbusSlaves().first();
        QApplication::processEvents();

        const auto widgetById = [&surface](const QString &id) -> HmiWidget * {
            for (HmiWidget *w : surface.widgets()) {
                if (w->property("fhmiWidgetId").toString() == id)
                    return w;
            }
            return nullptr;
        };
        auto *sw0 = widgetById("sw0")->findChild<FluentQt::SwitchButton *>("hmiToggleSwitch");
        auto *btn1 = widgetById("btn1")->findChild<QPushButton *>();
        QVERIFY(sw0 && btn1);
        QVERIFY(!sw0->isChecked());
        QVERIFY(surface.context()->projectData()->values().contains("Relay0"));
        QCOMPARE(surface.context()->projectData()->values().value("Relay0").toInt(), 0);
        QCOMPARE(surface.context()->projectData()->values().value("Relay1").toInt(), 0);

        // Toggle Relay0 ON -> sets bit 0 without touching bit 2 (4 -> 5).
        sw0->setChecked(true);
        QCOMPARE(slave->store().value(QStringLiteral("hr"), 10), quint16(5));
        QCOMPARE(surface.context()->projectData()->values().value("Relay0").toInt(), 1);
        QCOMPARE(surface.context()->projectData()->values().value("Relay1").toInt(), 0);

        // PushButton toggles Relay1 -> sets bit 1 (5 -> 7), then clears bit 1 (7 -> 5).
        btn1->click();
        QCOMPARE(slave->store().value(QStringLiteral("hr"), 10), quint16(7));
        QCOMPARE(surface.context()->projectData()->values().value("Relay1").toInt(), 1);
        btn1->click();
        QCOMPARE(slave->store().value(QStringLiteral("hr"), 10), quint16(5));
        QCOMPARE(surface.context()->projectData()->values().value("Relay1").toInt(), 0);

        // PropertyEditor hides mbDataType when binding.mbBit >= 0, shows ToggleSwitch mbBit only for 保持寄存器位,
        // and keeps PushButton mbBitAction visible when linked to a bit data point.
        PropertyEditor editor;
        editor.setProject(&project);
        editor.show();

        std::unique_ptr<CanvasItem> ledItem(CanvasItem::fromJson(project.widgets.at(4).toObject(), QString()));
        editor.showItem(ledItem.get());
        QApplication::processEvents();
        QWidget *bitSpin = fieldFor(&editor, QStringLiteral("位号"));
        QWidget *typeCombo = fieldFor(&editor, QStringLiteral("数据类型"));
        QVERIFY(bitSpin && !bitSpin->isHidden());
        QVERIFY(typeCombo && typeCombo->isHidden());

        // ToggleSwitch manual 保持寄存器位 vs 保持寄存器.
        std::unique_ptr<CanvasItem> swManual(CanvasItem::fromJson(QJsonObject{
            {"id", "swM"}, {"type", "ToggleSwitch"},
            {"properties", QJsonObject{{"commandMode", "Modbus"}, {"mbArea", "保持寄存器位"}, {"mbBit", 2}}}}, QString()));
        editor.showItem(swManual.get());
        QApplication::processEvents();
        auto hasVisibleLabel = [&editor](const QString &labelText) {
            for (auto *label : editor.findChildren<QLabel *>()) {
                if (label->text() == labelText && !label->isHidden())
                    return true;
            }
            return false;
        };
        QVERIFY(hasVisibleLabel(QStringLiteral("位号")));
        auto *areaCombo = qobject_cast<FluentQt::ComboBox *>(fieldFor(&editor, QStringLiteral("写入区")));
        QVERIFY(areaCombo);
        areaCombo->setCurrentIndex(areaCombo->findData(ModbusArea::HoldingRegisters));
        QApplication::processEvents();
        QVERIFY(!hasVisibleLabel(QStringLiteral("位号")));

        // PushButton linked to Relay1 (holding register bit): mbBitAction stays visible while mbBit is hidden.
        std::unique_ptr<CanvasItem> btnLinked(CanvasItem::fromJson(project.widgets.at(2).toObject(), QString()));
        editor.showItem(btnLinked.get());
        QApplication::processEvents();
        QVERIFY(hasVisibleLabel(QStringLiteral("位操作")));
        QVERIFY(!hasVisibleLabel(QStringLiteral("位号")));

        // FrameTemplateEditor for manual FC06 PushButton: setting modbusBitEditor switches to bit action & updates preview.
        std::unique_ptr<CanvasItem> btnManual(CanvasItem::fromJson(QJsonObject{
            {"id", "btnM"}, {"type", "PushButton"},
            {"properties", QJsonObject{{"commandMode", "Modbus"}, {"commandTargetId", "sim"},
                                       {"mbFunc", "06 写单寄存器"}, {"mbAddress", 10}}}}, QString()));
        editor.showItem(btnManual.get());
        QApplication::processEvents();
        auto *editBtn = editor.findChild<QPushButton *>(QStringLiteral("editFrameTemplate"));
        QVERIFY(editBtn && !editBtn->isHidden());
        editBtn->click();
        QPointer<AppDialog> dialog(editor.findChild<AppDialog *>(QStringLiteral("frameTemplateEditor")));
        QVERIFY(dialog);
        auto *bitEditor = dialog->findChild<QSpinBox *>(QStringLiteral("modbusBitEditor"));
        auto *preview = dialog->findChild<QPlainTextEdit *>(QStringLiteral("frameTemplatePreview"));
        QVERIFY(bitEditor && preview);
        bitEditor->setValue(3);
        QApplication::processEvents();
        QVERIFY(preview->toPlainText().contains(QStringLiteral("位 3 = 1")));
        dialog->yesButton()->click();
        QTRY_VERIFY(dialog.isNull());
        QCOMPARE(btnManual->propertyValue(QStringLiteral("mbBit")).toInt(), 3);
    }

    // CanMonitor filter accepts hex ID prefixes/substrings (e.g. "18FF" -> 0x18FF50E5), ranges, and DBC/inline
    // message names (case-insensitive), while rejecting inverted ranges without dumping every row.
    void canMonitorFuzzyIdAndNameFilter()
    {
        RuntimeContext context;
        context.setActive(true);
        std::unique_ptr<HmiWidget> widget(createHmiWidget("CanMonitor", {}));
        auto *monitor = dynamic_cast<CanMonitorWidget *>(widget.get());
        QVERIFY(monitor);
        monitor->attach(&context);
        auto *view = monitor->findChild<QTableView *>("canMessages");
        auto *filter = monitor->findChild<FluentQt::LineEdit *>("canIdFilter");
        QVERIFY(view && filter);

        QTemporaryDir dir;
        const QString dbcPath = dir.filePath(QStringLiteral("motor.dbc"));
        QFile dbc(dbcPath);
        QVERIFY(dbc.open(QIODevice::WriteOnly));
        dbc.write("BO_ 2566869221 MotorStatus: 8 ECU\n SG_ Speed : 0|16@1+ (1,0) [0|65535] \"rpm\" Vector__XXX\n");
        dbc.close();

        CanLink link;
        CanSettings settings;
        settings.adapter = CanAdapter::Virtual;
        settings.channel = QStringLiteral("fuzzy-filter");
        QString error;
        QVERIFY2(link.open(settings, dbcPath, {}, {}, &error), qPrintable(error));

        CanFrame ext;
        ext.id = 0x18FF50E5;
        ext.extended = true;
        ext.data = QByteArray(8, char(0x11));
        ext.timestampMs = 1700000000001LL;
        CanFrame std102;
        std102.id = 0x102;
        std102.data = QByteArray(2, char(0x22));
        std102.timestampMs = 1700000000002LL;
        CanFrame std200;
        std200.id = 0x200;
        std200.data = QByteArray(2, char(0x33));
        std200.timestampMs = 1700000000003LL;
        emit context.hub()->canFrames(&link, {ext, std102, std200}, false);
        QTRY_COMPARE(view->model()->rowCount(), 3);

        filter->setText(QStringLiteral("18FF"));
        QCOMPARE(view->model()->rowCount(), 1);
        QCOMPARE(view->model()->index(0, 2).data().toString(), QStringLiteral("0x18FF50E5"));
        QVERIFY(filter->toolTip().isEmpty());

        filter->setText(QStringLiteral("50e5"));
        QCOMPARE(view->model()->rowCount(), 1);

        filter->setText(QStringLiteral("motor"));
        QCOMPARE(view->model()->rowCount(), 1);
        QCOMPARE(view->model()->index(0, 2).data().toString(), QStringLiteral("0x18FF50E5"));
        QVERIFY(filter->toolTip().isEmpty());

        filter->setText(QStringLiteral("100-1FF, motor"));
        QCOMPARE(view->model()->rowCount(), 2);

        filter->setText(QStringLiteral("1FF-100"));
        QCOMPARE(view->model()->rowCount(), 0);
        QVERIFY(!filter->toolTip().isEmpty());

        link.close();
    }

    // Regression checks for P0 fixes: visibleWindow unwrapping, strict widget expression errors,
    // and DesignScene parent+child nudge deduplication.
    void p0VisibleWindowExpressionAndSceneNudge()
    {
        // 1. visibleWindow unwraps a QGraphicsProxyWidget to the hosting QGraphicsView's top-level window.
        QWidget hostWindow;
        QGraphicsScene proxyScene;
        QGraphicsView view(&proxyScene, &hostWindow);
        auto *embedded = new QWidget;
        auto *child = new QWidget(embedded);
        proxyScene.addWidget(embedded);
        QCOMPARE(visibleWindow(child), &hostWindow);

        // 2. Direct binding with a broken conversion expression flags ConversionFailed instead of showing raw value.
        Project project;
        project.widgets = QJsonArray{
            QJsonObject{{"id", "disp"}, {"type", "NumericDisplay"},
                        {"properties", QJsonObject{{"decimals", 1}}},
                        {"binding", QJsonObject{{"parseMode", "键值匹配"}, {"key", "raw"},
                                                {"expression", "value / 0"}}}}};
        RuntimeSurface surface;
        surface.build(project);
        surface.context()->setActive(true);
        HmiWidget *disp = nullptr;
        for (HmiWidget *w : surface.widgets()) {
            if (w->property("fhmiWidgetId").toString() == QLatin1String("disp"))
                disp = w;
        }
        QVERIFY(disp);
        emit surface.context()->hub()->unitReceived({}, Message{{"raw", 4095}}, DataSource{});
        for (const QLabel *label : disp->findChildren<QLabel *>()) {
            QVERIFY(!label->text().contains(QStringLiteral("4095")));
        }
        auto *badge = disp->findChild<QWidget *>("bindingQualityBadge");
        QVERIFY(badge && !badge->isHidden());
        QVERIFY(badge->accessibleDescription().contains(QStringLiteral("value / 0")));

        // 3. Arrow-key nudge on selected parent+child moves the child once, not twice.
        DesignScene scene;
        CanvasItem *card = scene.createItem(QStringLiteral("CardContainer"), QPointF(40, 40));
        CanvasItem *btn = scene.createItem(QStringLiteral("PushButton"), QPointF(60, 60));
        QVERIFY(card && btn);
        QCOMPARE(btn->containerParent(), card);
        card->setSelected(true);
        btn->setSelected(true);
        const QPointF cardBefore = card->scenePos();
        const QPointF btnBefore = btn->scenePos();
        QKeyEvent rightKey(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
        QCoreApplication::sendEvent(&scene, &rightKey);
        QCOMPARE(card->scenePos(), cardBefore + QPointF(1, 0));
        QCOMPARE(btn->scenePos(), btnBefore + QPointF(1, 0));
    }

    // Batch 2 UX & visualization regressions: container reorder/tab-move/outline, PropertyEditor S7/Modbus
    // enhancements, binding error badge tooltip, PlayerWindow monitor button, and runtime pause/clear controls.
    void batch2OutlinePropertyAndRuntimeToolbars()
    {
        // 1. CanvasItem child reordering & cross-tab move
        DesignScene scene;
        CanvasItem *vbox = scene.createItem(QStringLiteral("VBoxLayout"), QPointF(20, 20));
        QVERIFY(vbox);
        vbox->setSize(QSizeF(300, 260));
        CanvasItem *b1 = scene.createItem(QStringLiteral("PushButton"), QPointF(40, 40));
        CanvasItem *b2 = scene.createItem(QStringLiteral("PushButton"), QPointF(40, 80));
        QCOMPARE(b1->containerParent(), vbox);
        QCOMPARE(b2->containerParent(), vbox);
        QCOMPARE(vbox->containerChildren().first(), b1);
        QVERIFY(vbox->moveChildBy(b2, -1));
        QCOMPARE(vbox->containerChildren().first(), b2);
        vbox->removeChild(b2);
        QVERIFY(!b2->containerParent());

        CanvasItem *tabs = scene.createItem(QStringLiteral("TabWidget"), QPointF(400, 20));
        QVERIFY(tabs);
        tabs->setSize(QSizeF(300, 220));
        CanvasItem *tChild = scene.createItem(QStringLiteral("Label"), QPointF(430, 80));
        QCOMPARE(tChild->containerParent(), tabs);
        QCOMPARE(tChild->tabIndex(), 0);
        QVERIFY(tabs->moveChildToTab(tChild, 1));
        QCOMPARE(tChild->tabIndex(), 1);
        QCOMPARE(tabs->editorPage(), 1);

        // 2. PropertyEditor Modbus FC dropdown, 32-bit span hint, and S7 address builder button
        Project modbusProj;
        const QDir root(QStringLiteral(FHMI_EXAMPLES_DIR));
        {
            DesignerPage page;
            QVERIFY(page.openProject(root.filePath(QStringLiteral("tutorials/10-PLC恒压供水.uidp"))));
            auto *pgScene = page.findChild<DesignScene *>();
            QVERIFY(pgScene);
            for (CanvasItem *item : pgScene->allItems()) {
                if (item->widgetId() == QLatin1String("pressure"))
                    item->setSelected(true);
            }
            auto *fcBox = page.findChild<FluentQt::ComboBox *>(QStringLiteral("bindingMbFcFilter"));
            auto *span = page.findChild<QLabel *>(QStringLiteral("modbusRegisterSpanHint"));
            QVERIFY(fcBox);
            QVERIFY(span && !span->isHidden());
            QVERIFY(span->text().contains(QStringLiteral("100")));
            QVERIFY(span->text().contains(QStringLiteral("101")));
        }
        {
            DesignerPage page;
            QVERIFY(page.openProject(root.filePath(QStringLiteral("s7-demo.uidp"))));
            auto *pgScene = page.findChild<DesignScene *>();
            QVERIFY(pgScene);
            for (CanvasItem *item : pgScene->allItems()) {
                if (item->widgetId() == QLatin1String("temperature"))
                    item->setSelected(true);
            }
            QVERIFY(page.findChild<QToolButton *>(QStringLiteral("s7AddressBuilderButton")));
        }

        // 3. PlayerWindow places Project Data Monitor and Periodic Tasks buttons in its title bar (never outside canvas)
        {
            Project alarmProj;
            QString err;
            QVERIFY2(Project::load(root.filePath(QStringLiteral("tutorials/14-工程数据与报警.uidp")), &alarmProj, &err),
                     qPrintable(err));
            PlayerWindow player(alarmProj, nullptr, false);
            QVERIFY(player.titleBar()->findChild<QPushButton *>(QStringLiteral("playerDataMonitorButton")));
            QVERIFY(player.titleBar()->findChild<QPushButton *>(QStringLiteral("openPeriodicTasks")));
        }

        // 4. Binding error badge exposes tooltip and runtime data widgets support pause/clear
        Project rtProj;
        rtProj.widgets = QJsonArray{
            QJsonObject{{"id", "disp"}, {"type", "NumericDisplay"},
                        {"binding", QJsonObject{{"parseMode", "键值匹配"}, {"key", "v"}, {"expression", "value / 0"}}}},
            QJsonObject{{"id", "log"}, {"type", "LogBox"}},
            QJsonObject{{"id", "tbl"}, {"type", "DataTable"}},
            QJsonObject{{"id", "chart"}, {"type", "RealtimeChart"},
                        {"properties", QJsonObject{{"curveCount", 1}, {"_curve_0_key", "v"}}}}};
        RuntimeSurface surface;
        surface.build(rtProj);
        surface.context()->setActive(true);
        const auto byId = [&](const char *id) -> HmiWidget * {
            for (HmiWidget *w : surface.widgets())
                if (w->property("fhmiWidgetId").toString() == QLatin1String(id))
                    return w;
            return nullptr;
        };
        HmiWidget *disp = byId("disp");
        HmiWidget *logW = byId("log");
        HmiWidget *tblW = byId("tbl");
        HmiWidget *chartW = byId("chart");
        QVERIFY(disp && logW && tblW && chartW);
        emit surface.context()->hub()->unitReceived({}, Message{{"v", 42}}, DataSource{});
        auto *badge = disp->findChild<QWidget *>(QStringLiteral("bindingQualityBadge"));
        QVERIFY(badge && !badge->isHidden());
        QVERIFY(badge->toolTip().contains(QStringLiteral("value / 0")));
        QVERIFY(!badge->testAttribute(Qt::WA_TransparentForMouseEvents));

        auto *logPause = logW->findChild<QToolButton *>(QStringLiteral("logPause"));
        auto *logClear = logW->findChild<QToolButton *>(QStringLiteral("logClear"));
        auto *logEdit = logW->findChild<QTextEdit *>();
        QVERIFY(logPause && logClear && logEdit);
        logPause->setChecked(true);
        const QString beforePaused = logEdit->toPlainText();
        static_cast<LogBoxWidget *>(logW)->appendLog(QStringLiteral("ignored-while-paused"));
        QCOMPARE(logEdit->toPlainText(), beforePaused);
        logPause->setChecked(false);
        static_cast<LogBoxWidget *>(logW)->appendLog(QStringLiteral("visible-after-resume"));
        QVERIFY(logEdit->toPlainText().contains(QStringLiteral("visible-after-resume")));
        logClear->click();
        QVERIFY(logEdit->toPlainText().isEmpty());

        auto *tblPause = tblW->findChild<QToolButton *>(QStringLiteral("tablePause"));
        auto *tblClear = tblW->findChild<QToolButton *>(QStringLiteral("tableClear"));
        auto *chartPause = chartW->findChild<QToolButton *>(QStringLiteral("chartPause"));
        auto *chartClear = chartW->findChild<QToolButton *>(QStringLiteral("chartClear"));
        QVERIFY(tblPause && tblClear && chartPause && chartClear);
    }

    void batch3ExportAndCommRobustness()
    {
        // 1. ExportService upfront asset validation and safe overwrite
        if (!ExportService::runtimePath().isEmpty()) {
            QTemporaryDir outDir;
            QVERIFY(outDir.isValid());
            Project broken;
            broken.appName = QStringLiteral("BrokenAssetApp");
            broken.widgets.append(QJsonObject{
                {"id", "img1"}, {"type", "ImageDisplay"}, {"properties", QJsonObject{{"imagePath", "missing.png"}}}});
            QString error;
            QVERIFY(ExportService::exportApplication(broken, outDir.path(), &error).isEmpty());
            QVERIFY(error.contains(QStringLiteral("missing.png")));
            QVERIFY(!QFileInfo::exists(ExportService::targetPath(broken, outDir.path())));
            QVERIFY(!QFileInfo::exists(ExportService::targetPath(broken, outDir.path()) + QStringLiteral(".partial")));

            Project valid;
            valid.appName = QStringLiteral("OverwriteApp");
            error.clear();
            const QString first = ExportService::exportApplication(valid, outDir.path(), &error);
            QVERIFY2(!first.isEmpty(), qPrintable(error));
            QVERIFY(ExportService::exportApplication(valid, outDir.path(), &error, false).isEmpty());
            const QString second = ExportService::exportApplication(valid, outDir.path(), &error, true);
            QVERIFY2(!second.isEmpty(), qPrintable(error));
            QCOMPARE(second, first);
            QVERIFY(!QFileInfo::exists(first + QStringLiteral(".old")));
        }

        // 2. TcpServerLink rejects invalid listen address instead of silently binding to Any
        {
            TcpServerLink server;
            QString error;
            QVERIFY(!server.listen(QStringLiteral("not-an-ip-address"), 0, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!server.isConnected());
        }

        // 3. DesignerPage remembers opened project in recent list and last directory
        {
            const QDir root(QStringLiteral(FHMI_EXAMPLES_DIR));
            const QString demoPath = QDir::cleanPath(QFileInfo(root.filePath(QStringLiteral("demo.uidp"))).absoluteFilePath());
            DesignerPage page;
            QVERIFY(page.openProject(demoPath));
            const QStringList recent = QSettings().value(QStringLiteral("studio/recentProjects")).toStringList();
            QVERIFY(!recent.isEmpty());
            QCOMPARE(recent.first(), demoPath);
        }
    }

    void horizontalWheelScrollAndCanDemoRouting()
    {
        // 1. CanMonitorWidget supports Shift+wheel and Ctrl+wheel horizontal scrolling
        Project proj;
        proj.widgets = QJsonArray{QJsonObject{
            {"id", "mon"}, {"type", "CanMonitor"}, {"geometry", QJsonObject{{"x", 0}, {"y", 0}, {"w", 320}, {"h", 220}}}}};
        RuntimeSurface surface;
        surface.build(proj);
        surface.resize(400, 300);
        surface.show();
        QApplication::processEvents();
        auto *view = surface.findChild<QTableView *>(QStringLiteral("canMessages"));
        QVERIFY(view);
        QScrollBar *hbar = view->horizontalScrollBar();
        QVERIFY(hbar);
        QVERIFY(hbar->maximum() > 0);
        QCOMPARE(hbar->value(), 0);

        const QPointF pos(40, 40);
        QWheelEvent ctrlDown(pos, view->viewport()->mapToGlobal(pos), QPoint(), QPoint(0, -120), Qt::NoButton,
                             Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view->viewport(), &ctrlDown);
        QVERIFY(ctrlDown.isAccepted());
        const int afterCtrl = hbar->value();
        QVERIFY(afterCtrl > 0);

        QWheelEvent shiftDown(pos, view->viewport()->mapToGlobal(pos), QPoint(), QPoint(0, -120), Qt::NoButton,
                              Qt::ShiftModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view->viewport(), &shiftDown);
        QVERIFY(shiftDown.isAccepted());
        QVERIFY(hbar->value() > afterCtrl);

        QWheelEvent shiftUp(pos, view->viewport()->mapToGlobal(pos), QPoint(), QPoint(0, 240), Qt::NoButton,
                            Qt::ShiftModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(view->viewport(), &shiftUp);
        QVERIFY(shiftUp.isAccepted());
        QVERIFY(hbar->value() < afterCtrl);

        // 2. 12-CAN收发测试.uidp routes TX to can1 and RX displays to can2 (avoiding Ambiguous source)
        Project canDemo;
        QString err;
        const QDir root(QStringLiteral(FHMI_EXAMPLES_DIR));
        QVERIFY2(Project::load(root.filePath(QStringLiteral("tutorials/12-CAN收发测试.uidp")), &canDemo, &err),
                 qPrintable(err));
        int txCount = 0;
        int rxCount = 0;
        for (const QJsonValue &v : canDemo.widgets) {
            const QJsonObject w = v.toObject();
            const QJsonObject props = w.value(QStringLiteral("properties")).toObject();
            if (props.contains(QStringLiteral("commandTargetId"))) {
                QCOMPARE(props.value(QStringLiteral("commandTargetId")).toString(), QStringLiteral("can1"));
                ++txCount;
            }
            const QJsonObject binding = w.value(QStringLiteral("binding")).toObject();
            if (binding.contains(QStringLiteral("sourceCardId"))) {
                QCOMPARE(binding.value(QStringLiteral("sourceCardId")).toString(), QStringLiteral("can2"));
                ++rxCount;
            }
        }
        QVERIFY(txCount >= 5);
        QVERIFY(rxCount >= 5);
    }

    void projectDataMonitorStandaloneAndThrottled()
    {
        Project proj;
        QString err;
        const QDir root(QStringLiteral(FHMI_EXAMPLES_DIR));
        QVERIFY2(Project::load(root.filePath(QStringLiteral("tutorials/14-工程数据与报警.uidp")), &proj, &err),
                 qPrintable(err));
        PlayerWindow player(proj, nullptr, true);
        player.show();
        QApplication::processEvents();

        auto *btn = player.titleBar()->findChild<QPushButton *>(QStringLiteral("playerDataMonitorButton"));
        QVERIFY(btn);
        btn->click();
        QApplication::processEvents();

        const auto findMonitor = []() -> QWidget * {
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (w->objectName() == QLatin1String("projectDataMonitor") && w->isVisible())
                    return w;
            }
            return nullptr;
        };
        QPointer<QWidget> first = findMonitor();
        QVERIFY(first);
        QVERIFY(first->isWindow());
        QVERIFY(first->parentWidget() == nullptr); // decoupled from PlayerWindow's QGraphicsView

        // Second click reuses the existing monitor window instead of stacking duplicates.
        btn->click();
        QApplication::processEvents();
        int count = 0;
        for (QWidget *w : QApplication::topLevelWidgets()) {
            if (w->objectName() == QLatin1String("projectDataMonitor") && w->isVisible())
                ++count;
        }
        QCOMPARE(count, 1);
        QCOMPARE(findMonitor(), first.data());

        // Rapid updates are coalesced and shown after the 150ms flush interval.
        auto *data = player.surface()->context()->projectData();
        for (int i = 1; i <= 50; ++i) {
            data->setSample(QStringLiteral("电量"), i, {}, DataQuality::Current, QDateTime::currentDateTimeUtc());
            data->evaluate();
        }
        auto *table = first->findChild<QTableWidget *>(QStringLiteral("projectDataReadings"));
        QVERIFY(table);
        QTRY_COMPARE_WITH_TIMEOUT(table->item(2, 1)->text(), QStringLiteral("50"), 1000);

        // Closing PlayerWindow automatically closes the monitor window.
        player.close();
        QApplication::processEvents();
        QVERIFY(!first || !first->isVisible());
    }

    void aiAssistantPatchExtractApplyAndUndo()
    {
        DesignerPage page;
        page.show();
        QApplication::processEvents();

        auto *rightTabs = page.findChild<FluentQt::SegmentedWidget *>(QStringLiteral("rightPanelTabs"));
        auto *aiPanel = page.findChild<AiAssistantPanel *>(QStringLiteral("aiAssistantPanel"));
        auto *scene = page.findChild<DesignScene *>();
        QVERIFY(rightTabs && aiPanel && scene);
        QCOMPARE(page.snapshot().widgets.size(), 0);

        // 1. Extract and validate a patch that creates a current GaugeMeter (-2000..2000, 60A)
        const QString replyCreate = QStringLiteral(
            "为你创建一个电流仪表盘：\n"
            "```fhmi-patch\n"
            "{\n"
            "  \"title\": \"新增电流仪表盘\",\n"
            "  \"upsertWidgets\": [\n"
            "    {\n"
            "      \"id\": \"gaugeMeter1\",\n"
            "      \"type\": \"GaugeMeter\",\n"
            "      \"geometry\": {\"x\": 40, \"y\": 40, \"w\": 180, \"h\": 180},\n"
            "      \"properties\": {\"title\": \"电流\", \"unit\": \"A\", \"minimum\": -2000, \"maximum\": 2000, \"value\": 60}\n"
            "    }\n"
            "  ]\n"
            "}\n"
            "```");
        const AiPatchProposal createProposal = AiService::extractPatch(replyCreate, page.snapshot());
        QVERIFY(createProposal.hasPatch);
        QVERIFY2(createProposal.valid, qPrintable(createProposal.error));
        QCOMPARE(createProposal.patchedProject.widgets.size(), 1);
        QCOMPARE(createProposal.touchedWidgetIds, QStringList{QStringLiteral("gaugeMeter1")});

        // Apply through AiAssistantPanel signal -> DesignerPage undo stack
        emit aiPanel->applyProjectRequested(createProposal.patchedProject, createProposal.touchedWidgetIds);
        QApplication::processEvents();
        QCOMPARE(page.snapshot().widgets.size(), 1);
        QVERIFY(page.canUndo());
        CanvasItem *gauge = scene->findItem(QStringLiteral("gaugeMeter1"));
        QVERIFY(gauge);
        QCOMPARE(gauge->propertyValue(QStringLiteral("minimum")).toDouble(), -2000.0);
        QCOMPARE(gauge->propertyValue(QStringLiteral("maximum")).toDouble(), 2000.0);
        QCOMPARE(gauge->propertyValue(QStringLiteral("value")).toDouble(), 60.0);

        // 2. Partial update patch on existing widget (only id + changed property)
        const QString replyUpdate = QStringLiteral(
            "将最小值改为 0：\n"
            "```fhmi-patch\n"
            "{\n"
            "  \"title\": \"修改电流仪表盘量程\",\n"
            "  \"upsertWidgets\": [\n"
            "    {\n"
            "      \"id\": \"gaugeMeter1\",\n"
            "      \"properties\": {\"minimum\": 0}\n"
            "    }\n"
            "  ]\n"
            "}\n"
            "```");
        const AiPatchProposal updateProposal = AiService::extractPatch(replyUpdate, page.snapshot());
        QVERIFY(updateProposal.hasPatch && updateProposal.valid);
        emit aiPanel->applyProjectRequested(updateProposal.patchedProject, updateProposal.touchedWidgetIds);
        QApplication::processEvents();
        gauge = scene->findItem(QStringLiteral("gaugeMeter1"));
        QVERIFY(gauge);
        QCOMPARE(gauge->propertyValue(QStringLiteral("minimum")).toDouble(), 0.0);
        QCOMPARE(gauge->propertyValue(QStringLiteral("maximum")).toDouble(), 2000.0);

        // 3. Undo both AI steps via DesignerPage::undo()
        page.undo();
        QApplication::processEvents();
        gauge = scene->findItem(QStringLiteral("gaugeMeter1"));
        QVERIFY(gauge);
        QCOMPARE(gauge->propertyValue(QStringLiteral("minimum")).toDouble(), -2000.0);

        page.undo();
        QApplication::processEvents();
        QCOMPARE(page.snapshot().widgets.size(), 0);

        // 4. Patch with shorthand parseMode ("键值"), data points, alarms, and RealtimeChart curves
        const QString replyComplex = QStringLiteral(
            "```fhmi-patch\n"
            "{\n"
            "  \"title\": \"储能电池充放电监控台\",\n"
            "  \"upsertWidgets\": [\n"
            "    {\"id\": \"serial1\", \"type\": \"SerialConfigCard\", \"geometry\": {\"x\": 20, \"y\": 440, \"w\": 360, \"h\": 320}},\n"
            "    {\"id\": \"gauge1\", \"type\": \"GaugeMeter\", \"geometry\": {\"x\": 20, \"y\": 140, \"w\": 180, \"h\": 180},\n"
            "     \"properties\": {\"title\": \"电流\", \"unit\": \"A\", \"minimum\": 0, \"maximum\": 2000, \"value\": 125},\n"
            "     \"binding\": {\"parseMode\": \"键值\", \"key\": \"current\", \"expression\": \"x * 0.1\"}},\n"
            "    {\"id\": \"chart1\", \"type\": \"RealtimeChart\", \"geometry\": {\"x\": 420, \"y\": 140, \"w\": 400, \"h\": 280},\n"
            "     \"properties\": {\"title\": \"充放电趋势\", \"curveCount\": 2, \"_curve_0_name\": \"电压\", \"_curve_0_parseMode\": \"键值\", \"_curve_0_key\": \"voltage\", \"_curve_1_name\": \"电流\", \"_curve_1_parseMode\": \"键值\", \"_curve_1_key\": \"current\"}},\n"
            "    {\"id\": \"alarms1\", \"type\": \"AlarmList\", \"geometry\": {\"x\": 420, \"y\": 440, \"w\": 740, \"h\": 320}}\n"
            "  ],\n"
            "  \"points\": [\n"
            "    {\"name\": \"current\", \"key\": \"current\"}\n"
            "  ],\n"
            "  \"alarms\": [\n"
            "    {\"id\": \"过流报警\", \"condition\": \"current > 1500\", \"level\": \"紧急\", \"message\": \"电流超过 1500A\"}\n"
            "  ]\n"
            "}\n"
            "```");
        const AiPatchProposal complexProposal = AiService::extractPatch(replyComplex, page.snapshot());
        QVERIFY(complexProposal.hasPatch);
        QVERIFY2(complexProposal.valid, qPrintable(complexProposal.error));
        QCOMPARE(complexProposal.patchedProject.widgets.size(), 4);
        QCOMPARE(complexProposal.patchedProject.logic.value(QStringLiteral("points")).toArray().size(), 1);
        QCOMPARE(complexProposal.patchedProject.logic.value(QStringLiteral("alarms")).toArray().size(), 1);
    }
};

QTEST_MAIN(TestRuntime)
#include "tst_runtime.moc"
