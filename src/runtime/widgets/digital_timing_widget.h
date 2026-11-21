#pragma once

#include "core/chart_file.h"
#include "runtime/hmi_widget.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <optional>

namespace FluentQt {
class CaptionLabel;
class ComboBox;
class PushButton;
class StrongBodyLabel;
class ToolButton;
} // namespace FluentQt

namespace fhmi {

class DigitalTimingPlot;

class DigitalTimingChartWidget : public HmiWidget
{
  public:
    DigitalTimingChartWidget(const WidgetMeta &meta, QWidget *parent);
    void setBitProfiles(const QJsonObject &profiles);
    QJsonObject bitProfile() const;
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;
    bool saveHistory(const QString &path, QString *error = nullptr) const;
    bool loadHistory(const QString &path, QString *error = nullptr);

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;
    void bindingStatusChanged(BindingStatus status) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    std::optional<quint16> word(const QVariant &value) const;
    qreal elapsed() const;
    void bindChannels();
    void updatePointChannels(const Message &values);
    void appendSample(const TimingSample &sample);
    QJsonArray channelDefinitions() const;
    QJsonArray displayedChannels() const;
    QString channelPreference() const;
    void setWindowSeconds(int seconds);
    void editWindow();
    void openRecording();
    void returnToLive();
    void showExportMenu();
    void updateRuntimeState();
    void startAutoExport();
    void stopAutoExport();
    void writeAutoExport(const QByteArray &bytes);
    void flushAutoExport();
    void autoExportFailed(const QString &error);
    void refresh();
    void updateReadout();
    void updateRefreshTimer();
    void updateChannels();
    void selectChannels();

    DigitalTimingPlot *m_plot;
    FluentQt::StrongBodyLabel *m_title;
    FluentQt::CaptionLabel *m_readout;
    FluentQt::ComboBox *m_window;
    FluentQt::PushButton *m_channels;
    FluentQt::PushButton *m_follow;
    FluentQt::ToolButton *m_export;
    FluentQt::ToolButton *m_import;
    QJsonObject m_bitProfiles;
    QElapsedTimer m_clock;
    QTimer m_refreshTimer;
    TimingChartHistory m_history;
    bool m_historyFailed = false;
    int m_windowSeconds = 10;
    int m_customWindowIndex = -1;
    QStringList m_channelNames;
    QString m_recordingName;
    TimingMask m_recordingMask = AllTimingChannels;
    QFile m_autoFile;
    QTimer m_autoFlushTimer;
    QString m_exportDirectory;
    bool m_autoExport = false;
    qsizetype m_autoColumns = 0;
    struct Input
    {
        std::optional<BindingEntry> binding;
        RoutedSample source;
    };
    QList<Input> m_inputs;
    TimingMask m_channelMask = AllTimingChannels;
    int m_bitWidth = 8;
};

} // namespace fhmi
