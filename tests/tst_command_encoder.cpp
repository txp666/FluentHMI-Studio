#include "core/command_encoder.h"
#include "core/dbc.h"
#include "core/manual_command.h"
#include "core/protocol_parser.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtTest/QtTest>

using namespace fhmi;

class TestCommandEncoder : public QObject
{
    Q_OBJECT

  private slots:
    void typedRegisterWritesUseFc06OrFc16()
    {
        struct Case
        {
            int func, address;
            QString value, type, order;
            QByteArray pdu;
        };
        const QString ab = QStringLiteral("大端(AB CD)"), cd = QStringLiteral("小端(CD AB)");
        const QList<Case> cases = {
            {16, 100, "25.3", "float32", ab, QByteArray::fromHex("0110006400020441ca6666")},
            {16, 100, "25.3", "float32", cd, QByteArray::fromHex("01100064000204666641ca")},
            {16, 0, "-2", "int32", ab, QByteArray::fromHex("01100000000204fffffffe")},
            {16, 0, "70000", "uint32", ab, QByteArray::fromHex("01100000000204000111 70")},
            {16, 5, "42", "uint16", ab, QByteArray::fromHex("011000050001 02002a")},
            {6, 7, "-1", "int16", ab, QByteArray::fromHex("01060007ffff")},
        };
        for (const Case &c : cases) {
            QString error;
            const auto request = CommandEncoder::modbusRequest(1, c.func, c.address, c.value, c.type, c.order, &error);
            QVERIFY2(request, qPrintable(error));
            QCOMPARE(CommandEncoder::rtuFrame(*request), c.pdu + modbusCrc(c.pdu));
        }
        const auto request = CommandEncoder::modbusRequest(1, 16, 100, QStringLiteral("25.3"), "float32", ab);
        QCOMPARE(CommandEncoder::tcpFrame(*request, 0x1234).toHex(), QByteArray("12340000000b0110006400020441ca6666"));
        QCOMPARE(CommandEncoder::asciiFrame(*request), QByteArray(":0110006400020441CA6666AE\r\n"));
        const QList<std::tuple<int, int, QString, QString>> rejected = {
            {6, 7, "1", "float32"}, {16, 65535, "1", "uint32"}, {16, 0, "1e39", "float32"},
            {16, 0, "nan", "float32"}, {16, 0, "40000", "int16"}, {16, 0, "1.5", "int32"}, {16, 0, "1", "double"}};
        for (const auto &[func, address, value, type] : rejected) {
            QVERIFY2(!CommandEncoder::modbusRequest(1, func, address, value, type, ab), qPrintable(value + type));
        }
        // Widgets take the type and order from their properties.
        const CommandResult fromWidget = CommandEncoder::encode(
            {{"commandMode", "Modbus"}, {"mbSlave", 1}, {"mbFunc", "16 写多个寄存器"}, {"mbAddress", 100},
             {"mbDataType", "float32"}},
            QStringLiteral("25"));
        QVERIFY(fromWidget.command);
        QCOMPARE(CommandEncoder::rtuFrame(fromWidget.command->modbus).toHex(),
                 QByteArray("0110006400020441c8000061b6"));

        const auto fc06 = CommandEncoder::modbusFunctionSpec(0x06);
        QCOMPARE(fc06.dataTypes, QStringList({"uint16", "int16"}));
        QCOMPARE(CommandEncoder::modbusValueRange(QStringLiteral("uint32"))->maximum, 4294967295.0);
        const auto button = CommandEncoder::buttonFromProperties({{"commandMode", "Modbus"},
            {"mbFunc", "16 写多个寄存器"}, {"mbAddress", 100}, {"mbDataType", "float32"}, {"mbValue", "25.3"}});
        QVERIFY(button.command);
        QCOMPARE(CommandEncoder::rtuFrame(button.command->modbus), CommandEncoder::rtuFrame(*request));

        const QJsonObject fromPoint{{"commandMode", "Modbus"}, {"mbFunc", "16 写多个寄存器"},
            {"commandInputs", QJsonArray{QJsonObject{{"inputId", "@point:rpm"}, {"dataType", "uint16"}}}}};
        const auto withValue = [&](const QVariant &value) {
            return CommandEncoder::buttonFromProperties(fromPoint, [&](const QString &) {
                return std::optional{CommandEncoder::InputValue{value, QStringLiteral("rpm")}};
            });
        };
        const auto integral = withValue(1500.0);
        QVERIFY2(integral.command, qPrintable(integral.error));
        QCOMPARE(integral.command->modbus.registers, QList<quint16>{1500});
        QVERIFY(!withValue(1500.5).command);
        QVERIFY(!withValue(QStringLiteral("1500.0")).command);
    }

    void textUsesTemplateAndNewline()
    {
        const QJsonObject props{{"commandTemplate", "SET {value}"}, {"commandNewline", "\\n"}};
        const CommandResult r = CommandEncoder::encode(props, "42");
        QVERIFY(r.command);
        QCOMPARE(r.command->bytes, QByteArray("SET 42\n"));
    }

    void buttonTemplatesEncodeOneCommandFromCurrentInputs()
    {
        QJsonObject properties{
            {"commandInputs", QJsonArray{QJsonObject{{"inputId", "p"}, {"variable", "kp"}, {"dataType", "uint16"}},
                                         QJsonObject{{"inputId", "i"}, {"variable", "ki"}, {"dataType", "float32"}},
                                         QJsonObject{{"inputId", "d"}, {"variable", "extra"}, {"dataType", "HEX字符串"}}}},
            {"commandTargetId", "device"}, {"byteOrder", "小端(LE)"}, {"frameHeader", "F0"}, {"frameTail", "7F"}};
        QHash<QString, QString> values{{"p", "258"}, {"i", "0.25"}, {"d", " {kp} "}};
        QHash<QString, int> reads;
        const auto read = [&](const QString &id) -> std::optional<CommandEncoder::InputValue> {
            ++reads[id];
            return CommandEncoder::InputValue{values.value(id), {}};
        };

        properties.insert("commandMode", "文本");
        properties.insert("commandTemplate", "KP={kp},KI={ki},D={extra},KP={kp}");
        auto result = CommandEncoder::buttonFromProperties(properties, read);
        QVERIFY2(result.command, qPrintable(result.error));
        QCOMPARE(result.command->bytes, QByteArray("KP=258,KI=0.25,D= {kp} ,KP=258\r\n"));
        QCOMPARE(result.command->route.connectionId, QStringLiteral("device"));
        QCOMPARE(reads.value("p"), 1);
        QCOMPARE(result.command->inputValues.size(), 3);

        values["d"] = "0A 0B";
        properties.insert("commandMode", "HEX");
        properties.insert("commandTemplate", "A5 {kp} {ki} {extra}");
        result = CommandEncoder::buttonFromProperties(properties, read);
        QVERIFY2(result.command, qPrintable(result.error));
        QCOMPARE(result.command->bytes, QByteArray::fromHex("F0A502010000803E0A0B7F"));
        values["p"] = "70000";
        QVERIFY(!CommandEncoder::buttonFromProperties(properties, read).command);
        values["p"] = "258";

        properties.insert("commandMode", "CAN");
        properties.insert("canId", "0x123");
        properties.insert("canCycle", 30);
        properties.insert("commandTemplate", "{kp} {ki} {extra}");
        result = CommandEncoder::buttonFromProperties(properties, read);
        QVERIFY2(result.command, qPrintable(result.error));
        QString error;
        auto payload = CanSignalSet{}.preparePayload(result.command->can, &error);
        QVERIFY2(payload, qPrintable(error));
        QCOMPARE(payload->frame.data, QByteArray::fromHex("02010000803E0A0B"));
        QCOMPARE(payload->periodMs, 0);

        auto dbc = Dbc::parse("BO_ 291 PID: 3 Node\n SG_ Kp : 0|16@1+ (1,0) [0|65535] \"\" Node\n"
                              " SG_ Ki : 16|8@1+ (0.25,0) [0|63.75] \"\" Node\n"
                              "BA_ \"GenMsgCycleTime\" BO_ 291 50;\n");
        QVERIFY(dbc);
        const auto signalSet = CanSignalSet::build(std::make_shared<CanDatabase>(std::move(*dbc)), {});
        QVERIFY(signalSet);
        properties.insert("canId", "PID");
        properties.remove("canCycle");
        properties.insert("commandTemplate", "Kp={kp},Ki={ki}");
        result = CommandEncoder::buttonFromProperties(properties, read);
        QVERIFY2(result.command, qPrintable(result.error));
        payload = signalSet->preparePayload(result.command->can, &error);
        QVERIFY2(payload, qPrintable(error));
        QCOMPARE(payload->frame.data, QByteArray::fromHex("020101"));
        QCOMPARE(payload->periodMs, 0);
        QCOMPARE(CommandEncoder::inputValuesText(*result.command), QStringLiteral("p = 258\ni = 0.25"));
        const auto single = CommandEncoder::encode({{"commandMode", "CAN"}, {"canId", "PID"}}, "Kp=258");
        QVERIFY(single.command);
        const auto cyclic = signalSet->preparePayload(single.command->can, &error);
        QVERIFY2(cyclic, qPrintable(error));
        QCOMPARE(cyclic->periodMs, 50);

        values["p"] = "258,Ki=1";
        QVERIFY(!CommandEncoder::buttonFromProperties(properties, read).command);
        values["p"] = "258";
        properties.insert("commandTemplate", "Kp={typo}");
        result = CommandEncoder::buttonFromProperties(properties, read);
        QVERIFY(!result.command);
        QVERIFY(result.error.contains(QStringLiteral("{typo}")));

        properties.insert("commandMode", "JSON");
        properties.insert("command", "7");
        properties.insert("commandTemplate", "{\"legacy\":{value}}");
        result = CommandEncoder::buttonFromProperties(properties, read);
        QVERIFY2(result.command, qPrintable(result.error));
        QCOMPARE(result.command->bytes, QByteArray("{\"legacy\":7}"));
        properties.insert("jsonUseInputs", true);
        properties.insert("commandTemplate", "{\"kp\":{kp},\"extra\":{extra},\"ki\":{ki}}");
        values["d"] = QStringLiteral("x\"y\n");
        result = CommandEncoder::buttonFromProperties(properties, read);
        QVERIFY2(result.command, qPrintable(result.error));
        QCOMPARE(result.command->bytes, QByteArray("{\"kp\":258,\"extra\":\"x\\\"y\\n\",\"ki\":0.25}"));
        properties.insert("commandTemplate", "{\"kp\":\"{kp}\"}");
        QVERIFY(!CommandEncoder::buttonFromProperties(properties, read).command);
    }

    void binaryLengthAndChecksumsMatchTheReceiveRules()
    {
        QJsonObject layout{{"lengthOffset", 2}, {"lengthSize", 1}, {"checksum", "异或"}, {"checksumStart", 3}};
        QJsonObject properties{{"commandMode", "HEX"}, {"frameHeader", "AA 55"}, {"frameTail", "0D"}, {"txFrame", layout}};
        const QList<QPair<QString, QByteArray>> frames{
            {QStringLiteral("数据"), QByteArray::fromHex("AA5503102230020D")},
            {QStringLiteral("长度字段之后"), QByteArray::fromHex("AA5505102230020D")},
            {QStringLiteral("整帧"), QByteArray::fromHex("AA5508102230020D")}};
        for (const auto &[covers, expected] : frames) {
            layout.insert("lengthCovers", covers);
            properties.insert("txFrame", layout);
            const auto command = CommandEncoder::encode(properties, QStringLiteral("10 22 30"));
            QVERIFY2(command.command, qPrintable(command.error));
            QCOMPARE(command.command->bytes, expected);
            const auto manual = ManualCommand::encode(properties, QStringLiteral("10 22 30"));
            QVERIFY2(manual.command, qPrintable(manual.error));
            QCOMPARE(manual.command->bytes, expected);
            auto receive = layout;
            receive.insert("header", "AA 55");
            receive.insert("footer", "0D");
            QString error;
            const auto parser = createParser(QStringLiteral("hex"), receive, &error);
            QVERIFY2(parser, qPrintable(error));
            QCOMPARE(parser->frames(expected), QList<QByteArray>{expected});
        }
        const auto raw = ManualCommand::encode({{"commandMode", "HEX"}}, QStringLiteral("10 22 30"));
        QVERIFY(raw.command);
        QCOMPARE(raw.command->bytes, QByteArray::fromHex("102230"));
        const auto crc = buildHexFrame(QByteArray::fromHex("01030000000A"), {}, {},
                                       {{"checksum", "CRC16-Modbus"}, {"checksumEndian", "小端(LE)"}});
        QCOMPARE(crc, std::optional(QByteArray::fromHex("01030000000AC5CD")));
        HexFrameConfig crcReceive;
        crcReceive.header.clear();
        crcReceive.footer.clear();
        crcReceive.fixedLength = 8;
        crcReceive.checksum = HexChecksum::Crc16Modbus;
        crcReceive.checksumLittleEndian = true;
        QByteArray calculated;
        const HexFrameParser crcParser(crcReceive);
        QCOMPARE(crcParser.sampleIssue(QByteArray::fromHex("01030000000ACDC5"), &calculated), HexFrameParser::Issue::Checksum);
        QCOMPARE(calculated, QByteArray::fromHex("C5CD"));
        crcReceive.checksumStart = 7;
        QCOMPARE(HexFrameParser(crcReceive).sampleIssue(*crc, &calculated), HexFrameParser::Issue::ChecksumRange);
        QVERIFY(calculated.isEmpty());
        properties.insert("txFrame", QJsonObject{{"fixedLength", 6}});
        QVERIFY(CommandEncoder::encode(properties, QStringLiteral("10 22 30")).command);
        QVERIFY(!CommandEncoder::encode(properties, QStringLiteral("10 22")).command);
        QVERIFY(!buildHexFrame(QByteArray(256, '\0'), {}, {}, {{"lengthOffset", 0}, {"lengthSize", 1}}));
        const auto longFrame = buildHexFrame(QByteArray(258, '\0'), {}, {},
            {{"lengthOffset", 0}, {"lengthSize", 2}, {"lengthEndian", "小端(LE)"}});
        QVERIFY(longFrame);
        QCOMPARE(longFrame->left(2), QByteArray::fromHex("0201"));

        properties = {{"commandMode", "CAN"}, {"canId", "123"},
                      {"txFrame", QJsonObject{{"lengthOffset", 0}, {"checksum", "和校验"}, {"checksumStart", 1}}}};
        const auto can = CommandEncoder::encode(properties, QStringLiteral("10 22 30"));
        QVERIFY2(can.command, qPrintable(can.error));
        const auto payload = CanSignalSet{}.preparePayload(can.command->can);
        QVERIFY(payload);
        QCOMPARE(payload->frame.data, QByteArray::fromHex("0310223062"));
    }

    void namedTemplatesEncodeTextAndCanPayloads()
    {
        const QJsonObject templates{
            {"line", FrameTemplates::Entry{"Text", {"line", {{"encoding", "gbk"}, {"terminator", ";"}}}, {}}.toJson()},
            {"hex", FrameTemplates::Entry{"Binary", {"hex", {{"header", "AA55"}, {"footer", "0D"},
                {"lengthOffset", 2}, {"checksum", "异或"}, {"checksumStart", 3}}}, {}}.toJson()}};
        QJsonObject properties{{"commandMode", "JSON"}, {"commandTemplate", "{\"status\":{value}}"},
                               {FrameTemplates::SendId, "line"}};
        const auto text = CommandEncoder::encode(properties, QStringLiteral("开"), nullptr, templates);
        QVERIFY2(text.command, qPrintable(text.error));
        QCOMPARE(text.command->bytes, QByteArray::fromHex("7B22737461747573223A22BFAA227D3B"));
        QCOMPARE(CommandEncoder::describe(*text.command, "JSON"), QStringLiteral("{\"status\":\"开\"};"));

        properties = {{"commandMode", "文本"}, {FrameTemplates::SendId, "line"}, {"sendIntervalMs", 60}};
        QCOMPARE(CommandEncoder::periodicSendInterval(properties), 60);
        const auto line = CommandEncoder::encode(properties, QStringLiteral("开"), nullptr, templates);
        QVERIFY(line.command);
        QCOMPARE(line.command->bytes, QByteArray::fromHex("BFAA3B"));

        auto asciiTemplate = templates;
        asciiTemplate.insert("line", FrameTemplates::Entry{"ASCII", {"line", {{"encoding", "ascii"}, {"terminator", ";"}}}, {}}.toJson());
        const auto ascii = CommandEncoder::encode(properties, QStringLiteral("OK"), nullptr, asciiTemplate);
        QVERIFY(ascii.command);
        QCOMPARE(ascii.command->bytes, QByteArray("OK;"));
        QVERIFY(!CommandEncoder::encode(properties, QStringLiteral("开"), nullptr, asciiTemplate).command);
        properties.insert("commandMode", "JSON");
        QVERIFY(!CommandEncoder::encode(properties, QStringLiteral("开"), nullptr, asciiTemplate).command);

        properties = {{"commandMode", "CAN"}, {FrameTemplates::SendId, "hex"}};
        const auto can = ManualCommand::encode(properties, QStringLiteral("123##1010203040506070809"), templates);
        QVERIFY2(can.command, qPrintable(can.error));
        QCOMPARE(can.command->frame.data, QByteArray::fromHex("AA5509010203040506070809010D0000"));
    }

    void mappedTemplateFieldsKeepAbsoluteOffsetsAndValidateValues()
    {
        const QJsonObject settings{{"header", "AA55"},
            {"footer", "0D"}, {"lengthOffset", 4}, {"lengthSize", 2}, {"lengthEndian", "小端(LE)"},
            {"checksum", "异或"}, {"checksumStart", 2},
            {FrameTemplates::SendDefaults, QJsonObject{{"cmd", "1"}, {"rpm", "1000"}}},
            {FrameTemplates::SendRequired, QJsonArray{"cmd", "rpm"}},
            {"fields", QJsonArray{QJsonArray{"cmd", 0, 1, "uint8"}, QJsonArray{"len", 2, 2, "uint16"},
                                  QJsonArray{"rpm", 4, 2, "uint16", "小端(LE)"}, QJsonArray{"alias", 5, 1, "uint8"}}}};
        const QJsonObject library{{"speed", FrameTemplates::Entry{"Speed", {"hex", settings}, {}}.toJson()}};
        QJsonArray rows{QJsonObject{{"field", "rpm"}, {"inputId", "speedInput"}}};
        QJsonObject props{{"commandMode", "HEX"}, {FrameTemplates::SendId, "speed"},
                          {FrameTemplates::SendFieldsEnabled, true}, {FrameTemplates::SendBindings, rows}};
        CommandEncoder::BinaryFrameLayout preview;
        QString speed = QStringLiteral("1500");
        const auto input = [&speed](const QString &id) -> std::optional<CommandEncoder::InputValue> {
            return id == QLatin1String("speedInput") ? std::optional(CommandEncoder::InputValue{speed, id}) : std::nullopt;
        };
        const auto result = CommandEncoder::buttonFromProperties(props, input, &preview, library);
        QVERIFY2(result.command, qPrintable(result.error));
        QCOMPARE(result.command->bytes, QByteArray::fromHex("AA5501000200DC05DA0D"));
        QCOMPARE(preview.fields.size(), 2);
        QCOMPARE(preview.fields.at(1).offset, 6);
        QCOMPARE(result.command->inputValues.at(1).value, speed);
        QCOMPARE(FrameTemplates::sendFields(library, props).size(), 3); // The computed length is read-only.
        const auto manual = CommandEncoder::frameFromFieldValues(props, {{"rpm", speed}}, library);
        QVERIFY2(manual.command, qPrintable(manual.error));
        QCOMPARE(manual.command->bytes, result.command->bytes);
        QVERIFY(CommandEncoder::frameFieldConfigurationProblem(props, library).isEmpty());
        auto secondSettings = settings;
        secondSettings.insert("header", "A55A");
        auto sequenceLibrary = library;
        sequenceLibrary.insert("speed2", FrameTemplates::Entry{"Speed 2", {"hex", secondSettings}, {}}.toJson());
        auto sequence = props;
        sequence.insert(FrameTemplates::SendSequence, QJsonArray{
            QJsonObject{{FrameTemplates::SendId, "speed"}, {FrameTemplates::SendBindings, rows}},
            QJsonObject{{FrameTemplates::SendId, "speed2"}, {FrameTemplates::SendBindings, rows}}});
        int reads = 0;
        const auto mixed = CommandEncoder::buttonFromProperties(sequence, [&](const QString &id) {
            ++reads;
            return input(id);
        }, nullptr, sequenceLibrary);
        QVERIFY2(mixed.command, qPrintable(mixed.error));
        QCOMPARE(reads, 1);
        QCOMPARE(mixed.command->frames.size(), 2);
        QCOMPARE(mixed.command->bytes, QByteArray::fromHex("AA5501000200DC05DA0DA55A01000200DC05DA0D"));
        const auto missingMixed = CommandEncoder::buttonFromProperties(sequence, {}, nullptr, sequenceLibrary);
        QVERIFY(!missingMixed.command);
        QVERIFY(missingMixed.error.contains(QStringLiteral("第 1 帧")));
        QCOMPARE(CommandEncoder::commandInputIds(sequence), QStringList{QStringLiteral("speedInput")});
        const auto defaults = CommandEncoder::frameFromTemplateDefaults(settings);
        QVERIFY2(defaults.command, qPrintable(defaults.error));
        QCOMPARE(defaults.command->bytes, QByteArray::fromHex("AA5501000200E803E80D"));
        auto localOverride = props;
        auto localRows = rows;
        localRows.append(QJsonObject{{"field", "cmd"}, {"value", "2"}});
        localOverride.insert(FrameTemplates::SendBindings, localRows);
        const auto overridden = CommandEncoder::buttonFromProperties(localOverride, input, nullptr, library);
        QVERIFY2(overridden.command, qPrintable(overridden.error));
        QCOMPARE(overridden.command->bytes, QByteArray::fromHex("AA5502000200DC05D90D"));
        auto invalid = settings;
        invalid.insert(FrameTemplates::SendDefaults, QJsonObject{{"cmd", "256"}});
        QVERIFY(!FrameTemplates::defaultValuesProblem({"hex", invalid}).isEmpty());
        invalid.insert(FrameTemplates::SendDefaults, QJsonObject{{"len", "8"}});
        QVERIFY(!FrameTemplates::defaultValuesProblem({"hex", invalid}).isEmpty());
        invalid = settings;
        invalid.insert(FrameTemplates::SendRequired, QJsonArray{"len"});
        QVERIFY(FrameTemplates::sendFieldRulesProblem({"hex", invalid}).contains(QStringLiteral("len")));
        const auto missingSource = CommandEncoder::buttonFromProperties(props, {}, nullptr, library);
        QVERIFY(!missingSource.command);
        QVERIFY(missingSource.error.contains(QStringLiteral("必填字段 rpm")));
        speed = QStringLiteral("  ");
        const auto emptyOverride = CommandEncoder::buttonFromProperties(props, input, nullptr, library);
        QVERIFY(!emptyOverride.command);
        QVERIFY(emptyOverride.error.contains(QStringLiteral("必填字段 rpm")));
        auto missingSettings = settings;
        missingSettings.insert(FrameTemplates::SendDefaults, QJsonObject{{"cmd", "1"}});
        auto missingLibrary = library;
        missingLibrary.insert("speed", FrameTemplates::Entry{"Speed", {"hex", missingSettings}, {}}.toJson());
        auto noOverrides = props;
        noOverrides.remove(FrameTemplates::SendBindings);
        QVERIFY(FrameTemplates::sendFieldRulesProblem({"hex", missingSettings}).isEmpty());
        QVERIFY(CommandEncoder::frameFromTemplateDefaults(missingSettings).command);
        QVERIFY(CommandEncoder::frameFieldConfigurationProblem(noOverrides, missingLibrary).contains(QStringLiteral("必填字段 rpm")));
        const QJsonObject raw{{"commandMode", "HEX"}, {FrameTemplates::SendId, "speed"}};
        const auto shortBody = CommandEncoder::encode(raw, QStringLiteral("01 00"), nullptr, library);
        QVERIFY(!shortBody.command);
        QVERIFY(shortBody.error.contains(QStringLiteral("必填字段 rpm")));
        const auto completeBody = CommandEncoder::encode(raw, QStringLiteral("01 00 DC 05"), nullptr, library);
        QVERIFY2(completeBody.command, qPrintable(completeBody.error));
        QCOMPARE(completeBody.command->bytes, result.command->bytes);
        speed = QStringLiteral("65536");
        QVERIFY(!CommandEncoder::buttonFromProperties(props, input, nullptr, library).command);
        rows.append(QJsonObject{{"field", "alias"}, {"value", "2"}});
        props.insert(FrameTemplates::SendBindings, rows);
        QVERIFY(!CommandEncoder::frameFieldConfigurationProblem(props, library).isEmpty());
        props.insert(FrameTemplates::SendValueField, QStringLiteral("rpm"));
        const auto direct = CommandEncoder::encode(props, QStringLiteral("1500"), nullptr, library);
        QVERIFY2(direct.command, qPrintable(direct.error));
        QCOMPARE(direct.command->bytes, result.command->bytes);
        const auto emptyDirect = CommandEncoder::encode(props, QString(), nullptr, library);
        QVERIFY(!emptyDirect.command);
        QVERIFY(emptyDirect.error.contains(QStringLiteral("必填字段 rpm")));
        QVERIFY(CommandEncoder::encode(props, QStringLiteral("0"), nullptr, library).command);
        QVERIFY(!CommandEncoder::encode(props, QStringLiteral("1500")).command);

        QJsonObject shared{{"header", "A55A"}, {"footer", ""}, {"fixedLength", 6},
            {"matchOffset", 2}, {"matchBytes", "10"}, {"checksum", "CRC8"}, {"checksumStart", 2},
            {"fields", QJsonArray{QJsonValue(QJsonArray{"rpm", 1, 2, "uint16", "小端(LE)"})}}};
        const QJsonObject bidirectional{{"speed", FrameTemplates::Entry{"Speed", {"hex", shared}, {}}.toJson()}};
        auto named = raw;
        named.insert(FrameTemplates::SendValueField, "rpm");
        const auto generated = CommandEncoder::encode(named, "2300", nullptr, bidirectional);
        QVERIFY2(generated.command, qPrintable(generated.error));
        QCOMPARE(generated.command->bytes, QByteArray::fromHex("A55A10FC0872"));
        const auto supplied = CommandEncoder::encode(raw, "00 FC 08", nullptr, bidirectional);
        QVERIFY(supplied.command);
        QCOMPARE(supplied.command->bytes, QByteArray::fromHex("A55A00FC08D0"));
        QVERIFY(!supplied.command->framingNotice.isEmpty());
    }

    void singleInputUsesTheConfiguredProtocolValue()
    {
        const auto read = [](const QString &id) -> std::optional<CommandEncoder::InputValue> {
            if (id != QLatin1String("actual"))
                return std::nullopt;
            return CommandEncoder::InputValue{QStringLiteral("4"), QStringLiteral("设定值")};
        };
        auto result = CommandEncoder::buttonFromProperties({{"commandMode", "Modbus"}, {"mbInputId", "actual"},
            {"mbAddress", 100}, {"mbValue", "1"}}, read);
        QVERIFY2(result.command, qPrintable(result.error));
        QCOMPARE(CommandEncoder::rtuFrame(result.command->modbus).left(6), QByteArray::fromHex("010300640004"));
        QCOMPARE(CommandEncoder::inputValuesText(*result.command), QStringLiteral("设定值 = 4"));
        result = CommandEncoder::buttonFromProperties({{"commandMode", "S7"}, {"s7InputId", "actual"},
            {"commandTargetId", "plc"}, {"s7Command", QJsonObject{{"address", "DB1.DBW0"}, {"dataType", "INT"},
             {"value", "1"}}}}, read);
        QVERIFY2(result.command, qPrintable(result.error));
        QCOMPARE(result.command->s7.bytes, QByteArray::fromHex("0004"));
        QVERIFY(!CommandEncoder::buttonFromProperties({{"commandMode", "Modbus"}, {"mbInputId", "missing"}}, read).command);
    }

    void textTemplateWithoutPlaceholderIsSentAsIs()
    {
        const CommandResult r = CommandEncoder::encode({{"commandTemplate", "PING"}, {"commandNewline", "无"}}, "x");
        QCOMPARE(r.command->bytes, QByteArray("PING"));
    }

    void defaultNewlineIsCrLf()
    {
        QCOMPARE(CommandEncoder::encode({}, "RUN").command->bytes, QByteArray("RUN\r\n"));
    }

    void hexStringWithHeaderAndTail()
    {
        const QJsonObject props{{"commandMode", "HEX"}, {"frameHeader", "AA 55"}, {"frameTail", "D"}};
        const CommandResult r = CommandEncoder::encode(props, "01 02");
        QCOMPARE(r.command->bytes, QByteArray::fromHex("AA5501020D"));
    }

    void hexInvalidIsAnError()
    {
        const CommandResult r = CommandEncoder::encode({{"commandMode", "HEX"}}, "zz");
        QVERIFY(!r.command);
        QVERIFY(!r.error.isEmpty());
    }

    void hexPackedNumbers()
    {
        QJsonObject props{{"commandMode", "HEX"}, {"dataType", "int16"}, {"commandTemplate", "01{value}"}};
        QCOMPARE(CommandEncoder::encode(props, "-2").command->bytes, QByteArray::fromHex("01FFFE"));
        props["byteOrder"] = "小端(LE)";
        QCOMPARE(CommandEncoder::encode(props, "-2").command->bytes, QByteArray::fromHex("01FEFF"));
        props["dataType"] = "uint8";
        QVERIFY(!CommandEncoder::encode(props, "256").command);
        props["dataType"] = "float32";
        props["byteOrder"] = "大端(BE)";
        QCOMPARE(CommandEncoder::encode(props, "10").command->bytes, QByteArray::fromHex("0141200000"));
    }

    void modbusFromWidget()
    {
        const QJsonObject props{{"commandMode", "Modbus"}, {"mbSlave", 2}, {"mbFunc", "06 写单寄存器"},
                                {"mbAddress", 16}};
        const CommandResult r = CommandEncoder::encode(props, "300");
        QVERIFY(r.command);
        QCOMPARE(r.command->kind, EncodedCommand::Kind::Modbus);
        QCOMPARE(CommandEncoder::rtuFrame(r.command->modbus).left(6), QByteArray::fromHex("02060010012C"));
        QVERIFY(!CommandEncoder::encode(props, "70000").command);
        QVERIFY(!CommandEncoder::encode(props, "1.5").command);
    }

    void modbusReadsCheckQuantity()
    {
        QVERIFY(CommandEncoder::modbusRequest(1, 3, 0, 125));
        QVERIFY(!CommandEncoder::modbusRequest(1, 3, 0, 126));
        QVERIFY(!CommandEncoder::modbusRequest(0, 3, 0, 1));
        QVERIFY(!CommandEncoder::modbusRequest(1, 3, 65535, 2));
        QVERIFY(CommandEncoder::modbusRequest(1, 5, 0, 0xFF00));
        QVERIFY(!CommandEncoder::modbusRequest(1, 5, 0, 2));
        const auto coil = CommandEncoder::encode({{"commandMode", "Modbus"}, {"mbFunc", "05 写单线圈"}}, "1");
        QVERIFY(coil.command);
        QCOMPARE(coil.command->modbus.value, 0xFF00);
    }

    void selectionMovesLabelWithCommand()
    {
        const auto options = CommandEncoder::selectionOptions({QJsonObject{{"label", "Run"}, {"value", "1"}},
            QJsonObject{{"label", "Stop"}, {"value", "0"}}, QJsonObject{{"label", "Clear"}}});
        QCOMPARE(options.at(1).label, QStringLiteral("Stop"));
        QCOMPARE(CommandEncoder::selectionFromProperties({}, options.at(1)).command->bytes, QByteArray("0\r\n"));
        QCOMPARE(CommandEncoder::selectionFromProperties({}, options.at(2)).command->bytes, QByteArray("Clear\r\n"));
    }

    // tests/data/json_command_vectors.json: JSON-mode commands shared with PyQt UI Designer, the exact text
    // each sends or the template error.
    void jsonGoldenVectors()
    {
        QFile file(QStringLiteral(FHMI_TEST_DATA_DIR "/json_command_vectors.json"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        const QJsonArray cases = QJsonDocument::fromJson(file.readAll()).object().value("cases").toArray();
        QVERIFY(!cases.isEmpty());
        for (const QJsonValue &item : cases) {
            const QJsonObject c = item.toObject();
            const QByteArray name = c.value("name").toString().toUtf8();
            const CommandResult r = CommandEncoder::encode(c.value("props").toObject(), c.value("value").toString());
            if (c.contains("error")) {
                QVERIFY2(!r.command, name.constData());
                QCOMPARE(r.error, c.value("error").toString());
            } else {
                QVERIFY2(r.command, qPrintable(QString::fromUtf8(name) + QStringLiteral(": ") + r.error));
                QCOMPARE(r.command->bytes, c.value("out").toString().toUtf8());
            }
        }
    }

    void transactionsWrap()
    {
        const int first = CommandEncoder::nextTransaction();
        QCOMPARE(CommandEncoder::nextTransaction(), (first + 1) & 0xFFFF);
    }
};

QTEST_APPLESS_MAIN(TestCommandEncoder)
#include "tst_command_encoder.moc"
