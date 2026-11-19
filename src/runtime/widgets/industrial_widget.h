#pragma once

#include "core/industrial_catalog.h"
#include "runtime/hmi_widget.h"
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>

namespace fhmi {
class IndustrialWidget : public HmiWidget
{
  public:
    IndustrialWidget(const WidgetMeta &meta, QWidget *parent);
    void applyBoundValue(const QVariant &value, const BindingEntry &binding) override;
    void setBitProfiles(const QJsonObject &profiles);
    QJsonObject bitProfile() const;
    std::optional<int> encoderDirection() const { return m_encoderDirection; }
    QList<double> cellValues() const { return m_cells; }

    std::optional<double> reading() const { return m_reading; }
    bool animationRunning() const { return m_timer.isActive(); }
    double animationRate() const;
    QByteArray renderedSvg() const;
    QHash<QString, double> svgDrivers() const;
    QString readingText() const;

  protected:
    void apply() override;
    void paintEvent(QPaintEvent *) override;
    void bindingStatusChanged(BindingStatus status) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void onAttached() override;
    void onDetached() override;

  private:
    void setReading(const QVariant &value);
    void updateReadingDescription();
    void validateReading();
    bool limitsEnabled() const;
    QString limitText(double value) const;
    QString sampleText() const;
    double magnitude() const;
    double encoderPulses() const;
    bool active() const;
    bool moving() const;
    void updateAnimation();
    bool m_dataError = false;
    QJsonObject m_bitProfiles;
    std::optional<double> m_previousEncoder;
    std::optional<int> m_encoderDirection;
    QList<double> m_cells;
    QVariantMap m_supply;

    QJsonObject m_definition;
    IndustrialCapabilities m_capabilities;
    std::optional<double> m_reading;
    QTimer m_timer;
    double m_phase = 0;
    QElapsedTimer m_animationClock;
};
} // namespace fhmi
