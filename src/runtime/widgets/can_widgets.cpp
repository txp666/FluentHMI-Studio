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
    const QString current = m_channel->currentText();
    QString error;
    const QList<CanChannel> channels = CanAdapter::channels(adapter, &error);
    m_channel->clear();
    for (const CanChannel &channel : channels) {
        m_channel->addItem(channel.label, QIcon(), channel.id);
    }
    if (m_channel->findText(current) >= 0 || !current.isEmpty()) {
        m_channel->setCurrentText(current);
    } else if (!channels.isEmpty()) {
        m_channel->setCurrentText(channels.first().label);
    }
    // Only when asked: a project for a PEAK adapter also opens where its library is not installed.
    if (announce && !error.isEmpty() && isLive()) {
        context()->notify(tr("CAN"), error, NotifyType::Warning, 4000);
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
        }
        showConnection();
    }));
    live(connect(m_link, &CanLink::busStateChanged, this, &CanConfigCardWidget::showConnection));
    live(connect(m_link, &Link::errorOccurred, this, [this](const QString &message) {
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
        // The reason stays on the status line; the link has logged it and the card shown it.
        setConnectedUi(false, error, error);
        m_shownStatus.clear();
    }
}

void CanConfigCardWidget::showConnection()
{
    if (!m_link || !m_link->isConnected()) {
        return;
    }
    QString status = QStringLiteral("%1 · %2 · %3")
                         .arg(m_link->description(), tr("收 %1").arg(count(m_link->receivedCount())),
                              tr("发 %1").arg(count(m_link->sentCount())));
    if (m_link->busState() != CanBusState::Normal) {
        status += QStringLiteral(" · ") + canBusStateText(m_link->busState());
    }
    if (status != m_shownStatus) {
        m_shownStatus = status;
        setConnectedUi(true, status, status);
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
        Direction,
        Id,
        Name,
        Length,
        Data,
        Period,
        Count,
        Signals,
    };

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

    // New rows go in at their place in ID order; the rest repaint once.
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
            emit dataChanged(index(first, 0), index(last, columnCount() - 1), {Qt::DisplayRole});
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
        if (role != Qt::DisplayRole && role != Qt::ToolTipRole) {
            return QVariant();
        }
        switch (index.column()) {
        case Direction:
            return row.sent ? CanLink::tr("发") : CanLink::tr("收");
        case Id: {
            QString id = Can::idText(row.id);
            if (row.frame.fd) {
                id += QStringLiteral(" FD");
            }
            return id;
        }
        case Name:
            return row.name;
        case Length:
            return int(row.frame.data.size());
        case Data:
            return row.frame.rtr ? QStringLiteral("RTR") : toHexString(row.frame.data);
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
        case Direction:
            return CanLink::tr("方向");
        case Id:
            return QStringLiteral("ID");
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
    m_view = new FluentQt::TableView(this);
    m_view->setModel(m_model);
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
}

void CanMonitorWidget::apply()
{
    m_model->setShowSignals(flag("showSignals"));
    QHeaderView *header = m_view->horizontalHeader();
    header->setStretchLastSection(true);
    static constexpr int widths[] = {40, 96, 90, 40, 150, 64, 60};
    for (int column = 0; column < int(std::size(widths)) && column < m_model->columnCount(); ++column) {
        header->setSectionResizeMode(column, QHeaderView::Interactive);
        header->resizeSection(column, widths[column]);
    }
}

void CanMonitorWidget::onAttached()
{
    live(connect(context()->hub(), &CommHub::canFrames, this,
                 [this](CanLink *link, const QList<CanFrame> &frames, bool sent) { m_model->add(link, frames, sent); }));
    m_refresh->start();
}

void CanMonitorWidget::onDetached()
{
    m_refresh->stop();
    m_model->clear();
}

} // namespace fhmi
