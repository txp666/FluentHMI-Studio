#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantMap>

#include <memory>

namespace fhmi {

// One decoded message: field name -> value (qlonglong, double, QString, bool, or nested
// QVariantMap / QVariantList for JSON lines).
using Message = QVariantMap;

// Incremental frame parsers. feed() returns every complete message in arrival order and
// keeps partial input for the next call.
class Parser
{
  public:
    virtual ~Parser() = default;
    virtual QList<Message> feed(const QByteArray &data) = 0;
    virtual void reset() = 0;
};

// Frames raw widget bindings when both delimiters are configured. Without both, the read
// chunk stays the input unit: one widget's byte offset cannot imply a packet length.
class RawFrameBuffer
{
  public:
    explicit RawFrameBuffer(QByteArray header = {}, QByteArray footer = {}, int maxBuffer = 65536);

    QList<QByteArray> feed(const QByteArray &data);
    void reset() { m_buf.clear(); }

  private:
    QByteArray m_header;
    QByteArray m_footer;
    int m_maxBuffer;
    QByteArray m_buf;
};

// Newline-terminated text: JSON objects, key=value pairs, or bare CSV (named ch0, ch1, ...).
class LineParser : public Parser
{
  public:
    explicit LineParser(QString separator = QStringLiteral(","), int maxBuffer = 65536);

    QList<Message> feed(const QByteArray &data) override;
    void reset() override;

    // Exposed for links that treat one transport message as one line (MQTT).
    Message parseLine(const QString &text) const;
    static QVariant autoNumber(const QString &text);

  private:
    QString m_separator;
    int m_maxBuffer;
    QByteArray m_buf;
    bool m_discarding = false;
};

struct HexField
{
    QString name;
    int offset = 0;
    int length = 0;
    QString type; // uint8, int8, uint16, int16, float32, anything else = hex text
};

// Header ... footer delimited binary frames with fixed-offset big-endian fields.
class HexFrameParser : public Parser
{
  public:
    HexFrameParser(QByteArray header, QByteArray footer, QList<HexField> fields, int maxBuffer = 65536);

    QList<Message> feed(const QByteArray &data) override;
    void reset() override { m_buf.clear(); }

  private:
    Message parsePayload(const QByteArray &payload) const;

    QByteArray m_header;
    QByteArray m_footer;
    QList<HexField> m_fields;
    int m_maxBuffer;
    QByteArray m_buf;
};

// Modbus RTU responses, CRC-checked, resynchronising on noise.
class ModbusRtuParser : public Parser
{
  public:
    explicit ModbusRtuParser(QStringList fields = {});

    QList<Message> feed(const QByteArray &data) override;
    void reset() override { m_buf.clear(); }

    static int frameLength(const QByteArray &buf, int from = 0);

  private:
    QStringList m_fields;
    QByteArray m_buf;
};

// Modbus TCP response ADUs (MBAP header). Output matches ModbusRtuParser.
class ModbusTcpParser : public Parser
{
  public:
    explicit ModbusTcpParser(QStringList fields = {});

    QList<Message> feed(const QByteArray &data) override;
    void reset() override { m_buf.clear(); }

    static int frameLength(const QByteArray &buf);

  private:
    QStringList m_fields;
    QByteArray m_buf;
};

// The response PDU body after the function code, as the shared reg/coil dictionary.
Message modbusResponse(int slave, int fc, const QByteArray &data, const QStringList &fields = {});

QByteArray modbusCrc(const QByteArray &data);
QByteArray buildModbusRtuFrame(int slave, int func, int address, int value = 0, int count = 1);
QByteArray buildModbusTcpFrame(int unit, int func, int address, int value = 0, int count = 1, int transaction = 0);

// Build a parser from the project's "parser" / "parserConfig" settings.
// Returns nullptr and sets *error for an unknown type or an invalid configuration.
std::unique_ptr<Parser> createParser(const QString &type, const QJsonObject &config, QString *error = nullptr);

} // namespace fhmi
