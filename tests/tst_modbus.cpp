#include "comm/comm_hub.h"
#include "comm/link.h"
#include "comm/modbus_master.h"
#include "core/project.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QtTest>

using namespace fhmi;

Q_DECLARE_METATYPE(fhmi::ModbusBlock)

namespace QTest {
template <> char *toString(const fhmi::ModbusBlock &block)
{
    return qstrdup(QStringLiteral("[%1,%2,%3,%4,%5]")
                       .arg(block.slave)
                       .arg(block.fc)
                       .arg(block.start)
                       .arg(block.count)
                       .arg(block.period)
                       .toUtf8()
                       .constData());
}
} // namespace QTest

namespace {

// A link that records what is sent and injects what a device would answer.
class FakeLink : public Link
{
  public:
    using Link::Link;
    QString description() const override { return QStringLiteral("fake"); }
    bool isConnected() const override { return m_connected; }
    void close() override { setConnected(false); }
    void setConnected(bool connected)
    {
        if (connected != m_connected) {
            m_connected = connected;
            emit connectedChanged(connected);
        }
    }
    void inject(const QByteArray &bytes) { deliver(defaultParser(), bytes); }
    QList<QByteArray> sent;

  protected:
    qint64 writeBytes(const QByteArray &bytes) override
    {
        sent.append(bytes);
        return bytes.size();
    }

  private:
    bool m_connected = true;
};

QByteArray rtu(const char *hex)
{
    const QByteArray body = QByteArray::fromHex(hex);
    return body + modbusCrc(body);
}

ModbusRequest request(int slave, int func, int address, int value)
{
    return *CommandEncoder::modbusRequest(slave, func, address, value);
}

// Collects what each request ended with, in order.
struct Results
{
    QList<ModbusResult> list;
    ModbusMaster::Done done()
    {
        return [this](const ModbusResult &result) { list.append(result); };
    }
};

} // namespace

class TestModbus : public QObject
{
    Q_OBJECT

  private slots:
    // The golden vectors both programs plan by.
    void planMatchesTheGoldenVectors()
    {
        QFile file(QStringLiteral(FHMI_TEST_DATA_DIR "/poll_plan_vectors.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonArray cases = QJsonDocument::fromJson(file.readAll()).array();
        QCOMPARE(cases.size(), 17);
        for (const QJsonValue &value : cases) {
            const QJsonObject test = value.toObject();
            QList<ModbusNeed> needs;
            for (const QJsonValue &item : test.value("needs").toArray()) {
                const QJsonObject need = item.toObject();
                ModbusNeed n;
                n.slave = need.value("slave").toInt();
                n.table = need.value("table").toString();
                n.address = need.value("address").toInt();
                n.count = need.value("count").toInt();
                n.period = need.value("period").toInt();
                needs.append(n);
            }
            QJsonArray planned;
            for (const ModbusBlock &block : planBlocks(needs, test.value("defaultPeriod").toInt())) {
                planned.append(QJsonArray{block.slave, block.fc, block.start, block.count, block.period});
            }
            QVERIFY2(planned == test.value("blocks").toArray(),
                     qPrintable(test.value("name").toString() + QStringLiteral(": ") +
                                QString::fromUtf8(QJsonDocument(planned).toJson(QJsonDocument::Compact))));
        }
    }

    // A Modbus binding needs the address in its register table (holding registers when it names none) and the
    // registers its type takes; an area that is no table, or another parse mode, needs nothing.
    void bindingsNeedTheirRegisters()
    {
        const auto need = [](const QJsonObject &binding) {
            return modbusNeed(
                *BindingEntry::fromWidget(QJsonObject{{"id", "w"}, {"type", "NumericDisplay"}, {"binding", binding}}));
        };
        const auto wide = need({{"parseMode", "Modbus"},
                                {"mbArea", "输入寄存器"},
                                {"mbRegIndex", 7},
                                {"mbDataType", "float32"},
                                {"mbSlaveFilter", 3},
                                {"mbPeriod", 250}});
        QVERIFY(wide);
        QCOMPARE(wide->widgetId, QStringLiteral("w"));
        QCOMPARE(wide->table, QStringLiteral("ir"));
        QCOMPARE(wide->address, 7);
        QCOMPARE(wide->count, 2);
        QCOMPARE(wide->slave, 3);
        QCOMPARE(wide->period, 250);
        const auto coil = need({{"parseMode", "Modbus"}, {"mbArea", "线圈"}, {"mbDataType", "uint32"}});
        QVERIFY(coil);
        QCOMPARE(coil->count, 1);
        QCOMPARE(coil->slave, -1); // the poller's fallback slave
        const auto holding = need({{"parseMode", "Modbus"}, {"mbRegIndex", 2}});
        QVERIFY(holding);
        QCOMPARE(holding->table, QStringLiteral("hr"));
        QCOMPARE(holding->address, 2);
        QVERIFY(!need({{"parseMode", "Modbus"}, {"mbArea", "响应内序号"}, {"mbRegIndex", 2}}));
        QVERIFY(!need({{"parseMode", "键值匹配"}, {"key", "t"}}));
    }

    // The bindings of the two Modbus tutorials, with the slave and period of their Modbus card, plan to
    // the blocks the tutorials describe: 02 one block of holding registers 0–3 every 200 ms, 10 the coils,
    // the live values and the settings every 500 ms.
    void tutorialBindingsPlanTheirBlocks_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QList<ModbusBlock>>("blocks");
        QTest::newRow("02") << QStringLiteral("02-温控仪监控.uidp") << QList<ModbusBlock>{{1, 3, 0, 4, 200}};
        QTest::newRow("10") << QStringLiteral("10-PLC恒压供水.uidp")
                            << QList<ModbusBlock>{{1, 1, 0, 4, 500}, {1, 3, 100, 6, 500}, {1, 3, 200, 2, 500}};
    }

    void tutorialBindingsPlanTheirBlocks()
    {
        QFETCH(QString, file);
        QFETCH(QList<ModbusBlock>, blocks);
        Project project;
        QString error;
        QVERIFY2(Project::load(QStringLiteral(FHMI_EXAMPLES_DIR "/tutorials/") + file, &project, &error),
                 qPrintable(error));
        QList<ModbusNeed> needs;
        int slave = 0;
        int period = 0;
        visitWidgets(project.widgets, [&](const QJsonObject &widget) {
            const QJsonObject props = widget.value("properties").toObject();
            if (widget.value("type").toString() == QLatin1String("ModbusConfigCard") && slave == 0) {
                slave = props.value("defaultSlave").toInt(1);
                period = props.value("defaultPoll").toInt(0);
            }
            if (const auto binding = BindingEntry::fromWidget(widget)) {
                if (auto need = modbusNeed(*binding)) {
                    needs.append(*need);
                }
            }
            return true;
        });
        QCOMPARE(slave, 1);
        for (ModbusNeed &need : needs) {
            if (need.slave <= 0) {
                need.slave = slave;
            }
        }
        QCOMPARE(planBlocks(needs, period > 0 ? period : 500), blocks);
    }

    // One request on the line at a time: reads wait at the back, writes go ahead of every queued read
    // in the order they were made, and the next request goes out as soon as a reply arrives.
    void writesGoAheadOfQueuedReads()
    {
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus"), {}));
        ModbusMaster *master = link.modbusMaster();
        Results results;
        const auto read = ModbusMaster::Priority::Read;
        const auto write = ModbusMaster::Priority::Write;
        QCOMPARE(master->enqueue(request(1, 3, 0, 2), 1000, read, results.done()), rtu("010300000002"));
        master->enqueue(request(1, 3, 10, 1), 1000, read, results.done());
        master->enqueue(request(1, 6, 5, 7), 1000, write, results.done());
        master->enqueue(request(1, 5, 1, 0xFF00), 1000, write, results.done());
        QVERIFY(master->busy());
        QCOMPARE(master->pending(), 3);
        QCOMPARE(link.sent, QList<QByteArray>{rtu("010300000002")});

        link.inject(rtu("0103040001000a"));
        QCOMPARE(link.sent.last(), rtu("010600050007")); // the first write, ahead of the read
        link.inject(rtu("010600050007"));
        QCOMPARE(link.sent.last(), rtu("01050001ff00"));
        link.inject(rtu("01050001ff00"));
        QCOMPARE(link.sent.last(), rtu("0103000a0001"));
        link.inject(rtu("01030200ff"));
        QVERIFY(!master->busy());
        QCOMPARE(master->pending(), 0);
        QCOMPARE(results.list.size(), 4);
        for (const ModbusResult &result : std::as_const(results.list)) {
            QVERIFY(result.ok);
            QVERIFY(result.problem.isEmpty());
            QCOMPARE(result.master.data(), master);
        }
        QCOMPARE(results.list.at(0).message.value("hr1").toInt(), 10);
    }

    // A reply belongs to the request on the line by slave, function code and start (or echoed) address;
    // an exception names the device's code; anything else is left alone.
    void repliesPairBySlaveFunctionAndAddress()
    {
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus_tcp"), {}));
        ModbusMaster *master = link.modbusMaster();
        Results results;
        const QByteArray adu =
            master->enqueue(request(2, 3, 100, 2), 1000, ModbusMaster::Priority::Read, results.done());
        QCOMPARE(adu.mid(6), QByteArray::fromHex("0203006400 02"));
        const QByteArray tid = adu.left(2);
        const auto reply = [&tid](const char *pdu) {
            const QByteArray body = QByteArray::fromHex(pdu);
            return tid + QByteArray::fromHex("0000") + char(0) + char(body.size()) + body;
        };
        link.inject(reply("0103040001 0002")); // another slave
        link.inject(reply("0204040001 0002")); // another function code
        QVERIFY(results.list.isEmpty());
        link.inject(reply("0283 02"));
        QCOMPARE(results.list.size(), 1);
        QVERIFY(!results.list.at(0).ok);
        QVERIFY(!results.list.at(0).timedOut);
        QCOMPARE(results.list.at(0).problem, QStringLiteral("设备返回异常 02 非法数据地址"));
        QCOMPARE(modbusException(results.list.at(0).message), QStringLiteral("异常 02 非法数据地址"));

        // RTU names no transaction: the start address tells two reads apart, and a reply of another size
        // (no start) answers some other request.
        FakeLink serial;
        QVERIFY(serial.setParser(QStringLiteral("modbus"), {}));
        Results more;
        serial.modbusMaster()->enqueue(request(1, 1, 0, 3), 1000, ModbusMaster::Priority::Read, more.done());
        serial.inject(rtu("01010201ff")); // two bytes of coils for a read of three
        QVERIFY(more.list.isEmpty());
        serial.inject(rtu("018102")); // an exception names no address and still answers it
        QCOMPARE(more.list.size(), 1);
        QCOMPARE(more.list.at(0).problem, QStringLiteral("设备返回异常 02 非法数据地址"));
        const auto fc16 = CommandEncoder::modbusRequest(1, 16, 40, QStringLiteral("1"), QStringLiteral("uint16"),
                                                        CommandEncoder::registerOrders().first());
        QVERIFY(fc16);
        serial.modbusMaster()->enqueue(*fc16, 1000, ModbusMaster::Priority::Write, more.done());
        QCOMPARE(serial.sent.last(), rtu("0110002800010200 01"));
        serial.inject(rtu("011000290001")); // the echo of a write to 41
        QCOMPARE(more.list.size(), 1);
        serial.inject(rtu("011000280001"));
        QCOMPARE(more.list.size(), 2);
        QVERIFY(more.list.at(1).ok);
    }

    // A request without a reply times out and the next one goes out; a link going down ends every
    // request, the queued ones too, as timed out. A card can take back its reads not yet sent.
    void timeoutsAndDisconnects()
    {
        FakeLink link;
        QVERIFY(link.setParser(QStringLiteral("modbus"), {}));
        ModbusMaster *master = link.modbusMaster();
        Results results;
        master->enqueue(request(1, 3, 0, 1), 30, ModbusMaster::Priority::Read, results.done());
        master->enqueue(request(1, 3, 1, 1), 1000, ModbusMaster::Priority::Read, results.done());
        QTRY_COMPARE(results.list.size(), 1);
        QVERIFY(results.list.at(0).timedOut);
        QVERIFY(!results.list.at(0).cancelled);
        QCOMPARE(results.list.at(0).problem, QStringLiteral("无响应（30 ms 超时）"));
        QCOMPARE(link.sent.last(), rtu("010300010001"));

        int owner = 0;
        master->enqueue(request(1, 3, 2, 1), 500, ModbusMaster::Priority::Read, results.done());
        master->enqueue(request(1, 3, 3, 1), 500, ModbusMaster::Priority::Read, results.done(), &owner);
        master->cancelQueued(&owner);
        QCOMPARE(master->pending(), 1);
        link.setConnected(false);
        QCOMPARE(results.list.size(), 3);
        for (int i = 1; i < 3; ++i) {
            QVERIFY(results.list.at(i).timedOut);
            QVERIFY(results.list.at(i).cancelled);
        }
        QCOMPARE(results.list.at(2).problem, QStringLiteral("无响应（500 ms 超时）"));
        QVERIFY(!master->busy());
        QCOMPARE(master->pending(), 0);
        QVERIFY(master->enqueue(request(1, 3, 0, 1), 1000, ModbusMaster::Priority::Read).isEmpty());
    }

    // The hub queues a Modbus command on links that parse Modbus and sends links that do not the RTU
    // frame right away; a queued command counts as accepted.
    void hubQueuesOnModbusLinksOnly()
    {
        CommHub hub;
        FakeLink plc;
        QVERIFY(plc.setParser(QStringLiteral("modbus"), {}));
        FakeLink lines; // parses lines
        hub.addLink(&plc);
        hub.addLink(&lines);
        plc.modbusMaster()->enqueue(request(1, 3, 0, 1), 1000, ModbusMaster::Priority::Read);
        QList<QByteArray> sent;
        QCOMPARE(hub.sendModbus(request(1, 6, 2, 9), &sent), 2);
        QCOMPARE(sent, (QList<QByteArray>{rtu("010600020009"), rtu("010600020009")}));
        QCOMPARE(lines.sent, QList<QByteArray>{rtu("010600020009")});
        QCOMPARE(plc.sent, QList<QByteArray>{rtu("010300000001")}); // waits for the read's reply
        QCOMPARE(plc.modbusMaster()->pending(), 1);
        plc.inject(rtu("010302002a"));
        QCOMPARE(plc.sent.last(), rtu("010600020009"));

        QList<QPointer<ModbusMaster>> queuedOn;
        ModbusSend how;
        how.priority = ModbusMaster::Priority::Read;
        how.modbusLinksOnly = true;
        how.queuedOn = &queuedOn;
        QCOMPARE(hub.sendModbus(request(1, 3, 0, 1), nullptr, how), 1);
        QCOMPARE(queuedOn.size(), 1);
        QCOMPARE(queuedOn.first().data(), plc.modbusMaster());
        QCOMPARE(lines.sent.size(), 1);
        hub.removeLink(&plc);
        hub.removeLink(&lines);
    }
};

QTEST_GUILESS_MAIN(TestModbus)
#include "tst_modbus.moc"
