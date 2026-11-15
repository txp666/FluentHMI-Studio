#include "comm/s7/s7_client.h"
#include "core/s7.h"
#include "snap7.h"
#include <QtCore/QElapsedTimer>
#include <QtNetwork/QTcpServer>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace fhmi;
namespace {
int freePort() { QTcpServer server; if(!server.listen(QHostAddress::LocalHost,0))return 0; return server.serverPort(); }
QJsonObject point(const char *address,const char *type) {return {{QStringLiteral("address"),QLatin1String(address)},{QStringLiteral("dataType"),QLatin1String(type)}};}
struct Server {
    QByteArray db=QByteArray(512,'\0');
    S7Object native=Srv_Create();
    ~Server(){Srv_Stop(native);Srv_Destroy(&native);}
    bool start(int port){word p=word(port);Srv_SetParam(native,p_u16_LocalPort,&p);Srv_RegisterArea(native,srvAreaDB,1,db.data(),db.size());return Srv_StartTo(native,"127.0.0.1")==0;}
};
}
class S7Test:public QObject {
    Q_OBJECT
private slots:
    void scalarCodec();
    void nativeReadAndBitWrite();
    void replacingSessionDropsQueuedWrites();
    void silentPeerStops();
};
void S7Test::scalarCodec() {
    QString error;
    for(const auto &type:S7::dataTypes()) {
        S7::DataType parsed;QVERIFY(S7::parseDataType(type,&parsed,&error));
        QByteArray bytes;QVERIFY(S7::encodeValue(parsed.isBool()?QVariant(true):QVariant(1),parsed,&bytes,&error));
        QCOMPARE(S7::decodeValue(bytes.constData(),bytes.size(),parsed).toInt(),1);
    }
    auto p=S7::point(point("DB01.DBX0.7","BOOL"),&error);QVERIFY(p);QCOMPARE(p->address.canonical,QStringLiteral("DB1.DBX0.7"));
    QVERIFY(!S7::point(point("DB1.DBW4","REAL"),&error));
    QVERIFY(!S7::point(point("DB1.DBX0.8","BOOL"),&error));
    S7::DataType type;QVERIFY(S7::parseDataType(QStringLiteral("INT"),&type));QByteArray bytes;
    QVERIFY(!S7::encodeValue(QStringLiteral("1.0"),type,&bytes));
    QVERIFY(!S7::encodeValue(true,type,&bytes));
    QVERIFY(!S7::encodeValue(32768,type,&bytes));
    QVERIFY(S7::encodeValue(-2,type,&bytes));QCOMPARE(bytes.toHex(),QByteArray("fffe"));
}
void S7Test::nativeReadAndBitWrite() {
    const int port=freePort();QVERIFY(port);
    Server server;server.db[0]=char(0x80);QVERIFY(server.start(port));
    S7Client client;client.configure(QStringLiteral("plc"),{{QStringLiteral("port"),port},{QStringLiteral("period"),20},{QStringLiteral("timeout"),200},{QStringLiteral("autoReconnect"),false}});
    QSignalSpy samples(&client,&S7Client::sampleReceived);
    QSignalSpy writes(&client,&S7Client::writeFinished);
    const auto bit=*S7::point(point("DB1.DBX0.1","BOOL"));
    auto slower=bit;slower.period=500;
    client.setNeeds({{QStringLiteral("plc"),bit},{QStringLiteral("plc"),slower}});client.open(QStringLiteral("plc"));
    QTRY_VERIFY_WITH_TIMEOUT(client.connected(QStringLiteral("plc")),2000);
    QTRY_VERIFY_WITH_TIMEOUT(!samples.isEmpty(),2000);
    // A zero point period uses the configured 20 ms even when needs are registered before open.
    QTRY_VERIFY_WITH_TIMEOUT(samples.size()>=2,300);
    QByteArray bytes;QVERIFY(S7::encodeValue(true,bit.type,&bytes));
    QVERIFY(client.write({QStringLiteral("plc"),bit,bytes}));
    QTRY_VERIFY_WITH_TIMEOUT(!writes.isEmpty(),2000);
    QVERIFY(writes.first().at(1).toBool());
    Srv_LockArea(server.native,srvAreaDB,1);const auto result=quint8(server.db.at(0));Srv_UnlockArea(server.native,srvAreaDB,1);
    QCOMPARE(result,quint8(0x82));
    client.close(QStringLiteral("plc"));QVERIFY(!client.connected(QStringLiteral("plc")));
    client.clear();
}
void S7Test::replacingSessionDropsQueuedWrites() {
    const int port=freePort();QVERIFY(port);
    Server server;server.db[0]=char(0x80);QVERIFY(server.start(port));
    S7Client client;client.configure(QStringLiteral("plc"),{{QStringLiteral("port"),port},{QStringLiteral("period"),20},{QStringLiteral("timeout"),500},{QStringLiteral("autoReconnect"),false}});
    client.open(QStringLiteral("plc"));
    QTRY_VERIFY_WITH_TIMEOUT(client.connected(QStringLiteral("plc")),2000);
    const auto bit=*S7::point(point("DB1.DBX0.1","BOOL"));
    // Hold a native read so the write is still queued when close/open changes the session.
    Srv_LockArea(server.native,srvAreaDB,1);
    client.setNeeds({{QStringLiteral("plc"),bit}});
    QTest::qWait(50);
    QByteArray bytes;QVERIFY(S7::encodeValue(true,bit.type,&bytes));
    QVERIFY(client.write({QStringLiteral("plc"),bit,bytes}));
    client.close(QStringLiteral("plc"));client.open(QStringLiteral("plc"));
    Srv_UnlockArea(server.native,srvAreaDB,1);
    QTRY_VERIFY_WITH_TIMEOUT(client.connected(QStringLiteral("plc")),2000);
    QTest::qWait(50);
    client.clear();
    Srv_LockArea(server.native,srvAreaDB,1);const auto result=quint8(server.db.at(0));Srv_UnlockArea(server.native,srvAreaDB,1);
    QCOMPARE(result,quint8(0x80));
}
void S7Test::silentPeerStops() {
    QTcpServer peer;QVERIFY(peer.listen(QHostAddress::LocalHost,0));
    S7Client client;client.configure(QStringLiteral("silent"),{{QStringLiteral("port"),int(peer.serverPort())},{QStringLiteral("timeout"),100},{QStringLiteral("autoReconnect"),false}});
    client.open(QStringLiteral("silent"));QTest::qWait(30);
    QElapsedTimer clock;clock.start();client.clear();
    QVERIFY2(clock.elapsed()<1000,"A silent TCP peer must not leave a native worker running");
}
QTEST_GUILESS_MAIN(S7Test)
#include "tst_s7.moc"
