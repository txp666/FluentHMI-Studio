#pragma once

#include "runtime/hmi_widget.h"

#include <memory>
#include <vector>

class QLabel;
class QTextEdit;
class QTimer;

namespace FluentQt {
class BodyLabel;
class ComboBox;
class CaptionLabel;
class CheckBox;
class LineEdit;
class PushButton;
class StrongBodyLabel;
class TableWidget;
class ToolButton;
} // namespace FluentQt

namespace fhmi {

class ChartHeader;
class ChartHistory;
class ChartPlot;

// Multi-curve live chart; every curve has its own parse configuration, stored as
// _curve_{i}_{field} properties exactly as PyQt UI Designer stores them. As PyQt UI Designer's
// RealtimeLineChart does, it keeps every sample of the run and plots the last 显示点数 of them, shows each
// curve's value now or at a cursor in a header above the plot, saves the whole run and lays a saved run over
// the live curves for comparison.
class RealtimeChartWidget : public HmiWidget
{
  public:
    RealtimeChartWidget(const WidgetMeta &meta, QWidget *parent);
    ~RealtimeChartWidget() override;

    struct Curve
    {
        QString name;
        BindingEntry binding;
    };
    // The curve configurations a property set describes.
    static std::vector<Curve> curvesFromProperties(const QJsonObject &props);

    // A sample for a curve, as one arriving from a link is added.
    void appendSample(int curve, double value);
    // What 保存 and 加载 do once a file is chosen; a failure shows in the toolbar, where 实时 or the cursor's
    // sample does.
    void saveHistory(const QString &path);
    void loadReference(const QString &path);
    void clearReference();

  protected:
    void apply() override;
    void onAttached() override;
    void onDetached() override;

  private:
    struct Reference
    {
        QString name;
        QList<double> values;
    };

    void rebuildCurves();
    void applyTheme();
    // The plot and the header from the samples now; data arriving schedules it.
    void refresh();
    QColor curveColor(int index) const;
    // The sample number the live window starts at, which a loaded run is lined up with.
    qint64 windowStart() const;
    void showLiveValues();
    void showCursorValues();
    // The header: the live curves' readings, then the loaded run's at a sample number.
    void showValues(const QStringList &live, qint64 referenceSample);
    // A click on a header entry hides or shows its curve.
    void toggleCurve(int index);

    ChartHeader *m_header = nullptr;
    ChartPlot *m_plot = nullptr;
    QLabel *m_mode = nullptr;
    FluentQt::LineEdit *m_displayEdit = nullptr;
    FluentQt::ToolButton *m_clearReference = nullptr;
    QTimer *m_refreshTimer = nullptr;
    std::vector<Curve> m_curves;
    std::vector<std::unique_ptr<ChartHistory>> m_histories;
    QList<Reference> m_references;
    int m_displayPoints = 200;
    bool m_cursorMode = false;
    double m_cursorX = 0;
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
    // Bytes as HEX or text: received text the way its link spells it, what was sent as UTF-8.
    QString render(const QByteArray &bytes, bool received) const;

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
