/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The host clipboard through a clipboard agent — the server's half
 * (docs/design/clipboard-agent.md):
 *  - the wire protocol, both ways, and what it refuses;
 *  - the registry: the admin's switch, persistence, and the connection state
 *    machine (its tokens are in test_clipboard_agent_tokens.cpp, a security
 *    test);
 *  - which session gets which clipboard (ClipboardRoute), every combination;
 *  - the worker ⇄ server lines (ClipboardIpc) and the router that joins
 *    workers to agents;
 *  - the agents' WebSocket server against a fake agent on loopback;
 *  - what the admin page is given (ClipboardAgentSetup). The upgrade gate is
 *    in test_request_guard.cpp, with the other security gates.
 */

#include "test_framework.h"

#include "clipboard/ClipboardAgentProtocol.h"
#include "clipboard/ClipboardEcho.h"
#include "clipboard/ClipboardIpc.h"
#include "clipboard/ClipboardRoute.h"
#include "server/ClipboardAgentRegistry.h"
#include "server/ClipboardAgentRouter.h"
#include "server/ClipboardAgentServer.h"
#include "server/ClipboardAgentSetup.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QVariant>
#include <QTemporaryDir>
#include <QThread>
#include <QWebSocket>

#include <functional>

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

/// A stand-in for the agents, for the router.
class FakeLink : public ClipboardAgentLink
{
public:
    bool available = false;
    QStringList calls;
    QString lastSetText;
    std::function<void(bool)> pendingDone;

    bool isAvailable(const QString&) const override { return available; }
    void setWatching(const QString& host, bool on) override
    {
        calls << QStringLiteral("watch:%1:%2").arg(host).arg(on);
    }
    void requestAnnounce(const QString& host) override { calls << "announce:" + host; }
    void sendSet(const QString& host, const QString& text, std::function<void(bool)> done) override
    {
        calls << "set:" + host;
        lastSetText = text;
        pendingDone = std::move(done);
    }
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

void run_clipboard_agent_server_tests()
{
    // ── Protocol ────────────────────────────────────────────────────────────
    SECTION("ClipboardAgent protocol — every message survives a round trip");
    {
        P::Message h = P::decode(P::hello("Debian GNU/Linux 13", "wl-clipboard", "1.0.0"));
        CHECK(h.type == P::Type::Hello);
        CHECK_EQ(h.version, P::kProtocolVersion);
        CHECK_EQ(h.platform, QString("Debian GNU/Linux 13"));
        CHECK_EQ(h.backend, QString("wl-clipboard"));
        CHECK_EQ(h.agentVersion, QString("1.0.0"));

        P::Message w = P::decode(P::welcome(true));
        CHECK(w.type == P::Type::Welcome);
        CHECK(w.watch);
        CHECK_EQ(w.version, P::kProtocolVersion);
        CHECK(!P::decode(P::welcome(false)).watch);

        CHECK(P::decode(P::watch(true)).watch);
        CHECK(!P::decode(P::watch(false)).watch);
        CHECK(P::decode(P::watch(false)).type == P::Type::Watch);
        CHECK(P::decode(P::announce()).type == P::Type::Announce);
        CHECK(P::decode(P::ping()).type == P::Type::Ping);
        CHECK(P::decode(P::pong()).type == P::Type::Pong);

        P::Message s = P::decode(P::set(42, QString::fromUtf8("h\xc3\xa9llo \xf0\x9f\x98\x80\n")));
        CHECK(s.type == P::Type::Set);
        CHECK_EQ(s.id, qint64(42));
        CHECK_EQ(s.text, QString::fromUtf8("h\xc3\xa9llo \xf0\x9f\x98\x80\n"));
        CHECK(!s.truncated);

        P::Message a = P::decode(P::ack(7, false));
        CHECK(a.type == P::Type::Ack);
        CHECK_EQ(a.id, qint64(7));
        CHECK(!a.ok);
        CHECK(P::decode(P::ack(7, true)).ok);

        P::Message c = P::decode(P::changed("copied"));
        CHECK(c.type == P::Type::Changed);
        CHECK_EQ(c.text, QString("copied"));

        CHECK_EQ(P::typeName(P::Type::Set), QString("set"));
        CHECK(P::typeName(P::Type::Invalid).isEmpty());
        CHECK(P::encode(P::Message{}).isEmpty());
        CHECK_EQ(P::path(), QString("/ws/clipboard-agent"));
    }

    SECTION("ClipboardAgent protocol — what it refuses");
    {
        auto bad = [](const char* frame) {
            const P::Message m = P::decode(QByteArray(frame));
            return m.type == P::Type::Invalid && !m.error.isEmpty();
        };
        CHECK(bad(""));
        CHECK(bad("not json"));
        CHECK(bad("[1,2]"));
        CHECK(bad("{}"));
        CHECK(bad(R"({"type":5})"));
        CHECK(bad(R"({"type":"steal"})"));
        CHECK(bad(R"({"type":"hello"})"));
        CHECK(bad(R"({"type":"hello","version":0})"));
        CHECK(bad(R"({"type":"hello","version":1.5})"));
        CHECK(bad(R"({"type":"hello","version":"1"})"));
        CHECK(bad(R"({"type":"welcome","version":-3})"));
        CHECK(bad(R"({"type":"watch"})"));
        CHECK(bad(R"({"type":"watch","on":"yes"})"));
        // set: positive integer id, non-empty text.
        CHECK(bad(R"({"type":"set","text":"a"})"));
        CHECK(bad(R"({"type":"set","id":0,"text":"a"})"));
        CHECK(bad(R"({"type":"set","id":-1,"text":"a"})"));
        CHECK(bad(R"({"type":"set","id":1.5,"text":"a"})"));
        CHECK(bad(R"({"type":"set","id":"1","text":"a"})"));
        CHECK(bad(R"({"type":"set","id":1e300,"text":"a"})"));
        CHECK(bad(R"({"type":"set","id":1})"));
        CHECK(bad(R"({"type":"set","id":1,"text":""})"));
        CHECK(bad(R"({"type":"set","id":1,"text":7})"));
        CHECK(bad(R"({"type":"ack"})"));
        CHECK(bad(R"({"type":"ack","id":3,"ok":"no"})"));
        CHECK(bad(R"({"type":"changed"})"));
        CHECK(bad(R"({"type":"changed","text":""})"));
        // An ack without ok is an older agent that only echoes the id.
        const P::Message a = P::decode(R"({"type":"ack","id":3})");
        CHECK(a.type == P::Type::Ack);
        CHECK(a.ok);
        // Unknown extra fields are ignored, not refused.
        CHECK(P::decode(R"({"type":"ping","extra":1})").type == P::Type::Ping);
        // The largest id JSON carries exactly.
        CHECK_EQ(P::decode(R"({"type":"ack","id":9007199254740992})").id,
                 qint64(9007199254740992LL));
        // A frame bigger than any legitimate one is not even parsed.
        QByteArray huge(P::kMaxFrameBytes + 1, 'a');
        CHECK(P::decode(huge).type == P::Type::Invalid);
        // The reason never carries the payload.
        const P::Message noText = P::decode(R"({"type":"set","id":1,"text":""})");
        CHECK(!noText.error.contains("secret"));
    }

    SECTION("ClipboardAgent protocol — text is capped, never split mid-character");
    {
        QString big(ClipboardMsg::kMaxTextChars + 10, QLatin1Char('x'));
        const P::Message s = P::decode(P::set(1, big));
        CHECK_EQ(s.text.size(), ClipboardMsg::kMaxTextChars);
        // The encoder caps already; a peer that sends more gets it cut here.
        QJsonObject raw{{"type", "changed"}, {"text", big}};
        const P::Message c = P::decode(QJsonDocument(raw).toJson(QJsonDocument::Compact));
        CHECK(c.truncated);
        CHECK_EQ(c.text.size(), ClipboardMsg::kMaxTextChars);
        // A surrogate pair straddling the cap is dropped whole.
        QString emoji = QString(ClipboardMsg::kMaxTextChars - 1, QLatin1Char('y')) +
                        QString::fromUtf8("\xf0\x9f\x98\x80");
        QJsonObject raw2{{"type", "changed"}, {"text", emoji}};
        const P::Message e = P::decode(QJsonDocument(raw2).toJson(QJsonDocument::Compact));
        CHECK_EQ(e.text.size(), ClipboardMsg::kMaxTextChars - 1);
        CHECK(!e.text.back().isHighSurrogate());
    }

    SECTION("ClipboardAgent protocol — identity labels are made safe to log");
    {
        CHECK_EQ(P::sanitizeLabel("KDE\n\x1b[31mred\t"), QString("KDE[31mred"));
        CHECK_EQ(P::sanitizeLabel(QString(200, 'a')).size(), P::kMaxLabelChars);
        CHECK_EQ(P::sanitizeLabel(QString::fromUtf8("caf\xc3\xa9")), QString("caf"));
        const P::Message h =
            P::decode(QJsonDocument(QJsonObject{{"type", "hello"},
                                                {"version", 1},
                                                {"platform", QString(500, 'p') + "\r\nX-Evil: 1"}})
                          .toJson(QJsonDocument::Compact));
        CHECK(h.type == P::Type::Hello);
        CHECK_EQ(h.platform.size(), P::kMaxLabelChars);
        CHECK(!h.platform.contains('\n'));
    }

    // ── Echo suppression ────────────────────────────────────────────────────
    SECTION("ClipboardEcho — our own write is not news, someone else's is");
    {
        ClipboardEcho e;
        CHECK(!e.shouldAnnounce(""));
        CHECK(e.shouldAnnounce("a"));
        CHECK(!e.shouldAnnounce("a")); // the same copy twice
        e.noteWritten("from browser");
        CHECK(!e.shouldAnnounce("from browser")); // the echo of our write
        CHECK(e.shouldAnnounce("typed on host"));
        CHECK(e.shouldAnnounce("from browser")); // copied again later: news
        e.forget();
        CHECK(e.shouldAnnounce("from browser")); // an announce resends it
        CHECK_EQ(e.last(), QString("from browser"));
        // Clearing the clipboard (or an image) is not announced and does not
        // reset what we remember.
        CHECK(!e.shouldAnnounce(""));
        CHECK(!e.shouldAnnounce("from browser"));
    }

    // ── Routing ─────────────────────────────────────────────────────────────
    SECTION("ClipboardRoute — every combination");
    {
        using ClipboardRoute::Route;
        int agentRoutes = 0;
        for (int bits = 0; bits < 32; ++bits) {
            ClipboardRoute::Inputs in;
            in.hostIsSelf = bits & 1;
            in.unrestricted = bits & 2;
            in.guest = bits & 4;
            in.agentEnabled = bits & 8;
            in.agentConnected = bits & 16;
            const Route r = ClipboardRoute::decide(in);
            Route expected = Route::None;
            if (in.unrestricted && in.hostIsSelf)
                expected = Route::Local;
            else if (in.unrestricted && !in.guest && in.agentEnabled && in.agentConnected)
                expected = Route::Agent;
            CHECK(r == expected);
            if (r == Route::Agent) ++agentRoutes;
            // A restricted session never reads the host clipboard, whatever else.
            if (!in.unrestricted) CHECK(r == Route::None);
            // A guest never goes through an agent.
            if (in.guest) CHECK(r != Route::Agent);
            // The local rule is unchanged: the agent never overrides it.
            if (in.hostIsSelf && in.unrestricted) CHECK(r == Route::Local);
        }
        CHECK_EQ(agentRoutes, 1);

        CHECK(ClipboardRoute::mayUseAgent(false, true, false));
        CHECK(!ClipboardRoute::mayUseAgent(true, true, false));
        CHECK(!ClipboardRoute::mayUseAgent(false, false, false));
        CHECK(!ClipboardRoute::mayUseAgent(false, true, true));

        CHECK(ClipboardRoute::workerProvider(true, true, true) == Route::Local);
        CHECK(ClipboardRoute::workerProvider(true, true, false) == Route::Local);
        CHECK(ClipboardRoute::workerProvider(false, true, true) == Route::Agent);
        CHECK(ClipboardRoute::workerProvider(false, true, false) == Route::None);
        for (int bits = 0; bits < 4; ++bits)
            CHECK(ClipboardRoute::workerProvider(bits & 1, false, bits & 2) == Route::None);
        CHECK_EQ(QString(ClipboardRoute::name(Route::Agent)), QString("agent"));
        CHECK_EQ(QString(ClipboardRoute::name(Route::Local)), QString("local"));
        CHECK_EQ(QString(ClipboardRoute::name(Route::None)), QString("none"));
    }

    // ── Worker IPC ──────────────────────────────────────────────────────────
    SECTION("ClipboardIpc — server → worker commands");
    {
        using K = ClipboardIpc::Command::Kind;
        auto c = ClipboardIpc::decodeCommand(ClipboardIpc::availableCommand(true));
        CHECK(c.kind == K::Available);
        CHECK(c.available);
        CHECK(!ClipboardIpc::decodeCommand(ClipboardIpc::availableCommand(false)).available);
        c = ClipboardIpc::decodeCommand(ClipboardIpc::textCommand("host text"));
        CHECK(c.kind == K::Text);
        CHECK_EQ(c.text, QString("host text"));
        c = ClipboardIpc::decodeCommand(ClipboardIpc::ackCommand(9, true));
        CHECK(c.kind == K::Ack);
        CHECK_EQ(c.id, qint64(9));
        CHECK(c.ok);
        CHECK(!ClipboardIpc::decodeCommand(ClipboardIpc::ackCommand(9, false)).ok);
        // On the wire it is one JSON line the worker's pump reads.
        CHECK_EQ(ClipboardIpc::availableCommand(true).value("cmd").toString(),
                 QString("clipboardAgent"));
        // Malformed or foreign commands are not clipboard commands.
        CHECK(ClipboardIpc::decodeCommand(QJsonObject{{"cmd", "quit"}}).kind == K::None);
        CHECK(ClipboardIpc::decodeCommand(QJsonObject{{"cmd", "clipboardAgent"}}).kind == K::None);
        CHECK(
            ClipboardIpc::decodeCommand(QJsonObject{{"cmd", "clipboardText"}, {"text", ""}}).kind ==
            K::None);
        CHECK(ClipboardIpc::decodeCommand(QJsonObject{{"cmd", "clipboardAck"}, {"id", 0}}).kind ==
              K::None);
        CHECK(ClipboardIpc::decodeCommand(QJsonObject{{"cmd", "clipboardAck"}, {"id", "3"}}).kind ==
              K::None);
        // Text over the cap is cut on the way in, too.
        c = ClipboardIpc::decodeCommand(QJsonObject{
            {"cmd", "clipboardText"}, {"text", QString(ClipboardMsg::kMaxTextChars + 5, 'z')}});
        CHECK_EQ(c.text.size(), ClipboardMsg::kMaxTextChars);
    }

    SECTION("ClipboardIpc — worker → server events");
    {
        using K = ClipboardIpc::Event::Kind;
        auto e = ClipboardIpc::decodeEvent(ClipboardIpc::setEvent(5, "browser text", true));
        CHECK(e.kind == K::Set);
        CHECK_EQ(e.id, qint64(5));
        CHECK_EQ(e.text, QString("browser text"));
        CHECK(e.paste);
        CHECK(!ClipboardIpc::decodeEvent(ClipboardIpc::setEvent(5, "x", false)).paste);
        CHECK(ClipboardIpc::decodeEvent(ClipboardIpc::announceEvent()).kind == K::Announce);
        CHECK(ClipboardIpc::isEvent("clipboardSet"));
        CHECK(ClipboardIpc::isEvent("clipboardAnnounce"));
        CHECK(!ClipboardIpc::isEvent("response"));
        CHECK(!ClipboardIpc::isEvent("clipboard"));
        CHECK(ClipboardIpc::decodeEvent(QJsonObject{{"event", "ended"}}).kind == K::None);
        CHECK(
            ClipboardIpc::decodeEvent(QJsonObject{{"event", "clipboardSet"}, {"text", "x"}}).kind ==
            K::None);
        CHECK_EQ(ClipboardIpc::setEvent(1, "x", false).value("event").toString(),
                 QString("clipboardSet"));
    }

    // ── Registry ────────────────────────────────────────────────────────────
    SECTION("ClipboardAgentRegistry — state machine and availability");
    {
        using S = ClipboardAgentRegistry::State;
        ClipboardAgentRegistry reg{QString()};
        Spy avail;
        spyOn(avail, &reg, &ClipboardAgentRegistry::availabilityChanged);
        CHECK(reg.state("H") == S::Disconnected);
        CHECK(!reg.isAvailable("H"));
        CHECK(!reg.isEnabled("H"));

        reg.markConnected("H", 10);
        CHECK(reg.state("H") == S::Connected);
        CHECK(!reg.isAvailable("H")); // no hello yet, and not enabled
        reg.markReady("H", "Linux", "qt", "1.0", 11);
        CHECK(reg.state("H") == S::Ready);
        CHECK(!reg.isAvailable("H")); // the admin has not turned it on
        CHECK_EQ(avail.count(), 0);
        CHECK(reg.statusJson("H").value("connected").toBool());
        CHECK(!reg.statusJson("H").value("available").toBool());

        reg.setEnabled("H", true);
        CHECK(reg.isAvailable("H"));
        CHECK_EQ(avail.count(), 1);
        CHECK(avail.last().at(1).toBool());
        reg.setEnabled("H", true); // no change, no signal
        CHECK_EQ(avail.count(), 1);

        reg.markDisconnected("H", 20);
        CHECK(reg.state("H") == S::Disconnected);
        CHECK(!reg.isAvailable("H"));
        CHECK_EQ(avail.count(), 2);
        CHECK(!avail.last().at(1).toBool());
        CHECK_EQ(reg.statusJson("H").value("last_seen").toDouble(), 20.0);
        reg.markDisconnected("H", 30); // already: nothing
        CHECK_EQ(avail.count(), 2);
        CHECK_EQ(reg.statusJson("H").value("last_seen").toDouble(), 20.0);

        // Connected but not yet Ready is not available even when enabled.
        reg.markConnected("H", 40);
        CHECK(!reg.isAvailable("H"));
        CHECK_EQ(avail.count(), 2);
        reg.markReady("H", "Linux", "wl-clipboard", "1.0", 41);
        CHECK(reg.isAvailable("H"));
        CHECK_EQ(avail.count(), 3);
        CHECK_EQ(reg.statusJson("H").value("backend").toString(), QString("wl-clipboard"));

        // Disabling a ready host takes it away at once.
        reg.setEnabled("H", false);
        CHECK(!reg.isAvailable("H"));
        CHECK_EQ(avail.count(), 4);

        // Forgetting an available host says so.
        reg.setEnabled("H", true);
        CHECK_EQ(avail.count(), 5);
        reg.forgetHost("H");
        CHECK_EQ(avail.count(), 6);
        CHECK(!reg.isAvailable("H"));

        // The status never carries the token or its hash.
        const QString t = reg.issueToken("Z", 1);
        const QByteArray status = QJsonDocument(reg.statusJson("Z")).toJson();
        CHECK(!status.contains(t.toLatin1()));
        CHECK(!status.contains(ClipboardAgentRegistry::hashToken(t).toLatin1()));
        CHECK(reg.statusJson("Z").value("has_token").toBool());
        CHECK(!reg.statusJson("nobody").value("has_token").toBool());
    }

    SECTION("ClipboardAgentRegistry — what survives a restart, and what does not");
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("sub/clipboard-agents.json");
        QString token;
        {
            ClipboardAgentRegistry reg(path);
            token = reg.issueToken("H", 5);
            reg.setEnabled("H", true);
            reg.markConnected("H", 6);
            reg.markReady("H", "Linux", "qt", "1.0", 7);
        }
        ClipboardAgentRegistry again(path);
        CHECK_EQ(again.hostForToken(token), QString("H"));
        CHECK(again.isEnabled("H"));
        // A connection never outlives the process.
        CHECK(again.state("H") == ClipboardAgentRegistry::State::Disconnected);
        CHECK(!again.isAvailable("H"));
        CHECK_EQ(again.statusJson("H").value("last_seen").toDouble(), 7.0);
        CHECK_EQ(again.statusJson("H").value("token_created").toDouble(), 5.0);

        // A damaged file loads as empty rather than crashing.
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{not json");
        f.close();
        ClipboardAgentRegistry broken(path);
        CHECK(!broken.hasToken("H"));
    }

    // ── Router ──────────────────────────────────────────────────────────────
    SECTION("ClipboardAgentRouter — attach, watch, broadcast, detach");
    {
        FakeLink link;
        ClipboardAgentRouter router(link);
        QList<QJsonObject> w1, w2, w3;
        link.available = true;
        const int h1 = router.attach("H", [&](const QJsonObject& c) { w1 << c; });
        CHECK_EQ(w1.size(), 1);
        CHECK(w1[0] == ClipboardIpc::availableCommand(true));
        CHECK(link.calls == QStringList{"watch:H:1"});
        const int h2 = router.attach("H", [&](const QJsonObject& c) { w2 << c; });
        CHECK(link.calls == QStringList{"watch:H:1"}); // already watching
        const int h3 = router.attach("G", [&](const QJsonObject& c) { w3 << c; });
        CHECK(link.calls.last() == "watch:G:1");
        CHECK_EQ(router.attachedCount("H"), 2);
        CHECK(h1 != h2 && h2 != h3);

        // Host text goes to every worker on that host, and only there.
        router.onAgentText("H", "copied on H");
        CHECK(w1.last() == ClipboardIpc::textCommand("copied on H"));
        CHECK(w2.last() == ClipboardIpc::textCommand("copied on H"));
        CHECK_EQ(w3.size(), 1);
        router.onAgentText("H", ""); // nothing to say
        CHECK_EQ(w1.size(), 2);

        // Availability likewise.
        router.onAvailabilityChanged("G", false);
        CHECK(w3.last() == ClipboardIpc::availableCommand(false));
        CHECK_EQ(w1.size(), 2);

        // A browser connected: the agent is asked to announce.
        router.onWorkerEvent(h2, ClipboardIpc::announceEvent());
        CHECK(link.calls.last() == "announce:H");

        // A write: to the agent, and its answer back to that worker only.
        router.onWorkerEvent(h1, ClipboardIpc::setEvent(77, "from browser", true));
        CHECK(link.calls.last() == "set:H");
        CHECK_EQ(link.lastSetText, QString("from browser"));
        link.pendingDone(true);
        CHECK(w1.last() == ClipboardIpc::ackCommand(77, true));
        CHECK_EQ(w2.size(), 2);

        // Answer arriving after the worker left: dropped, no crash.
        router.onWorkerEvent(h2, ClipboardIpc::setEvent(78, "late", false));
        router.detach(h2);
        link.pendingDone(false);
        CHECK_EQ(w2.size(), 2);
        CHECK(link.calls.last() == "set:H"); // h1 still holds H: no unwatch

        // Garbage and unknown handles are ignored.
        const int before = link.calls.size();
        router.onWorkerEvent(h1, QJsonObject{{"event", "nonsense"}});
        router.onWorkerEvent(9999, ClipboardIpc::announceEvent());
        CHECK_EQ(link.calls.size(), before);

        // The last worker on a host leaving stops the watch.
        router.detach(h1);
        CHECK(link.calls.last() == "watch:H:0");
        router.detach(h1); // twice: harmless
        CHECK(link.calls.last() == "watch:H:0");
        router.detach(h3);
        CHECK(link.calls.last() == "watch:G:0");
        CHECK_EQ(router.attachedCount("H"), 0);

        // A worker attaching while the agent is away starts without it.
        link.available = false;
        QList<QJsonObject> w4;
        router.attach("H", [&](const QJsonObject& c) { w4 << c; });
        CHECK(w4[0] == ClipboardIpc::availableCommand(false));
        CHECK(!router.isAvailable("H"));
    }

    // ── Agent server, against a raw fake agent ──────────────────────────────
    SECTION("ClipboardAgentServer — bearer token parsing");
    {
        CHECK_EQ(ClipboardAgentServer::bearerToken("Bearer abc"), QString("abc"));
        CHECK_EQ(ClipboardAgentServer::bearerToken("bearer   abc  "), QString("abc"));
        CHECK(ClipboardAgentServer::bearerToken("Basic abc").isEmpty());
        CHECK(ClipboardAgentServer::bearerToken("Bearer").isEmpty());
        CHECK(ClipboardAgentServer::bearerToken("Bearer ").isEmpty());
        CHECK(ClipboardAgentServer::bearerToken("").isEmpty());
    }

    SECTION("ClipboardAgentServer — authentication, hello, welcome, watch");
    {
        ClipboardAgentRegistry reg{QString()};
        ClipboardAgentServer server(reg);
        CHECK(server.listen());
        CHECK(server.port() != 0);
        const QString token = reg.issueToken("H", 1);
        reg.setEnabled("H", true);

        // No token, a wrong token: closed, nothing marked.
        {
            RawAgent none;
            none.open(server.port(), QString());
            CHECK(waitFor([&] { return none.closed; }));
            RawAgent wrong;
            wrong.open(server.port(), "mwca_wrong");
            CHECK(waitFor([&] { return wrong.closed; }));
            CHECK(reg.state("H") == ClipboardAgentRegistry::State::Disconnected);
        }

        RawAgent agent;
        CHECK(agent.open(server.port(), token));
        CHECK(waitFor([&] { return reg.state("H") == ClipboardAgentRegistry::State::Connected; }));
        CHECK(!reg.isAvailable("H"));
        // Nothing but hello is accepted first.
        agent.send(P::hello("Linux", "qt", "1.0"));
        const QJsonObject welcome = agent.next("welcome");
        CHECK_EQ(welcome.value("version").toInt(), P::kProtocolVersion);
        CHECK(!welcome.value("watch").toBool()); // no session yet
        CHECK(waitFor([&] { return reg.isAvailable("H"); }));
        CHECK_EQ(reg.statusJson("H").value("platform").toString(), QString("Linux"));

        // A change while nobody watches is dropped.
        Spy text;
        spyOn(text, &server, &ClipboardAgentServer::agentText);
        agent.send(P::changed("unasked"));
        pump(50);
        CHECK_EQ(text.count(), 0);

        // The router asks for a watch: the agent hears it, its changes pass.
        server.setWatching("H", true);
        CHECK(server.wantsWatch("H"));
        CHECK(agent.next("watch").value("on").toBool());
        agent.send(P::changed("copied"));
        CHECK(waitFor([&] { return text.count() == 1; }));
        CHECK_EQ(text.last().at(0).toString(), QString("H"));
        CHECK_EQ(text.last().at(1).toString(), QString("copied"));

        // The admin turns it off: the agent stops watching, changes stop.
        reg.setEnabled("H", false);
        CHECK(!agent.next("watch").value("on").toBool());
        CHECK(!server.wantsWatch("H"));
        agent.send(P::changed("after disable"));
        pump(50);
        CHECK_EQ(text.count(), 1);
        server.requestAnnounce("H");
        pump(50);
        CHECK(!agent.hasFrame("announce"));
        reg.setEnabled("H", true);
        CHECK(agent.next("watch").value("on").toBool());

        server.requestAnnounce("H");
        CHECK(!agent.next("announce").isEmpty());

        // Ping is answered.
        agent.send(P::ping());
        CHECK(!agent.next("pong").isEmpty());

        server.setWatching("H", false);
        CHECK(!agent.next("watch").value("on").toBool());
        server.setWatching("H", false); // no change: no frame
        pump(30);
        CHECK(!agent.hasFrame("watch"));
    }

    SECTION("ClipboardAgentServer — set, ack, refusal, timeout");
    {
        ClipboardAgentRegistry reg{QString()};
        ClipboardAgentServer server(reg);
        server.setAckTimeoutMs(150);
        CHECK(server.listen());
        const QString token = reg.issueToken("H", 1);

        // No agent at all: false at once.
        bool called = false, result = true;
        server.sendSet("H", "x", [&](bool ok) {
            called = true;
            result = ok;
        });
        CHECK(called);
        CHECK(!result);

        RawAgent agent;
        agent.open(server.port(), token);
        agent.send(P::hello("Linux", "qt", "1.0"));
        agent.next("welcome");
        // Connected but disabled: still refused.
        called = false;
        server.sendSet("H", "x", [&](bool ok) {
            called = true;
            result = ok;
        });
        CHECK(called && !result);

        reg.setEnabled("H", true);
        // Empty text is never sent.
        called = false;
        server.sendSet("H", "", [&](bool ok) {
            called = true;
            result = ok;
        });
        CHECK(called && !result);

        // Acknowledged.
        int answers = 0;
        bool last = false;
        server.sendSet("H", "from browser", [&](bool ok) {
            ++answers;
            last = ok;
        });
        const QJsonObject set = agent.next("set");
        CHECK_EQ(set.value("text").toString(), QString("from browser"));
        const qint64 id = static_cast<qint64>(set.value("id").toDouble());
        CHECK(id > 0);
        agent.send(P::ack(id, true));
        CHECK(waitFor([&] { return answers == 1; }));
        CHECK(last);
        // A second ack for the same id changes nothing.
        agent.send(P::ack(id, false));
        pump(30);
        CHECK_EQ(answers, 1);

        // Refused by the agent's clipboard.
        server.sendSet("H", "y", [&](bool ok) {
            ++answers;
            last = ok;
        });
        const qint64 id2 = static_cast<qint64>(agent.next("set").value("id").toDouble());
        CHECK(id2 > id);
        agent.send(P::ack(id2, false));
        CHECK(waitFor([&] { return answers == 2; }));
        CHECK(!last);

        // Never answered: false after the timeout, exactly once.
        server.sendSet("H", "z", [&](bool ok) {
            ++answers;
            last = ok;
        });
        agent.next("set");
        CHECK(waitFor([&] { return answers == 3; }, 1000));
        CHECK(!last);
        pump(200);
        CHECK_EQ(answers, 3);

        // Pending writes fail when the agent goes.
        server.sendSet("H", "w", [&](bool ok) {
            ++answers;
            last = ok;
        });
        agent.next("set");
        agent.ws.close();
        CHECK(waitFor([&] { return answers == 4; }));
        CHECK(!last);
        CHECK(waitFor([&] { return !reg.isAvailable("H"); }));
    }

    SECTION("ClipboardAgentServer — protocol errors, replacement, revocation");
    {
        ClipboardAgentRegistry reg{QString()};
        ClipboardAgentServer server(reg);
        server.setHelloTimeoutMs(150);
        CHECK(server.listen());
        const QString token = reg.issueToken("H", 1);
        reg.setEnabled("H", true);

        // Something other than hello first.
        {
            RawAgent a;
            a.open(server.port(), token);
            a.send(P::changed("x"));
            CHECK(waitFor([&] { return a.closed; }));
            CHECK_EQ(a.closeCode, int(P::CloseProtocolError));
        }
        // Garbage.
        {
            RawAgent a;
            a.open(server.port(), token);
            a.send("{{{");
            CHECK(waitFor([&] { return a.closed; }));
            CHECK_EQ(a.closeCode, int(P::CloseProtocolError));
        }
        // Binary frames.
        {
            RawAgent a;
            a.open(server.port(), token);
            a.ws.sendBinaryMessage("bin");
            CHECK(waitFor([&] { return a.closed; }));
        }
        // Silence: no hello in time.
        {
            RawAgent a;
            a.open(server.port(), token);
            CHECK(waitFor([&] { return a.closed; }, 2000));
            CHECK_EQ(a.closeCode, int(P::CloseProtocolError));
        }
        // Another version.
        {
            RawAgent a;
            a.open(server.port(), token);
            a.send(R"({"type":"hello","version":99})");
            CHECK(waitFor([&] { return a.closed; }));
            CHECK_EQ(a.closeCode, int(P::CloseVersion));
            CHECK(!reg.isAvailable("H"));
        }
        // Messages only the server sends are refused from an agent.
        {
            RawAgent a;
            a.open(server.port(), token);
            a.send(P::hello("L", "qt", "1"));
            a.next("welcome");
            a.send(P::set(1, "x"));
            CHECK(waitFor([&] { return a.closed; }));
            CHECK_EQ(a.closeCode, int(P::CloseProtocolError));
        }

        // A second agent for the same host replaces the first.
        RawAgent first;
        first.open(server.port(), token);
        first.send(P::hello("L", "qt", "1"));
        first.next("welcome");
        CHECK(waitFor([&] { return reg.isAvailable("H"); }));
        RawAgent second;
        second.open(server.port(), token);
        CHECK(waitFor([&] { return first.closed; }));
        CHECK_EQ(first.closeCode, int(P::CloseReplaced));
        second.send(P::hello("L", "qt", "1"));
        second.next("welcome");
        CHECK(waitFor([&] { return reg.isAvailable("H"); }));
        // The replaced socket's late goodbye does not mark the host gone.
        pump(50);
        CHECK(reg.isAvailable("H"));

        // Rotating the token drops the live agent.
        reg.issueToken("H", 2);
        CHECK(waitFor([&] { return second.closed; }));
        CHECK_EQ(second.closeCode, int(P::CloseRevoked));
        CHECK(!reg.isAvailable("H"));

        // So does revoking it.
        const QString t3 = reg.issueToken("H", 3);
        RawAgent third;
        third.open(server.port(), t3);
        third.send(P::hello("L", "qt", "1"));
        third.next("welcome");
        reg.revokeToken("H");
        CHECK(waitFor([&] { return third.closed; }));
        CHECK_EQ(third.closeCode, int(P::CloseRevoked));
    }

    SECTION("ClipboardAgentServer — a watch asked before the agent connects");
    {
        ClipboardAgentRegistry reg{QString()};
        ClipboardAgentServer server(reg);
        CHECK(server.listen());
        const QString token = reg.issueToken("H", 1);
        reg.setEnabled("H", true);
        server.setWatching("H", true);
        RawAgent agent;
        agent.open(server.port(), token);
        agent.send(P::hello("L", "qt", "1"));
        CHECK(agent.next("welcome").value("watch").toBool());
        pump(30);
        CHECK(!agent.hasFrame("watch")); // the welcome said it all
        // Disabled host: the welcome says not to watch.
        ClipboardAgentRegistry reg2{QString()};
        ClipboardAgentServer server2(reg2);
        CHECK(server2.listen());
        const QString t2 = reg2.issueToken("H", 1);
        server2.setWatching("H", true);
        RawAgent a2;
        a2.open(server2.port(), t2);
        a2.send(P::hello("L", "qt", "1"));
        CHECK(!a2.next("welcome").value("watch").toBool());
    }

    // ── What the admin page is given ────────────────────────────────────────
    SECTION("ClipboardAgentSetup — the server URL the agent should dial");
    {
        using ClipboardAgentSetup::serverUrl;
        const QList<QHostAddress> lan{QHostAddress("::1"), QHostAddress("192.0.2.10")};
        CHECK_EQ(serverUrl("192.0.2.5:8443", 443, lan), QString("https://192.0.2.5:443"));
        CHECK_EQ(serverUrl("mw.example.com", 44729, lan), QString("https://mw.example.com:44729"));
        CHECK_EQ(serverUrl("mw.example.com:80", 443, lan), QString("https://mw.example.com:443"));
        CHECK_EQ(serverUrl("[2001:db8::5]:443", 443, lan), QString("https://[2001:db8::5]:443"));
        // The admin on the server itself: the LAN address stands in.
        CHECK_EQ(serverUrl("localhost:47990", 443, lan), QString("https://192.0.2.10:443"));
        CHECK_EQ(serverUrl("127.0.0.1", 443, lan), QString("https://192.0.2.10:443"));
        CHECK_EQ(serverUrl("[::1]:443", 443, lan), QString("https://192.0.2.10:443"));
        CHECK_EQ(serverUrl("", 443, lan), QString("https://192.0.2.10:443"));
        CHECK_EQ(serverUrl("localhost", 443, {}), QString("https://localhost:443"));
    }

    SECTION("ClipboardAgentSetup — command lines, quoting, and the token's place");
    {
        using ClipboardAgentSetup::shellQuote;
        CHECK_EQ(shellQuote("abc"), QString("'abc'"));
        CHECK_EQ(shellQuote("a'b"), QString("'a'\\''b'"));
        CHECK_EQ(shellQuote(""), QString("''"));

        const QString token = "mwca_" + QString(43, 'A');
        const QJsonObject d = ClipboardAgentSetup::describe(token, "https://192.0.2.10:443",
                                                            {"sha256/AAA=", "sha256/BBB="});
        CHECK_EQ(d.value("token").toString(), token);
        CHECK_EQ(d.value("server").toString(), QString("https://192.0.2.10:443"));
        CHECK_EQ(d.value("pins").toArray().size(), 2);
        const QString cmd = d.value("command").toString();
        CHECK(cmd.startsWith("moonlightweb-clipboard-agent --server 'https://192.0.2.10:443'"));
        CHECK(cmd.contains("--pin 'sha256/AAA=,sha256/BBB='"));
        // The token is never an argument: it would be in the process list.
        CHECK(!cmd.contains(token));
        const QJsonArray args = d.value("args").toArray();
        for (const QJsonValue& a : args)
            CHECK(!a.toString().contains(token));
        CHECK_EQ(args.size(), 4);
        // The setup line stores it owner-only.
        const QString setup = d.value("setup").toString();
        CHECK(setup.contains("umask 077"));
        CHECK(setup.contains("'" + token + "'"));
        CHECK(setup.contains(ClipboardAgentSetup::defaultTokenFile()));
        // The settings file the agent reads by itself, written by the setup line.
        const QString settings = d.value("settings").toString();
        CHECK_EQ(settings, QString("MW_CLIPBOARD_AGENT_SERVER=https://192.0.2.10:443\n"
                                   "MW_CLIPBOARD_AGENT_PIN=sha256/AAA=,sha256/BBB=\n"));
        CHECK(!settings.contains(token));
        CHECK(setup.contains(ClipboardAgentSetup::defaultSettingsFile()));
        CHECK_EQ(d.value("settings_file").toString(), ClipboardAgentSetup::defaultSettingsFile());
        // One line, so a paste cannot be cut in pieces by the terminal.
        CHECK(!setup.contains(QLatin1Char('\n')));
#ifdef Q_OS_UNIX
        // And it does what it says, in a real shell: both files, owner-only,
        // with the newlines printf writes.
        {
            QTemporaryDir home;
            QProcess sh;
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("HOME", home.path());
            sh.setProcessEnvironment(env);
            sh.start("/bin/sh", {"-c", setup});
            CHECK(sh.waitForFinished(5000));
            CHECK_EQ(sh.exitCode(), 0);
            QFile tf(home.filePath(".config/moonlightweb/clipboard-agent.token"));
            QFile sf(home.filePath(".config/moonlightweb/clipboard-agent.env"));
            CHECK(tf.open(QIODevice::ReadOnly));
            CHECK(sf.open(QIODevice::ReadOnly));
            CHECK_EQ(QString::fromUtf8(tf.readAll()), token + "\n");
            CHECK_EQ(QString::fromUtf8(sf.readAll()), settings);
            const auto loose = QFileDevice::ReadGroup | QFileDevice::ReadOther |
                               QFileDevice::WriteGroup | QFileDevice::WriteOther;
            CHECK_EQ(tf.permissions() & loose, QFileDevice::Permissions());
            CHECK_EQ(sf.permissions() & loose, QFileDevice::Permissions());
        }
#endif
        // No pins: no --pin.
        const QJsonObject n = ClipboardAgentSetup::describe(token, "https://x:1", {});
        CHECK(!n.value("command").toString().contains("--pin"));
        CHECK(!n.value("settings").toString().contains("PIN"));
        CHECK_EQ(n.value("args").toArray().size(), 2);
    }
}
