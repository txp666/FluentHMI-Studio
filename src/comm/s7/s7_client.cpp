#include "comm/s7/s7_client.h"
#include "snap7.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QHash>
#include <QtCore/QQueue>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <atomic>
#include <algorithm>
#include <limits>
#include <map>

namespace fhmi {
namespace {
QString problem(int code) { char text[512] = {}; Cli_ErrorText(code,text,sizeof text); return QString::fromLocal8Bit(text); }
int area(const S7::Address &address) {
    switch(address.area) {
    case S7::MemoryArea::DataBlock:return S7AreaDB;
    case S7::MemoryArea::Marker:return S7AreaMK;
    case S7::MemoryArea::Input:return S7AreaPE;
    case S7::MemoryArea::Output:return S7AreaPA;
    } return S7AreaDB;
}
bool networkError(int code) { return (quint32(code) & 0x000FFFFFU) != 0; }
}

class S7Worker final : public QObject {
    Q_OBJECT
public:
    explicit S7Worker(std::shared_ptr<std::atomic<quint64>> activeLife, const S7::Config &config)
        : m_config(config), m_activeLife(std::move(activeLife)) {
        m_clock.start();
        m_timer = new QTimer(this); m_timer->setSingleShot(true);
        connect(m_timer,&QTimer::timeout,this,&S7Worker::step);
    }
    ~S7Worker() override { shutdown(); }
    void start(const QString &id, const S7::Config &config, quint64 life) {
        shutdown(); m_id=id; m_config=config; m_life=life; m_epoch=life<<32;
        m_active=true; m_attempted=false; m_retryAt=0; m_timer->start(0);
    }
    void shutdown() {
        m_active=false; m_timer->stop(); m_writes.clear(); disconnectNative();
    }
    void needs(const QList<S7::Point> &points) {
        QHash<QString, Read> next;
        for(const auto &point:points) {
            const QString key=point.key();
            Read read=m_reads.value(key,Read{point,0});
            read.point=point;
            read.point.period=point.period>0?point.period:m_config.period;
            if(next.contains(key)) read.point.period=std::min(next.value(key).point.period,read.point.period);
            next.insert(key,read);
        }
        m_reads=std::move(next); if(m_active) m_timer->start(0);
    }
    void write(const S7::Command &command, quint64 life, quint64 epoch) {
        if(life!=m_life || epoch!=m_epoch || !m_connected || cancelled()) {
            emit written(life,false,tr("S7 未连接，写入未发送")); return;
        }
        m_writes.enqueue(command); m_timer->start(0);
    }
signals:
    void state(quint64 life, quint64 epoch, bool connected, const QString &message, bool error = false);
    void sample(quint64 life, const fhmi::S7::Sample &sample);
    void written(quint64 life, bool success, const QString &error);
private:
    struct Read { S7::Point point; qint64 due=0; };
    bool cancelled() const { return m_activeLife->load() != m_life; }
    void disconnectNative() {
        if(m_native) { Cli_Disconnect(m_native); Cli_Destroy(&m_native); }
        m_connected=false;
    }
    void publish(const S7::Point &point,const QVariant &value,const QString &quality) {
        if(cancelled()) return;
        emit sample(m_life,S7::Sample{m_id,point.address.canonical,point.type.name,value,quality,QDateTime::currentDateTimeUtc(),m_epoch});
    }
    void lost(int code) {
        disconnectNative(); m_writes.clear(); m_retryAt=m_clock.elapsed()+m_config.reconnectInterval;
        emit state(m_life,m_epoch,false,tr("S7 连接断开：%1").arg(problem(code)),true);
        for(const auto &read:m_reads) publish(read.point,{},QStringLiteral("disconnected"));
    }
    void step() {
        if(!m_active || cancelled()) return;
        if(!m_connected) {
            if(m_attempted && !m_config.autoReconnect) return;
            const qint64 remaining=m_retryAt-m_clock.elapsed();
            if(remaining>0) { m_timer->start(int(remaining)); return; }
            m_attempted=true;
            emit state(m_life,m_epoch,false,tr("正在连接 S7…"));
            m_native=Cli_Create();
            word port=word(m_config.port);
            int timeout=m_config.timeout;
            Cli_SetParam(m_native,p_u16_RemotePort,&port);
            Cli_SetParam(m_native,p_i32_PingTimeout,&timeout);
            Cli_SetParam(m_native,p_i32_SendTimeout,&timeout);
            Cli_SetParam(m_native,p_i32_RecvTimeout,&timeout);
            Cli_SetConnectionType(m_native,word(m_config.connectionType));
            const int code=Cli_ConnectTo(m_native,m_config.host.toLatin1().constData(),m_config.rack,m_config.slot);
            if(cancelled()) { disconnectNative(); return; }
            if(code) { lost(code); if(m_config.autoReconnect) m_timer->start(m_config.reconnectInterval); return; }
            m_connected=true; ++m_epoch;
            for(auto it=m_reads.begin();it!=m_reads.end();++it) it->due=0;
            emit state(m_life,m_epoch,true,tr("S7 已连接"));
        }
        if(!m_writes.isEmpty()) {
            const auto command=m_writes.dequeue();
            const auto &point=command.point;
            QByteArray bytes=command.bytes;
            const int start=point.type.isBool()?point.address.byteOffset*8+point.address.bit:point.address.byteOffset;
            const int amount=point.type.isBool()?1:point.type.byteCount;
            const int code=Cli_WriteArea(m_native,area(point.address),point.address.dbNumber,start,amount,
                                         point.type.isBool()?S7WLBit:S7WLByte,bytes.data());
            if(cancelled()) return;
            emit written(m_life,code==0,code?(networkError(code)?tr("S7 写入结果未知，未自动重发：%1").arg(problem(code)):tr("S7 写入失败：%1").arg(problem(code))):QString());
            if(code && networkError(code)) lost(code);
        } else {
            const qint64 now=m_clock.elapsed();
            QString selected; qint64 earliest=std::numeric_limits<qint64>::max();
            for(auto it=m_reads.constBegin();it!=m_reads.constEnd();++it) if(it->due<earliest) { earliest=it->due;selected=it.key(); }
            if(!selected.isEmpty() && earliest<=now) {
                Read &read=m_reads[selected]; const auto point=read.point;
                read.due=now+(point.period>0?point.period:m_config.period);
                QByteArray bytes(point.type.byteCount,'\0');
                const int code=Cli_ReadArea(m_native,area(point.address),point.address.dbNumber,point.address.byteOffset,
                                            point.type.byteCount,S7WLByte,bytes.data());
                if(cancelled()) return;
                publish(point,code?QVariant():S7::decodeValue(bytes.constData(),bytes.size(),point.type,point.type.isBool()?point.address.bit:0),
                        code?QStringLiteral("bad"):QStringLiteral("good"));
                if(code && networkError(code)) lost(code);
            }
        }
        if(!m_active || cancelled()) return;
        int next=500;
        if(!m_connected) next=m_config.reconnectInterval;
        else if(!m_writes.isEmpty()) next=1;
        else for(const auto &read:m_reads) next=std::min(next,int(std::max<qint64>(1,read.due-m_clock.elapsed())));
        m_timer->start(next);
    }
    QString m_id;
    S7::Config m_config;
    S7Object m_native=0;
    QTimer *m_timer;
    QElapsedTimer m_clock;
    QHash<QString,Read> m_reads;
    QQueue<S7::Command> m_writes;
    std::shared_ptr<std::atomic<quint64>> m_activeLife;
    quint64 m_life=0,m_epoch=0;
    qint64 m_retryAt=0;
    bool m_active=false,m_connected=false,m_attempted=false;
};

struct S7Client::Impl {
    struct Session {
        S7::Config config;
        QList<S7::Point> points;
        QThread thread;
        S7Worker *worker=nullptr;
        std::shared_ptr<std::atomic<quint64>> activeLife=std::make_shared<std::atomic<quint64>>(0);
        quint64 life=0,epoch=0;
        bool connected=false;
        ~Session() {
            activeLife->store(0);
            if(thread.isRunning()) {
                QMetaObject::invokeMethod(worker,[this]{worker->shutdown();},Qt::BlockingQueuedConnection);
                thread.quit();thread.wait();
            }
        }
    };
    std::map<QString,std::unique_ptr<Session>> sessions;
    quint64 nextLife=0;
};
S7Client::S7Client(QObject *parent):QObject(parent),d(std::make_unique<Impl>()) { qRegisterMetaType<S7::Sample>(); }
S7Client::~S7Client() { clear(); }
void S7Client::configure(const QString &id,const QJsonObject &properties) {
    if(d->sessions.count(id)) remove(id);
    QString error; const auto config=S7::config(properties,&error);
    if(!config) { emit statusChanged(id,false,error);emit errorOccurred(id,error);return; }
    auto session=std::make_unique<Impl::Session>();auto *s=session.get();s->config=*config;
    s->worker=new S7Worker(s->activeLife,*config);s->worker->moveToThread(&s->thread);
    connect(&s->thread,&QThread::finished,s->worker,&QObject::deleteLater);
    connect(s->worker,&S7Worker::state,this,[this,id](quint64 life,quint64 epoch,bool connected,const QString &message,bool error) {
        const auto it=d->sessions.find(id);if(it==d->sessions.end()||life!=it->second->life)return;
        auto *s=it->second.get();s->epoch=epoch;s->connected=connected;emit statusChanged(id,connected,message);emit logMessage(id+QStringLiteral(": ")+message);
        if(error)emit errorOccurred(id,message);
    });
    connect(s->worker,&S7Worker::sample,this,[this,id](quint64 life,const S7::Sample &sample) {
        const auto it=d->sessions.find(id);if(it!=d->sessions.end()&&life==it->second->life&&sample.sessionGeneration==it->second->epoch)emit sampleReceived(sample);
    });
    connect(s->worker,&S7Worker::written,this,[this,id](quint64 life,bool ok,const QString &error) {
        const auto it=d->sessions.find(id);if(it!=d->sessions.end()&&life==it->second->life)emit writeFinished(id,ok,error);
    });
    s->thread.start();d->sessions.emplace(id,std::move(session));
    if(config->autoConnect) open(id);
}
void S7Client::open(const QString &id) {
    const auto found=d->sessions.find(id);if(found==d->sessions.end())return;auto *s=found->second.get();
    s->life=++d->nextLife;s->activeLife->store(s->life);s->connected=false;
    QMetaObject::invokeMethod(s->worker,[s,id,life=s->life]{s->worker->start(id,s->config,life);},Qt::QueuedConnection);
}
void S7Client::close(const QString &id) {
    const auto found=d->sessions.find(id);if(found==d->sessions.end())return;auto *s=found->second.get();
    s->life=++d->nextLife;s->activeLife->store(s->life);s->connected=false;
    s->epoch=s->life<<32;
    QMetaObject::invokeMethod(s->worker,[s]{s->worker->shutdown();},Qt::QueuedConnection);
    for(const auto &point:s->points)
        emit sampleReceived(S7::Sample{id,point.address.canonical,point.type.name,{},QStringLiteral("disconnected"),QDateTime::currentDateTimeUtc(),s->epoch});
    emit statusChanged(id,false,tr("S7 未连接"));
}
void S7Client::remove(const QString &id) {
    auto it=d->sessions.find(id);if(it==d->sessions.end())return;
    // Remove queued signal deliveries before Session's captured pointer expires.
    it->second->worker->disconnect(this);d->sessions.erase(it);
}
void S7Client::clear() { while(!d->sessions.empty()) remove(d->sessions.begin()->first); }
bool S7Client::connected(const QString &id) const {const auto it=d->sessions.find(id);return it!=d->sessions.end()&&it->second->connected;}
void S7Client::setNeeds(const QList<S7::Need> &needs) {
    for(const auto &entry:d->sessions) {
        QList<S7::Point> points;
        for(const auto &need:needs) if(need.sourceCardId==entry.first)points.append(need.point);
        auto *s=entry.second.get();s->points=points;QMetaObject::invokeMethod(s->worker,[s,points]{s->worker->needs(points);},Qt::QueuedConnection);
    }
}
bool S7Client::write(const S7::Command &command,QString *error) {
    if(command.point.address.area==S7::MemoryArea::Input) {
        if(error)*error=tr("S7 输入区 I 只读，请选择 DB、M 或 Q 写入");return false;
    }
    const auto it=d->sessions.find(command.targetId);
    if(it==d->sessions.end()||!it->second->connected) { if(error)*error=tr("S7 未连接，写入未发送");return false; }
    auto *s=it->second.get();
    QMetaObject::invokeMethod(s->worker,[s,command,life=s->life,epoch=s->epoch]{s->worker->write(command,life,epoch);},Qt::QueuedConnection);
    return true;
}
} // namespace fhmi
#include "s7_client.moc"
