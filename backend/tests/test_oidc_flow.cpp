/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The OpenID Connect login end to end, minus the browser: OidcLogin's start
 * and callback against a stand-in provider on 127.0.0.1 that serves
 * discovery, a key set and a token endpoint, with keys generated here. Plus
 * the pieces it stands on — OidcClient's parsers and limits, the OIDC sessions
 * in AuthManager, and the settings in AppSettings.
 */
#include "test_framework.h"
#include "jwt_test_keys.h"

#include "network/OidcClient.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/OidcFlow.h"
#include "server/OidcLogin.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>

using namespace jwttest;

namespace {

template <class F> bool waitFor(F done, int ms = 8000)
{
    QElapsedTimer t;
    t.start();
    while (!done()) {
        if (t.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

/// A provider on 127.0.0.1: discovery, JWKS and token endpoint, each reply
/// settable, every request recorded.
class FakeIdp : public QObject
{
public:
    struct Seen
    {
        QString method;
        QString path;
        QMap<QString, QString> headers; // lowercase
        QByteArray body;
    };

    FakeIdp()
    {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* s = server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [this, s]() { onData(s); });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        discovery = QJsonObject{{"issuer", issuer()},
                                {"authorization_endpoint", issuer() + "/authorize"},
                                {"token_endpoint", issuer() + "/token"},
                                {"jwks_uri", issuer() + "/jwks"},
                                {"code_challenge_methods_supported", QJsonArray{"S256"}}};
    }

    QString issuer() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }
    int hits(const QString& path) const
    {
        int n = 0;
        for (const Seen& s : seen)
            n += s.path == path;
        return n;
    }
    const Seen* last(const QString& path) const
    {
        for (int i = seen.size() - 1; i >= 0; --i)
            if (seen[i].path == path) return &seen[i];
        return nullptr;
    }

    QTcpServer server;
    QJsonObject discovery;
    QJsonObject jwks;
    QByteArray idToken;
    int discoveryStatus = 200;
    int tokenStatus = 200;
    QByteArray tokenBody; // overrides the {"id_token": ...} reply when set
    QByteArray jwksBody;  // overrides the JWKS when set
    QList<Seen> seen;

private:
    void onData(QTcpSocket* s)
    {
        QByteArray& buf = m_buffers[s];
        buf += s->readAll();
        const int end = buf.indexOf("\r\n\r\n");
        if (end < 0) return;
        Seen req;
        const QList<QByteArray> lines = buf.left(end).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        req.method = QString::fromLatin1(first.value(0));
        req.path = QUrl(QString::fromLatin1(first.value(1))).path();
        for (int i = 1; i < lines.size(); ++i) {
            const int colon = lines[i].indexOf(':');
            if (colon > 0)
                req.headers[QString::fromLatin1(lines[i].left(colon)).trimmed().toLower()] =
                    QString::fromLatin1(lines[i].mid(colon + 1)).trimmed();
        }
        const int len = req.headers.value("content-length").toInt();
        if (buf.size() < end + 4 + len) return;
        req.body = buf.mid(end + 4, len);
        m_buffers.remove(s);
        seen.append(req);

        int status = 404;
        QByteArray body = "{}";
        if (req.path == "/.well-known/openid-configuration") {
            status = discoveryStatus;
            body = QJsonDocument(discovery).toJson(QJsonDocument::Compact);
        } else if (req.path == "/jwks") {
            status = 200;
            body =
                jwksBody.isEmpty() ? QJsonDocument(jwks).toJson(QJsonDocument::Compact) : jwksBody;
        } else if (req.path == "/token" && req.method == "POST") {
            status = tokenStatus;
            body = !tokenBody.isEmpty()
                       ? tokenBody
                       : QJsonDocument(QJsonObject{{"access_token", "at"},
                                                   {"token_type", "Bearer"},
                                                   {"id_token", QString::fromLatin1(idToken)}})
                             .toJson(QJsonDocument::Compact);
        }
        QByteArray reply = "HTTP/1.1 " + QByteArray::number(status) + " X\r\n";
        if (status == 302) reply += "Location: http://127.0.0.1:1/elsewhere\r\n";
        reply +=
            "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
            "\r\nConnection: close\r\n\r\n" + body;
        s->write(reply);
        s->disconnectFromHost();
    }
    QHash<QTcpSocket*, QByteArray> m_buffers;
};

const QString kClient = QStringLiteral("moonlightweb");
const QString kHost = QStringLiteral("mw.example.com");
const QString kLanHost = QStringLiteral("192.0.2.10:8443"); // a second configured origin

struct Begun
{
    OidcLogin::Reply reply;
    QString state;
    QString nonce;
    QString challenge;
    QString binding;
};

/// One provider, one MoonlightWeb, one clock.
struct Harness
{
    QTemporaryDir tmp;
    AppSettings settings;
    std::unique_ptr<AuthManager> auth;
    OidcClient client;
    std::unique_ptr<OidcLogin> login;
    FakeIdp idp;
    Key rsa = rsaKey();
    qint64 now = QDateTime::currentSecsSinceEpoch();
    QHash<QString, QString> env;

    Harness()
    {
        settings.m_FilePath = tmp.path() + "/settings.json";
        auth = std::make_unique<AuthManager>(&settings);
        auth->destroyAllSessions();
        login = std::make_unique<OidcLogin>(settings, *auth, client);
        login->setClock([this] { return now; });
        login->setEnv([this](const char* n) { return env.value(QString::fromLatin1(n)); });
        idp.jwks = jwks({jwk(rsa, "k1")});
        Oidc::Settings s;
        s.enabled = true;
        s.issuer = idp.issuer();
        s.clientId = kClient;
        s.publicUrls = {"https://" + kHost, "https://" + kLanHost};
        settings.setOidcSettings(s);
    }

    void edit(const std::function<void(Oidc::Settings&)>& f)
    {
        Oidc::Settings s = settings.oidcSettings();
        f(s);
        settings.setOidcSettings(s);
    }

    OidcLogin::Request request(const QString& ip = "198.51.100.7") const
    {
        OidcLogin::Request r;
        r.host = kHost;
        r.clientAddress = ip;
        r.attemptAddress = ip;
        return r;
    }

    OidcLogin::Reply run(bool isStart, const OidcLogin::Request& r)
    {
        bool done = false;
        OidcLogin::Reply out;
        const auto cb = [&](const OidcLogin::Reply& reply) {
            out = reply;
            done = true;
        };
        if (isStart)
            login->start(r, cb);
        else
            login->callback(r, cb);
        CHECK(waitFor([&] { return done; }));
        return out;
    }

    Begun begin(const QString& ip = "198.51.100.7", const QString& host = kHost)
    {
        Begun b;
        OidcLogin::Request r = request(ip);
        r.host = host;
        b.reply = run(true, r);
        const QUrlQuery q{QUrl(b.reply.location)};
        b.state = q.queryItemValue("state");
        b.nonce = q.queryItemValue("nonce");
        b.challenge = q.queryItemValue("code_challenge");
        const QString c = b.reply.setCookie;
        if (c.startsWith("mw_oidc=")) b.binding = c.mid(8, c.indexOf(';') - 8);
        return b;
    }

    QJsonObject claims(const QString& nonce) const
    {
        return QJsonObject{{"iss", idp.issuer()}, {"sub", "user-123"},
                           {"aud", kClient},      {"exp", now + 300},
                           {"iat", now - 5},      {"nonce", nonce},
                           {"name", "Alice"},     {"groups", QJsonArray{"streamers"}}};
    }

    OidcLogin::Reply finish(const Begun& b, const QString& ip = "198.51.100.7",
                            const QString& code = "the-code")
    {
        OidcLogin::Request r = request(ip);
        r.query = {{"state", b.state}, {"code", code}};
        r.bindingCookie = b.binding;
        return run(false, r);
    }

    /// The whole round trip with a token made from @p c.
    OidcLogin::Reply login_(const std::function<void(QJsonObject&)>& c = {},
                            const QString& ip = "198.51.100.7")
    {
        const Begun b = begin(ip);
        QJsonObject cl = claims(b.nonce);
        if (c) c(cl);
        idp.idToken = token(rsa, QJsonObject{{"alg", "RS256"}, {"kid", "k1"}}, cl);
        return finish(b, ip);
    }
};

QMap<QString, QString> form(const QByteArray& body)
{
    QMap<QString, QString> out;
    for (const QByteArray& kv : body.split('&')) {
        const int eq = kv.indexOf('=');
        out[QString::fromLatin1(kv.left(eq))] = QUrl::fromPercentEncoding(kv.mid(eq + 1));
    }
    return out;
}

} // namespace

void run_oidc_flow_tests()
{
    SECTION("OIDC — OidcClient parsers");
    {
        CHECK_EQ(OidcClient::discoveryUrl("https://idp.example.com/o/mw/"),
                 QString("https://idp.example.com/o/mw/.well-known/openid-configuration"));
        CHECK_EQ(OidcClient::discoveryUrl("https://idp.example.com/o/mw"),
                 QString("https://idp.example.com/o/mw/.well-known/openid-configuration"));

        const QJsonObject doc{{"issuer", "https://idp.example.com/o/mw/"},
                              {"authorization_endpoint", "https://idp.example.com/authorize"},
                              {"token_endpoint", "https://idp.example.com/token"},
                              {"jwks_uri", "https://idp.example.com/jwks"}};
        const auto parse = [](const QJsonObject& o, const QString& issuer, QString* err) {
            return OidcClient::parseDiscovery(QJsonDocument(o).toJson(), issuer, err);
        };
        QString err;
        auto p = parse(doc, "https://idp.example.com/o/mw/", &err);
        CHECK(p.has_value());
        CHECK(err.isEmpty());
        CHECK_EQ(p->tokenEndpoint, QString("https://idp.example.com/token"));
        // The configured issuer without its slash still matches; iss keeps the document's.
        p = parse(doc, "https://idp.example.com/o/mw", &err);
        CHECK(p.has_value());
        CHECK_EQ(p->issuer, QString("https://idp.example.com/o/mw/"));
        // Any other issuer: the document does not speak for ours.
        CHECK(!parse(doc, "https://idp.example.com/o/other/", &err).has_value());
        CHECK_EQ(err, QString("issuer_mismatch"));
        QJsonObject bad = doc;
        bad["token_endpoint"] = "http://idp.example.com/token";
        CHECK(!parse(bad, "https://idp.example.com/o/mw/", &err).has_value());
        CHECK_EQ(err, QString("endpoint_not_allowed"));
        bad = doc;
        bad.remove("jwks_uri");
        CHECK(!parse(bad, "https://idp.example.com/o/mw/", &err).has_value());
        bad = doc;
        bad["code_challenge_methods_supported"] = QJsonArray{"plain"};
        CHECK(!parse(bad, "https://idp.example.com/o/mw/", &err).has_value());
        CHECK_EQ(err, QString("pkce_s256_unsupported"));
        bad["code_challenge_methods_supported"] = QJsonArray{"plain", "S256"};
        CHECK(parse(bad, "https://idp.example.com/o/mw/", &err).has_value());
        CHECK(!OidcClient::parseDiscovery("<html>", "https://idp.example.com/o/mw/", &err));
        CHECK_EQ(err, QString("discovery_not_json"));

        CHECK_EQ(OidcClient::parseTokenResponse(R"({"id_token":"a.b.c"})", &err),
                 QByteArray("a.b.c"));
        CHECK(OidcClient::parseTokenResponse(R"({"error":"invalid_grant"})", &err).isEmpty());
        CHECK_EQ(err, QString("invalid_grant"));
        CHECK(OidcClient::parseTokenResponse(R"({"access_token":"x"})", &err).isEmpty());
        CHECK_EQ(err, QString("no_id_token"));

        OidcClient::TokenRequest tr;
        tr.code = "a&b=c";
        tr.redirectUri = "https://mw.example.com/auth/oidc/callback";
        tr.codeVerifier = "v";
        tr.clientId = "my client";
        const auto f = form(OidcClient::tokenRequestBody(tr));
        CHECK_EQ(f.value("grant_type"), QString("authorization_code"));
        CHECK_EQ(f.value("code"), QString("a&b=c")); // one field, not two
        CHECK(!f.contains("b"));
        CHECK_EQ(f.value("redirect_uri"), tr.redirectUri);
        CHECK_EQ(f.value("client_id"), QString("my client"));
        tr.clientSecret = "s";
        CHECK(!form(OidcClient::tokenRequestBody(tr)).contains("client_id"));

        CHECK(OidcClient::basicAuthorization("id", "").isEmpty());
        CHECK_EQ(OidcClient::basicAuthorization("id", "secret"),
                 QByteArray("Basic ") + QByteArray("id:secret").toBase64());
        // RFC 6749 §2.3.1: form-encoded first, so a ':' in the id cannot move.
        CHECK_EQ(OidcClient::basicAuthorization("a:b", "c d"),
                 QByteArray("Basic ") + QByteArray("a%3Ab:c%20d").toBase64());
    }

    SECTION("OIDC — AuthManager sessions");
    {
        QTemporaryDir tmp;
        AppSettings settings;
        settings.m_FilePath = tmp.path() + "/settings.json";
        Oidc::Settings os;
        os.enabled = true;
        os.issuer = "https://idp.example.com/";
        os.clientId = kClient;
        os.publicUrls = {"https://mw.example.com"};
        settings.setOidcSettings(os);

        AuthManager auth(&settings);
        auth.destroyAllSessions();
        const QString pin = auth.createSession("198.51.100.1", "Laptop");
        const QString user = auth.createOidcSession("198.51.100.2", "sub-1", "Alice", false, 3600);
        const QString admin = auth.createOidcSession("198.51.100.3", "sub-2", "Bob", true, 3600);

        CHECK(auth.validateSession(user));
        CHECK(auth.isOidcSession(user));
        CHECK(!auth.isOidcSession(pin));
        CHECK(!auth.isOidcAdminSession(user));
        CHECK(auth.isOidcAdminSession(admin));
        // Not the password door's flag, and not the host's.
        CHECK(!auth.isAdminSession(admin));
        CHECK(!auth.isHostSession(admin));
        const SessionInfo info = auth.sessionForToken(admin);
        CHECK_EQ(info.authMethod, QString("oidc"));
        CHECK_EQ(info.subject, QString("sub-2"));
        CHECK_EQ(info.displayName, QString("Bob"));
        CHECK_EQ(info.machineName, QString("Bob"));
        CHECK_EQ(info.expiresAt, info.createdAt + 3600);
        const QJsonObject j = info.toJson();
        CHECK_EQ(j.value("auth_method").toString(), QString("oidc"));
        CHECK(j.value("oidc_admin").toBool());
        CHECK_EQ(static_cast<qint64>(j.value("expires_at").toDouble()), info.expiresAt);
        CHECK(!auth.sessionForToken(pin).toJson().contains("auth_method"));

        // The remote admin password's revocation leaves the provider's grant alone.
        auth.demoteAdminSessions();
        CHECK(auth.isOidcAdminSession(admin));

        // Survives a restart while the login is usable...
        {
            AuthManager reloaded(&settings);
            CHECK(reloaded.isOidcAdminSession(admin));
            CHECK(reloaded.isOidcSession(user));
            CHECK(reloaded.validateSession(pin));
        }
        // ...but not one where the login is switched off.
        os.enabled = false;
        settings.setOidcSettings(os);
        {
            AuthManager reloaded(&settings);
            CHECK(!reloaded.validateSession(admin));
            CHECK(!reloaded.validateSession(user));
            CHECK(reloaded.validateSession(pin));
        }
        os.enabled = true;
        settings.setOidcSettings(os);

        // A hand-edited file cannot make a PIN session an OIDC admin, nor keep
        // an OIDC session past its end.
        const QString path =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/sessions.json";
        auth.saveSessions();
        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        QJsonArray arr = root["sessions"].toArray();
        for (int i = 0; i < arr.size(); ++i) {
            QJsonObject s = arr[i].toObject();
            if (s["machine_name"] == "Laptop") s["oidc_admin"] = true;
            if (s["machine_name"] == "Alice") s["expires_at"] = 1000;
            if (s["machine_name"] == "Bob") s.remove("expires_at");
            arr[i] = s;
        }
        root["sessions"] = arr;
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(QJsonDocument(root).toJson());
        f.close();
        {
            AuthManager reloaded(&settings);
            CHECK(reloaded.validateSession(pin));
            CHECK(!reloaded.isOidcAdminSession(pin));
            CHECK(!reloaded.validateSession(user));  // past its end
            CHECK(!reloaded.validateSession(admin)); // an OIDC session with no end
        }

        // destroyOidcSessions takes the OIDC ones and nothing else.
        AuthManager live(&settings);
        live.destroyAllSessions();
        const QString keep = live.createSession("198.51.100.1", "Laptop");
        const QString a1 = live.createOidcSession("198.51.100.2", "s1", "A", false, 3600);
        const QString a2 = live.createOidcSession("198.51.100.3", "s2", "B", true, 3600);
        CHECK_EQ(live.destroyOidcSessions(), 2);
        CHECK(live.validateSession(keep));
        CHECK(!live.validateSession(a1));
        CHECK(!live.validateSession(a2));
        CHECK_EQ(live.destroyOidcSessions(), 0);

        // The end is absolute: activity does not move it.
        const QString shortLived = live.createOidcSession("198.51.100.4", "s3", "C", false, 1);
        QThread::msleep(1100);
        live.touchSession(shortLived);
        CHECK(!live.validateSession(shortLived));
        CHECK(!live.isOidcSession(shortLived));
        live.purgeExpiredSessions();
        CHECK(live.sessionForToken(shortLived).token.isEmpty());

        // Its own rate-limit bucket: failures here do not lock the PIN out.
        const QString ip = "203.0.113.50";
        CHECK_EQ(live.oidcLockout(ip), 0);
        for (int i = 0; i < 3; ++i)
            live.recordOidcAttempt(ip, false);
        CHECK(live.oidcLockout(ip) > 0);
        CHECK(!live.isRateLimited(ip));
        CHECK_EQ(live.oidcLockout("203.0.113.51"), 0);
        live.destroyAllSessions();
    }

    SECTION("OIDC — the login flow against a stand-in provider");

    // ── start ───────────────────────────────────────────────────────────────
    {
        Harness h;
        Begun b = h.begin();
        CHECK_EQ(b.reply.status, 302);
        const QUrl to(b.reply.location);
        CHECK_EQ(to.toString(QUrl::RemoveQuery), h.idp.issuer() + "/authorize");
        const QUrlQuery q(to);
        CHECK_EQ(q.queryItemValue("client_id"), kClient);
        CHECK_EQ(q.queryItemValue("redirect_uri", QUrl::FullyDecoded),
                 QString("https://mw.example.com/auth/oidc/callback"));
        CHECK_EQ(q.queryItemValue("code_challenge_method"), QString("S256"));
        CHECK(!b.state.isEmpty() && !b.nonce.isEmpty() && !b.challenge.isEmpty());
        CHECK(b.reply.setCookie.contains("HttpOnly"));
        CHECK(b.reply.setCookie.contains("Secure"));
        CHECK(b.reply.setCookie.contains("SameSite=Lax"));
        CHECK(b.reply.setCookie.contains("Path=/auth/oidc"));
        CHECK(b.reply.setCookie.contains("Max-Age=600"));
        CHECK(!b.binding.isEmpty());
        CHECK(b.binding != b.state);
        CHECK_EQ(h.login->pending().size(), 1);
        // Discovery is cached: a second start asks the provider nothing.
        h.begin();
        CHECK_EQ(h.idp.hits("/.well-known/openid-configuration"), 1);

        // Each configured origin gets its own redirect_uri.
        const Begun lan = h.begin("198.51.100.7", kLanHost);
        CHECK_EQ(
            QUrlQuery(QUrl(lan.reply.location)).queryItemValue("redirect_uri", QUrl::FullyDecoded),
            QString("https://192.0.2.10:8443/auth/oidc/callback"));
        OidcLogin::Request other = h.request();
        other.host = "MW.EXAMPLE.COM:443";
        CHECK(h.run(true, other).location.startsWith(h.idp.issuer()));
        // Reached under a name that is not configured: refused, nothing started.
        const int pendingBefore = h.login->pending().size();
        for (const char* host : {"192.0.2.10", "192.0.2.10:9443", "evil.example", ""}) {
            other.host = QString::fromLatin1(host);
            const OidcLogin::Reply refused = h.run(true, other);
            CHECK_EQ(refused.status, 403);
            CHECK_EQ(refused.error, QString("origin_not_configured"));
            CHECK(refused.location.isEmpty());
            CHECK(refused.setCookie.isEmpty());
            CHECK(!refused.authFailure);
        }
        CHECK_EQ(h.login->pending().size(), pendingBefore);

        // Closed (tunnel, booth) or unusable: the routes are not there.
        OidcLogin::Request closed = h.request();
        closed.closed = true;
        CHECK_EQ(h.run(true, closed).status, 404);
        CHECK_EQ(h.run(false, closed).status, 404);
        h.edit([](Oidc::Settings& s) { s.enabled = false; });
        CHECK_EQ(h.run(true, h.request()).status, 404);
        CHECK_EQ(h.run(false, h.request()).status, 404);
    }

    // ── A full login, public client ─────────────────────────────────────────
    {
        Harness h;
        const Begun b = h.begin();
        h.idp.idToken =
            token(h.rsa, QJsonObject{{"alg", "RS256"}, {"kid", "k1"}}, h.claims(b.nonce));
        const OidcLogin::Reply r = h.finish(b);
        CHECK_EQ(r.status, 200);
        CHECK(r.error.isEmpty());
        CHECK(!r.authFailure);
        CHECK(r.setCookie.startsWith("mw_session=" + r.sessionToken + ";"));
        CHECK(r.setCookie.contains("SameSite=Strict"));
        CHECK(r.setCookie.contains("HttpOnly"));
        CHECK(r.setCookie.contains("Secure"));
        CHECK(r.setCookie.contains("Max-Age=86400"));
        CHECK(h.auth->isOidcSession(r.sessionToken));
        CHECK(!h.auth->isOidcAdminSession(r.sessionToken));
        const SessionInfo s = h.auth->sessionForToken(r.sessionToken);
        CHECK_EQ(s.subject, QString("user-123"));
        CHECK_EQ(s.displayName, QString("Alice"));
        CHECK_EQ(s.ip, QString("198.51.100.7"));
        CHECK_EQ(s.expiresAt - s.createdAt, 86400);

        // What reached the token endpoint: the code, our redirect_uri, the
        // verifier whose S256 is the challenge the browser carried, and the
        // client id in the body with no Authorization header (public client).
        const FakeIdp::Seen* t = h.idp.last("/token");
        CHECK(t != nullptr);
        if (t) {
            const auto f = form(t->body);
            CHECK_EQ(f.value("grant_type"), QString("authorization_code"));
            CHECK_EQ(f.value("code"), QString("the-code"));
            CHECK_EQ(f.value("redirect_uri"), QString("https://mw.example.com/auth/oidc/callback"));
            CHECK_EQ(Oidc::pkceChallenge(f.value("code_verifier")), b.challenge);
            CHECK_EQ(f.value("client_id"), kClient);
            CHECK(!t->headers.contains("authorization"));
            CHECK(t->headers.value("content-type").startsWith("application/x-www-form-urlencoded"));
        }
        CHECK_EQ(h.login->pending().size(), 0);

        // Replaying the very same callback: the state is spent.
        const OidcLogin::Reply replay = h.finish(b);
        CHECK_EQ(replay.status, 302);
        CHECK_EQ(replay.error, QString("invalid_state"));
        CHECK_EQ(replay.location, QString("/?oidc_error=invalid_state"));
        CHECK(replay.authFailure);
        CHECK(replay.sessionToken.isEmpty());
        CHECK_EQ(h.idp.hits("/token"), 1); // never reached the provider again

        // The landing page navigates by meta refresh, with no script for the CSP to refuse.
        const QByteArray page = OidcLogin::landingPage();
        CHECK(page.contains("http-equiv=\"refresh\" content=\"0;url=/\""));
        CHECK(!page.contains("<script"));
    }

    // ── Several origins: a flow finishes where it started ───────────────────
    {
        Harness h;
        // Started and finished on the LAN origin: the code is exchanged with
        // that origin's redirect_uri.
        Begun b = h.begin("198.51.100.8", kLanHost);
        h.idp.idToken =
            token(h.rsa, QJsonObject{{"alg", "RS256"}, {"kid", "k1"}}, h.claims(b.nonce));
        OidcLogin::Request r = h.request("198.51.100.8");
        r.host = kLanHost;
        r.query = {{"state", b.state}, {"code", "c"}};
        r.bindingCookie = b.binding;
        CHECK_EQ(h.run(false, r).status, 200);
        const FakeIdp::Seen* t = h.idp.last("/token");
        CHECK(t && form(t->body).value("redirect_uri") ==
                       QString("https://192.0.2.10:8443/auth/oidc/callback"));

        // Started on one origin, called back on another — even with the right
        // cookie, which a browser would not carry across: refused, state spent.
        b = h.begin("198.51.100.9", kHost);
        r = h.request("198.51.100.9");
        r.host = kLanHost;
        r.query = {{"state", b.state}, {"code", "c"}};
        r.bindingCookie = b.binding;
        const int tokens = h.idp.hits("/token");
        OidcLogin::Reply out = h.run(false, r);
        CHECK_EQ(out.error, QString("wrong_origin"));
        CHECK(out.authFailure);
        CHECK(out.sessionToken.isEmpty());
        CHECK_EQ(h.idp.hits("/token"), tokens);
        CHECK_EQ(h.finish(b, "198.51.100.9").error, QString("invalid_state"));
        // Called back under a name that is not configured at all.
        b = h.begin("198.51.100.10", kHost);
        r.host = "evil.example";
        r.query = {{"state", b.state}, {"code", "c"}};
        r.bindingCookie = b.binding;
        CHECK_EQ(h.run(false, r).error, QString("wrong_origin"));
    }

    // ── Confidential client: client_secret_basic, env first ─────────────────
    {
        Harness h;
        h.edit([](Oidc::Settings& s) { s.clientSecret = "from-settings"; });
        CHECK_EQ(h.login_().status, 200);
        const FakeIdp::Seen* t = h.idp.last("/token");
        CHECK(t && t->headers.value("authorization") ==
                       "Basic " + QByteArray(kClient.toLatin1() + ":from-settings").toBase64());
        CHECK(t && !form(t->body).contains("client_id"));
        h.env["MW_OIDC_CLIENT_SECRET"] = "from-env";
        CHECK_EQ(h.login_().status, 200);
        t = h.idp.last("/token");
        CHECK(t && t->headers.value("authorization") ==
                       "Basic " + QByteArray(kClient.toLatin1() + ":from-env").toBase64());
    }

    // ── state: missing cookie, foreign cookie, expired, unknown ────────────
    {
        Harness h;
        Begun b = h.begin("198.51.100.20");
        b.binding.clear();
        OidcLogin::Reply r = h.finish(b, "198.51.100.20");
        CHECK_EQ(r.error, QString("invalid_state"));
        CHECK(r.authFailure);
        CHECK_EQ(h.idp.hits("/token"), 0);

        b = h.begin("198.51.100.21");
        b.binding = "an-attacker-s-cookie";
        CHECK_EQ(h.finish(b, "198.51.100.21").error, QString("invalid_state"));

        b = h.begin("198.51.100.22");
        h.now += Oidc::PendingLogins::TTL_SECS + 1;
        CHECK_EQ(h.finish(b, "198.51.100.22").error, QString("invalid_state"));

        b = h.begin("198.51.100.23");
        b.state = "never-issued";
        CHECK_EQ(h.finish(b, "198.51.100.23").error, QString("invalid_state"));
        CHECK_EQ(h.idp.hits("/token"), 0);

        // The provider reporting an error (the user declined): not the user's
        // fault, not counted, and the state is spent all the same.
        b = h.begin("198.51.100.24");
        OidcLogin::Request req = h.request("198.51.100.24");
        req.query = {{"state", b.state}, {"error", "access_denied"}};
        req.bindingCookie = b.binding;
        r = h.run(false, req);
        CHECK_EQ(r.error, QString("idp_error"));
        CHECK(!r.authFailure);
        CHECK_EQ(h.finish(b, "198.51.100.24").error, QString("invalid_state"));
        // A callback without a code.
        b = h.begin("198.51.100.24");
        req.query = {{"state", b.state}};
        req.bindingCookie = b.binding;
        CHECK_EQ(h.run(false, req).error, QString("idp_error"));
    }

    // ── The ID token: every refusal ends in invalid_token ───────────────────
    {
        Harness h;
        int n = 30;
        const auto refused = [&h, &n](const std::function<void(QJsonObject&)>& c) {
            const QString ip = QStringLiteral("198.51.100.%1").arg(n++);
            const OidcLogin::Reply r = h.login_(c, ip);
            return r.error == QLatin1String("invalid_token") && r.authFailure &&
                   r.sessionToken.isEmpty();
        };
        CHECK(refused([](QJsonObject& c) { c["nonce"] = "another-login"; }));
        CHECK(refused([](QJsonObject& c) { c.remove("nonce"); }));
        CHECK(refused([](QJsonObject& c) { c["aud"] = "another-client"; }));
        CHECK(refused([](QJsonObject& c) { c["iss"] = "https://evil.example/"; }));
        CHECK(refused([&h](QJsonObject& c) { c["exp"] = h.now - 120; }));
        CHECK(refused([&h](QJsonObject& c) { c["iat"] = h.now + 3600; }));
        CHECK(refused([](QJsonObject& c) { c["azp"] = "another-client"; }));

        // Signed by a key the provider does not publish.
        Begun b = h.begin("198.51.100.60");
        h.idp.idToken =
            token(rsaKey(), QJsonObject{{"alg", "RS256"}, {"kid", "k1"}}, h.claims(b.nonce));
        CHECK_EQ(h.finish(b, "198.51.100.60").error, QString("invalid_token"));
        // alg none, and HS256 keyed with the public key.
        b = h.begin("198.51.100.61");
        h.idp.idToken = b64u(compact(QJsonObject{{"alg", "none"}})) + '.' +
                        b64u(compact(h.claims(b.nonce))) + '.';
        CHECK_EQ(h.finish(b, "198.51.100.61").error, QString("invalid_token"));
        b = h.begin("198.51.100.62");
        h.idp.idToken = hs256(compact(jwk(h.rsa, "k1")),
                              QJsonObject{{"alg", "HS256"}, {"kid", "k1"}}, h.claims(b.nonce));
        CHECK_EQ(h.finish(b, "198.51.100.62").error, QString("invalid_token"));
        // A token issued for another login of the same browser cannot be used here.
        const Begun first = h.begin("198.51.100.63");
        const Begun second = h.begin("198.51.100.63");
        h.idp.idToken =
            token(h.rsa, QJsonObject{{"alg", "RS256"}, {"kid", "k1"}}, h.claims(first.nonce));
        CHECK_EQ(h.finish(second, "198.51.100.63").error, QString("invalid_token"));
    }

    // ── Key rotation: one refetch on an unknown kid, never a loop ───────────
    {
        Harness h;
        CHECK_EQ(h.login_().status, 200);
        CHECK_EQ(h.idp.hits("/jwks"), 1);
        CHECK_EQ(h.login_().status, 200);
        CHECK_EQ(h.idp.hits("/jwks"), 1); // cached

        const Key next = rsaKey();
        h.idp.jwks = jwks({jwk(h.rsa, "k1"), jwk(next, "k2")});
        Begun b = h.begin();
        h.idp.idToken =
            token(next, QJsonObject{{"alg", "RS256"}, {"kid", "k2"}}, h.claims(b.nonce));
        CHECK_EQ(h.finish(b).status, 200);
        CHECK_EQ(h.idp.hits("/jwks"), 2);

        // A kid nobody publishes: one more look, then refused.
        b = h.begin("198.51.100.70");
        h.idp.idToken =
            token(rsaKey(), QJsonObject{{"alg", "RS256"}, {"kid", "k9"}}, h.claims(b.nonce));
        CHECK_EQ(h.finish(b, "198.51.100.70").error, QString("invalid_token"));
        CHECK_EQ(h.idp.hits("/jwks"), 3);

        // ES256 from a provider that signs with P-256.
        const Key ec = ecKey();
        h.idp.jwks = jwks({jwk(ec, "e1")});
        b = h.begin();
        h.idp.idToken = token(ec, QJsonObject{{"alg", "ES256"}, {"kid", "e1"}}, h.claims(b.nonce));
        CHECK_EQ(h.finish(b).status, 200);
    }

    // ── Groups ──────────────────────────────────────────────────────────────
    {
        Harness h;
        h.edit([](Oidc::Settings& s) {
            s.allowedGroups = {"streamers"};
            s.adminGroups = {"mw-admins"};
        });
        OidcLogin::Reply r = h.login_();
        CHECK_EQ(r.status, 200);
        CHECK(!h.auth->isOidcAdminSession(r.sessionToken));

        r = h.login_([](QJsonObject& c) { c["groups"] = QJsonArray{"mw-admins"}; });
        CHECK_EQ(r.status, 200);
        CHECK(h.auth->isOidcAdminSession(r.sessionToken));

        r = h.login_([](QJsonObject& c) { c["groups"] = QJsonArray{"guests"}; }, "198.51.100.80");
        CHECK_EQ(r.error, QString("not_allowed"));
        CHECK(r.authFailure);
        CHECK(r.sessionToken.isEmpty());
        r = h.login_([](QJsonObject& c) { c.remove("groups"); }, "198.51.100.81");
        CHECK_EQ(r.error, QString("not_allowed"));

        // The TTL follows the setting.
        h.edit([](Oidc::Settings& s) { s.sessionTtlHours = 8; });
        r = h.login_();
        CHECK(r.setCookie.contains("Max-Age=28800"));
        const SessionInfo s = h.auth->sessionForToken(r.sessionToken);
        CHECK_EQ(s.expiresAt - s.createdAt, 8 * 3600);
    }

    // ── The provider failing ────────────────────────────────────────────────
    {
        Harness h;
        h.idp.tokenStatus = 400;
        h.idp.tokenBody = R"({"error":"invalid_grant"})";
        OidcLogin::Reply r = h.login_({}, "198.51.100.90");
        CHECK_EQ(r.error, QString("token_exchange"));
        CHECK(r.authFailure);

        h.idp.tokenStatus = 500;
        h.idp.tokenBody = R"({"error":"server_error"})";
        r = h.login_({}, "198.51.100.91");
        CHECK_EQ(r.error, QString("idp_unreachable"));
        CHECK(!r.authFailure);

        h.idp.tokenStatus = 200;
        h.idp.tokenBody = R"({"access_token":"only"})";
        CHECK_EQ(h.login_({}, "198.51.100.92").error, QString("idp_unreachable"));
        h.idp.tokenBody.clear();

        // A key set too large to be one is not read.
        h.idp.jwksBody = QByteArray(OidcClient::MAX_BODY_BYTES + 1024, ' ');
        h.client.clearCache();
        CHECK_EQ(h.login_({}, "198.51.100.93").error, QString("idp_unreachable"));
        h.idp.jwksBody.clear();

        // Discovery naming another issuer, or redirecting elsewhere.
        h.client.clearCache();
        h.idp.discovery["issuer"] = "https://evil.example/";
        CHECK_EQ(h.begin("198.51.100.94").reply.error, QString("idp_unreachable"));
        h.idp.discovery["issuer"] = h.idp.issuer();
        h.idp.discoveryStatus = 302;
        CHECK_EQ(h.begin("198.51.100.95").reply.error, QString("idp_unreachable"));
        h.idp.discoveryStatus = 200;
        CHECK_EQ(h.idp.hits("/elsewhere"), 0);

        // Gone altogether.
        h.client.clearCache();
        h.idp.server.close();
        const Begun b = h.begin("198.51.100.96");
        CHECK_EQ(b.reply.error, QString("idp_unreachable"));
        CHECK(!b.reply.authFailure);
        CHECK(b.reply.setCookie.isEmpty());
    }

    // ── Rate limit: counted refusals lock the caller out, success resets ────
    {
        Harness h;
        const QString ip = "203.0.113.77";
        for (int i = 0; i < 3; ++i) {
            Begun b = h.begin(ip);
            b.binding.clear();
            CHECK_EQ(h.finish(b, ip).error, QString("invalid_state"));
        }
        CHECK_EQ(h.begin(ip).reply.error, QString("rate_limited"));
        OidcLogin::Request req = h.request(ip);
        req.query = {{"state", "x"}, {"code", "y"}};
        const OidcLogin::Reply r = h.run(false, req);
        CHECK_EQ(r.error, QString("rate_limited"));
        CHECK(!r.authFailure);
        // Another address is untouched.
        CHECK_EQ(h.login_({}, "203.0.113.78").status, 200);
    }

    // ── AppSettings ─────────────────────────────────────────────────────────
    {
        QTemporaryDir tmp;
        AppSettings settings;
        settings.m_FilePath = tmp.path() + "/settings.json";
        CHECK(!settings.oidcSettings().enabled);
        CHECK_EQ(settings.oidcSettings().sessionTtlHours, 24);
        settings.setPacketMarking(false);
        Oidc::Settings s;
        s.enabled = true;
        s.issuer = "https://idp.example.com/";
        s.adminGroups = {"mw-admins"};
        settings.setOidcSettings(s);
        CHECK(settings.oidcSettings().enabled);
        CHECK_EQ(settings.oidcSettings().adminGroups, QStringList({"mw-admins"}));
        CHECK(!settings.packetMarking()); // the rest of the file kept
        CHECK_EQ(settings.readAll().value("oidc_admin_groups").toArray().size(), 1);
    }
}
