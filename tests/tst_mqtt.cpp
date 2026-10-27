#include "comm/mqtt_link.h"
#include "core/mqtt_codec.h"

#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>
#include <QtTest/QtTest>

using namespace fhmi;

namespace {

// Just enough of a broker for one client: answers CONNECT, SUBSCRIBE, PINGREQ and the
// QoS handshakes, records what the client sent, and publishes on request.
class FakeBroker : public QObject
{
  public:
    FakeBroker()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            client = server.nextPendingConnection();
            decoder.reset();
            ++connections;
            connect(client, &QTcpSocket::readyRead, this, [this]() {
                for (const Mqtt::Packet &p : decoder.feed(client->readAll())) {
                    received.append(p);
                    answer(p);
                }
            });
        });
    }

    quint16 port() const { return server.serverPort(); }

    void publish(const QString &topic, const QByteArray &payload, int qos = 0, quint16 id = 0, bool dup = false)
    {
        Mqtt::PublishMessage m;
        m.topic = topic;
        m.payload = payload;
        m.qos = qos;
        m.packetId = id;
        m.duplicate = dup;
        client->write(Mqtt::encodePublish(m));
    }

    QList<Mqtt::Packet> ofType(quint8 type) const
    {
        QList<Mqtt::Packet> out;
        for (const Mqtt::Packet &p : received) {
            if (p.type == type) {
                out.append(p);
            }
        }
        return out;
    }

    QTcpServer server;
    QTcpSocket *client = nullptr;
    Mqtt::StreamDecoder decoder;
    QList<Mqtt::Packet> received;
    int connAckCode = 0;
    int connections = 0;

  private:
    void answer(const Mqtt::Packet &p)
    {
        switch (p.type) {
        case Mqtt::Connect:
            client->write(QByteArray::fromHex("2002") + QByteArray(1, '\0') + QByteArray(1, char(connAckCode)));
            break;
        case Mqtt::Subscribe: {
            QByteArray body = p.body.left(2);
            int at = 2;
            while (at + 2 < p.body.size()) {
                const int length = (quint8(p.body.at(at)) << 8) | quint8(p.body.at(at + 1));
                const QString topic = QString::fromUtf8(p.body.mid(at + 2, length));
                body.append(topic.startsWith(QLatin1String("deny/")) ? char(0x80) : p.body.at(at + 2 + length));
                at += 3 + length;
            }
            client->write(QByteArray(1, char(0x90)) + QByteArray(1, char(body.size())) + body);
            break;
        }
        case Mqtt::Publish: {
            const auto m = Mqtt::decodePublish(p);
            if (m && m->qos == 1) {
                client->write(Mqtt::encodeAck(Mqtt::PubAck, m->packetId));
            } else if (m && m->qos == 2) {
                client->write(Mqtt::encodeAck(Mqtt::PubRec, m->packetId));
            }
            break;
        }
        case Mqtt::PubRel:
            client->write(Mqtt::encodeAck(Mqtt::PubComp, *Mqtt::packetId(p)));
            break;
        case Mqtt::PingReq:
            client->write(QByteArray::fromHex("D000"));
            break;
        default:
            break;
        }
    }
};

MqttSettings settingsFor(const FakeBroker &broker)
{
    MqttSettings s;
    s.port = broker.port();
    s.clientId = QStringLiteral("test-client");
    s.username = QStringLiteral("user");
    s.password = QStringLiteral("secret");
    s.topics = {QStringLiteral("device/data"), QStringLiteral("sensor/#")};
    s.publishTopic = QStringLiteral("device/cmd");
    return s;
}

} // namespace

class TestMqtt : public QObject
{
    Q_OBJECT

  private slots:
    void connectPacketBytes()
    {
        Mqtt::ConnectOptions options;
        options.clientId = QStringLiteral("c1");
        QCOMPARE(Mqtt::encodeConnect(options), QByteArray::fromHex("100E00044D5154540402003C000263" "31"));
        options.username = QStringLiteral("u");
        options.password = QStringLiteral("p");
        const QByteArray withAuth = Mqtt::encodeConnect(options);
        QCOMPARE(quint8(withAuth.at(9)), quint8(0xC2));
        QVERIFY(withAuth.endsWith(QByteArray::fromHex("0001750001" "70")));
    }

    void subscribeAndAckBytes()
    {
        QCOMPARE(Mqtt::encodeSubscribe(1, {QStringLiteral("a/b")}, 1), QByteArray::fromHex("82080001000361" "2F6201"));
        QCOMPARE(Mqtt::encodeAck(Mqtt::PubRel, 0x1234), QByteArray::fromHex("62021234"));
        QCOMPARE(Mqtt::encodeAck(Mqtt::PubAck, 7), QByteArray::fromHex("40020007"));
    }

    void publishRoundTripsThroughDecoder()
    {
        Mqtt::PublishMessage m;
        m.topic = QStringLiteral("温度/1");
        m.payload = QByteArray(300, 'x'); // a two-byte remaining length
        m.qos = 2;
        m.packetId = 513;
        m.retain = true;
        const QByteArray wire = Mqtt::encodePublish(m);
        Mqtt::StreamDecoder decoder;
        QVERIFY(decoder.feed(wire.left(3)).isEmpty());
        const QList<Mqtt::Packet> packets = decoder.feed(wire.mid(3) + Mqtt::encodePingReq());
        QCOMPARE(packets.size(), 2);
        const auto decoded = Mqtt::decodePublish(packets.first());
        QVERIFY(decoded);
        QCOMPARE(decoded->topic, m.topic);
        QCOMPARE(decoded->payload, m.payload);
        QCOMPARE(decoded->qos, 2);
        QCOMPARE(decoded->packetId, quint16(513));
        QVERIFY(decoded->retain);
        QCOMPARE(packets.at(1).type, quint8(Mqtt::PingReq));
    }

    void malformedLengthStopsDecoding()
    {
        Mqtt::StreamDecoder decoder;
        decoder.feed(QByteArray::fromHex("30FFFFFFFF01"));
        QVERIFY(decoder.hasError());
    }

    void topicListsAndTexts()
    {
        QCOMPARE(Mqtt::topics(QStringLiteral(" a/b, c;\nd ,a/b")),
                 (QStringList{QStringLiteral("a/b"), QStringLiteral("c"), QStringLiteral("d")}));
        QCOMPARE(Mqtt::connAckText(4), QStringLiteral("用户名或密码错误"));
    }

    void invalidSettingsAreRejected()
    {
        MqttLink link;
        QString error;
        MqttSettings s;
        s.host = QStringLiteral("  ");
        QVERIFY(!link.connectTo(s, &error));
        QVERIFY(error.contains(QStringLiteral("服务器地址")));
        s.host = QStringLiteral("localhost");
        s.qos = 3;
        QVERIFY(!link.connectTo(s, &error));
        QVERIFY(!link.isActive());
    }

    void connectsSubscribesAndReceives()
    {
        FakeBroker broker;
        MqttLink link;
        QSignalSpy connected(&link, &Link::connectedChanged);
        QSignalSpy messages(&link, &Link::messageReceived);
        QVERIFY(link.connectTo(settingsFor(broker)));
        QTRY_VERIFY(link.isConnected());
        QCOMPARE(connected.first().first().toBool(), true);

        const QByteArray connect = broker.ofType(Mqtt::Connect).first().body;
        QVERIFY(connect.contains("test-client"));
        QVERIFY(connect.contains("secret"));
        QTRY_COMPARE(broker.ofType(Mqtt::Subscribe).size(), 1);
        QVERIFY(broker.ofType(Mqtt::Subscribe).first().body.contains("sensor/#"));

        // A line payload without a newline is still one frame.
        broker.publish(QStringLiteral("device/data"), "temperature=25.5,running=1");
        QTRY_COMPARE(messages.size(), 1);
        Message m = messages.takeFirst().first().value<Message>();
        QCOMPARE(m.value("temperature").toDouble(), 25.5);
        QCOMPARE(m.value("running").toLongLong(), 1);

        // A bare value is also reported under its topic.
        broker.publish(QStringLiteral("sensor/t"), "42", 1, 10);
        QTRY_COMPARE(messages.size(), 1);
        m = messages.takeFirst().first().value<Message>();
        QCOMPARE(m.value("sensor/t").toLongLong(), 42);
        QTRY_COMPARE(broker.ofType(Mqtt::PubAck).size(), 1);

        // QoS 2: a redelivery before PUBREL is the same message, delivered once.
        broker.publish(QStringLiteral("device/data"), "x=1", 2, 11);
        broker.publish(QStringLiteral("device/data"), "x=1", 2, 11, true);
        QTRY_COMPARE(broker.ofType(Mqtt::PubRec).size(), 2);
        QTest::qWait(50);
        QCOMPARE(messages.size(), 1);
        broker.client->write(Mqtt::encodeAck(Mqtt::PubRel, 11));
        QTRY_COMPARE(broker.ofType(Mqtt::PubComp).size(), 1);
    }

    void sendsCommandsToThePublishTopic()
    {
        FakeBroker broker;
        MqttLink link;
        MqttSettings s = settingsFor(broker);
        s.qos = 2;
        QSignalSpy sent(&link, &Link::bytesSent);
        QVERIFY(link.connectTo(s));
        QTRY_VERIFY(link.isConnected());
        QVERIFY(link.send("START\r\n"));
        QTRY_COMPARE(broker.ofType(Mqtt::Publish).size(), 1);
        const auto m = Mqtt::decodePublish(broker.ofType(Mqtt::Publish).first());
        QCOMPARE(m->topic, QStringLiteral("device/cmd"));
        QCOMPARE(m->payload, QByteArray("START\r\n"));
        QCOMPARE(m->qos, 2);
        QTRY_COMPARE(broker.ofType(Mqtt::PubRel).size(), 1); // the client finished the QoS 2 handshake
        QCOMPARE(sent.size(), 1);
    }

    void missingPublishTopicIsExplained()
    {
        FakeBroker broker;
        MqttLink link;
        MqttSettings s = settingsFor(broker);
        s.publishTopic.clear();
        QSignalSpy errors(&link, &Link::errorOccurred);
        QVERIFY(link.connectTo(s));
        QTRY_VERIFY(link.isConnected());
        QVERIFY(!link.send("x"));
        QCOMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("发布主题")));
    }

    void refusalIsReportedAndFinal()
    {
        FakeBroker broker;
        broker.connAckCode = 5;
        MqttLink link;
        QSignalSpy errors(&link, &Link::errorOccurred);
        QVERIFY(link.connectTo(settingsFor(broker)));
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("未授权")));
        QVERIFY(!link.isActive());
        QTest::qWait(1500);
        QCOMPARE(broker.connections, 1); // no retry after a refusal
    }

    void failedSubscriptionIsReported()
    {
        FakeBroker broker;
        MqttLink link;
        MqttSettings s = settingsFor(broker);
        s.topics = {QStringLiteral("ok/a"), QStringLiteral("deny/b")};
        QSignalSpy errors(&link, &Link::errorOccurred);
        QVERIFY(link.connectTo(s));
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("deny/b")));
        QVERIFY(link.isConnected());
    }

    void reconnectsAfterADroppedConnection()
    {
        FakeBroker broker;
        MqttLink link;
        QSignalSpy errors(&link, &Link::errorOccurred);
        QVERIFY(link.connectTo(settingsFor(broker)));
        QTRY_VERIFY(link.isConnected());
        broker.client->abort();
        QTRY_VERIFY(!link.isConnected());
        QVERIFY(errors.last().first().toString().contains(QStringLiteral("正在重连")));
        QVERIFY(link.isActive());
        QTRY_VERIFY_WITH_TIMEOUT(link.isConnected(), 5000);
        QCOMPARE(broker.connections, 2);
        link.close();
        QVERIFY(!link.isActive());
    }

    void hexFramesBufferPerTopic()
    {
        FakeBroker broker;
        MqttLink link;
        // Built step by step: QJsonArray{QJsonArray{...}} is a copy of the inner array, not
        // an array holding it, whenever list-initialization picks the copy constructor.
        QJsonArray fields;
        fields.append(QJsonArray{"v", 0, 1, "uint8"});
        QString error;
        QVERIFY2(link.setParser(QStringLiteral("hex"),
                                QJsonObject{{"header", "AA55"}, {"footer", "55AA"}, {"fields", fields}}, &error),
                 qPrintable(error));
        QSignalSpy messages(&link, &Link::messageReceived);
        QVERIFY(link.connectTo(settingsFor(broker)));
        QTRY_VERIFY(link.isConnected());
        broker.publish(QStringLiteral("sensor/a"), QByteArray::fromHex("AA5507"));
        broker.publish(QStringLiteral("sensor/b"), QByteArray::fromHex("55AA")); // another topic's tail
        QTest::qWait(100);
        QCOMPARE(messages.size(), 0);
        broker.publish(QStringLiteral("sensor/a"), QByteArray::fromHex("55AA"));
        QTRY_COMPARE(messages.size(), 1);
        QCOMPARE(messages.first().first().value<Message>().value("v").toLongLong(), 7);
    }
};

QTEST_GUILESS_MAIN(TestMqtt)
#include "tst_mqtt.moc"
