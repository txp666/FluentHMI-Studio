#include "designer/can_signal_editor.h"

#include "core/binding.h"
#include "core/dbc.h"
#include "core/project.h"
#include "runtime/app_dialog.h"
#include "runtime/bit_map_view.h"
#include "runtime/frame_bytes_view.h"

#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>
#include <FluentQtWidgets/Widgets/SpinBox.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QLocale>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace fhmi {

std::shared_ptr<const CanDatabase> designerCanDatabase(const Project *project, const QString &file, QString *error)
{
    if (file.trimmed().isEmpty())
        return {};

    struct Cached
    {
        QDateTime modified;
        qint64 size;
        std::shared_ptr<const CanDatabase> database;
        QString error;
    };
    static QHash<QString, Cached> cache;
    const QFileInfo info(QDir(project ? project->assetBaseDir() : QString()).absoluteFilePath(file.trimmed()));
    const QString path = info.absoluteFilePath();
    auto it = cache.find(path);
    if (it == cache.end() || it->modified != info.lastModified() || it->size != info.size()) {
        QString problem;
        auto database = Dbc::load(path, &problem);
        it = cache.insert(path, {info.lastModified(), info.size(),
                                database ? std::make_shared<const CanDatabase>(std::move(*database)) : nullptr,
                                problem});
    }
    if (error)
        *error = it->error;
    return it->database;
}

namespace {

QString tr(const char *source) { return QCoreApplication::translate("fhmi", source); }

QString number(double value) { return QString::number(value, 'g', QLocale::FloatingPointShortest); }

BitMapView *makeCanBitMap(QWidget *parent)
{
    auto *bits = new BitMapView(parent);
    bits->setObjectName(QStringLiteral("canSignalBits"));
    bits->setAccessibleName(tr("CAN 报文位图"));
    return bits;
}

void showSignalBits(BitMapView *view, int size, const CanSignalDef *signal, const QByteArray *sample, int locate = -1)
{
    QList<BitRange> ranges;
    if (signal) {
        const auto color = frameByteColor(FrameByteKind::Field, 1);
        for (const int bit : signal->bitPositions())
            ranges.append({bit, 1, color});
    }
    view->setBits(size, sample, std::move(ranges), locate);
}

class CanSignalForm final : public QWidget
{
  public:
    struct Values
    {
        QJsonObject binding;
        CanId id;
        CanSignalDef signal;
    };

    CanSignalForm(const QJsonObject &binding, QWidget *parent,
                  const CanSignalSet *signalSet, bool fdAvailable, const CanSignalDef *definition = nullptr)
        : QWidget(parent), m_signalSet(signalSet), m_fdAvailable(fdAvailable)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *columns = new QHBoxLayout;
        columns->setSpacing(18);
        auto *basics = new QFormLayout;
        auto *scaling = new QFormLayout;
        for (auto *form : {basics, scaling}) {
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            form->setVerticalSpacing(8);
            columns->addLayout(form, 1);
        }
        layout->addLayout(columns);
        const auto addRow = [this](QFormLayout *form, const QString &title, QWidget *field) {
            auto *label = new FluentQt::BodyLabel(title, this);
            label->setBuddy(field);
            field->setAccessibleName(title);
            form->addRow(label, field);
        };
        const auto line = [&](QFormLayout *form, const QString &title, const QString &value) {
            auto *field = new FluentQt::LineEdit(this);
            field->setText(value);
            addRow(form, title, field);
            return field;
        };
        const auto spin = [&](const QString &title, int min, int max, int value) {
            auto *field = new FluentQt::SpinBox(this);
            field->setRange(min, max);
            field->setValue(value);
            addRow(basics, title, field);
            return field;
        };
        m_id = line(basics, tr("报文 ID"), binding.value(QStringLiteral("canId")).toString());
        m_id->setObjectName(QStringLiteral("canSignalFrameId"));
        m_id->setPlaceholderText(tr("如 18FF50E5"));
        if (definition) {
            m_name = line(basics, tr("信号名称"), definition->name);
            m_name->setObjectName(QStringLiteral("canInlineName"));
            m_name->setPlaceholderText(QStringLiteral("ready"));
        }
        m_start = spin(tr("起始位"), 0, 511, binding.value(QStringLiteral("canStartBit")).toInt());
        m_start->setObjectName(QStringLiteral("canSignalStartBit"));
        m_length = spin(tr("位长度"), 1, 64, binding.value(QStringLiteral("canBitLength")).toInt(8));
        m_length->setObjectName(QStringLiteral("canSignalBitLength"));
        m_order = new FluentQt::ComboBox(this);
        m_order->setObjectName(QStringLiteral("canSignalByteOrder"));
        for (const auto &order : CanByteOrder::options()) {
            if (!definition || order != CanByteOrder::MotorolaLsb)
                m_order->addItem(order, {}, order);
        }
        const auto order = binding.value(QStringLiteral("canByteOrder")).toString(CanByteOrder::Intel);
        m_order->setCurrentIndex(std::max(0, m_order->findData(order)));
        auto *ordering = definition ? basics : scaling;
        addRow(ordering, tr("字节序"), m_order);
        m_sign = new FluentQt::ComboBox(this);
        m_sign->addItems({tr("无符号"), tr("有符号")});
        m_sign->setCurrentIndex(binding.value(QStringLiteral("canSigned")).toBool() ? 1 : 0);
        addRow(ordering, tr("符号"), m_sign);
        m_factor = line(scaling, tr("比例"), number(binding.value(QStringLiteral("canFactor")).toDouble(1)));
        m_offset = line(scaling, tr("偏移"), number(binding.value(QStringLiteral("canOffset")).toDouble()));
        if (definition) {
            m_unit = line(scaling, tr("单位"), definition->unit);
            m_minimum = line(scaling, tr("最小值"), number(definition->minimum));
            m_maximum = line(scaling, tr("最大值"), number(definition->maximum));
        }
        m_viewSize = new FluentQt::ComboBox(this);
        m_viewSize->setObjectName(QStringLiteral("canBitMapLength"));
        m_viewSize->addItem(tr("自动"), {}, 0);
        for (const int size : {8, 12, 16, 20, 24, 32, 48, 64})
            m_viewSize->addItem(QString::number(size), {}, size);
        m_viewSizeLabel = new FluentQt::BodyLabel(tr("位图字节数"), this);
        m_viewSizeLabel->setBuddy(m_viewSize);
        m_viewSize->setAccessibleName(m_viewSizeLabel->text());
        (definition ? scaling : basics)->addRow(m_viewSizeLabel, m_viewSize);

        auto *sample = new QFormLayout;
        m_sample = line(sample, tr("样例报文"), {});
        m_sample->setObjectName(QStringLiteral("canSignalSample"));
        m_sample->setPlaceholderText(tr("HEX 数据或带 ID 的报文，如 01 20 或 201#0120"));
        m_sample->setClearButtonEnabled(true);
        layout->addLayout(sample);
        m_sampleStatus = new FluentQt::CaptionLabel(this);
        m_sampleStatus->setObjectName(QStringLiteral("canSignalSampleStatus"));
        m_sampleStatus->setWordWrap(true);
        layout->addWidget(m_sampleStatus);

        layout->addWidget(new FluentQt::CaptionLabel(tr("点击位图设置起始位"), this));
        m_bits = makeCanBitMap(this);
        layout->addWidget(m_bits, 1);
        m_problem = new FluentQt::CaptionLabel(this);
        m_problem->setTextColor(QColor(0xc4, 0x2b, 0x1c), QColor(0xff, 0x99, 0xa4));
        m_problem->setWordWrap(true);
        layout->addWidget(m_problem);

        const auto refresh = [this] { refreshBits(); };
        for (auto *field : {m_id, m_factor, m_offset, m_sample})
            connect(field, &QLineEdit::textChanged, this, refresh);
        for (auto *field : {m_start, m_length})
            connect(field, &QSpinBox::valueChanged, this, refresh);
        for (auto *field : {m_order, m_sign, m_viewSize})
            connect(field, &FluentQt::ComboBox::currentIndexChanged, this, refresh);
        connect(m_bits, &BitMapView::bitClicked, m_start, &QSpinBox::setValue);
        connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged,
                this, refresh);
        refreshBits();
        (m_name ? m_name : m_id)->setFocus();
    }

    void setSample(const QString &text) { m_sample->setText(text); }

    std::optional<Values> values()
    {
        QString error;
        const auto id = Can::parseId(m_id->text(), &error);
        if (!id)
            return fail(error);
        auto binding = readBinding(&error);
        if (!binding)
            return fail(error);
        auto signal = canBitField(*binding, &error);
        if (!signal)
            return fail(error);
        if (m_name) {
            signal->name = m_name->text().trimmed();
            static const QRegularExpression identifier(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
            if (!identifier.match(signal->name).hasMatch())
                return fail(tr("信号名称须以字母或下划线开头，只能包含字母、数字和下划线"));
            signal->unit = m_unit->text();
            if (!readNumber(m_minimum, signal->minimum) || !readNumber(m_maximum, signal->maximum))
                return fail(tr("比例、偏移和范围须为有效数字"));
            if (const auto message = m_signalSet ? m_signalSet->target(Can::idText(*id)) : std::nullopt;
                message && !message->name.isEmpty() && !signal->fits(message->length))
                return fail(tr("信号超出 DBC 报文的 %1 字节").arg(message->length));
        }
        return Values{std::move(*binding), *id, std::move(*signal)};
    }

    std::nullopt_t fail(const QString &error)
    {
        m_problem->setText(error);
        m_problem->setVisible(!error.isEmpty());
        return std::nullopt;
    }

  private:
    static bool readNumber(QLineEdit *field, double &value)
    {
        bool ok = false;
        value = field->text().toDouble(&ok);
        return ok && std::isfinite(value);
    }

    std::optional<QJsonObject> readBinding(QString *error) const
    {
        double factor;
        double offset;
        if (!readNumber(m_factor, factor) || !readNumber(m_offset, offset)) {
            *error = tr("比例和偏移须为有效数字");
            return std::nullopt;
        }
        return QJsonObject{{QStringLiteral("canId"), m_id->text().trimmed()},
                           {QStringLiteral("canStartBit"), m_start->value()},
                           {QStringLiteral("canBitLength"), m_length->value()},
                           {QStringLiteral("canByteOrder"), m_order->currentData().toString()},
                           {QStringLiteral("canSigned"), m_sign->currentIndex() == 1},
                           {QStringLiteral("canFactor"), factor},
                           {QStringLiteral("canOffset"), offset}};
    }

    void refreshBits()
    {
        QString error;
        const auto binding = readBinding(&error);
        const auto signal = binding ? canBitField(*binding, &error) : std::nullopt;
        fail(error);

        const QString text = m_sample->text().trimmed();
        const auto sample = text.isEmpty() ? Can::FrameSample{} : Can::parseFrameSample(text);
        const auto id = Can::parseId(m_id->text());
        if (sample.id && sample.error.isEmpty() && m_id->text().trimmed().isEmpty()) {
            m_id->setText(Can::idText(*sample.id));
            return;
        }
        QString status = sample.error;
        if (status.isEmpty() && sample.id && id != sample.id)
            status = tr("样例 ID %1 与当前报文不匹配").arg(Can::idText(*sample.id));
        const bool hasSample = !text.isEmpty() && status.isEmpty();
        if (hasSample && signal) {
            const auto decoded = signal->decode(sample.bytes);
            status = decoded.isValid() ? tr("解析值 %1").arg(displayString(decoded)) : tr("样例过短");
        }
        m_sampleStatus->setText(status);
        m_sampleStatus->setVisible(!status.isEmpty());
        const auto message = m_signalSet && id ? m_signalSet->target(Can::idText(*id)) : std::nullopt;
        const int needed = signal ? signal->bytesNeeded() : 0;
        const bool fixedLength = message && !message->name.isEmpty();
        const bool showSize = !fixedLength && (m_fdAvailable || sample.fd || needed > 8 || (message && message->fd));
        m_viewSize->setVisible(showSize);
        m_viewSizeLabel->setVisible(showSize);
        const int selected = showSize ? m_viewSize->currentData().toInt() : 0;
        const int defined = message ? message->length : 8;
        const int bytes = std::max({selected == 0 ? defined : selected, needed, hasSample ? int(sample.bytes.size()) : 0});
        showSignalBits(m_bits, bytes, signal ? &*signal : nullptr, hasSample ? &sample.bytes : nullptr, m_start->value());
    }

    const CanSignalSet *m_signalSet;
    bool m_fdAvailable;
    FluentQt::LineEdit *m_id;
    FluentQt::SpinBox *m_start;
    FluentQt::SpinBox *m_length;
    FluentQt::ComboBox *m_order;
    FluentQt::ComboBox *m_sign;
    FluentQt::LineEdit *m_factor;
    FluentQt::LineEdit *m_offset;
    FluentQt::LineEdit *m_name = nullptr;
    FluentQt::LineEdit *m_unit = nullptr;
    FluentQt::LineEdit *m_minimum = nullptr;
    FluentQt::LineEdit *m_maximum = nullptr;
    FluentQt::ComboBox *m_viewSize;
    FluentQt::BodyLabel *m_viewSizeLabel;
    FluentQt::LineEdit *m_sample;
    FluentQt::CaptionLabel *m_sampleStatus;
    FluentQt::CaptionLabel *m_problem;
    BitMapView *m_bits;
};

struct SignalCard
{
    QString id;
    QString title;
    QString dbcFile;
    QString inlineText;
    bool fd;
};

SignalCard signalCard(const QJsonObject &props, const QString &id = {})
{
    return {id, props.value(QStringLiteral("title")).toString(),
            props.value(QStringLiteral("dbcFile")).toString(), props.value(QStringLiteral("canSignals")).toString(),
            props.value(QStringLiteral("canFd")).toBool()};
}

class CanSignalDialog : public AppDialog
{
  public:
    CanSignalDialog(const Project *project, QList<SignalCard> cards, const QString &connection, const QString &key,
                    bool editing, QWidget *parent)
        : AppDialog(editing ? tr("CAN 信号编辑") : tr("选择 CAN 信号"), parent),
          m_project(project), m_cards(std::move(cards)), m_editing(editing)
    {
        setObjectName(editing ? QStringLiteral("canSignalEditor") : QStringLiteral("canSignalPicker"));
        setPreferredSize({1080, 620});
        yesButton()->setText(editing ? tr("保存") : tr("选用信号"));
        setValidator([this] { return m_editing ? m_inlineError.isEmpty() : bool(selection()); });

        auto *filter = new QHBoxLayout;
        if (!editing) {
            m_connection = new FluentQt::ComboBox(widget());
            m_connection->setObjectName(QStringLiteral("canSignalConnection"));
            m_connection->setAccessibleName(tr("来源连接"));
            for (const auto &card : std::as_const(m_cards)) {
                const auto label = card.title.isEmpty() || card.title == card.id ? card.id
                                    : QStringLiteral("%1 · %2").arg(card.title, card.id);
                m_connection->addItem(label, {}, card.id);
            }
            const int index = m_connection->findData(connection);
            m_connection->setCurrentIndex(index < 0 ? 0 : index);
            m_connection->setEnabled(m_cards.size() > 1);
            filter->addWidget(new FluentQt::BodyLabel(tr("来源连接"), widget()));
            filter->addWidget(m_connection, 1);
        }
        m_id = new FluentQt::ComboBox(widget());
        m_id->setObjectName(QStringLiteral("canSignalId"));
        m_id->setAccessibleName(tr("报文 ID"));
        filter->addWidget(new FluentQt::BodyLabel(tr("报文 ID"), widget()));
        filter->addWidget(m_id, 1);
        if (editing) {
            auto *textButton = new FluentQt::TransparentPushButton(tr("编辑文本…"), widget());
            filter->addWidget(textButton);
            connect(textButton, &QPushButton::clicked, this, &CanSignalDialog::editText);
        }
        viewLayout()->addLayout(filter);
        m_definitionStatus = new FluentQt::CaptionLabel(widget());
        m_definitionStatus->setWordWrap(true);
        viewLayout()->addWidget(m_definitionStatus);

        auto *sampleRow = new QHBoxLayout;
        auto *sampleLabel = new FluentQt::BodyLabel(tr("样例报文"), widget());
        m_sample = new FluentQt::LineEdit(widget());
        m_sample->setObjectName(QStringLiteral("canSignalSample"));
        m_sample->setAccessibleName(sampleLabel->text());
        m_sample->setPlaceholderText(tr("HEX 数据或带 ID 的报文，如 01 20 或 201#0120"));
        m_sample->setClearButtonEnabled(true);
        sampleLabel->setBuddy(m_sample);
        sampleRow->addWidget(sampleLabel);
        sampleRow->addWidget(m_sample, 1);
        viewLayout()->addLayout(sampleRow);
        m_frameStatus = new FluentQt::CaptionLabel(widget());
        m_frameStatus->setObjectName(QStringLiteral("canSignalSampleStatus"));
        m_frameStatus->setWordWrap(true);
        viewLayout()->addWidget(m_frameStatus);

        m_inspection = new QBoxLayout(QBoxLayout::LeftToRight);
        m_inspection->setSpacing(16);
        auto *bitSection = new QVBoxLayout;
        bitSection->setSpacing(8);
        auto *bitTitle = new FluentQt::StrongBodyLabel(tr("CAN 报文位图"), widget());
        bitTitle->setMinimumHeight(32);
        bitSection->addWidget(bitTitle);
        m_bits = makeCanBitMap(widget());
        bitSection->addWidget(m_bits, 1);
        m_inspection->addLayout(bitSection, 5);

        auto *signalSection = new QVBoxLayout;
        signalSection->setSpacing(8);
        auto *actions = new QHBoxLayout;
        auto *signalTitle = new FluentQt::StrongBodyLabel(tr("信号列表"), widget());
        signalTitle->setMinimumHeight(32);
        actions->addWidget(signalTitle, 1);
        if (editing) {
            m_add = new FluentQt::PushButton(tr("添加信号"), widget());
            m_add->setObjectName(QStringLiteral("canSignalAdd"));
            m_edit = new FluentQt::PushButton(tr("编辑信号"), widget());
            m_edit->setObjectName(QStringLiteral("canSignalEdit"));
            m_remove = new FluentQt::PushButton(tr("删除信号"), widget());
            m_remove->setObjectName(QStringLiteral("canSignalRemove"));
            actions->addWidget(m_add);
            actions->addWidget(m_edit);
            actions->addWidget(m_remove);
            connect(m_add, &QPushButton::clicked, this, [this] { editSignal(-1); });
            connect(m_edit, &QPushButton::clicked, this, [this] { editSignal(m_table->currentRow()); });
            connect(m_remove, &QPushButton::clicked, this, &CanSignalDialog::removeSignal);
        }
        signalSection->addLayout(actions);

        m_table = new FluentQt::TableWidget(widget());
        m_table->setObjectName(QStringLiteral("canSignalRows"));
        m_table->setAccessibleName(tr("信号列表"));
        m_table->setSelectionMode(QAbstractItemView::SingleSelection);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setBorderVisible(true);
        m_table->setAlternatingRowColors(true);
        m_table->setShowGrid(false);
        m_table->setColumnCount(6);
        m_table->setHorizontalHeaderLabels({tr("信号"), tr("定义来源"), tr("起始位"), tr("位长度"), tr("字节序"), tr("解析值")});
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
        m_table->verticalHeader()->hide();
        m_table->verticalHeader()->setDefaultSectionSize(30);
        signalSection->addWidget(m_table, 1);
        m_details = new FluentQt::CaptionLabel(widget());
        m_details->setObjectName(QStringLiteral("canSignalDetails"));
        m_details->setWordWrap(true);
        signalSection->addWidget(m_details);
        m_inspection->addLayout(signalSection, 6);
        viewLayout()->addLayout(m_inspection, 1);

        if (m_connection) {
            connect(m_connection, &FluentQt::ComboBox::currentIndexChanged, this, [this] {
                loadCard();
                parseSampleText();
            });
        }
        connect(m_id, &FluentQt::ComboBox::currentIndexChanged, this, [this] { showMessage(); });
        connect(m_sample, &QLineEdit::textChanged, this, [this] { parseSampleText(); });
        connect(m_table, &QTableWidget::currentCellChanged, this, [this] { updateSelection(true); });
        connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row) {
            if (m_editing) {
                if (m_rows.at(row).line >= 0)
                    editSignal(row);
            } else if (yesButton()->isEnabled()) {
                yesButton()->click();
            }
        });
        connect(m_bits, &BitMapView::bitClicked, this, [this](int bit) {
            if (const auto *row = currentRow(); row && row->signal.bitPositions().contains(bit))
                return;
            for (int row = 0; row < m_rows.size(); ++row) {
                if (m_rows.at(row).signal.bitPositions().contains(bit)) {
                    m_table->setCurrentCell(row, 0);
                    break;
                }
            }
        });
        connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged,
                this, [this] { updateSelection(); });
        loadCard(key);
        if (m_editing) {
            for (const auto &entry : m_inline) {
                const QString original = inlineSignalKey(entry.id, entry.signal.name);
                m_originalKeys.insert(original, original);
            }
        }
    }

    CanSignalEdit edit() const
    {
        CanSignalEdit result{m_inlineText, {}};
        for (auto it = m_originalKeys.cbegin(); it != m_originalKeys.cend(); ++it)
            if (it.value() != it.key())
                result.renamedKeys.insert(it.value(), it.key());
        return result;
    }

    void reviewBeforeSave(std::function<bool(const CanSignalEdit &, QWidget *)> approve)
    {
        setValidator([this, approve = std::move(approve)] {
            return m_inlineError.isEmpty() && approve(edit(), this);
        });
    }

    std::optional<CanSignalSelection> selection() const
    {
        if (!m_inlineError.isEmpty() || !m_dbcError.isEmpty())
            return std::nullopt;
        if (const auto *row = currentRow(); row && m_connection && m_connection->currentIndex() >= 0)
            return CanSignalSelection{m_connection->currentData().toString(), row->key, row->signal.name};
        return std::nullopt;
    }

  protected:
    void resizeEvent(QResizeEvent *event) override
    {
        AppDialog::resizeEvent(event);
        if (m_inspection)
            m_inspection->setDirection(widget()->width() < 850 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    }

  private:
    struct Row
    {
        CanSignalDef signal;
        QString key;
        int line = -1;
    };

    const Row *currentRow() const
    {
        const int index = m_table->currentRow();
        return index >= 0 && index < m_rows.size() ? &m_rows.at(index) : nullptr;
    }

    CanId currentId() const { return m_ids.value(m_id->currentIndex(), CanId{0x100, false}); }

    void loadCard(const QString &key = {})
    {
        const auto card = m_cards.value(m_connection ? m_connection->currentIndex() : 0);
        m_dbcError.clear();
        m_dbc = designerCanDatabase(m_project, card.dbcFile, &m_dbcError);
        setInlineText(card.inlineText, {}, key);
    }

    void setInlineText(const QString &text, std::optional<CanId> preferred = {}, const QString &key = {})
    {
        m_inlineText = text;
        m_inlineError.clear();
        m_inline = parseInlineSignals(text, &m_inlineError).value_or(QList<InlineSignal>{});
        m_signalSet = CanSignalSet::build(m_dbc, text, &m_inlineError);
        if (!m_signalSet)
            m_signalSet = CanSignalSet::build(m_dbc, {});

        m_ids.clear();
        QStringList names;
        const auto addId = [&](const CanId &id, const QString &name) {
            if (m_ids.contains(id))
                return;
            m_ids.append(id);
            names.append(name.isEmpty() ? Can::idText(id.id, id.extended)
                                         : QStringLiteral("%1 · %2").arg(Can::idText(id.id, id.extended), name));
        };
        // An older binding may use the short name. Prefer an exact key; fall back to the first short-name match.
        int keyScore = 0;
        const auto locate = [&](const CanId &id, const CanSignalDef &signal, const QString &qualified) {
            if (key.isEmpty())
                return;
            const int score = key == qualified ? 2 : (key == signal.name ? 1 : 0);
            if (score > keyScore) {
                keyScore = score;
                preferred = id;
            }
        };
        if (m_dbc) {
            for (const auto &message : m_dbc->messages) {
                addId(message.id, message.name);
                for (const auto &signal : message.signalDefs)
                    locate(message.id, signal, message.name + QLatin1Char('.') + signal.name);
            }
        }
        for (const auto &entry : std::as_const(m_inline)) {
            addId(entry.id, {});
            locate(entry.id, entry.signal, inlineSignalKey(entry.id, entry.signal.name));
        }
        {
            const QSignalBlocker blocker(m_id);
            m_id->clear();
            m_id->addItems(names);
            const int index = preferred ? int(m_ids.indexOf(*preferred)) : -1;
            m_id->setCurrentIndex(index < 0 && !m_ids.isEmpty() ? 0 : index);
            m_id->setEnabled(!m_ids.isEmpty());
        }
        const QString problem = m_inlineError.isEmpty() ? m_dbcError : m_inlineError;
        m_definitionStatus->setText(!problem.isEmpty() ? problem
                                    : m_ids.isEmpty() ? tr("请先加载 DBC 或定义 CAN 信号") : QString());
        m_definitionStatus->setVisible(!m_definitionStatus->text().isEmpty());
        showMessage(key);
    }

    void showMessage(const QString &key = {})
    {
        m_rows.clear();
        const auto id = currentId();
        if (m_id->currentIndex() >= 0) {
            if (const auto *message = m_dbc ? m_dbc->find(id) : nullptr) {
                for (const auto &signal : message->signalDefs)
                    m_rows.append({signal, message->name + QLatin1Char('.') + signal.name});
            }
            for (const auto &entry : std::as_const(m_inline)) {
                if (entry.id == id)
                    m_rows.append({entry.signal, inlineSignalKey(id, entry.signal.name), entry.line});
            }
        }
        const QSignalBlocker blocker(m_table);
        m_table->setRowCount(0);
        m_table->setRowCount(int(m_rows.size()));
        int selected = m_rows.isEmpty() ? -1 : 0;
        for (int i = 0; i < m_rows.size(); ++i) {
            const auto &row = m_rows.at(i);
            const auto &sig = row.signal;
            const QString origin = row.line >= 0 ? tr("内联") : m_editing ? tr("DBC · 只读") : QStringLiteral("DBC");
            const QStringList cells{sig.name, origin,
                                    QString::number(sig.start), QString::number(sig.length),
                                    sig.motorola ? QStringLiteral("Motorola") : QStringLiteral("Intel"), {}};
            for (int j = 0; j < cells.size(); ++j)
                m_table->setItem(i, j, new QTableWidgetItem(cells.at(j)));
            if (row.key == key || (m_rows.at(selected).key != key && sig.name == key))
                selected = i;
        }
        if (selected >= 0)
            m_table->setCurrentCell(selected, 0);
        updateSample();
        updateSelection(true);
    }

    void parseSampleText()
    {
        m_sampleText = m_sample->text().trimmed();
        m_parsedSample = Can::parseFrameSample(m_sampleText);
        if (m_parsedSample.id && m_parsedSample.error.isEmpty()) {
            const int index = int(m_ids.indexOf(*m_parsedSample.id));
            if (index >= 0 && index != m_id->currentIndex()) {
                m_id->setCurrentIndex(index);
                return;
            }
        }
        updateSample();
        updateSelection();
    }

    void updateSample()
    {
        const auto id = currentId();
        const QString idText = Can::idText(id.id, id.extended);
        const auto target = m_id->currentIndex() >= 0 && m_signalSet ? m_signalSet->target(idText) : std::nullopt;
        QString status;
        m_payload.clear();
        m_haveSample = false;
        const auto frameType = target && target->fd ? QStringLiteral("CAN FD") : QStringLiteral("CAN");
        if (target)
            status = tr("%1 · 定义 %2 字节 · 单元格显示位编号").arg(frameType).arg(target->length);
        if (!m_sampleText.isEmpty()) {
            if (!m_parsedSample.error.isEmpty()) {
                status = m_parsedSample.error;
            } else if (m_parsedSample.id && (*m_parsedSample.id != id || m_id->currentIndex() < 0)) {
                const auto &sampleId = *m_parsedSample.id;
                status = tr("样例 ID %1 与当前报文不匹配").arg(Can::idText(sampleId.id, sampleId.extended));
            } else if (m_id->currentIndex() >= 0) {
                m_payload = m_parsedSample.bytes;
                m_haveSample = true;
                status = tr("%1 · 样例 %2 字节 · 定义 %3 字节").arg(frameType).arg(m_payload.size()).arg(target->length);
            }
        }
        m_byteCount = std::max(target ? target->length : 0, int(m_payload.size()));
        m_frameStatus->setText(status);
        m_frameStatus->setVisible(!status.isEmpty());
        for (int i = 0; i < m_rows.size(); ++i) {
            const auto &signal = m_rows.at(i).signal;
            QString value = QStringLiteral("—");
            if (m_haveSample) {
                const auto decoded = signal.decode(m_payload);
                value = decoded.isValid() ? displayString(decoded) : tr("样例过短");
                if (decoded.isValid() && !signal.unit.isEmpty())
                    value += QLatin1Char(' ') + signal.unit;
                const auto description = signal.valueText(m_payload);
                if (!description.isEmpty())
                    value += QStringLiteral(" · ") + description;
            }
            m_table->item(i, 5)->setText(value);
        }
    }

    void updateSelection(bool locate = false)
    {
        const auto *row = currentRow();
        showSignalBits(m_bits, m_byteCount, row ? &row->signal : nullptr, m_haveSample ? &m_payload : nullptr,
                       locate && row ? row->signal.start : -1);
        QString details;
        if (row) {
            const auto &s = row->signal;
            const auto type = s.valueType == CanSignalDef::ValueType::Float32 ? QStringLiteral("Float32")
                             : s.valueType == CanSignalDef::ValueType::Double ? QStringLiteral("Float64")
                             : s.isSigned ? tr("有符号") : tr("无符号");
            details = tr("%1 · %2 · 比例 %3 · 偏移 %4").arg(row->key, type, number(s.factor), number(s.offset));
            if (!s.unit.isEmpty())
                details += tr(" · 单位 %1").arg(s.unit);
            if (s.minimum != s.maximum)
                details += tr(" · 范围 %1～%2").arg(number(s.minimum), number(s.maximum));
        }
        m_details->setText(details);
        m_details->setVisible(!details.isEmpty());
        const bool editable = row && row->line >= 0;
        if (m_editing) {
            m_add->setEnabled(m_inlineError.isEmpty());
            m_edit->setEnabled(editable && m_inlineError.isEmpty());
            m_remove->setEnabled(editable);
        }
        yesButton()->setEnabled(m_editing ? m_inlineError.isEmpty() : bool(selection()));
    }

    void editText();
    void editSignal(int index);

    void removeSignal()
    {
        const auto *row = currentRow();
        if (!row || row->line < 0)
            return;
        auto lines = m_inlineText.split(QLatin1Char('\n'));
        lines.removeAt(row->line);
        m_originalKeys.remove(row->key);
        setInlineText(lines.join(QLatin1Char('\n')), currentId());
    }

    const Project *m_project;
    QList<SignalCard> m_cards;
    bool m_editing;
    std::shared_ptr<const CanDatabase> m_dbc;
    std::optional<CanSignalSet> m_signalSet;
    QList<InlineSignal> m_inline;
    QList<CanId> m_ids;
    QList<Row> m_rows;
    QString m_inlineText;
    QHash<QString, QString> m_originalKeys;
    QString m_inlineError;
    QString m_dbcError;
    FluentQt::ComboBox *m_connection = nullptr;
    FluentQt::ComboBox *m_id;
    FluentQt::LineEdit *m_sample;
    FluentQt::CaptionLabel *m_definitionStatus;
    FluentQt::CaptionLabel *m_frameStatus;
    FluentQt::CaptionLabel *m_details;
    QBoxLayout *m_inspection = nullptr;
    BitMapView *m_bits;
    FluentQt::TableWidget *m_table;
    FluentQt::PushButton *m_add = nullptr;
    FluentQt::PushButton *m_edit = nullptr;
    FluentQt::PushButton *m_remove = nullptr;
    QString m_sampleText;
    Can::FrameSample m_parsedSample;
    QByteArray m_payload;
    bool m_haveSample = false;
    int m_byteCount = 0;
};

void CanSignalDialog::editText()
{
    AppDialog editor(tr("内联信号文本"), this);
    editor.setPreferredSize({730, 420});
    auto *text = new FluentQt::PlainTextEdit(editor.widget());
    text->setAccessibleName(tr("内联信号文本"));
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setPlaceholderText(QStringLiteral("201 ready : 0|1@1+ (1,0) [0|1] \"\""));
    text->setPlainText(m_inlineText);
    editor.viewLayout()->addWidget(text, 1);
    auto *problem = new FluentQt::CaptionLabel(editor.widget());
    problem->setWordWrap(true);
    editor.viewLayout()->addWidget(problem);
    editor.setValidator([&] {
        QString error;
        const bool valid = bool(CanSignalSet::build(m_dbc, text->toPlainText(), &error));
        problem->setText(error);
        return valid;
    });
    if (editor.exec() == QDialog::Accepted) {
        setInlineText(text->toPlainText(), currentId());
        QSet<QString> keys;
        for (const auto &entry : m_inline)
            keys.insert(inlineSignalKey(entry.id, entry.signal.name));
        m_originalKeys.removeIf([&](auto it) { return !keys.contains(it.key()); });
    }
}

void CanSignalDialog::editSignal(int index)
{
    const Row *row = index >= 0 ? &m_rows.at(index) : nullptr;
    if (row && row->line < 0)
        return;
    InlineSignal entry{currentId(), row ? row->signal : CanSignalDef{}, row ? row->line : -1};
    if (!row) {
        QSet<int> occupied;
        for (const auto &existing : std::as_const(m_rows)) {
            for (const int bit : existing.signal.bitPositions())
                occupied.insert(bit);
        }
        while (entry.signal.start < 511 && occupied.contains(entry.signal.start))
            ++entry.signal.start;
    }
    const auto &signal = entry.signal;
    const QString oldKey = row ? row->key : QString();
    const CanId oldId = entry.id;

    AppDialog editor(row ? tr("编辑信号") : tr("添加信号"), this);
    editor.setObjectName(QStringLiteral("canInlineSignalForm"));
    editor.setPreferredSize({875, 694});
    const QJsonObject binding{{QStringLiteral("canId"), Can::idText(entry.id)},
                              {QStringLiteral("canStartBit"), signal.start},
                              {QStringLiteral("canBitLength"), signal.length},
                              {QStringLiteral("canByteOrder"), signal.motorola ? CanByteOrder::MotorolaMsb : CanByteOrder::Intel},
                              {QStringLiteral("canSigned"), signal.isSigned},
                              {QStringLiteral("canFactor"), signal.factor},
                              {QStringLiteral("canOffset"), signal.offset}};
    auto *form = new CanSignalForm(binding, editor.widget(), m_signalSet ? &*m_signalSet : nullptr,
                                   m_cards.value(0).fd, &signal);
    editor.viewLayout()->addWidget(form, 1);
    form->setSample(m_sampleText);

    QString draft;
    editor.setValidator([&] {
        const auto values = form->values();
        if (!values)
            return false;
        entry.id = values->id;
        entry.signal = values->signal;

        auto lines = m_inlineText.split(QLatin1Char('\n'));
        const QString formatted = formatInlineSignal(entry);
        if (entry.line >= 0)
            lines[entry.line] = formatted;
        else if (lines.last().isEmpty())
            lines.last() = formatted;
        else
            lines.append(formatted);
        draft = lines.join(QLatin1Char('\n'));
        QString error;
        if (CanSignalSet::build(m_dbc, draft, &error))
            return true;
        form->fail(error);
        return false;
    });
    if (editor.exec() == QDialog::Accepted) {
        const QString next = inlineSignalKey(entry.id, entry.signal.name);
        const QString original = m_originalKeys.take(oldKey);
        if (!original.isEmpty() && oldId == entry.id)
            m_originalKeys.insert(next, original);
        setInlineText(draft, entry.id, next);
    }
}

} // namespace

std::optional<CanSignalEdit> editCanSignals(const Project *project, const QJsonObject &properties, QWidget *parent,
                                           std::function<bool(const CanSignalEdit &, QWidget *)> approve)
{
    CanSignalDialog dialog(project, {signalCard(properties)}, {}, {}, true, parent);
    dialog.reviewBeforeSave(std::move(approve));
    if (dialog.exec() == QDialog::Accepted)
        return dialog.edit();
    return std::nullopt;
}

std::optional<QJsonObject> editCanBitBinding(const Project *project, const QJsonObject &binding,
                                            const QJsonObject &card, QWidget *parent)
{
    const auto dbc = designerCanDatabase(project, card.value(QStringLiteral("dbcFile")).toString());
    auto signalSet = CanSignalSet::build(dbc, card.value(QStringLiteral("canSignals")).toString());
    if (!signalSet)
        signalSet = CanSignalSet::build(dbc, {});
    AppDialog dialog(tr("编辑 CAN 位域"), parent);
    dialog.setObjectName(QStringLiteral("canBitBindingDialog"));
    dialog.setPreferredSize({800, 650});
    auto *form = new CanSignalForm(binding, dialog.widget(), signalSet ? &*signalSet : nullptr,
                                   card.isEmpty() || card.value(QStringLiteral("canFd")).toBool());
    dialog.viewLayout()->addWidget(form, 1);
    std::optional<QJsonObject> result;
    dialog.setValidator([&] {
        if (auto values = form->values())
            result = std::move(values->binding);
        return result.has_value();
    });
    return dialog.exec() == QDialog::Accepted ? result : std::nullopt;
}

std::optional<CanSignalSelection> chooseCanSignal(const Project *project, const QJsonArray &widgets,
                                                  const QString &connection, const QString &key, QWidget *parent,
                                                  bool onlyCurrentConnection)
{
    QList<SignalCard> cards;
    visitWidgets(widgets, [&](const QJsonObject &widget) {
        const auto id = widget.value(QStringLiteral("id")).toString();
        if (widget.value(QStringLiteral("type")).toString() == QLatin1String("CanConfigCard") &&
            (!onlyCurrentConnection || id == connection))
            cards.append(signalCard(widget.value(QStringLiteral("properties")).toObject(), id));
        return true;
    });
    CanSignalDialog dialog(project, std::move(cards), connection, key, false, parent);
    if (dialog.exec() == QDialog::Accepted)
        return dialog.selection();
    return std::nullopt;
}

} // namespace fhmi
