#include "runtime/widgets/can_widgets.h"

#include "comm/can/can_link.h"
#include "core/can_timing.h"
#include "core/hex_utils.h"
#include "runtime/runtime_context.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/FluentIcon.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/Menu.h>
#include <FluentQtWidgets/Widgets/Selection.h>

#include <QtCore/QAbstractTableModel>
#include <QtCore/QDir>
#include <QtCore/QLocale>
#include <QtCore/QSortFilterProxyModel>
#include <QtCore/QTimer>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <memory>
#include <tuple>

namespace fhmi {

namespace {

FluentQt::ComboBox *comboOf(QWidget *parent, const QStringList &values)
{
    auto *combo = new FluentQt::ComboBox(parent);
    addValueItems(combo, values);
    return combo;
}

QStringList enumOptions(const WidgetMeta &meta, const char *name)
{
    const PropDef *def = meta.prop(QLatin1String(name));
    return def ? def->options : QStringList();
}

QStringList numbers(int last)
{
    QStringList out;
    for (int i = 0; i <= last; ++i) {
        out << QString::number(i);
    }
    return out;
}

// Counters with thousands separators, the same in every language.
QString count(quint64 value)
{
    return QLocale(QLocale::English, QLocale::UnitedStates).toString(qulonglong(value));
}

constexpr int kStatusRefreshMs = 250;
constexpr int kMonitorRefreshMs = 100;

} // namespace

// ---------------------------------------------------------------------------
// CanConfigCard
// ---------------------------------------------------------------------------

CanConfigCardWidget::CanConfigCardWidget(const WidgetMeta &meta, QWidget *parent) : LinkCardBase(meta, parent)
{
    m_parser->hide(); // a CAN link parses no bytes
    m_adapter = comboOf(this, enumOptions(meta, "defaultAdapter"));

    m_channel = new FluentQt::EditableComboBox(this);
    m_channel->setPlaceholderText(tr("选择或输入通道"));
    m_refresh = new FluentQt::ToolButton(this);
    m_refresh->setIcon(FluentQt::icon(FluentQt::FluentIcon::Sync));
    fluentToolTip(m_refresh, tr("搜索通道"));
    keepSquareBeside(m_refresh, m_channel);
    m_channelRow = row({{m_channel, 1}, {m_refresh, 0}});

    m_zlgModel = comboOf(this, enumOptions(meta, "zlgModel"));
    m_device = comboOf(this, numbers(15));
    m_zlgChannel = comboOf(this, numbers(7));
    fluentToolTip(m_device, tr("同型号插了多个时的序号，从 0 开始"));
    fluentToolTip(m_zlgChannel, tr("设备上的第几路 CAN，从 0 开始"));
    // A ZLG model sits beside the adapter, its device index and channel in the channel row: in one row the three
    // would not fit a 380 px card.
    for (FluentQt::ComboBox *combo : {m_adapter, m_zlgModel}) {
        combo->setSizePolicy(QSizePolicy::Ignored, combo->sizePolicy().verticalPolicy());
    }
    m_adapterRow = row({{m_adapter, 1}, {m_zlgModel, 2}});
    m_zlgRow = row({{new FluentQt::CaptionLabel(tr("设备"), this), 0},
                    {m_device, 1},
                    {new FluentQt::CaptionLabel(QStringLiteral("CAN"), this), 0},
                    {m_zlgChannel, 1}});
    // One grid row for both, so switching adapters never changes the card's height.
    m_targetRow = new QWidget(this);
    auto *targetLayout = new QHBoxLayout(m_targetRow);
    targetLayout->setContentsMargins(0, 0, 0, 0);
    m_channelRow->setParent(m_targetRow);
    m_zlgRow->setParent(m_targetRow);
    targetLayout->addWidget(m_channelRow);
    targetLayout->addWidget(m_zlgRow);
    m_targetRow->hide();

    m_bitrate = comboOf(this, enumOptions(meta, "defaultBitrate"));
    m_samplePoint = comboOf(this, enumOptions(meta, "defaultSamplePoint"));
    m_bitrateRow = row({{m_bitrate, 1}, {new FluentQt::CaptionLabel(tr("采样点"), this), 0}, {m_samplePoint, 1}});

    m_fd = new FluentQt::CheckBox(QStringLiteral("FD"), this);
    m_dataBitrate = comboOf(this, enumOptions(meta, "defaultDataBitrate"));
    m_dataSamplePoint = comboOf(this, enumOptions(meta, "defaultDataSamplePoint"));
    m_fdRow = row({{m_fd, 0}, {m_dataBitrate, 1}, {new FluentQt::CaptionLabel(tr("采样点"), this), 0},
                   {m_dataSamplePoint, 1}});

    // Disabled fields say why in their tooltip.
    for (QWidget *w : {static_cast<QWidget *>(m_bitrate), static_cast<QWidget *>(m_samplePoint),
                       static_cast<QWidget *>(m_fd), static_cast<QWidget *>(m_dataBitrate),
                       static_cast<QWidget *>(m_dataSamplePoint), static_cast<QWidget *>(m_connect)}) {
        fluentToolTip(w);
    }

    m_hint = new FluentQt::CaptionLabel(this);
    m_hint->setWordWrap(true);
    m_hint->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_hint->hide();

    // Under the fields, in the room the card has there (the base layout is header, fields, stretch).
    m_detail = new FluentQt::CaptionLabel(this);
    m_detail->setWordWrap(true);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detail->hide();
    static_cast<QVBoxLayout *>(layout())->insertWidget(2, m_detail);

    m_statusTimer = new QTimer(this);
    m_statusTimer->setInterval(kStatusRefreshMs);
    connect(m_statusTimer, &QTimer::timeout, this, &CanConfigCardWidget::showConnection);
    connect(m_adapter, &FluentQt::ComboBox::currentIndexChanged, this, [this]() {
        updateAdapterUi();
        if (isLive()) {
            refreshChannels();
        }
    });
    connect(m_zlgModel, &FluentQt::ComboBox::currentIndexChanged, this, &CanConfigCardWidget::updateAdapterUi);
    connect(m_fd, &QCheckBox::toggled, this, &CanConfigCardWidget::updateAdapterUi);
    for (FluentQt::ComboBox *combo : {m_bitrate, m_samplePoint, m_dataBitrate, m_dataSamplePoint}) {
        connect(combo, &FluentQt::ComboBox::currentIndexChanged, this, &CanConfigCardWidget::updateAdapterUi);
    }
    connect(m_channel, &FluentQt::EditableComboBox::currentTextChanged, this, &CanConfigCardWidget::updateAdapterUi);
    connect(m_refresh, &QToolButton::clicked, this, [this]() { refreshChannels(true); });
}

CanConfigCardWidget::~CanConfigCardWidget()
{
    detach();
}

QWidget *CanConfigCardWidget::row(std::initializer_list<QPair<QWidget *, int>> widgets)
{
    auto *container = new QWidget(this);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    for (const auto &entry : widgets) {
        entry.first->setParent(container);
        layout->addWidget(entry.first, entry.second);
    }
    container->hide();
    return container;
}

void CanConfigCardWidget::apply()
{
    m_title->setText(text("title"));
    selectValue(m_adapter, text("defaultAdapter"));
    selectValue(m_zlgModel, text("zlgModel"));
    selectValue(m_device, QString::number(std::clamp(integer("defaultDeviceIndex"), 0, 15)));
    selectValue(m_zlgChannel, QString::number(std::clamp(integer("defaultChannel"), 0, 7)));
    selectValue(m_bitrate, text("defaultBitrate"));
    selectValue(m_samplePoint, text("defaultSamplePoint"));
    m_fd->setChecked(flag("canFd"));
    selectValue(m_dataBitrate, text("defaultDataBitrate"));
    selectValue(m_dataSamplePoint, text("defaultDataSamplePoint"));
    updatePolling();

    clearFields();
    m_fdRow->hide();
    m_hint->hide();
    addField(tr("适配器"), m_adapterRow);
    addField(tr("波特率"), m_bitrateRow);
    if (text("displayMode") != QStringLiteral("简洁")) { // i18n: value
        addField(tr("数据域"), m_fdRow);
    }
    // SocketCAN's command: shown only for that adapter (updateAdapterUi()).
    addField(QStringLiteral("ip link"), m_hint, true);
    m_hintCaption = m_fieldLabels.last();
    // The channel comes last so that 连接 ends its row, as on the other cards, whichever adapter hides which rows.
    addField(tr("通道"), m_targetRow);
    finishFields();
    updateAdapterUi();
}

CanSettings CanConfigCardWidget::settings() const
{
    CanSettings s;
    s.adapter = selectedValue(m_adapter);
    const QString typed = m_channel->currentText().trimmed();
    // A listed channel stores its id as data (a PCAN handle); a typed one (a pty, vcan0) is itself.
    const int index = m_channel->findText(typed);
    s.channel = index >= 0 && m_channel->itemData(index).isValid() ? m_channel->itemData(index).toString() : typed;
    s.zlgModel = selectedValue(m_zlgModel);
    s.deviceIndex = selectedValue(m_device).toInt();
    s.channelIndex = selectedValue(m_zlgChannel).toInt();
    s.bitrate = CanBitTiming::bitrateValue(selectedValue(m_bitrate));
    s.samplePoint = CanBitTiming::samplePointValue(selectedValue(m_samplePoint));
    s.fd = m_fd->isChecked();
    s.dataBitrate = CanBitTiming::bitrateValue(selectedValue(m_dataBitrate));
    s.dataSamplePoint = CanBitTiming::samplePointValue(selectedValue(m_dataSamplePoint));
    s.brs = flag("canBrs");
    s.customTiming = text("customTiming").trimmed();
    s.listenOnly = flag("listenOnly");
    s.termination = flag("termination");
    return s;
}

void CanConfigCardWidget::updateAdapterUi()
{
    const QString adapter = selectedValue(m_adapter);
    const CanAdapterCaps caps = CanAdapter::caps(adapter, selectedValue(m_zlgModel));
    const bool zlg = adapter == CanAdapter::Zlg;
    m_channelRow->setVisible(!zlg);
    m_zlgRow->setVisible(zlg);
    m_zlgModel->setVisible(zlg);
    const bool editable = !(m_link && m_link->isConnected()); // settings are locked while connected
    const bool custom = !text("customTiming").trimmed().isEmpty() && caps.customTiming;
    const QString customTip = tr("使用卡片的自定义位时序");
    const auto field = [editable](QWidget *widget, bool settable, const QString &why) {
        widget->setEnabled(editable && settable);
        widget->setToolTip(settable ? QString() : why);
    };
    field(m_bitrate, caps.bitrate && !custom, custom ? customTip : caps.fixedTimingReason);
    field(m_samplePoint, caps.samplePoint && !custom, custom ? customTip : caps.fixedTimingReason);
    field(m_fd, caps.fd, tr("此适配器不支持 CAN FD"));
    const bool fd = caps.fd && m_fd->isChecked();
    field(m_dataBitrate, fd && !custom, custom ? customTip : tr("勾选 FD 后可选"));
    field(m_dataSamplePoint, fd && caps.dataSamplePoint && !custom,
          custom ? customTip : fd ? caps.fixedTimingReason : tr("勾选 FD 后可选"));
    field(m_refresh, caps.channelList, QString());
    m_channel->setEnabled(editable);
    m_adapter->setEnabled(editable);
    for (QWidget *w : {static_cast<QWidget *>(m_zlgModel), static_cast<QWidget *>(m_device),
                       static_cast<QWidget *>(m_zlgChannel)}) {
        w->setEnabled(editable);
    }
    const bool socketCan = adapter == CanAdapter::SocketCan;
    if (socketCan) {
        // The card's bitrate and sample points, which the interface must be set to by hand.
        CanSettings s = settings();
        s.bitrate = CanBitTiming::bitrateValue(text("defaultBitrate"));
        s.samplePoint = CanBitTiming::samplePointValue(text("defaultSamplePoint"));
        s.dataBitrate = CanBitTiming::bitrateValue(text("defaultDataBitrate"));
        s.dataSamplePoint = CanBitTiming::samplePointValue(text("defaultDataSamplePoint"));
        m_hint->setText(CanAdapter::socketCanCommand(s.channel, s));
    }
    m_hint->setVisible(socketCan);
    if (m_hintCaption) {
        m_hintCaption->setVisible(socketCan);
    }
    if (!caps.supported && !(m_link && m_link->isConnected())) {
        m_connect->setToolTip(tr("此系统不支持 %1 适配器").arg(adapter));
    } else {
        m_connect->setToolTip(QString());
    }
}

void CanConfigCardWidget::refreshChannels(bool announce)
{
    const QString adapter = selectedValue(m_adapter);
    if (adapter == CanAdapter::Zlg) {
        return;
    }
    QString current = m_channel->currentText();
    // A listed entry (it carries a handle) that is gone from the new list is stale; a typed channel is kept.
    const int listed = m_channel->findText(current);
    const bool wasListed = listed >= 0 && m_channel->itemData(listed).isValid();
    QString error;
    const QList<CanChannel> channels = CanAdapter::channels(adapter, &error);
    m_channel->clear();
    for (const CanChannel &channel : channels) {
        m_channel->addItem(channel.label, QIcon(), channel.id);
    }
    if (wasListed && m_channel->findText(current) < 0) {
        current.clear();
    }
    if (!current.isEmpty()) {
        m_channel->setCurrentText(current);
    } else {
        m_channel->setCurrentText(channels.isEmpty() ? QString() : channels.first().label);
    }
    // Only when asked: a project for a PEAK adapter also opens where its library is not installed.
    if (announce && isLive()) {
        if (!error.isEmpty()) {
            context()->notify(tr("CAN"), error, NotifyType::Warning, 4000);
        } else if (channels.isEmpty()) {
            context()->notify(tr("CAN"), tr("没有找到 %1 通道").arg(adapter), NotifyType::Warning, 4000);
        }
    }
}

void CanConfigCardWidget::onAttached()
{
    m_link = new CanLink(this);
    context()->hub()->addLink(m_link);
    refreshChannels();
    live(connect(m_connect, &QPushButton::clicked, this, &CanConfigCardWidget::toggleConnection));
    live(connect(m_link, &Link::connectedChanged, this, [this](bool connected) {
        setFieldsEnabled(!connected);
        if (connected) {
            m_statusTimer->start();
        } else {
            m_statusTimer->stop();
            setConnectedUi(false, tr("未连接"));
            showDetail(QString(), false);
        }
        showConnection();
    }));
    live(connect(m_link, &CanLink::busStateChanged, this, &CanConfigCardWidget::showConnection));
    live(connect(m_link, &Link::errorOccurred, this, [this](const QString &message) {
        // A refused or dropped connection stays explained on the card; errors while connected are toasts only.
        if (!m_link->isConnected()) {
            showFailure(message);
        }
        if (message == m_lastError && m_lastErrorAt.isValid() && m_lastErrorAt.elapsed() < 4000) {
            return;
        }
        m_lastError = message;
        m_lastErrorAt.start();
        context()->notify(tr("CAN 错误"), message, NotifyType::Error, 4000);
    }));
    LinkCardBase::onAttached();
}

Link *CanConfigCardWidget::currentLink() const
{
    return m_link;
}

void CanConfigCardWidget::onDetached()
{
    m_statusTimer->stop();
    if (m_link) {
        m_link->close();
        context()->hub()->removeLink(m_link);
        delete m_link;
        m_link = nullptr;
    }
    LinkCardBase::onDetached();
    m_shownStatus.clear();
    showDetail(QString(), false);
    setFieldsEnabled(true);
}

void CanConfigCardWidget::setFieldsEnabled(bool)
{
    updateAdapterUi(); // reads whether the link is connected
}

void CanConfigCardWidget::toggleConnection()
{
    if (m_link->isConnected()) {
        m_link->close();
        return;
    }
    QString dbc = text("dbcFile").trimmed();
    if (!dbc.isEmpty() && QDir::isRelativePath(dbc) && !assetBaseDir().isEmpty()) {
        dbc = QDir(assetBaseDir()).absoluteFilePath(dbc);
    }
    QString error;
    if (!m_link->open(settings(), dbc, text("canSignals"), text("canFilter"), &error)) {
        showFailure(error); // the link has logged it and toasted it too
    }
}

void CanConfigCardWidget::showFailure(const QString &reason)
{
    setConnectedUi(false, tr("连接失败"), reason);
    m_shownStatus.clear();
    showDetail(reason, true);
}

void CanConfigCardWidget::showDetail(const QString &text, bool error)
{
    m_detail->setText(text);
    // Fluent's text and critical colours.
    if (error) {
        m_detail->setTextColor(QColor(0xC4, 0x2B, 0x1C), QColor(0xFF, 0x99, 0xA4));
    } else {
        m_detail->setTextColor(QColor(0, 0, 0, 228), QColor(255, 255, 255));
    }
    m_detail->setVisible(!text.isEmpty());
}

void CanConfigCardWidget::showConnection()
{
    if (!m_link || !m_link->isConnected()) {
        return;
    }
    // The header says connected (and how the bus is when not normally); the counters go below the fields. The
    // adapter, channel and bitrate are in the fields; the tooltip has them with the rest.
    QString state = tr("已连接");
    if (m_link->busState() != CanBusState::Normal) {
        state += QStringLiteral(" · ") + canBusStateText(m_link->busState());
    }
    const QString counters = QStringLiteral("%1 · %2").arg(tr("收 %1").arg(count(m_link->receivedCount())),
                                                           tr("发 %1").arg(count(m_link->sentCount())));
    const QString tip = m_link->description() + QStringLiteral(" · ") + state + QStringLiteral(" · ") + counters;
    if (tip != m_shownStatus) {
        m_shownStatus = tip;
        setConnectedUi(true, state, tip);
        showDetail(counters, false);
    }
}

// ---------------------------------------------------------------------------
// CanMonitor
// ---------------------------------------------------------------------------

// The rows of the monitor. Frames only update the rows' data; the view hears of it in refresh(), at most every
// 100 ms, so a busy bus costs a hash lookup per frame and no relayout.
class CanMonitorModel : public QAbstractTableModel
{
  public:
    enum Column
    {
        Id,
        Direction,
        Type,
        Length,
        Data,
        Period,
        Count,
        Name,
        Signals,
    };
    // What a header click sorts by (the numbers as numbers), and how many lines a row's data takes.
    static constexpr int SortRole = Qt::UserRole;
    static constexpr int LinesRole = Qt::UserRole + 1;
    static constexpr int BytesPerLine = 8;

    using QAbstractTableModel::QAbstractTableModel;

    void setShowSignals(bool show)
    {
        if (show == m_showSignals) {
            return;
        }
        beginResetModel();
        m_showSignals = show;
        endResetModel();
    }

    void clear()
    {
        beginResetModel();
        m_rows.clear();
        m_index.clear();
        m_pending.clear();
        endResetModel();
    }

    void add(CanLink *link, const QList<CanFrame> &frames, bool sent)
    {
        std::shared_ptr<const CanSignalSet> signalSet = link ? link->signalSet() : nullptr;
        for (const CanFrame &frame : frames) {
            const CanId id{frame.id, frame.extended};
            const quint64 key = (id.key() << 1) | (sent ? 1 : 0);
            auto it = m_index.find(key);
            Row *row = nullptr;
            if (it != m_index.end()) {
                row = &m_rows[*it];
            } else {
                auto pending = m_pending.find(key);
                if (pending == m_pending.end()) {
                    Row fresh;
                    fresh.sent = sent;
                    fresh.id = id;
                    fresh.name = signalSet ? signalSet->messageName(id) : QString();
                    pending = m_pending.insert(key, fresh);
                }
                row = &*pending;
            }
            if (row->lastMs > 0) {
                row->intervals[row->next] = double(frame.timestampMs - row->lastMs);
                row->next = (row->next + 1) % int(row->intervals.size());
                row->intervalCount = std::min(row->intervalCount + 1, int(row->intervals.size()));
            }
            row->lastMs = frame.timestampMs;
            row->frame = frame;
            row->signalSet = signalSet;
            ++row->count;
            row->dirty = true;
            m_dirty = true;
        }
    }

    // New rows go in at their place in ID order (the view sorts them as it is asked to); the rest repaint once.
    void refresh()
    {
        if (!m_dirty) {
            return;
        }
        m_dirty = false;
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            const auto place = std::lower_bound(m_rows.begin(), m_rows.end(), it.value(),
                                                [](const Row &a, const Row &b) { return a.order() < b.order(); });
            const int at = int(place - m_rows.begin());
            beginInsertRows(QModelIndex(), at, at);
            m_rows.insert(at, it.value());
            endInsertRows();
        }
        if (!m_pending.isEmpty()) {
            m_pending.clear();
            m_index.clear();
            for (int i = 0; i < m_rows.size(); ++i) {
                m_index.insert(m_rows.at(i).key(), i);
            }
        }
        int first = -1;
        int last = -1;
        for (int i = 0; i < m_rows.size(); ++i) {
            Row &row = m_rows[i];
            if (!row.dirty) {
                continue;
            }
            row.dirty = false;
            if (m_showSignals) {
                row.signalText = row.signalSet ? row.signalSet->signalsText(row.frame) : QString();
            }
            first = first < 0 ? i : first;
            last = i;
        }
        if (first >= 0) {
            emit dataChanged(index(first, 0), index(last, columnCount() - 1), {Qt::DisplayRole, SortRole, LinesRole});
        }
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_rows.size());
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : (m_showSignals ? Signals + 1 : Signals);
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size()) {
            return QVariant();
        }
        const Row &row = m_rows.at(index.row());
        if (role == Qt::TextAlignmentRole) {
            return index.column() == Length || index.column() == Period || index.column() == Count
                       ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                       : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
        if (role == LinesRole) {
            return row.lines();
        }
        if (role == SortRole) {
            switch (index.column()) {
            case Id:
                return row.sortKey();
            case Direction:
                return row.sent;
            case Length:
                return int(row.frame.data.size());
            case Period:
                return row.intervalCount > 0 ? row.meanInterval() : -1.0;
            case Count:
                return row.count;
            default:
                break; // the text sorts as text
            }
            return data(index, Qt::DisplayRole);
        }
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole) {
            return QVariant();
        }
        switch (index.column()) {
        case Id:
            return Can::idText(row.id);
        case Direction:
            return row.sent ? CanLink::tr("发") : CanLink::tr("收");
        case Type:
            // Not translated: these are the names the bus tools use.
            return row.frame.rtr  ? QStringLiteral("RTR")
                   : row.frame.fd ? (row.frame.brs ? QStringLiteral("FD BRS") : QStringLiteral("FD"))
                                  : QStringLiteral("Data");
        case Name:
            return row.name;
        case Length:
            return int(row.frame.data.size());
        case Data:
            return row.dataText();
        case Period:
            return row.intervalCount > 0 ? QString::number(row.meanInterval(), 'f', 1) : QString();
        case Count:
            return count(row.count);
        case Signals:
            return row.signalText;
        }
        return QVariant();
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return QVariant();
        }
        switch (section) {
        case Id:
            return QStringLiteral("ID");
        case Direction:
            return CanLink::tr("方向");
        case Type:
            return CanLink::tr("类型");
        case Name:
            return CanLink::tr("名称");
        case Length:
            return CanLink::tr("长度");
        case Data:
            return CanLink::tr("数据");
        case Period:
            return CanLink::tr("周期(ms)");
        case Count:
            return CanLink::tr("次数");
        case Signals:
            return CanLink::tr("信号");
        }
        return QVariant();
    }

  private:
    struct Row
    {
        bool sent = false;
        CanId id;
        QString name;
        CanFrame frame; // the last one
        std::shared_ptr<const CanSignalSet> signalSet;
        QString signalText;
        quint64 count = 0;
        qint64 lastMs = 0;
        std::array<double, 10> intervals{};
        int next = 0;
        int intervalCount = 0;
        bool dirty = false;

        quint64 key() const { return (id.key() << 1) | (sent ? 1 : 0); }
        // ID order; a standard ID before the extended one of the same value, received before sent.
        std::tuple<quint32, bool, bool> order() const { return {id.id, id.extended, sent}; }
        quint64 sortKey() const { return (quint64(id.id) << 2) | (quint64(id.extended) << 1) | quint64(sent); }
        // Hex bytes, BytesPerLine to a line; nothing for a remote frame, which has no data.
        QString dataText() const
        {
            QStringList lines;
            if (!frame.rtr) {
                for (qsizetype at = 0; at < frame.data.size(); at += BytesPerLine) {
                    lines << toHexString(frame.data.mid(at, BytesPerLine));
                }
            }
            return lines.join(QLatin1Char('\n'));
        }
        int lines() const
        {
            return frame.rtr ? 1 : std::max(1, int((frame.data.size() + BytesPerLine - 1) / BytesPerLine));
        }
        double meanInterval() const
        {
            double sum = 0;
            for (int i = 0; i < intervalCount; ++i) {
                sum += intervals[i];
            }
            return intervalCount > 0 ? sum / intervalCount : 0;
        }
    };

    QList<Row> m_rows;
    QHash<quint64, int> m_index;  // key -> row
    QHash<quint64, Row> m_pending; // seen since the last refresh, not in m_rows yet
    bool m_showSignals = false;
    bool m_dirty = false;
};

CanMonitorWidget::CanMonitorWidget(const WidgetMeta &meta, QWidget *parent) : HmiWidget(meta, parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    m_model = new CanMonitorModel(this);
    m_sorted = new QSortFilterProxyModel(this);
    m_sorted->setSourceModel(m_model);
    m_sorted->setSortRole(CanMonitorModel::SortRole);
    // Rows are updated ten times a second; with this the proxy keeps the sort as they change.
    m_sorted->setDynamicSortFilter(true);
    m_view = new FluentQt::TableView(this);
    m_view->setModel(m_sorted);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(CanMonitorModel::Id, Qt::AscendingOrder);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->verticalHeader()->setVisible(false);
    m_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_view->setWordWrap(false);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_view);
    connect(m_view, &QWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        auto *menu = new FluentQt::RoundMenu(this);
        connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
        connect(menu->addAction(FluentQt::FluentIcon::Delete, tr("清空")), &QAction::triggered, m_model,
                [this]() { m_model->clear(); });
        menu->exec(m_view->viewport()->mapToGlobal(position), true);
    });
    m_refresh = new QTimer(this);
    m_refresh->setInterval(kMonitorRefreshMs);
    connect(m_refresh, &QTimer::timeout, m_model, [this]() { m_model->refresh(); });
    // Whatever moves or changes a row (a new one, a longer frame, a sort) can change which height it needs.
    for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
        connect(m_sorted, signal, this, [this]() { fitRowHeights(); });
    }
    connect(m_sorted, &QAbstractItemModel::dataChanged, this, [this]() { fitRowHeights(); });
    connect(m_sorted, &QAbstractItemModel::layoutChanged, this, [this]() { fitRowHeights(); });
    connect(m_sorted, &QAbstractItemModel::modelReset, this, [this]() { fitRowHeights(); });
}

void CanMonitorWidget::fitRowHeights()
{
    if (m_heightsQueued) {
        return;
    }
    m_heightsQueued = true;
    QTimer::singleShot(0, this, [this]() {
        m_heightsQueued = false;
        QHeaderView *header = m_view->verticalHeader();
        const int base = header->defaultSectionSize();
        for (int row = 0; row < m_sorted->rowCount(); ++row) {
            const int lines = m_sorted->index(row, 0).data(CanMonitorModel::LinesRole).toInt();
            // The delegate knows what its lines and padding take; a one-line row keeps the default height.
            const int height =
                lines > 1
                    ? std::max(base, m_view->sizeHintForIndex(m_sorted->index(row, CanMonitorModel::Data)).height())
                    : base;
            if (header->sectionSize(row) != height) {
                header->resizeSection(row, height);
            }
        }
    });
}

void CanMonitorWidget::apply()
{
    m_model->setShowSignals(flag("showSignals"));
    QHeaderView *header = m_view->horizontalHeader();
    header->setStretchLastSection(true);
    // Wide enough for the widest thing a column holds, and for its header with the sort arrow.
    const QFontMetrics body = m_view->fontMetrics();
    const QFontMetrics head(header->font());
    const int pad = 24;
    const auto fit = [&](int column, int contentWidth) {
        const QString title = m_model->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString();
        return std::max(contentWidth + pad, head.horizontalAdvance(title) + pad + 12);
    };
    const int widths[] = {
        fit(CanMonitorModel::Id, int(body.horizontalAdvance(QStringLiteral("0x18FF1001")) * 1.2)),
        fit(CanMonitorModel::Direction, body.horizontalAdvance(QStringLiteral("00"))),
        fit(CanMonitorModel::Type, body.horizontalAdvance(QStringLiteral("FD BRS"))),
        fit(CanMonitorModel::Length, body.horizontalAdvance(QStringLiteral("00000"))),
        fit(CanMonitorModel::Data, body.horizontalAdvance(QStringLiteral("00 00 00 00 00 00 00 00"))),
        fit(CanMonitorModel::Period, body.horizontalAdvance(QStringLiteral("0000.0"))),
        fit(CanMonitorModel::Count, body.horizontalAdvance(QStringLiteral("0,000,000"))),
        fit(CanMonitorModel::Name, body.horizontalAdvance(QStringLiteral("MessageName_00"))),
    };
    for (int column = 0; column < int(std::size(widths)) && column < m_model->columnCount(); ++column) {
        header->setSectionResizeMode(column, QHeaderView::Interactive);
        header->resizeSection(column, widths[column]);
    }
}

void CanMonitorWidget::onAttached()
{
    // The first CAN link to connect starts afresh; a second card connecting keeps what the first one has shown.
    live(connect(context()->hub(), &CommHub::canLinkConnected, this, [this](CanLink *link) {
        const QList<Link *> connected = context()->hub()->connectedLinks();
        const bool others = std::any_of(connected.begin(), connected.end(),
                                        [link](const Link *other) { return other != link && other->isCan(); });
        if (!others) {
            m_model->clear();
        }
    }));
    live(
        connect(context()->hub(), &CommHub::canFrames, this,
                [this](CanLink *link, const QList<CanFrame> &frames, bool sent) { m_model->add(link, frames, sent); }));
    m_refresh->start();
}

void CanMonitorWidget::onDetached()
{
    m_refresh->stop();
    m_model->clear();
}

} // namespace fhmi
