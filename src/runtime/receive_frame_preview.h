#pragma once

#include "core/binding.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <functional>
#include <memory>

class QJsonObject;
class QHBoxLayout;
class QPlainTextEdit;
class QLabel;

namespace FluentQt {
class BodyLabel;
class TableWidget;
}

namespace fhmi {

class FrameBytesView;
class FrameDiagram;
class FrameCoveragePreview;
class CanSignalSet;
struct CanFrame;

struct ReceivedField
{
    QString key;
    bool jsonPath = false;
};

class ReceivePreview : public QWidget
{
    Q_OBJECT

  public:
    explicit ReceivePreview(bool binary, QWidget *parent = nullptr);
    void addSampleAction(QWidget *action);
    void setSettings(const QJsonObject &settings, std::optional<BindingEntry> binding = std::nullopt,
                     bool strict = false, bool rawSample = false);
    void setSample(const QString &sample);
    void loadFrameSample(const QJsonObject &settings, const std::optional<QString> &sample);
    QString savedSample() const;
    void generateSample();
    void locateBinding();
    void showConfiguredFields();
    std::optional<ReceivedField> selectedField() const;
    void selectOnlyDecodedField();
    int selectedHexField() const;
    int selectedByte() const { return m_selectedByte; }
    QByteArray selectedSampleByte() const;
    void selectHexField(int index);

  signals:
    void selectionChanged();
    void sampleEdited();
    void samplePasted(const QString &previousSample);

  private:
    void refresh();
    void refreshHex();
    void refreshText();
    void markBindingRow(int row);
    void showBindingValue(const QString &selection, const QVariant &value);

    const bool m_binary;
    bool m_generated = false;
    bool m_showConfiguredFields = false;
    bool m_strict = false;
    bool m_rawSample = false;
    int m_bindingRow = -1;
    int m_selectedByte = -1;
    QJsonObject m_settings;
    std::optional<BindingEntry> m_binding;
    QHBoxLayout *m_sampleActions = nullptr;
    QPlainTextEdit *m_input = nullptr;
    FluentQt::BodyLabel *m_status = nullptr;
    QLabel *m_bindingValue = nullptr;
    FrameBytesView *m_bytes = nullptr;
    FrameDiagram *m_diagram = nullptr;
    FrameCoveragePreview *m_frameCoverage = nullptr;
    FluentQt::TableWidget *m_table = nullptr;
};

using SaveReceivedField = std::function<void(const QString &field, bool jsonPath)>;
using SaveReceivedBinding = std::function<void(const QJsonObject &binding, const QString &name)>;
void openReceiveFramePreview(const QJsonObject &settings, QWidget *parent, const QByteArray &sample = {},
                             SaveReceivedField saveField = {}, const QString &templateName = {});
void openReceiveGroupPreview(const QJsonObject &settings, QWidget *parent, SaveReceivedField saveField = {},
                             const QByteArray &sample = {});
void openReceiveTextPreview(const QJsonObject &settings, QWidget *parent, const QString &sample = {},
                            SaveReceivedField saveField = {});
void openCanPacketPreview(const CanFrame &frame, std::shared_ptr<const CanSignalSet> definitions,
                          const DataSource &source, QWidget *parent, SaveReceivedBinding save = {});
void openModbusPacketPreview(const QByteArray &packet, const QString &parser, bool request, const Message &decoded,
                             const DataSource &source, QWidget *parent, SaveReceivedBinding save = {});

} // namespace fhmi
