/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The host clipboard of a remote host, in the stream worker
 * (docs/design/clipboard-agent.md):
 *  - RemoteClipboardProvider: writes, acks, timeouts, availability, and the
 *    paste chord that never overtakes the write;
 *  - RelayClipboard, the three relays' shared end: clipboardcaps as the agent
 *    comes and goes, host text, the browser's messages;
 *  - the whole chain in one process: a fake agent on loopback, the real
 *    server and router, and the worker's provider fed by the router.
 */

#include "test_framework.h"

#include "clipboard/ClipboardAgentProtocol.h"
#include "clipboard/ClipboardIpc.h"
#include "server/ClipboardAgentRegistry.h"
#include "server/ClipboardAgentRouter.h"
#include "server/ClipboardAgentServer.h"
#include "streaming/IMediaEngine.h"
#include "streaming/RelayClipboard.h"
#include "streaming/RemoteClipboardProvider.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QThread>
#include <QVariant>
#include <QWebSocket>

#include <functional>
#include <memory>

namespace P = ClipboardAgent;

namespace {

bool waitFor(const std::function<bool()>& done, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return done();
}

void pump(int ms)
{
    waitFor([] { return false; }, ms);
}

/// What a signal carried, call by call (QSignalSpy without Qt Test).
struct Spy
{
    QList<QVariantList> calls;
    int count() const { return calls.size(); }
    const QVariantList& last() const { return calls.last(); }
};

template <class Obj, class Base, class... A> void spyOn(Spy& spy, Obj* obj, void (Base::*sig)(A...))
{
    QObject::connect(obj, sig, [&spy](A... a) {
        spy.calls << QVariantList{QVariant::fromValue(std::decay_t<A>(a))...};
    });
}

QJsonObject json(const QByteArray& frame)
{
    return QJsonDocument::fromJson(frame).object();
}

/// Records the keys a paste injects.
class FakeEngine : public IMediaEngine
{
public:
    QStringList keys;
    void stopConnection() override {}
    void interruptConnection() override {}
    bool isConnected() const override { return true; }
    void requestIdrFrame() override {}
    void videoFrameDelivered() override {}
    bool takeWorkerDroppedDelta() override { return false; }
    int64_t workerDropCount() const override { return 0; }
    int pendingVideoFrames() const override { return 0; }
    int negotiatedVideoFormat() const override { return 0; }
    int audioSamplesPerFrame() const override { return 0; }
    void sendKeyEvent(short keyCode, bool down, char, char, bool) override
    {
        keys << QStringLiteral("%1%2").arg(keyCode, 0, 16).arg(down ? "v" : "^");
    }
    void sendUtf8Text(const QString&) override {}
    void sendKeyChar(const QString&, bool, char, const CharKeyHints&) override {}
    void sendSecureAttention() override {}
    void sendMouseMove(short, short) override {}
    void sendMousePosition(short, short, short, short) override {}
    void sendMouseButton(bool, int, bool) override {}
    void sendMouseScroll(short) override {}
    void sendMouseHScroll(short) override {}
    void sendControllerArrival(uint8_t, uint16_t, uint8_t, bool) override {}
    void sendControllerState(short, short, int, unsigned char, unsigned char, short, short, short,
                             short) override
    {}
    void sendControllerRemoval(uint8_t, uint16_t) override {}
    void setControllerOffset(int) override {}
    void syncLockKeys(bool, bool, bool) override {}
    void syncHeldInputs(const QVector<HeldKey>&, quint32, bool) override {}
    void releaseHeldInputs(bool) override {}
    qint64 noteClientAlive() override { return 0; }
    double takeHostProcessingLatencyMs() override { return 0; }
    int64_t frameSubmitTimeUs() const override { return 0; }
    int64_t framePresentationTimeUs() const override { return 0; }
    int64_t firstFrameArrivalSteadyMs() const override { return 0; }
};

/// A fake agent: a raw WebSocket client speaking the protocol by hand.
struct RawAgent
{
    QWebSocket ws;
    QList<QByteArray> frames;
    bool closed = false;
    int closeCode = 0;

    RawAgent()
    {
        QObject::connect(&ws, &QWebSocket::textMessageReceived,
                         [this](const QString& m) { frames << m.toUtf8(); });
        QObject::connect(&ws, &QWebSocket::disconnected, [this]() {
            closed = true;
            closeCode = ws.closeCode();
        });
    }
    bool open(quint16 port, const QString& token)
    {
        QNetworkRequest req(QUrl(QStringLiteral("ws://127.0.0.1:%1/ws/clipboard-agent").arg(port)));
        if (!token.isNull()) req.setRawHeader("Authorization", "Bearer " + token.toLatin1());
        ws.open(req);
        return waitFor([this] { return ws.state() == QAbstractSocket::ConnectedState || closed; });
    }
    void send(const QByteArray& f) { ws.sendTextMessage(QString::fromUtf8(f)); }
    /// The next frame of @p type, waiting for it.
    QJsonObject next(const char* type, int ms = 2000)
    {
        QJsonObject found;
        waitFor(
            [&] {
                for (int i = 0; i < frames.size(); ++i) {
                    const QJsonObject o = json(frames[i]);
                    if (o.value("type").toString() == QLatin1String(type)) {
                        found = o;
                        frames.removeAt(i);
                        return true;
                    }
                }
                return false;
            },
            ms);
        return found;
    }
    bool hasFrame(const char* type) const
    {
        for (const QByteArray& f : frames)
            if (json(f).value("type").toString() == QLatin1String(type)) return true;
        return false;
    }
};

} // namespace

void run_clipboard_provider_tests()
{
    // ── Router + server + worker, end to end in process ─────────────────────
    SECTION("Clipboard agent — browser text reaches the agent, host text the worker");
    {
        ClipboardAgentRegistry reg{QString()};
        ClipboardAgentServer server(reg);
        CHECK(server.listen());
        ClipboardAgentRouter router(server);
        QObject::connect(&reg, &ClipboardAgentRegistry::availabilityChanged,
                         [&](const QString& h, bool a) { router.onAvailabilityChanged(h, a); });
        QObject::connect(&server, &ClipboardAgentServer::agentText,
                         [&](const QString& h, const QString& t) { router.onAgentText(h, t); });
        const QString token = reg.issueToken("H", 1);
        reg.setEnabled("H", true);

        // The worker end: a real RemoteClipboardProvider whose events go to
        // the router, and the router's commands back to it.
        int handle = 0;
        RemoteClipboardProvider* remotePtr = nullptr;
        RemoteClipboardProvider remote(
            [&](const QJsonObject& e) { router.onWorkerEvent(handle, e); }, false);
        remotePtr = &remote;
        handle = router.attach("H", [&](const QJsonObject& c) {
            remotePtr->handleCommand(ClipboardIpc::decodeCommand(c));
        });
        CHECK(!remote.available());

        Spy availability;
        spyOn(availability, &remote, &ClipboardProvider::availabilityChanged);
        Spy hostText;
        spyOn(hostText, &remote, &ClipboardProvider::hostTextChanged);
        RawAgent agent;
        agent.open(server.port(), token);
        agent.send(P::hello("L", "qt", "1"));
        CHECK(agent.next("welcome").value("watch").toBool());
        CHECK(waitFor([&] { return remote.available(); }));
        CHECK_EQ(availability.count(), 1);

        agent.send(P::changed("copied on the host"));
        CHECK(waitFor([&] { return hostText.count() == 1; }));
        CHECK_EQ(hostText.last().at(0).toString(), QString("copied on the host"));

        // A paste: the chord waits for the agent's ack.
        FakeEngine engine;
        remote.pasteFromClient(&engine, "pasted from the browser", false);
        const QJsonObject set = agent.next("set");
        CHECK_EQ(set.value("text").toString(), QString("pasted from the browser"));
        pump(30);
        CHECK(engine.keys.isEmpty());
        agent.send(P::ack(static_cast<qint64>(set.value("id").toDouble()), true));
        CHECK(waitFor([&] { return engine.keys.size() == 2; }));
        CHECK(engine.keys == QStringList({"56v", "56^"}));
        CHECK_EQ(remote.pendingCount(), 0);

        // A browser connecting asks for an announce.
        remote.requestAnnounce();
        CHECK(!agent.next("announce").isEmpty());

        // The agent goes: the worker hears it.
        agent.ws.close();
        CHECK(waitFor([&] { return !remote.available(); }));
        CHECK_EQ(availability.count(), 2);
        router.detach(handle);
        CHECK(!server.wantsWatch("H"));
    }

    // ── The worker's provider on its own ────────────────────────────────────
    SECTION("RemoteClipboardProvider — writes, acks, timeouts, availability");
    {
        QList<QJsonObject> events;
        RemoteClipboardProvider remote([&](const QJsonObject& e) { events << e; }, true);
        remote.setAckTimeoutMs(120);
        FakeEngine engine;

        // Set: one event, no chord, settled by the ack.
        remote.setFromClient("panel text");
        CHECK(waitFor([&] { return events.size() == 1; }));
        auto e = ClipboardIpc::decodeEvent(events[0]);
        CHECK(e.kind == ClipboardIpc::Event::Kind::Set);
        CHECK(!e.paste);
        CHECK_EQ(e.text, QString("panel text"));
        CHECK_EQ(remote.pendingCount(), 1);
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::ackCommand(e.id, true)));
        CHECK_EQ(remote.pendingCount(), 0);

        // Paste with wrapCtrl: Ctrl around V, only after the ack.
        remote.pasteFromClient(&engine, "p", true);
        CHECK(waitFor([&] { return events.size() == 2; }));
        e = ClipboardIpc::decodeEvent(events[1]);
        CHECK(e.paste);
        CHECK(engine.keys.isEmpty());
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::ackCommand(e.id, true)));
        CHECK(engine.keys == QStringList({"11v", "56v", "56^", "11^"}));

        // A refusal still pastes: the host's own clipboard.
        engine.keys.clear();
        remote.pasteFromClient(&engine, "q", false);
        CHECK(waitFor([&] { return events.size() == 3; }));
        e = ClipboardIpc::decodeEvent(events[2]);
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::ackCommand(e.id, false)));
        CHECK(engine.keys == QStringList({"56v", "56^"}));

        // No answer: the chord after the timeout, once; a late ack is ignored.
        engine.keys.clear();
        remote.pasteFromClient(&engine, "r", false);
        CHECK(waitFor([&] { return events.size() == 4; }));
        e = ClipboardIpc::decodeEvent(events[3]);
        pump(40);
        CHECK(engine.keys.isEmpty());
        CHECK(waitFor([&] { return engine.keys.size() == 2; }, 1000));
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::ackCommand(e.id, true)));
        CHECK_EQ(engine.keys.size(), 2);
        CHECK_EQ(remote.pendingCount(), 0);

        // Empty paste: the chord alone, nothing sent.
        engine.keys.clear();
        remote.pasteFromClient(&engine, "", false);
        CHECK(waitFor([&] { return engine.keys.size() == 2; }));
        CHECK_EQ(events.size(), 4);
        // Empty set: nothing at all.
        remote.setFromClient("");
        pump(20);
        CHECK_EQ(events.size(), 4);

        // Host text reaches the relays only while the agent is there.
        Spy text;
        spyOn(text, &remote, &ClipboardProvider::hostTextChanged);
        Spy avail;
        spyOn(avail, &remote, &ClipboardProvider::availabilityChanged);
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::textCommand("host")));
        CHECK_EQ(text.count(), 1);
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::availableCommand(false)));
        CHECK_EQ(avail.count(), 1);
        CHECK(!remote.available());
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::availableCommand(false)));
        CHECK_EQ(avail.count(), 1); // no change, no signal
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::textCommand("late")));
        CHECK_EQ(text.count(), 1);

        // Without the agent: a paste is the bare chord, a set is dropped,
        // and no announce is asked.
        engine.keys.clear();
        remote.pasteFromClient(&engine, "while away", false);
        CHECK(waitFor([&] { return engine.keys.size() == 2; }));
        remote.setFromClient("while away");
        remote.requestAnnounce();
        pump(20);
        CHECK_EQ(events.size(), 4);

        // An engine that died before the ack: no crash, nothing pressed.
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::availableCommand(true)));
        auto dying = std::make_unique<FakeEngine>();
        remote.pasteFromClient(dying.get(), "s", false);
        CHECK(waitFor([&] { return events.size() == 5; }));
        e = ClipboardIpc::decodeEvent(events[4]);
        dying.reset();
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::ackCommand(e.id, true)));
        CHECK_EQ(remote.pendingCount(), 0);

        // Install / instance.
        CHECK(RemoteClipboardProvider::instance() == nullptr);
        {
            RemoteClipboardProvider installed({}, false);
            RemoteClipboardProvider::install(&installed);
            CHECK(RemoteClipboardProvider::instance() == &installed);
        }
        CHECK(RemoteClipboardProvider::instance() == nullptr); // its destructor cleared it
    }

    // ── The relays' clipboard end ───────────────────────────────────────────
    SECTION("RelayClipboard — caps, host text, availability, the browser's messages");
    {
        QObject relay;
        QList<QByteArray> sent;
        RemoteClipboardProvider remote([](const QJsonObject&) {}, false);

        RelayClipboard off;
        off.attach(nullptr, &relay, [&](const QByteArray& j) { sent << j; });
        CHECK(!off.active());
        QList<QByteArray> now;
        off.clientConnected([&](const QByteArray& j) { now << j; });
        CHECK(now.isEmpty());
        // A clipboard message with sync off is consumed and dropped.
        CHECK(off.handle(nullptr, "clipboardset", QJsonObject{{"text", "x"}}, "[t]"));
        CHECK(!off.handle(nullptr, "keydown", QJsonObject{}, "[t]"));

        RelayClipboard rc;
        rc.attach(&remote, &relay, [&](const QByteArray& j) { sent << j; });
        CHECK(rc.provider() == &remote);
        CHECK(!rc.active());
        rc.clientConnected([&](const QByteArray& j) { now << j; });
        CHECK(now.isEmpty()); // agent away at connect: say nothing yet

        // The agent arrives: caps true pushed, host text flows.
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::availableCommand(true)));
        CHECK(waitFor([&] { return sent.size() == 1; }));
        CHECK_EQ(sent[0], RelayClipboard::capsMessage(true));
        CHECK(rc.active());
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::textCommand("hi")));
        CHECK(waitFor([&] { return sent.size() == 2; }));
        const QJsonObject clip = json(sent[1]);
        CHECK_EQ(clip.value("type").toString(), QString("clipboard"));
        CHECK_EQ(clip.value("text").toString(), QString("hi"));

        // It goes: caps false, and text stops.
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::availableCommand(false)));
        CHECK(waitFor([&] { return sent.size() == 3; }));
        CHECK_EQ(sent[2], RelayClipboard::capsMessage(false));
        CHECK(!rc.active());

        // Connected while available: caps true at once.
        remote.handleCommand(ClipboardIpc::decodeCommand(ClipboardIpc::availableCommand(true)));
        CHECK(waitFor([&] { return sent.size() == 4; }));
        now.clear();
        rc.clientConnected([&](const QByteArray& j) { now << j; });
        CHECK_EQ(now.size(), 1);
        CHECK_EQ(now[0], RelayClipboard::capsMessage(true));

        CHECK(json(RelayClipboard::capsMessage(false)).value("available").isBool());
        CHECK(!json(RelayClipboard::capsMessage(false)).value("available").toBool());
        CHECK_EQ(json(RelayClipboard::textMessage("a\"b")).value("text").toString(),
                 QString("a\"b"));
    }
}
