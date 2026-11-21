#pragma once

#include "core/command_encoder.h"
#include "core/protocol_parser.h"

#include <QtCore/QMap>
#include <QtWidgets/QWidget>

namespace FluentQt {
class ComboBox;
class TableWidget;
class DropDownPushButton;
class PushButton;
}
class QToolButton;

namespace fhmi {

class CommandInputsEditor : public QWidget
{
    Q_OBJECT
  public:
    using InputValues = QMap<QString, CommandEncoder::InputValue>;

    CommandInputsEditor(const QJsonObject &properties, InputValues values, const QStringList &dataTypes,
                        const QJsonArray &widgets, bool canCreate, QWidget *parent);

    const QList<CommandEncoder::CommandInput> &rows() const { return m_rows; }
    QJsonArray toJson() const;
    InputValues values() const;
    QMap<QString, QString> pendingInputs() const;
    QString validate() const;
    void useRows(const QList<CommandEncoder::CommandInput> &rows);
    void setRegisterAddress(int address);

  signals:
    void changed(const QString &previousVariable, const QString &variable);

  private:
    void append(const QString &id);
    void move(int step);
    void populate();
    void updateButtons();
    void fillSources(FluentQt::ComboBox *source, const QString &current);
    void updateRegisterCells();

    InputValues m_existing;
    QMap<QString, QString> m_sourceTypes;
    QMap<QString, QString> m_pending;
    QList<CommandEncoder::CommandInput> m_rows;
    QStringList m_dataTypes;
    bool m_hasVariables;
    bool m_single;
    bool m_registers;
    int m_registerAddress = 0;
    int m_newInput = 0;
    FluentQt::TableWidget *m_table = nullptr;
    FluentQt::ComboBox *m_singleSource = nullptr;
    FluentQt::PushButton *m_create;
    FluentQt::DropDownPushButton *m_choose;
    QToolButton *m_up;
    QToolButton *m_down;
    QToolButton *m_remove;
};

class FrameFieldInputsEditor : public QWidget
{
    Q_OBJECT
  public:
    using InputValues = CommandInputsEditor::InputValues;
    FrameFieldInputsEditor(const QJsonArray &bindings, InputValues inputs, bool canCreate, QWidget *parent);

    void setFormat(const std::optional<HexFrameConfig> &format, const QJsonObject &settings);
    QJsonArray bindings() const;
    InputValues sampleValues() const;
    QMap<QString, QString> pendingInputs() const;
    QString validate() const;
    int fieldRow(const QString &name) const { return m_fields.indexOf(name); }
    void selectField(const QString &name);

  signals:
    void changed();
    void fieldSelected(const QString &name);

  private:
    void populate();

    InputValues m_existing;
    QMap<QString, QString> m_pending;
    QMap<QString, QJsonObject> m_bindings;
    QMap<QString, HexField> m_definitions;
    QJsonObject m_defaults;
    QStringList m_required;
    QStringList m_fields;
    qsizetype m_headerSize = 0;
    FluentQt::TableWidget *m_table = nullptr;
    FluentQt::PushButton *m_create = nullptr;
};

} // namespace fhmi
