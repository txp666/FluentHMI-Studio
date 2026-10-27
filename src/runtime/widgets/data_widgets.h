#pragma once

#include "runtime/hmi_widget.h"

#include <vector>

class QTextEdit;
class QTimer;

namespace FluentQt {
class BodyLabel;
class ComboBox;
class CaptionLabel;
class CheckBox;
class LineEdit;
class PushButton;
class RealtimePlotWidget;
class StrongBodyLabel;
class TableWidget;
} // namespace FluentQt

namespace fhmi {

// Multi-curve live chart; every curve has its own parse configuration, stored as
// _curve_{i}_{field} properties exactly as PyQt UI Designer stores them.
class RealtimeChartWidget : public HmiWidget
{
  public:
    RealtimeChartWidget(const WidgetMeta &meta, QWidget *parent);

    struct Curve
    {
        QString name;
        BindingEntry binding;
    };
    // The curve configurations a property set describes.
    static std::vector<Curve> curvesFromProperties(const QJsonObject &props);

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    void exportCsv();

    FluentQt::StrongBodyLabel *m_title = nullptr;
    FluentQt::RealtimePlotWidget *m_plot = nullptr;
    std::vector<Curve> m_curves;
};

class DataTableWidget : public HmiWidget
{
  public:
    DataTableWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;

  private:
    void onMessage(const Message &message);
    bool logging() const;

    FluentQt::TableWidget *m_table = nullptr;
    QStringList m_columns;
};

// Time-stamped log of every parsed frame and every link event.
class LogBoxWidget : public HmiWidget
{
  public:
    LogBoxWidget(const WidgetMeta &meta, QWidget *parent);
    void appendLog(const QString &text, const QString &level = QStringLiteral("info"));

  protected:
    void apply() override;
    void onAttached() override;

  private:
    QTextEdit *m_output = nullptr;
};

// Raw received bytes (text or HEX), what was sent, and link events.
class DataRecvCardWidget : public HmiWidget
{
  public:
    DataRecvCardWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;
    void paintEvent(QPaintEvent *event) override;

  private:
    void append(const QString &html);
    QString render(const QByteArray &bytes) const;

    FluentQt::StrongBodyLabel *m_title = nullptr;
    FluentQt::CaptionLabel *m_counter = nullptr;
    FluentQt::CheckBox *m_hex = nullptr;
    FluentQt::CheckBox *m_autoScroll = nullptr;
    QTextEdit *m_output = nullptr;
    int m_count = 0;
};

class DataSendCardWidget : public HmiWidget
{
  public:
    DataSendCardWidget(const WidgetMeta &meta, QWidget *parent);

  protected:
    void apply() override;
    void onAttached() override;
    void paintEvent(QPaintEvent *event) override;

  private:
    void sendInput();
    void flashTip(const QString &tip);

    FluentQt::StrongBodyLabel *m_title = nullptr;
    FluentQt::LineEdit *m_input = nullptr;
    FluentQt::CheckBox *m_hex = nullptr;
    FluentQt::ComboBox *m_newline = nullptr;
    FluentQt::PushButton *m_send = nullptr;
    QTimer *m_tipTimer = nullptr;
};

// The dark terminal of the log and receive views, the same in both themes.
QTextEdit *createTerminal(QWidget *parent);
QString terminalEscape(const QString &text);

} // namespace fhmi
