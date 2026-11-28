#include "designer/enum_mapping_editor.h"

#include "core/value_text.h"
#include "runtime/app_dialog.h"
#include "runtime/hmi_widget.h"
#include "runtime/ui_helpers.h"

#include <FluentQtWidgets/Color.h>
#include <FluentQtWidgets/Theme.h>
#include <FluentQtWidgets/Views/ItemViews.h>
#include <FluentQtWidgets/Widgets/Button.h>
#include <FluentQtWidgets/Widgets/ComboBox.h>
#include <FluentQtWidgets/Widgets/Label.h>
#include <FluentQtWidgets/Widgets/LineEdit.h>

#include <QtCore/QSignalBlocker>
#include <QtGui/QAction>
#include <QtGui/QPainter>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QVBoxLayout>

namespace fhmi {
namespace {

class MappingDialog final : public AppDialog
{
  public:
    MappingDialog(const QString &type, const QJsonObject &props, const QJsonObject &tables, bool canShare,
                  QWidget *parent)
        : AppDialog(tr("可视化枚举"), parent), m_props(props), m_led(type == u"LEDIndicator"), m_combo(type == u"ComboBox"),
          m_edit{props.value("textList").toString(), props.value("enumTable").toString(), tables,
                 props.value("ledEffects").toObject()}
    {
        setObjectName(QStringLiteral("enumMappingEditor"));
        setPreferredSize({720, 510});
        auto *source = new QHBoxLayout;
        m_source = new FluentQt::ComboBox(this);
        m_source->addItem(m_combo ? tr("新枚举表") : tr("仅此控件"), {}, QString());
        for (const auto &name : tables.keys())
            m_source->addItem(name, {}, name);
        selectValue(m_source, m_edit.tableName);
        source->addWidget(m_source, 1);
        auto *name = new FluentQt::LineEdit(this);
        name->setPlaceholderText(tr("新枚举表名称"));
        source->addWidget(name, 1);
        auto *share = new FluentQt::PushButton(tr("另存为工程枚举"), this);
        source->addWidget(share);
        viewLayout()->addLayout(source);
        m_source->setVisible(canShare);
        name->setVisible(canShare);
        share->setVisible(canShare);

        m_rows = new FluentQt::TableWidget(this);
        m_rows->setObjectName(QStringLiteral("enumMappingRows"));
        QStringList headers{tr("数值"), tr("文字"), tr("颜色")};
        if (m_led)
            headers.append(tr("灯效"));
        m_rows->setColumnCount(headers.size());
        m_rows->setHorizontalHeaderLabels(headers);
        m_rows->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        m_rows->verticalHeader()->setDefaultSectionSize(38);
        m_rows->setSelectionBehavior(QAbstractItemView::SelectRows);
        viewLayout()->addWidget(m_rows, 1);
        auto *actions = new QHBoxLayout;
        auto *add = new FluentQt::PushButton(tr("添加"), this);
        auto *remove = new FluentQt::PushButton(tr("删除所选"), this);
        actions->addWidget(add);
        actions->addWidget(remove);
        actions->addStretch();
        viewLayout()->addLayout(actions);

        auto *sample = new QHBoxLayout;
        sample->addWidget(new FluentQt::BodyLabel(tr("预览值"), this));
        m_sample = new FluentQt::LineEdit(this);
        m_sample->setObjectName(QStringLiteral("enumPreviewValue"));
        m_sample->setText(props.value(m_led ? "stateValue" : "value").toVariant().toString());
        if (m_sample->text().isEmpty())
            m_sample->setText(QStringLiteral("0"));
        m_sample->setMaximumWidth(110);
        sample->addWidget(m_sample);
        m_preview = createHmiWidget(type, props, this);
        m_preview->setMinimumSize(180, m_led ? 45 : 70);
        sample->addWidget(m_preview, 1);
        viewLayout()->addLayout(sample);
        m_problem = new FluentQt::BodyLabel(this);
        m_problem->setWordWrap(true);
        viewLayout()->addWidget(m_problem);

        connect(add, &QPushButton::clicked, this, [this] {
            addRow({}, {}, {}, {});
            m_rows->setCurrentCell(m_rows->rowCount() - 1, 0);
            m_rows->editItem(m_rows->currentItem());
            preview();
        });
        connect(remove, &QPushButton::clicked, this, [this] {
            if (m_rows->currentRow() >= 0)
                m_rows->removeRow(m_rows->currentRow());
            preview();
        });
        connect(m_sample, &QLineEdit::textChanged, this, [this] { preview(); });
        connect(m_rows, &QTableWidget::itemChanged, this, [this] { preview(); });
        connect(FluentQt::ThemeManager::instance(), &FluentQt::ThemeManager::effectiveThemeChanged, this,
                [this] { preview(); });
        connect(m_source, &FluentQt::ComboBox::currentIndexChanged, this, [this] {
            if (!storeCurrent()) {
                const QSignalBlocker blocker(m_source);
                selectValue(m_source, m_edit.tableName);
                return;
            }
            m_edit.tableName = selectedValue(m_source);
            loadRows();
        });
        connect(share, &QPushButton::clicked, this, [this, name] {
            const auto target = name->text().trimmed();
            if (target.isEmpty() || m_edit.tables.contains(target)) {
                m_problem->setText(tr("请填写尚未使用的枚举表名称"));
                m_problem->show();
                name->setFocus();
                return;
            }
            QString mapping;
            QJsonObject effects;
            if (!readValidRows(mapping, effects))
                return;
            // Save As forks the edited rows; widgets using the source table keep their definition.
            m_edit.tables.insert(target, mapping == m_originalRows ? currentMapping() : mapping);
            if (m_led && effects != m_originalEffects)
                m_edit.effects = effects;
            const QSignalBlocker blocker(m_source);
            m_source->addItem(target, {}, target);
            selectValue(m_source, target);
            m_edit.tableName = target;
            loadRows();
            name->clear();
        });
        loadRows();
        setValidator([this] {
            if (m_combo && m_edit.tableName.isEmpty()) {
                m_problem->setText(tr("请为枚举表命名并另存为工程枚举"));
                m_problem->show();
                return false;
            }
            return storeCurrent();
        });
    }

    const EnumMappingEdit &edit() const { return m_edit; }

  private:
    QString currentMapping() const
    {
        return m_edit.tableName.isEmpty() ? m_edit.localMapping : m_edit.tables.value(m_edit.tableName).toString();
    }

    void addRow(const QString &value, const QString &label, const QString &color, const QString &effect)
    {
        const QSignalBlocker blocker(m_rows);
        const int row = m_rows->rowCount();
        m_rows->insertRow(row);
        m_rows->setItem(row, 0, new QTableWidgetItem(value));
        m_rows->setItem(row, 1, new QTableWidgetItem(label));
        auto *tint = new FluentQt::LineEdit(m_rows);
        tint->setPlaceholderText(tr("自动"));
        tint->setText(color);
        auto *pick = tint->addAction(QIcon(), QLineEdit::TrailingPosition);
        pick->setToolTip(tr("选择颜色"));
        const auto swatch = [pick](const QString &text) {
            QPixmap icon(18, 18);
            icon.fill(Qt::transparent);
            QPainter painter(&icon);
            painter.setPen(Qt::gray);
            const QColor color(text);
            painter.setBrush(color.isValid() ? color : Qt::transparent);
            painter.drawRoundedRect(1, 1, 15, 15, 3, 3);
            painter.end();
            pick->setIcon(QIcon(icon));
        };
        swatch(color);
        connect(pick, &QAction::triggered, this, [this, tint] {
            const auto chosen = chooseColor(QColor(tint->text()), tr("选择颜色"), this);
            if (chosen.isValid())
                tint->setText(chosen.name());
        });
        connect(tint, &QLineEdit::textChanged, this, [this, swatch](const QString &text) {
            swatch(text);
            preview();
        });
        m_rows->setCellWidget(row, 2, tint);
        if (m_led) {
            auto *choice = new FluentQt::ComboBox(m_rows);
            choice->addItem(tr("常亮"), {}, QStringLiteral("steady"));
            choice->addItem(tr("熄灭"), {}, QStringLiteral("off"));
            choice->addItem(tr("慢闪（1 秒）"), {}, QStringLiteral("slow"));
            choice->addItem(tr("快闪（0.5 秒）"), {}, QStringLiteral("fast"));
            selectValue(choice, effect.isEmpty() ? QStringLiteral("steady") : effect);
            m_rows->setCellWidget(row, 3, choice);
            connect(choice, &FluentQt::ComboBox::currentIndexChanged, this, [this] { preview(); });
        }
    }

    void loadRows()
    {
        const QSignalBlocker blocker(m_rows);
        m_rows->setRowCount(0);
        const auto mapping = currentMapping();
        const auto entries = parseValueText(mapping);
        const auto effects = parseLedEffects(m_edit.effects);
        for (const auto &line : mapping.split(u'\n', Qt::SkipEmptyParts)) {
            if (line.trimmed().isEmpty())
                continue;
            const auto value = line.section(u'=', 0, 0).trimmed();
            const auto key = valueTextKey(value);
            const auto entry = entries.value(key);
            addRow(value, entry.text, entry.color, effects.value(key));
        }
        m_originalEffects = {};
        readRows(m_originalRows, m_originalEffects);
        preview();
    }

    QString highlightDuplicateValues()
    {
        QStringList keys;
        QHash<QString, int> counts;
        for (int row = 0; row < m_rows->rowCount(); ++row) {
            keys.append(valueTextKey(m_rows->item(row, 0)->text().trimmed()));
            ++counts[keys.last()];
        }

        const QSignalBlocker blocker(m_rows);
        const QBrush background(FluentQt::color(FluentQt::FluentSystemColor::CriticalBackground));
        const QBrush foreground(FluentQt::color(FluentQt::FluentSystemColor::CriticalForeground));
        QStringList conflicts;
        for (int row = 0; row < keys.size(); ++row) {
            auto *cell = m_rows->item(row, 0);
            const bool duplicate = !keys[row].isEmpty() && counts.value(keys[row]) > 1;
            const QString hint = duplicate ? tr("第 %1 行的数值重复").arg(row + 1) : QString();
            cell->setData(Qt::BackgroundRole, duplicate ? QVariant(background) : QVariant());
            cell->setData(Qt::ForegroundRole, duplicate ? QVariant(foreground) : QVariant());
            cell->setToolTip(hint);
            cell->setData(Qt::AccessibleDescriptionRole, hint);
            if (duplicate)
                conflicts.append(QString::number(row + 1));
        }
        return conflicts.isEmpty() ? QString() : tr("第 %1 行包含重复的枚举值").arg(conflicts.join(QStringLiteral(", ")));
    }

    QString readRows(QString &mapping, QJsonObject &effects)
    {
        const auto duplicates = highlightDuplicateValues();
        if (!duplicates.isEmpty())
            return duplicates;
        QStringList lines;
        for (int row = 0; row < m_rows->rowCount(); ++row) {
            const auto value = m_rows->item(row, 0)->text().trimmed();
            const auto key = valueTextKey(value);
            const auto label = m_rows->item(row, 1)->text().trimmed();
            const auto color = static_cast<QLineEdit *>(m_rows->cellWidget(row, 2))->text().trimmed();
            if (key.isEmpty() || label.isEmpty() || label.contains(u'|') || label.contains(u'\n'))
                return tr("请检查第 %1 行的数值和文字").arg(row + 1);
            lines.append(value + u'=' + label + (color.isEmpty() ? QString() : u'|' + color));
            if (m_led) {
                const auto effect = selectedValue(static_cast<FluentQt::ComboBox *>(m_rows->cellWidget(row, 3)));
                if (effect != u"steady")
                    effects.insert(key, effect);
            }
        }
        mapping = lines.join(u'\n');
        QString problem;
        parseValueText(mapping, &problem);
        return problem;
    }

    bool readValidRows(QString &mapping, QJsonObject &effects)
    {
        const auto problem = readRows(mapping, effects);
        m_problem->setText(problem);
        m_problem->setVisible(!problem.isEmpty());
        m_preview->setVisible(problem.isEmpty());
        return problem.isEmpty();
    }

    void preview()
    {
        QString mapping;
        QJsonObject effects;
        if (!readValidRows(mapping, effects))
            return;
        auto props = m_props;
        if (m_combo) {
            const QString name = m_edit.tableName.isEmpty() ? QStringLiteral("_preview") : m_edit.tableName;
            props.insert(QStringLiteral("enumTable"), name);
            m_preview->setEnumTables({{name, mapping}});
            m_preview->setProperties(props);
            m_preview->applyBoundValue(m_sample->text(), {});
            return;
        }
        props.insert(QStringLiteral("enumTable"), m_edit.tableName);
        if (m_edit.tableName.isEmpty())
            props.insert(QStringLiteral("textList"), mapping);
        else
            m_preview->setEnumTables({{m_edit.tableName, mapping}});
        if (m_led)
            props.insert(QStringLiteral("ledEffects"), effects);
        props.insert(m_led ? QStringLiteral("stateValue") : QStringLiteral("value"), m_sample->text());
        m_preview->setProperties(props);
    }

    bool storeCurrent()
    {
        QString mapping;
        QJsonObject effects;
        if (!readValidRows(mapping, effects))
            return false;
        if (mapping != m_originalRows) {
            if (m_edit.tableName.isEmpty())
                m_edit.localMapping = mapping;
            else
                m_edit.tables.insert(m_edit.tableName, mapping);
            m_originalRows = mapping;
        }
        if (m_led && effects != m_originalEffects) {
            m_edit.effects = effects;
            m_originalEffects = effects;
        }
        return true;
    }

    QJsonObject m_props;
    bool m_led;
    bool m_combo;
    EnumMappingEdit m_edit;
    QString m_originalRows;
    QJsonObject m_originalEffects;
    FluentQt::ComboBox *m_source;
    FluentQt::TableWidget *m_rows;
    FluentQt::LineEdit *m_sample;
    FluentQt::BodyLabel *m_problem;
    HmiWidget *m_preview;
};
} // namespace

std::optional<EnumMappingEdit> editEnumMapping(const QString &type, const QJsonObject &properties,
                                               const QJsonObject &tables, bool canShare, QWidget *parent)
{
    MappingDialog dialog(type, properties, tables, canShare, parent);
    if (dialog.exec() == QDialog::Accepted)
        return dialog.edit();
    return std::nullopt;
}
} // namespace fhmi
