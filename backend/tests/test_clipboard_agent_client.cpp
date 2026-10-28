/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The clipboard agent — the half that runs on a streamed host
 * (backend/clipboard-agent, docs/design/clipboard-agent.md):
 *  - its configuration: server address, key pins, token file;
 *  - which clipboard backend it picks, over made-up environments;
 *  - the key pin, checked against the server's real local certificate, and
 *    why it pins the key (it survives the certificate's re-issue);
 *  - the reconnect backoff;
 *  - AgentClient against the real ClipboardAgentServer on loopback, with a
 *    fake clipboard: watch, announce, echo suppression, writes and acks,
 *    reconnects, and the cases where it must give up;
 *  - the pin over real TLS: the token never reaches a server with another key;
 *  - the wl-clipboard backend against stand-in wl-paste / wl-copy scripts.
 */

#include "test_framework.h"

#include "AgentClient.h"
#include "AgentConfig.h"
#include "BackendSelect.h"
#include "ClipboardBackend.h"
#include "PinCheck.h"
#include "WlClipboardBackend.h"
#include "clipboard/ClipboardAgentProtocol.h"
#include "server/CertManager.h"
#include "server/ClipboardAgentRegistry.h"
#include "server/ClipboardAgentServer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSslKey>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QWebSocket>
#include <QWebSocketServer>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <functional>
#include <memory>

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

/// A clipboard in memory. A write comes back as a change while watching,
/// the way every real backend reports its own writes.
class FakeBackend : public ClipboardBackend
{
public:
    QString clip;
    bool watching = false;
    int reads = 0;
    QStringList writes;
    bool writeOk = true;

    QString name() const override { return QStringLiteral("fake"); }
    void start() override { emit ready(); }
    void setWatching(bool on) override { watching = on; }
    void read() override
    {
        ++reads;
        emit observed(clip);
    }
    void write(const QString& text, std::function<void(bool)> done) override
    {
        writes << text;
        if (writeOk) {
            clip = text;
            if (watching) emit observed(text);
        }
        done(writeOk);
    }
    /// Someone on the host copies @p text.
    void userCopies(const QString& text)
    {
        clip = text;
        if (watching) emit observed(text);
    }
};

struct Texts
{
    QStringList list;
};

AgentConfig configFor(quint16 port, const QString& token)
{
    AgentConfig c;
    c.url = QUrl(QStringLiteral("ws://127.0.0.1:%1/ws/clipboard-agent").arg(port));
    c.token = token;
    return c;
}

quint16 freePort()
{
    QTcpServer s;
    s.listen(QHostAddress::LocalHost, 0);
    const quint16 p = s.serverPort();
    s.close();
    return p;
}

/// SPKI SHA-256 of @p cert computed by OpenSSL itself, to check Qt's bytes.
QByteArray opensslSpkiSha256(const QSslCertificate& cert)
{
    const QByteArray pem = cert.toPem();
    BIO* bio = BIO_new_mem_buf(pem.constData(), static_cast<int>(pem.size()));
    X509* x = bio ? PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr;
    QByteArray out;
    if (x) {
        unsigned char* der = nullptr;
        const int len = i2d_PUBKEY(X509_get0_pubkey(x), &der);
        if (len > 0)
            out = QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(der), len),
                                           QCryptographicHash::Sha256);
        OPENSSL_free(der);
        X509_free(x);
    }
    if (bio) BIO_free(bio);
    return out;
}

} // namespace

void run_clipboard_agent_client_tests()
{
    using namespace AgentConfigParse;

    // ── Configuration ───────────────────────────────────────────────────────
    SECTION("Agent config — the server address becomes the agent endpoint");
    {
        QString err;
        CHECK_EQ(agentUrl("https://192.0.2.10:44729", &err).toString(),
                 QString("wss://192.0.2.10:44729/ws/clipboard-agent"));
        CHECK_EQ(agentUrl("wss://mw.example.com", &err).toString(),
                 QString("wss://mw.example.com/ws/clipboard-agent"));
        CHECK_EQ(agentUrl("192.0.2.10:443", &err).toString(),
                 QString("wss://192.0.2.10:443/ws/clipboard-agent"));
        CHECK_EQ(agentUrl("  https://mw.example.com/some/path?q=1  ", &err).toString(),
                 QString("wss://mw.example.com/ws/clipboard-agent"));
        CHECK_EQ(agentUrl("https://[2001:db8::1]:443", &err).host(), QString("2001:db8::1"));
        // Plain text only to this machine.
        CHECK_EQ(agentUrl("http://127.0.0.1:8080", &err).toString(),
                 QString("ws://127.0.0.1:8080/ws/clipboard-agent"));
        CHECK_EQ(agentUrl("ws://localhost:1", &err).scheme(), QString("ws"));
        err.clear();
        CHECK(!agentUrl("http://192.0.2.10", &err).isValid());
        CHECK(err.contains("clear"));
        err.clear();
        CHECK(!agentUrl("ftp://192.0.2.10", &err).isValid());
        CHECK(!err.isEmpty());
        err.clear();
        CHECK(!agentUrl("", &err).isValid());
        CHECK(!err.isEmpty());
        err.clear();
        CHECK(!agentUrl("https://user:pw@192.0.2.10", &err).isValid());
        CHECK(!err.isEmpty());
    }

    SECTION("Agent config — key pins");
    {
        QList<QByteArray> pins;
        QString err;
        const QByteArray d1(32, '\x01'), d2(32, '\x02');
        CHECK(parsePins("sha256/" + d1.toBase64(), &pins, &err));
        CHECK_EQ(pins.size(), 1);
        CHECK(pins[0] == d1);
        CHECK(parsePins("sha256/" + d1.toBase64() + ", " + d2.toBase64(), &pins, &err));
        CHECK_EQ(pins.size(), 2);
        CHECK(pins[1] == d2);
        CHECK(parsePins("sha256//" + d1.toBase64(), &pins, &err)); // curl's spelling
        CHECK_EQ(pins.size(), 1);
        CHECK(parsePins("", &pins, &err));
        CHECK(pins.isEmpty());
        CHECK(!parsePins("sha256/abc", &pins, &err));
        CHECK(pins.isEmpty());
        CHECK(!err.isEmpty());
        CHECK(!parsePins("sha256/" + QByteArray(20, 'x').toBase64(), &pins, &err)); // SHA-1 size
        CHECK(!parsePins("sha256/!!!notbase64!!!", &pins, &err));
        // One bad pin spoils the list: never half a configuration.
        CHECK(!parsePins(d1.toBase64() + ",bad", &pins, &err));
        CHECK(pins.isEmpty());
    }

    SECTION("Agent config — token, file, environment, backend");
    {
        const QString token = "mwca_" + QString(43, 'a');
        CHECK(looksLikeToken(token));
        CHECK(!looksLikeToken("mwca_short"));
        CHECK(!looksLikeToken(token + "x"));
        CHECK(!looksLikeToken("xxxx_" + QString(43, 'a')));
        CHECK(!looksLikeToken("mwca_" + QString(42, 'a') + "!"));
        CHECK_EQ(tokenFromFile("\n\n  " + token.toLatin1() + "  \r\nsecond\n"), token);
        CHECK(tokenFromFile("").isEmpty());
        CHECK(tokenFromFile("\n \n").isEmpty());

        Inputs in;
        in.server = "https://192.0.2.10:443";
        in.tokenFile = token.toLatin1() + "\n";
        in.tokenFileRead = true;
        AgentConfig c;
        QString err;
        CHECK(resolve(in, &c, &err));
        CHECK_EQ(c.token, token);
        CHECK_EQ(c.backend, QString("auto"));
        CHECK(c.pins.isEmpty());
        CHECK_EQ(c.url.scheme(), QString("wss"));

        // The environment wins over the file.
        const QString envToken = "mwca_" + QString(43, 'b');
        in.envToken = envToken;
        CHECK(resolve(in, &c, &err));
        CHECK_EQ(c.token, envToken);

        // No token at all.
        Inputs none;
        none.server = "https://192.0.2.10";
        CHECK(!resolve(none, &c, &err));
        CHECK(err.contains("token"));
        // A malformed token.
        none.envToken = "hunter2";
        CHECK(!resolve(none, &c, &err));
        // An unknown backend; a known one.
        Inputs b = in;
        b.backend = "magic";
        CHECK(!resolve(b, &c, &err));
        CHECK(err.contains("magic"));
        b.backend = "WL-Clipboard";
        CHECK(resolve(b, &c, &err));
        CHECK_EQ(c.backend, QString("wl-clipboard"));
        // Bad pins, bad server.
        Inputs p = in;
        p.pins = "nope";
        CHECK(!resolve(p, &c, &err));
        p = in;
        p.server = "http://192.0.2.10";
        CHECK(!resolve(p, &c, &err));
    }

    SECTION("Agent config — the settings file");
    {
        const QMap<QString, QString> m =
            parseEnvFile("# from the admin page\n"
                         "MW_CLIPBOARD_AGENT_SERVER=https://192.0.2.10:443\n"
                         "\n"
                         "  MW_CLIPBOARD_AGENT_PIN = 'sha256/AAA=' \r\n"
                         "MW_CLIPBOARD_AGENT_BACKEND=\"qt\"\n"
                         "=nokey\n"
                         "garbage line\n");
        CHECK_EQ(m.value("MW_CLIPBOARD_AGENT_SERVER"), QString("https://192.0.2.10:443"));
        CHECK_EQ(m.value("MW_CLIPBOARD_AGENT_PIN"), QString("sha256/AAA="));
        CHECK_EQ(m.value("MW_CLIPBOARD_AGENT_BACKEND"), QString("qt"));
        CHECK_EQ(m.size(), 3);
        CHECK(parseEnvFile("").isEmpty());
        // A value may hold '=' (base64 padding).
        CHECK_EQ(parseEnvFile("K=a=b=\n").value("K"), QString("a=b="));
    }

    // ── Backend selection ───────────────────────────────────────────────────
    SECTION("Agent backends — chosen from the session, best first");
    {
        using BackendSelect::Backend;
        using BackendSelect::candidates;
        BackendSelect::Environment kde;
        kde.os = "linux";
        kde.wayland = kde.x11 = kde.wlPaste = kde.wlCopy = kde.kde = kde.dbusBuilt = true;
        // KDE Wayland with wl-clipboard: data-control first, Klipper, then XWayland.
        CHECK(candidates(kde) ==
              QList<Backend>({Backend::WlClipboard, Backend::Klipper, Backend::QtXWayland}));
        // Without wl-clipboard: Klipper.
        auto noWl = kde;
        noWl.wlPaste = false;
        CHECK(candidates(noWl) == QList<Backend>({Backend::Klipper, Backend::QtXWayland}));
        // wl-copy alone is not enough.
        noWl = kde;
        noWl.wlCopy = false;
        CHECK(candidates(noWl).first() == Backend::Klipper);
        // Built without D-Bus: no Klipper.
        auto noDbus = kde;
        noDbus.dbusBuilt = false;
        CHECK(candidates(noDbus) == QList<Backend>({Backend::WlClipboard, Backend::QtXWayland}));
        // A wlroots compositor, no XWayland.
        BackendSelect::Environment sway;
        sway.os = "linux";
        sway.wayland = sway.wlPaste = sway.wlCopy = true;
        CHECK(candidates(sway) == QList<Backend>({Backend::WlClipboard}));
        // GNOME Wayland without wl-clipboard but with XWayland.
        BackendSelect::Environment gnome;
        gnome.os = "linux";
        gnome.wayland = gnome.x11 = true;
        CHECK(candidates(gnome) == QList<Backend>({Backend::QtXWayland}));
        // XFCE on X11 (wl-clipboard installed or not, irrelevant).
        BackendSelect::Environment xfce;
        xfce.os = "linux";
        xfce.x11 = true;
        xfce.wlPaste = xfce.wlCopy = true;
        CHECK(candidates(xfce) == QList<Backend>({Backend::Qt}));
        // KDE on X11.
        auto kdeX11 = kde;
        kdeX11.wayland = false;
        CHECK(candidates(kdeX11) == QList<Backend>({Backend::Qt}));
        // No display at all (an SSH session).
        BackendSelect::Environment headless;
        headless.os = "linux";
        CHECK(candidates(headless).isEmpty());
        // Windows and macOS: QClipboard, display variables or not.
        BackendSelect::Environment win;
        win.os = "windows";
        CHECK(candidates(win) == QList<Backend>({Backend::Qt}));
        win.os = "macos";
        CHECK(candidates(win) == QList<Backend>({Backend::Qt}));
        // Forced.
        auto forced = kde;
        forced.forced = "qt";
        CHECK(candidates(forced) == QList<Backend>({Backend::Qt}));
        forced.forced = "KLIPPER";
        CHECK(candidates(forced) == QList<Backend>({Backend::Klipper}));
        forced.dbusBuilt = false;
        CHECK(candidates(forced).isEmpty());
        forced.forced = "nonsense";
        CHECK(candidates(forced).isEmpty());
        forced.forced = "auto";
        forced.dbusBuilt = true;
        CHECK(candidates(forced) == candidates(kde));

        // Names round-trip; the GUI ones are the QClipboard ones.
        for (const QString& n : BackendSelect::allNames()) {
            const auto b = BackendSelect::fromName(n);
            CHECK(b.has_value());
            CHECK_EQ(BackendSelect::name(*b), n);
            CHECK(!BackendSelect::describe(*b).isEmpty());
        }
        CHECK(!BackendSelect::fromName("x").has_value());
        CHECK(BackendSelect::needsGui(Backend::Qt));
        CHECK(BackendSelect::needsGui(Backend::QtXWayland));
        CHECK(!BackendSelect::needsGui(Backend::WlClipboard));
        CHECK(!BackendSelect::needsGui(Backend::Klipper));
        // This process's environment can be read.
        CHECK(!BackendSelect::detect("auto").os.isEmpty());
    }

    // ── Backoff ─────────────────────────────────────────────────────────────
    SECTION("Agent reconnect backoff");
    {
        CHECK_EQ(backoffDelayMs(0, 1000, 60000, 0.5), 1000);
        CHECK_EQ(backoffDelayMs(1, 1000, 60000, 0.5), 2000);
        CHECK_EQ(backoffDelayMs(3, 1000, 60000, 0.5), 8000);
        CHECK_EQ(backoffDelayMs(10, 1000, 60000, 0.5), 60000);
        CHECK_EQ(backoffDelayMs(1000, 1000, 60000, 0.5), 60000); // no overflow
        CHECK_EQ(backoffDelayMs(0, 1000, 60000, 0.0), 800);
        CHECK_EQ(backoffDelayMs(0, 1000, 60000, 1.0), 1200);
        CHECK_EQ(backoffDelayMs(0, 1000, 60000, 7.0), 1200); // jitter clamped
        CHECK_EQ(backoffDelayMs(0, 1000, 60000, -1.0), 800);
        for (int a = 0; a < 20; ++a)
            CHECK(backoffDelayMs(a + 1, 500, 30000, 0.5) >= backoffDelayMs(a, 500, 30000, 0.5));
    }

    // ── The pin, against the server's own local certificate ─────────────────
    SECTION("Key pin — the server's pin is the agent's, and outlives a re-issue");
    {
        const QString certDir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cert/";
        QDir(certDir).removeRecursively();
        CertManager first;
        first.ensureLocalSslConfig();
        const QSslCertificate cert1 = first.localConfig().localCertificate();
        CHECK(!cert1.isNull());
        const QString pin = CertManager::spkiPin(cert1);
        CHECK(pin.startsWith("sha256/"));
        // The agent computes the same, and so does OpenSSL by hand.
        CHECK_EQ(PinCheck::pinText(cert1), pin);
        CHECK(PinCheck::spkiSha256(cert1) == opensslSpkiSha256(cert1));
        CHECK_EQ(PinCheck::spkiSha256(cert1).size(), 32);
        CHECK(first.keyPins().contains(pin));

        QList<QByteArray> pins;
        QString err;
        CHECK(AgentConfigParse::parsePins(pin, &pins, &err));
        CHECK(PinCheck::matches(cert1, pins));
        CHECK(!PinCheck::matches(cert1, {}));
        CHECK(!PinCheck::matches(cert1, {QByteArray(32, '\0')}));
        CHECK(!PinCheck::matches(QSslCertificate(), pins));
        CHECK(CertManager::spkiPin(QSslCertificate()).isEmpty());
        CHECK(PinCheck::pinText(QSslCertificate()).isEmpty());

        // A restart re-issues the certificate (fresh SANs) with the same key:
        // the certificate's own fingerprint changes, the pin does not.
        CertManager second;
        second.ensureLocalSslConfig();
        const QSslCertificate cert2 = second.localConfig().localCertificate();
        CHECK(cert2.digest(QCryptographicHash::Sha256) !=
                  cert1.digest(QCryptographicHash::Sha256) ||
              cert2.serialNumber() != cert1.serialNumber());
        CHECK_EQ(CertManager::spkiPin(cert2), pin);
        CHECK(PinCheck::matches(cert2, pins));

        // A rotated key (the key file deleted) changes the pin.
        QFile::remove(certDir + "local-key.pem");
        CertManager third;
        third.ensureLocalSslConfig();
        CHECK(CertManager::spkiPin(third.localConfig().localCertificate()) != pin);
        CHECK(!PinCheck::matches(third.localConfig().localCertificate(), pins));
        QDir(certDir).removeRecursively();
    }

    // ── AgentClient against the real server ─────────────────────────────────
    SECTION("AgentClient — hello, watch, announce, echo suppression, writes");
    {
        ClipboardAgentRegistry reg{QString()};
        ClipboardAgentServer server(reg);
        CHECK(server.listen());
        const QString token = reg.issueToken("H", 1);
        reg.setEnabled("H", true);
        QStringList hostTexts;
        QObject::connect(&server, &ClipboardAgentServer::agentText,
                         [&](const QString&, const QString& t) { hostTexts << t; });

        FakeBackend backend;
        backend.clip = "already there";
        AgentClient client(configFor(server.port(), token), &backend);
        int welcomes = 0;
        QObject::connect(&client, &AgentClient::welcomed, [&] { ++welcomes; });
        client.start();
        CHECK(waitFor([&] { return client.isWelcomed() && reg.isAvailable("H"); }));
        CHECK_EQ(welcomes, 1);
        CHECK_EQ(reg.statusJson("H").value("backend").toString(), QString("fake"));
        CHECK_EQ(reg.statusJson("H").value("agent_version").toString(), AgentClient::version());
        CHECK(!reg.statusJson("H").value("platform").toString().isEmpty());

        // Nobody watching: the clipboard is not even read.
        CHECK(!client.isWatching());
        CHECK(!backend.watching);
        backend.userCopies("private");
        pump(50);
        CHECK(hostTexts.isEmpty());
        CHECK_EQ(backend.reads, 0);

        // A session starts: the current clipboard goes out once.
        server.setWatching("H", true);
        CHECK(waitFor([&] { return hostTexts.size() == 1; }));
        CHECK_EQ(hostTexts[0], QString("private"));
        CHECK(backend.watching);
        CHECK(client.isWatching());

        // A copy on the host: out. The same copy again: not.
        backend.userCopies("copied");
        CHECK(waitFor([&] { return hostTexts.size() == 2; }));
        CHECK_EQ(hostTexts[1], QString("copied"));
        backend.userCopies("copied");
        backend.userCopies(""); // cleared / an image
        pump(50);
        CHECK_EQ(hostTexts.size(), 2);

        // The browser writes: the clipboard takes it, the echo stays home.
        bool acked = false, okv = false;
        server.sendSet("H", "from browser", [&](bool ok) {
            acked = true;
            okv = ok;
        });
        CHECK(waitFor([&] { return acked; }));
        CHECK(okv);
        CHECK(backend.writes == QStringList{"from browser"});
        CHECK_EQ(backend.clip, QString("from browser"));
        pump(50);
        CHECK_EQ(hostTexts.size(), 2);

        // A clipboard that refuses: the ack says so.
        backend.writeOk = false;
        acked = false;
        server.sendSet("H", "refused", [&](bool ok) {
            acked = true;
            okv = ok;
        });
        CHECK(waitFor([&] { return acked; }));
        CHECK(!okv);
        backend.writeOk = true;

        // An announce resends the current text even though it was our write.
        server.requestAnnounce("H");
        CHECK(waitFor([&] { return hostTexts.size() == 3; }));
        CHECK_EQ(hostTexts[2], QString("from browser"));

        // Session over: the watch stops and changes stay on the host.
        server.setWatching("H", false);
        CHECK(waitFor([&] { return !backend.watching; }));
        backend.userCopies("after the session");
        server.requestAnnounce("H"); // not watching: no announce either
        pump(80);
        CHECK_EQ(hostTexts.size(), 3);

        // Watching again starts from the clipboard as it is.
        server.setWatching("H", true);
        CHECK(waitFor([&] { return hostTexts.size() == 4; }));
        CHECK_EQ(hostTexts[3], QString("after the session"));

        client.stop();
        CHECK(waitFor([&] { return !reg.isAvailable("H"); }));
    }

    SECTION("AgentClient — reconnects with backoff when the server comes back");
    {
        const quint16 port = freePort();
        ClipboardAgentRegistry reg{QString()};
        const QString token = reg.issueToken("H", 1);
        FakeBackend backend;
        AgentClient client(configFor(port, token), &backend);
        client.setReconnectDelays(40, 160);
        client.start();
        pump(250); // refused a few times meanwhile
        CHECK(!client.isWelcomed());
        auto server = std::make_unique<ClipboardAgentServer>(reg);
        CHECK(server->listen(QHostAddress::LocalHost, port));
        CHECK(waitFor([&] { return client.isWelcomed(); }, 3000));

        // The server goes away and comes back: so does the agent.
        int lost = 0;
        QObject::connect(&client, &AgentClient::connectionLost, [&] { ++lost; });
        server.reset();
        CHECK(waitFor([&] { return lost == 1 && !client.isWelcomed(); }));
        server = std::make_unique<ClipboardAgentServer>(reg);
        CHECK(server->listen(QHostAddress::LocalHost, port));
        CHECK(waitFor([&] { return client.isWelcomed(); }, 3000));
        client.stop();
    }

    SECTION("AgentClient — gives up for good when it must");
    {
        ClipboardAgentRegistry reg{QString()};
        ClipboardAgentServer server(reg);
        CHECK(server.listen());
        const QString token = reg.issueToken("H", 1);

        // Its token rotated on the admin page.
        {
            FakeBackend backend;
            AgentClient client(configFor(server.port(), token), &backend);
            int code = 0;
            QObject::connect(&client, &AgentClient::fatal,
                             [&](int c, const QString&) { code = c; });
            client.setReconnectDelays(20, 40);
            client.start();
            CHECK(waitFor([&] { return client.isWelcomed(); }));
            const QString newToken = reg.issueToken("H", 2);
            CHECK(waitFor([&] { return code != 0; }));
            CHECK_EQ(code, int(AgentClient::ExitTokenRefused));
            pump(150);
            CHECK(!client.isWelcomed()); // and it did not come back

            // Another agent took the host over.
            FakeBackend b1, b2;
            AgentClient one(configFor(server.port(), newToken), &b1);
            AgentClient two(configFor(server.port(), newToken), &b2);
            int code1 = 0;
            QObject::connect(&one, &AgentClient::fatal, [&](int c, const QString&) { code1 = c; });
            one.start();
            CHECK(waitFor([&] { return one.isWelcomed(); }));
            two.start();
            CHECK(waitFor([&] { return code1 != 0; }));
            CHECK_EQ(code1, int(AgentClient::ExitReplaced));
            CHECK(waitFor([&] { return two.isWelcomed(); }));
            two.stop();
        }

        // The upgrade answered 401 (what HttpServer says to an unknown token).
        {
            QTcpServer http;
            CHECK(http.listen(QHostAddress::LocalHost, 0));
            int hits = 0;
            QObject::connect(&http, &QTcpServer::newConnection, [&] {
                QTcpSocket* s = http.nextPendingConnection();
                ++hits;
                QObject::connect(s, &QTcpSocket::readyRead, [s] {
                    s->readAll();
                    s->write("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
                    s->disconnectFromHost();
                });
            });
            FakeBackend backend;
            AgentClient client(configFor(http.serverPort(), token), &backend);
            client.setReconnectDelays(20, 40);
            int code = 0;
            QObject::connect(&client, &AgentClient::fatal,
                             [&](int c, const QString&) { code = c; });
            client.start();
            CHECK(waitFor([&] { return code != 0; }));
            CHECK_EQ(code, int(AgentClient::ExitTokenRefused));
            pump(150);
            CHECK_EQ(hits, 1); // no hammering the failed-login counter
        }

        // A server that speaks another version.
        {
            QWebSocketServer fake("v2", QWebSocketServer::NonSecureMode);
            CHECK(fake.listen(QHostAddress::LocalHost, 0));
            QObject::connect(&fake, &QWebSocketServer::newConnection, [&] {
                QWebSocket* s = fake.nextPendingConnection();
                QObject::connect(s, &QWebSocket::textMessageReceived, [s](const QString&) {
                    s->sendTextMessage(R"({"type":"welcome","version":2,"watch":false})");
                });
            });
            FakeBackend backend;
            AgentClient client(configFor(fake.serverPort(), token), &backend);
            int code = 0;
            QObject::connect(&client, &AgentClient::fatal,
                             [&](int c, const QString&) { code = c; });
            client.start();
            CHECK(waitFor([&] { return code != 0; }));
            CHECK_EQ(code, int(AgentClient::ExitVersion));
        }
    }

    SECTION("AgentClient — the hello carries no secret, the handshake the bearer");
    {
        QWebSocketServer fake("spy", QWebSocketServer::NonSecureMode);
        CHECK(fake.listen(QHostAddress::LocalHost, 0));
        QByteArray auth, hello;
        QObject::connect(&fake, &QWebSocketServer::newConnection, [&] {
            QWebSocket* s = fake.nextPendingConnection();
            auth = s->request().rawHeader("Authorization");
            QObject::connect(s, &QWebSocket::textMessageReceived,
                             [&](const QString& m) { hello = m.toUtf8(); });
        });
        const QString token = "mwca_" + QString(43, 'q');
        FakeBackend backend;
        AgentClient client(configFor(fake.serverPort(), token), &backend);
        client.start();
        CHECK(waitFor([&] { return !hello.isEmpty(); }));
        CHECK_EQ(auth, "Bearer " + token.toLatin1());
        CHECK(!hello.contains(token.toLatin1()));
        const ClipboardAgent::Message h = ClipboardAgent::decode(hello);
        CHECK(h.type == ClipboardAgent::Type::Hello);
        CHECK_EQ(h.backend, QString("fake"));
        client.stop();
    }

    // ── The pin over real TLS ───────────────────────────────────────────────
    SECTION("AgentClient — TLS: the token reaches only the pinned key");
    {
        const QString certDir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/cert/";
        QDir(certDir).removeRecursively();
        CertManager certs;
        certs.ensureLocalSslConfig();
        QSslConfiguration tls = certs.localConfig();
        QWebSocketServer wss("tls", QWebSocketServer::SecureMode);
        wss.setSslConfiguration(tls);
        CHECK(wss.listen(QHostAddress::LocalHost, 0));
        QList<QByteArray> seenAuth;
        QObject::connect(&wss, &QWebSocketServer::newConnection, [&] {
            QWebSocket* s = wss.nextPendingConnection();
            seenAuth << s->request().rawHeader("Authorization");
        });
        const QString token = "mwca_" + QString(43, 't');
        auto tlsConfig = [&](const QString& pinSpec) {
            AgentConfig c;
            c.url =
                QUrl(QStringLiteral("wss://127.0.0.1:%1/ws/clipboard-agent").arg(wss.serverPort()));
            c.token = token;
            QString err;
            AgentConfigParse::parsePins(pinSpec, &c.pins, &err);
            return c;
        };

        // A wrong pin: no handshake, so no token, ever reaches the server.
        {
            FakeBackend backend;
            AgentClient client(tlsConfig("sha256/" + QByteArray(32, '\x07').toBase64()), &backend);
            client.setReconnectDelays(50, 100);
            client.start();
            pump(600);
            CHECK(seenAuth.isEmpty());
            client.stop();
        }
        // No pin: a self-signed certificate does not verify; nothing sent.
        {
            FakeBackend backend;
            AgentClient client(tlsConfig(QString()), &backend);
            client.setReconnectDelays(50, 100);
            client.start();
            pump(400);
            CHECK(seenAuth.isEmpty());
            client.stop();
        }
        // The right pin (among others): connected, bearer delivered.
        {
            FakeBackend backend;
            AgentClient client(tlsConfig("sha256/" + QByteArray(32, '\x07').toBase64() + "," +
                                         CertManager::spkiPin(tls.localCertificate())),
                               &backend);
            client.start();
            CHECK(waitFor([&] { return seenAuth.size() == 1; }));
            CHECK_EQ(seenAuth.value(0), "Bearer " + token.toLatin1());
            client.stop();
        }
        QDir(certDir).removeRecursively();
    }

    // ── wl-clipboard, against stand-in scripts ──────────────────────────────
#ifdef Q_OS_UNIX
    SECTION("WlClipboardBackend — stand-in wl-paste / wl-copy");
    {
        QTemporaryDir dir;
        const QString d = dir.path();
        auto script = [&](const QString& name, const QString& body) {
            const QString path = d + "/" + name;
            QFile f(path);
            f.open(QIODevice::WriteOnly);
            f.write(("#!/bin/sh\n" + body).toUtf8());
            f.close();
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner);
            return path;
        };
        // wl-paste: --watch prints a line per change (and one at start);
        // otherwise prints the clipboard, or fails when it holds nothing.
        const QString paste = script(
            "wl-paste",
            QStringLiteral("D='%1'\n"
                           "if [ \"$1\" = --watch ]; then\n"
                           "  echo\n"
                           "  while :; do\n"
                           "    if [ -f \"$D/changed\" ]; then rm -f \"$D/changed\"; echo; fi\n"
                           "    sleep 0.02\n"
                           "  done\n"
                           "fi\n"
                           "[ -f \"$D/clip\" ] || exit 1\n"
                           "cat \"$D/clip\"\n")
                .arg(d));
        // wl-copy: takes the text on stdin — never as an argument.
        const QString copy = script("wl-copy", QStringLiteral("D='%1'\n"
                                                              "echo \"$*\" > \"$D/args\"\n"
                                                              "cat > \"$D/clip\"\n"
                                                              "touch \"$D/changed\"\n")
                                                   .arg(d));
        const QString noDataControl =
            script("wl-paste-gnome",
                   "echo 'Watch mode requires a compositor that supports the wlr data control "
                   "protocol' >&2\nexit 1\n");

        // No data-control: fails with wl-paste's own reason.
        {
            WlClipboardBackend b(noDataControl, copy);
            QString why;
            bool ready = false;
            QObject::connect(&b, &ClipboardBackend::failed, [&](const QString& w) { why = w; });
            QObject::connect(&b, &ClipboardBackend::ready, [&] { ready = true; });
            b.start();
            CHECK(waitFor([&] { return !why.isEmpty(); }));
            CHECK(why.contains("data control"));
            CHECK(!ready);
        }
        // Not installed at all.
        {
            WlClipboardBackend b(d + "/does-not-exist", copy);
            QString why;
            QObject::connect(&b, &ClipboardBackend::failed, [&](const QString& w) { why = w; });
            b.start();
            CHECK(waitFor([&] { return !why.isEmpty(); }));
        }
        // Working.
        {
            WlClipboardBackend b(paste, copy);
            b.setStartGraceMs(150);
            bool ready = false;
            QStringList seen;
            QObject::connect(&b, &ClipboardBackend::ready, [&] { ready = true; });
            QObject::connect(&b, &ClipboardBackend::observed, [&](const QString& t) { seen << t; });
            b.start();
            CHECK(waitFor([&] { return ready; }));

            // Empty clipboard: an empty reading.
            b.read();
            CHECK(waitFor([&] { return seen.size() == 1; }));
            CHECK(seen[0].isEmpty());

            // Not watching: changes are not read.
            int ok = -1;
            b.write(QString::fromUtf8("h\xc3\xa9llo\nworld"), [&](bool v) { ok = v; });
            CHECK(waitFor([&] { return ok != -1; }));
            CHECK_EQ(ok, 1);
            pump(150);
            CHECK_EQ(seen.size(), 1);
            QFile args(d + "/args");
            CHECK(args.open(QIODevice::ReadOnly));
            const QByteArray a = args.readAll();
            CHECK(!a.contains("llo")); // the text never on the command line
            CHECK(a.contains("text/plain"));

            b.read();
            CHECK(waitFor([&] { return seen.size() == 2; }));
            CHECK_EQ(seen[1], QString::fromUtf8("h\xc3\xa9llo\nworld"));

            // Watching: a change on the host is read.
            b.setWatching(true);
            {
                QFile clip(d + "/clip");
                clip.open(QIODevice::WriteOnly | QIODevice::Truncate);
                clip.write("copied on host");
                clip.close();
                QFile(d + "/changed").open(QIODevice::WriteOnly);
            }
            CHECK(waitFor([&] { return seen.contains("copied on host"); }));

            // Many reads at once collapse instead of piling up processes.
            const int before = seen.size();
            for (int i = 0; i < 10; ++i)
                b.read();
            pump(400);
            CHECK(seen.size() - before <= 3);
            CHECK(seen.size() - before >= 1);
        }
        // wl-copy missing: the write fails, it does not hang.
        {
            WlClipboardBackend b(paste, d + "/no-wl-copy");
            int ok = -1;
            b.write("x", [&](bool v) { ok = v; });
            CHECK(waitFor([&] { return ok != -1; }));
            CHECK_EQ(ok, 0);
        }
    }
#endif
}
