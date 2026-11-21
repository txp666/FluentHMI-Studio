#pragma once

#include "core/chart_limits.h"

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QIODevice;
class QTemporaryFile;

namespace fhmi {

// The real-time chart's recordings, byte for byte as PyQt UI Designer's RealtimeLineChart saves and reads
// them (tests/data/chart_file_vectors.json), so a file saved by either app loads in the other.

// Python's repr(float): the shortest digits that read back as the same double, laid out the way Python lays
// them out (0.0001, 1e-05, 100.0, 1e+16, -0.0).
QString pythonFloatRepr(double value);

enum class ChartFileFormat
{
    Csv,
    Json,
};
// JSON for a path ending in .json (any case), CSV for anything else, as the Python chart decides.
ChartFileFormat chartFileFormat(const QString &path);

// One curve's samples in order: each call stores the next one in *value; false after the last.
using ChartSamples = std::function<bool(double *value)>;

// CSV: a header row of names, then one row per sample number, blank where a curve has fewer (rows end with
// CRLF, fields quoted as Python's csv module quotes them). JSON: {"name":[...],...}, or
// [{"name":...,"values":[...]},...] when two curves share a name.
bool writeChartFile(QIODevice *out, ChartFileFormat format, const QStringList &names,
                    const QList<ChartSamples> &samples, QString *error = nullptr);

// Curves from a saved file (UTF-8), read as the Python chart reads them, or false with *error for a file it
// refuses (a sample that is not a finite number, JSON of another shape).
bool readChartFile(const QByteArray &content, ChartFileFormat format, QStringList *names, QList<QList<double>> *values,
                   QString *error);

// A JSON document as Python's json module reads it: object members keep their order (repeated names too),
// integers and floats stay apart and -0.0 keeps its sign. QJsonDocument sorts members and turns -0.0 into 0.
struct PyJsonValue
{
    enum Kind
    {
        Null,
        Bool,
        Int,
        Float,
        String,
        Array,
        Object,
    };
    Kind kind = Null;
    bool flag = false;
    double number = 0; // an Int as float() gives it (infinite when too large)
    QString text;
    std::vector<QString> keys;      // Object: member names, in order
    std::vector<PyJsonValue> items; // Array: elements; Object: member values
};
bool parsePyJson(const QString &text, PyJsonValue *out, QString *error);

// Every sample of one curve for the whole run: all of them in a temporary file (float64), the newest
// `capacity` in memory for the plot.
class ChartHistory
{
  public:
    explicit ChartHistory(int capacity = 10000);
    ~ChartHistory();
    ChartHistory(const ChartHistory &) = delete;
    ChartHistory &operator=(const ChartHistory &) = delete;

    // False for a sample that is not finite, or (with *error) when the file cannot be written.
    bool append(double value, QString *error = nullptr);
    qint64 count() const { return m_count; }
    // The newest `n` samples, at most `capacity`.
    QList<double> tail(int n) const;
    std::optional<double> last() const;
    // Every sample from the first. Reading sets *failed if the file cannot be read; nothing may be appended
    // while the samples are being read.
    ChartSamples samples(bool *failed) const;
    void clear();

  private:
    int m_capacity;
    std::deque<double> m_cache;
    qint64 m_count = 0;
    std::unique_ptr<QTemporaryFile> m_file;
    mutable bool m_readSinceWrite = false;
};

struct TimingLevels
{
    TimingMask high = 0;
    TimingMask valid = 0;

    bool operator==(const TimingLevels &other) const { return high == other.high && valid == other.valid; }
    std::optional<bool> level(TimingMask mask) const;
    void setLevel(int index, std::optional<bool> value);
};

struct TimingSample
{
    double seconds = 0;
    TimingLevels levels;
};

struct TimingRecording
{
    QStringList names;
    std::deque<TimingSample> samples;
};

std::optional<TimingRecording> readTimingRecording(const QByteArray &content, ChartFileFormat format, QString *error);

// Transition records share one clock. The temporary recording keeps complete history independently of the viewport.
class TimingChartHistory
{
  public:
    TimingChartHistory();
    ~TimingChartHistory();
    bool isEmpty() const { return m_count == 0; }
    bool append(const TimingSample &sample, QString *error);
    bool write(QIODevice *out, ChartFileFormat format, const QStringList &names, double end, QString *error) const;
    void clear();

  private:
    std::unique_ptr<QTemporaryFile> m_file;
    qint64 m_count = 0;
};

QByteArray timingCsvHeader(const QStringList &names);
QByteArray timingCsvRow(const TimingSample &sample, qsizetype channels);

} // namespace fhmi
