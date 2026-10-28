/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The OIDC login's decisions (server/OidcFlow): which settings make a usable
 * login, which URLs may be talked to, the single-use attempts that bind a
 * callback to the browser that started it, and what the groups grant.
 */
#include "test_framework.h"

#include "server/OidcFlow.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QSet>
#include <QTemporaryDir>
#include <QUrlQuery>

namespace {

const QString kCb = QStringLiteral("https://mw.example.com/auth/oidc/callback");

Oidc::Settings usable()
{
    Oidc::Settings s;
    s.enabled = true;
    s.issuer = QStringLiteral("https://idp.example.com/application/o/mw/");
    s.clientId = QStringLiteral("moonlightweb");
    s.publicUrls = {QStringLiteral("https://mw.example.com")};
    return s;
}

Oidc::EnvReader env(const QHash<QString, QString>& vars)
{
    return [vars](const char* name) { return vars.value(QString::fromLatin1(name)); };
}

} // namespace

void run_oidc_rules_tests()
{
    SECTION("OIDC — settings, URLs, attempts, groups");

    // ── Settings round trip and defaults ────────────────────────────────────
    {
        const Oidc::Settings d = Oidc::Settings::fromJson({});
        CHECK(!d.enabled);
        CHECK_EQ(d.scopes, QString("openid profile email"));
        CHECK_EQ(d.groupsClaim, QString("groups"));
        CHECK_EQ(d.sessionTtlHours, 24);
        CHECK(d.allowedGroups.isEmpty());
        CHECK(!d.disablePin);

        Oidc::Settings s = usable();
        s.clientSecret = QStringLiteral("s3cret");
        s.allowedGroups = {"streamers", "family"};
        s.adminGroups = {"mw-admins"};
        s.buttonLabel = QStringLiteral("Authentik");
        s.disablePin = true;
        s.sessionTtlHours = 8;
        QJsonObject file{{"unrelated", 42}};
        s.writeTo(file);
        CHECK_EQ(file.value("unrelated").toInt(), 42); // other keys untouched
        const Oidc::Settings back = Oidc::Settings::fromJson(file);
        CHECK(back.enabled);
        CHECK_EQ(back.issuer, s.issuer);
        CHECK_EQ(back.clientId, s.clientId);
        CHECK_EQ(back.clientSecret, QString("s3cret"));
        CHECK_EQ(back.allowedGroups, s.allowedGroups);
        CHECK_EQ(back.adminGroups, s.adminGroups);
        CHECK_EQ(back.buttonLabel, QString("Authentik"));
        CHECK(back.disablePin);
        CHECK_EQ(back.sessionTtlHours, 8);
        CHECK_EQ(back.publicUrls, s.publicUrls);
        CHECK(file.value("public_url").isArray());
        // The single string an older file holds is still read, as a list of one.
        CHECK_EQ(Oidc::Settings::fromJson({{"public_url", "https://mw.example.com/"}}).publicUrls,
                 QStringList({"https://mw.example.com"}));
        CHECK_EQ(Oidc::Settings::fromJson({{"public_url", "https://a.example, https://b.example"}})
                     .publicUrls,
                 QStringList({"https://a.example", "https://b.example"}));

        // Out-of-range TTLs fall back to the default rather than to 0 or forever.
        CHECK_EQ(Oidc::Settings::fromJson({{"oidc_session_ttl_hours", 0}}).sessionTtlHours, 24);
        CHECK_EQ(Oidc::Settings::fromJson({{"oidc_session_ttl_hours", 721}}).sessionTtlHours, 24);
        CHECK_EQ(Oidc::Settings::fromJson({{"oidc_session_ttl_hours", 720}}).sessionTtlHours, 720);
        CHECK_EQ(Oidc::Settings::fromJson({{"oidc_session_ttl_hours", "8"}}).sessionTtlHours, 24);
        // Empty scopes / claim keep their defaults; a hand-edited string list parses.
        CHECK_EQ(Oidc::Settings::fromJson({{"oidc_scopes", "  "}}).scopes,
                 QString("openid profile email"));
        CHECK_EQ(Oidc::Settings::fromJson({{"oidc_groups_claim", ""}}).groupsClaim,
                 QString("groups"));
        CHECK_EQ(Oidc::Settings::fromJson({{"oidc_allowed_groups", "a, b"}}).allowedGroups,
                 QStringList({"a", "b"}));
        // A truthy non-bool is not a decision to enable anything.
        CHECK(!Oidc::Settings::fromJson({{"oidc_enabled", "true"}}).enabled);
        CHECK(!Oidc::Settings::fromJson({{"oidc_disable_pin", 1}}).disablePin);
        CHECK_EQ(
            Oidc::Settings::fromJson({{"oidc_button_label", QString(80, 'x')}}).buttonLabel.size(),
            Oidc::MAX_LABEL);
    }

    // ── Lists ───────────────────────────────────────────────────────────────
    {
        CHECK_EQ(Oidc::parseList(QJsonArray{" a ", "b", "a", "", 3}), QStringList({"a", "b"}));
        CHECK_EQ(Oidc::parseList(QString("a,b\nc,, a")), QStringList({"a", "b", "c"}));
        CHECK(Oidc::parseList(QJsonValue()).isEmpty());
        CHECK(Oidc::parseList(42).isEmpty());
        QJsonArray many;
        for (int i = 0; i < 100; ++i)
            many.append(QString::number(i));
        CHECK_EQ(Oidc::parseList(many).size(), Oidc::MAX_GROUPS);
        // Case is significant: "Admins" and "admins" are two groups.
        CHECK_EQ(Oidc::parseList(QString("Admins,admins")).size(), 2);
    }

    // ── The client secret: environment first ────────────────────────────────
    {
        Oidc::Settings s = usable();
        Oidc::SecretSource src{};
        CHECK(Oidc::effectiveClientSecret(s, env({}), &src).isEmpty());
        CHECK(src == Oidc::SecretSource::None);
        s.clientSecret = QStringLiteral("from-file");
        CHECK_EQ(Oidc::effectiveClientSecret(s, env({}), &src), QString("from-file"));
        CHECK(src == Oidc::SecretSource::Settings);
        CHECK_EQ(Oidc::effectiveClientSecret(s, env({{"MW_OIDC_CLIENT_SECRET", "from-env"}}), &src),
                 QString("from-env"));
        CHECK(src == Oidc::SecretSource::Env);

        QTemporaryDir dir;
        const QString path = dir.path() + "/secret";
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("docker-secret\n");
        f.close();
        CHECK_EQ(Oidc::effectiveClientSecret(s, env({{"MW_OIDC_CLIENT_SECRET_FILE", path}}), &src),
                 QString("docker-secret"));
        CHECK(src == Oidc::SecretSource::EnvFile);
        // The direct variable wins over the file.
        CHECK_EQ(Oidc::effectiveClientSecret(s, env({{"MW_OIDC_CLIENT_SECRET", "e"},
                                                     {"MW_OIDC_CLIENT_SECRET_FILE", path}})),
                 QString("e"));
        // A missing file falls back to settings.json.
        CHECK_EQ(Oidc::effectiveClientSecret(
                     s, env({{"MW_OIDC_CLIENT_SECRET_FILE", dir.path() + "/none"}}), &src),
                 QString("from-file"));
        CHECK(src == Oidc::SecretSource::Settings);
        CHECK(Oidc::processEnv()("MW_OIDC_TEST_NEVER_SET").isEmpty());
    }

    // ── Provider URLs: https, or http to loopback only ──────────────────────
    {
        CHECK(Oidc::isAllowedProviderUrl(QUrl("https://idp.example.com/")));
        CHECK(Oidc::isAllowedProviderUrl(QUrl("https://idp.example.com:8443/realms/x")));
        CHECK(Oidc::isAllowedProviderUrl(QUrl("http://127.0.0.1:49055")));
        CHECK(Oidc::isAllowedProviderUrl(QUrl("http://127.8.9.10/")));
        CHECK(Oidc::isAllowedProviderUrl(QUrl("http://localhost:9000/")));
        CHECK(Oidc::isAllowedProviderUrl(QUrl("http://[::1]:9000/")));
        CHECK(!Oidc::isAllowedProviderUrl(QUrl("http://idp.example.com/")));
        CHECK(!Oidc::isAllowedProviderUrl(QUrl("http://192.0.2.10/")));
        CHECK(!Oidc::isAllowedProviderUrl(QUrl("http://localhost.idp.example.com/")));
        CHECK(!Oidc::isAllowedProviderUrl(QUrl("ftp://idp.example.com/")));
        CHECK(!Oidc::isAllowedProviderUrl(QUrl("https://user:pw@idp.example.com/")));
        CHECK(!Oidc::isAllowedProviderUrl(QUrl("idp.example.com")));
        CHECK(!Oidc::isAllowedProviderUrl(QUrl()));
    }

    // ── public_url: an https origin ─────────────────────────────────────────
    {
        CHECK(Oidc::isAllowedPublicUrl(QUrl("https://mw.example.com")));
        CHECK(Oidc::isAllowedPublicUrl(QUrl("https://mw.example.com/")));
        CHECK(Oidc::isAllowedPublicUrl(QUrl("https://192.0.2.10:8443")));
        CHECK(!Oidc::isAllowedPublicUrl(QUrl("http://mw.example.com")));
        CHECK(!Oidc::isAllowedPublicUrl(QUrl("http://localhost")));
        CHECK(!Oidc::isAllowedPublicUrl(QUrl("https://mw.example.com/app")));
        CHECK(!Oidc::isAllowedPublicUrl(QUrl("https://mw.example.com/?x=1")));
        CHECK(!Oidc::isAllowedPublicUrl(QUrl("https://mw.example.com/#f")));
        CHECK(!Oidc::isAllowedPublicUrl(QUrl("https://a:b@mw.example.com")));

        CHECK_EQ(Oidc::originOf("https://mw.example.com"), QString("https://mw.example.com"));
        CHECK_EQ(Oidc::originOf("https://MW.Example.com:443/"), QString("https://mw.example.com"));
        CHECK_EQ(Oidc::originOf("https://192.0.2.10:8443"), QString("https://192.0.2.10:8443"));
        CHECK_EQ(Oidc::originOf("https://[2001:db8::1]:8443"),
                 QString("https://[2001:db8::1]:8443"));
        CHECK(Oidc::originOf("http://mw.example.com").isEmpty());
        CHECK_EQ(Oidc::redirectUriFor("https://192.0.2.10:8443"),
                 QString("https://192.0.2.10:8443/auth/oidc/callback"));
        CHECK(Oidc::redirectUriFor(QString()).isEmpty());

        // Several origins: the one the request's Host names, or none.
        Oidc::Settings s = usable();
        s.publicUrls = {"https://mw.example.com", "https://192.0.2.10:8443",
                        "https://MW.example.com/", "https://[2001:db8::1]"};
        CHECK_EQ(Oidc::publicOrigins(s),
                 QStringList({"https://mw.example.com", "https://192.0.2.10:8443",
                              "https://[2001:db8::1]"}));
        CHECK_EQ(Oidc::originFor(s, "mw.example.com"), QString("https://mw.example.com"));
        CHECK_EQ(Oidc::originFor(s, "MW.EXAMPLE.COM:443"), QString("https://mw.example.com"));
        CHECK_EQ(Oidc::originFor(s, "192.0.2.10:8443"), QString("https://192.0.2.10:8443"));
        CHECK_EQ(Oidc::originFor(s, "[2001:db8::1]:443"), QString("https://[2001:db8::1]"));
        // Same host, another port; another host; no Host at all: not offered.
        CHECK(Oidc::originFor(s, "192.0.2.10").isEmpty());
        CHECK(Oidc::originFor(s, "192.0.2.10:9443").isEmpty());
        CHECK(Oidc::originFor(s, "mw.example.com:8443").isEmpty());
        CHECK(Oidc::originFor(s, "evil.example").isEmpty());
        CHECK(Oidc::originFor(s, "mw.example.com.evil.example").isEmpty());
        CHECK(Oidc::originFor(s, QString()).isEmpty());
        s.publicUrls.clear();
        CHECK(Oidc::originFor(s, "mw.example.com").isEmpty());
    }

    // ── When the settings make a usable login ───────────────────────────────
    {
        CHECK(Oidc::isUsable(usable()));
        CHECK(Oidc::configProblem(usable()).isEmpty());
        Oidc::Settings s = usable();
        s.enabled = false;
        CHECK_EQ(Oidc::configProblem(s), QString("disabled"));
        s = usable();
        s.issuer = QStringLiteral("http://idp.example.com/");
        CHECK_EQ(Oidc::configProblem(s), QString("issuer"));
        s.issuer.clear();
        CHECK_EQ(Oidc::configProblem(s), QString("issuer"));
        s = usable();
        s.clientId.clear();
        CHECK_EQ(Oidc::configProblem(s), QString("client_id"));
        s = usable();
        s.publicUrls.clear();
        CHECK_EQ(Oidc::configProblem(s), QString("public_url"));
        // One bad entry among good ones is a problem, not a silently dropped origin.
        s.publicUrls = {"https://mw.example.com", "http://192.0.2.10"};
        CHECK_EQ(Oidc::configProblem(s), QString("public_url"));
        s = usable();
        s.scopes = QStringLiteral("profile email");
        CHECK_EQ(Oidc::configProblem(s), QString("scopes"));
        s.scopes = QStringLiteral("openidx profile");
        CHECK_EQ(Oidc::configProblem(s), QString("scopes"));

        // disable_pin only bites while the login it hands over to can run.
        s = usable();
        s.disablePin = true;
        CHECK(Oidc::pinDisabled(s));
        s.enabled = false;
        CHECK(!Oidc::pinDisabled(s));
        s = usable();
        s.disablePin = true;
        s.publicUrls.clear();
        CHECK(!Oidc::pinDisabled(s));
    }

    // ── The authorization request ───────────────────────────────────────────
    {
        // RFC 7636 appendix B's own example.
        CHECK_EQ(Oidc::pkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
                 QString("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));

        QSet<QString> seen;
        for (int i = 0; i < 50; ++i)
            seen.insert(Oidc::randomToken());
        CHECK_EQ(seen.size(), 50);
        CHECK_EQ(Oidc::randomToken().size(), 43); // 32 bytes, unpadded
        CHECK_EQ(Oidc::randomToken(3).size(), 4);
        CHECK(!Oidc::randomToken().contains('=') && !Oidc::randomToken().contains('+'));

        Oidc::Settings s = usable();
        s.clientId = QStringLiteral("odd&id=x");
        const QUrl url =
            Oidc::authorizationUrl("https://idp.example.com/authorize?tenant=t1", s,
                                   "https://mw.example.com/auth/oidc/callback", "st", "no", "ch");
        const QUrlQuery q(url);
        CHECK_EQ(url.host(), QString("idp.example.com"));
        CHECK_EQ(q.queryItemValue("tenant"), QString("t1"));
        CHECK_EQ(q.queryItemValue("response_type"), QString("code"));
        CHECK_EQ(q.queryItemValue("client_id", QUrl::FullyDecoded), QString("odd&id=x"));
        CHECK(!q.hasQueryItem("id")); // the '&' did not split the value
        CHECK_EQ(q.queryItemValue("redirect_uri", QUrl::FullyDecoded),
                 QString("https://mw.example.com/auth/oidc/callback"));
        CHECK_EQ(q.queryItemValue("scope", QUrl::FullyDecoded), QString("openid profile email"));
        CHECK_EQ(q.queryItemValue("state"), QString("st"));
        CHECK_EQ(q.queryItemValue("nonce"), QString("no"));
        CHECK_EQ(q.queryItemValue("code_challenge"), QString("ch"));
        CHECK_EQ(q.queryItemValue("code_challenge_method"), QString("S256"));
    }

    // ── Attempts: single use, bound to the browser, ten minutes ─────────────
    {
        Oidc::PendingLogins logins;
        QString binding;
        Oidc::Attempt a;
        const QString state = logins.begin(1000, kCb, &binding, &a);
        CHECK(!state.isEmpty() && !binding.isEmpty());
        CHECK(state != binding);
        CHECK_EQ(a.verifier.size(), 43);
        CHECK_EQ(a.redirectUri, kCb);
        CHECK(!a.nonce.isEmpty() && a.nonce != a.verifier);
        CHECK_EQ(a.bindingHash,
                 QCryptographicHash::hash(binding.toLatin1(), QCryptographicHash::Sha256));
        CHECK_EQ(logins.size(), 1);

        Oidc::TakeError err{};
        const auto got = logins.take(state, binding, 1001, &err);
        CHECK(got.has_value());
        CHECK(err == Oidc::TakeError::None);
        CHECK_EQ(got->nonce, a.nonce);
        CHECK_EQ(got->verifier, a.verifier);
        // Replay of the same callback.
        CHECK(!logins.take(state, binding, 1002, &err).has_value());
        CHECK(err == Oidc::TakeError::UnknownState);

        // Missing or foreign mw_oidc cookie — and the state is spent anyway.
        const QString s2 = logins.begin(1000, kCb, &binding, nullptr);
        CHECK(!logins.take(s2, QString(), 1001, &err).has_value());
        CHECK(err == Oidc::TakeError::WrongBrowser);
        CHECK(!logins.take(s2, binding, 1001, &err).has_value());
        CHECK(err == Oidc::TakeError::UnknownState);
        const QString s3 = logins.begin(1000, kCb, &binding, nullptr);
        CHECK(!logins.take(s3, binding + "x", 1001, &err).has_value());
        CHECK(err == Oidc::TakeError::WrongBrowser);

        // Expired: TTL is inclusive at 600 s, refused at 601.
        const QString s4 = logins.begin(1000, kCb, &binding, nullptr);
        CHECK(logins.take(s4, binding, 1600, &err).has_value());
        const QString s5 = logins.begin(1000, kCb, &binding, nullptr);
        CHECK(!logins.take(s5, binding, 1601, &err).has_value());
        CHECK(err == Oidc::TakeError::Expired);

        CHECK(!logins.take(QString(), binding, 1000, &err).has_value());
        CHECK(err == Oidc::TakeError::UnknownState);
        CHECK(!logins.take("never-issued", binding, 1000, &err).has_value());
        CHECK(err == Oidc::TakeError::UnknownState);

        // Old attempts are pruned when a new one begins.
        Oidc::PendingLogins prune;
        prune.begin(0, kCb, nullptr, nullptr);
        prune.begin(0, kCb, nullptr, nullptr);
        prune.begin(10'000, kCb, nullptr, nullptr);
        CHECK_EQ(prune.size(), 1);

        // Bounded: a flood evicts the oldest, never grows past the cap.
        Oidc::PendingLogins flood;
        QString firstBinding;
        const QString first = flood.begin(5000, kCb, &firstBinding, nullptr);
        for (int i = 0; i < Oidc::PendingLogins::MAX_PENDING + 10; ++i)
            flood.begin(5001 + i / 100, kCb, nullptr, nullptr);
        CHECK_EQ(flood.size(), Oidc::PendingLogins::MAX_PENDING);
        CHECK(!flood.take(first, firstBinding, 5010).has_value());
    }

    // ── Groups → access ─────────────────────────────────────────────────────
    {
        const QJsonObject c{{"groups", QJsonArray{"streamers", "family"}}};
        CHECK_EQ(Oidc::groupsFromClaims(c, "groups"), QStringList({"streamers", "family"}));
        CHECK_EQ(Oidc::groupsFromClaims({{"groups", "solo"}}, "groups"), QStringList({"solo"}));
        CHECK(Oidc::groupsFromClaims({{"groups", 7}}, "groups").isEmpty());
        CHECK(Oidc::groupsFromClaims({{"groups", QJsonObject{}}}, "groups").isEmpty());
        CHECK_EQ(Oidc::groupsFromClaims({{"roles", QJsonArray{"x", 1}}}, "roles"),
                 QStringList({"x"}));

        Oidc::Settings s = usable();
        // No allow-list: anyone the provider authenticates, nobody admin.
        Oidc::Access a = Oidc::decideAccess(c, s);
        CHECK(a.allowed);
        CHECK(!a.admin);
        CHECK(Oidc::decideAccess({}, s).allowed);

        s.allowedGroups = {"streamers"};
        CHECK(Oidc::decideAccess(c, s).allowed);
        CHECK(!Oidc::decideAccess({{"groups", QJsonArray{"guests"}}}, s).allowed);
        CHECK(!Oidc::decideAccess({}, s).allowed);
        // Exact and case-sensitive.
        CHECK(!Oidc::decideAccess({{"groups", QJsonArray{"Streamers"}}}, s).allowed);
        CHECK(!Oidc::decideAccess({{"groups", QJsonArray{"streamers-x"}}}, s).allowed);

        s.adminGroups = {"mw-admins"};
        a = Oidc::decideAccess({{"groups", QJsonArray{"mw-admins"}}}, s);
        CHECK(a.admin);
        CHECK(a.allowed); // an admin group admits on its own
        a = Oidc::decideAccess(c, s);
        CHECK(a.allowed);
        CHECK(!a.admin);
        // The claim name is configurable.
        s.groupsClaim = QStringLiteral("roles");
        CHECK(!Oidc::decideAccess(c, s).allowed);
        CHECK(Oidc::decideAccess({{"roles", QJsonArray{"mw-admins"}}}, s).admin);
    }

    // ── Admin page updates ──────────────────────────────────────────────────
    {
        Oidc::Settings s;
        // Enabling an incomplete login is refused, and nothing is changed.
        Oidc::UpdateResult r = Oidc::applyUpdate(s, {{"enabled", true}, {"client_id", "x"}});
        CHECK(!r.ok);
        CHECK_EQ(r.error, QString("incomplete"));
        CHECK_EQ(r.problem, QString("issuer"));
        CHECK(!s.enabled);
        CHECK(s.clientId.isEmpty());

        r = Oidc::applyUpdate(s, {{"enabled", true},
                                  {"issuer", "https://idp.example.com/o/mw/"},
                                  {"client_id", " moonlightweb "},
                                  {"client_secret", "s3cret"},
                                  {"public_url", "https://mw.example.com/"},
                                  {"allowed_groups", "streamers, family"},
                                  {"admin_groups", QJsonArray{"mw-admins"}},
                                  {"button_label", "Authentik"},
                                  {"disable_pin", true},
                                  {"session_ttl_hours", 12}});
        CHECK(r.ok);
        CHECK(r.accessChanged); // another provider than none
        CHECK(Oidc::isUsable(s));
        CHECK_EQ(s.clientId, QString("moonlightweb"));
        CHECK_EQ(s.clientSecret, QString("s3cret"));
        CHECK_EQ(s.publicUrls, QStringList({"https://mw.example.com"})); // slash dropped
        CHECK(Oidc::applyUpdate(s, {{"public_url", QJsonArray{"https://mw.example.com",
                                                              "https://192.0.2.10:8443/"}}})
                  .ok);
        CHECK_EQ(s.publicUrls, QStringList({"https://mw.example.com", "https://192.0.2.10:8443"}));
        CHECK(Oidc::applyUpdate(s, {{"public_url", "https://mw.example.com"}}).ok);
        CHECK_EQ(s.publicUrls, QStringList({"https://mw.example.com"}));
        CHECK_EQ(s.allowedGroups, QStringList({"streamers", "family"}));
        CHECK_EQ(s.sessionTtlHours, 12);
        CHECK(s.disablePin);

        // The secret is write-only: absent or empty leaves it, clear removes it.
        CHECK(Oidc::applyUpdate(s, {{"client_secret", ""}}).ok);
        CHECK_EQ(s.clientSecret, QString("s3cret"));
        CHECK(Oidc::applyUpdate(s, {{"button_label", "SSO"}}).ok);
        CHECK_EQ(s.clientSecret, QString("s3cret"));
        r = Oidc::applyUpdate(s, {{"clear_client_secret", true}});
        CHECK(r.ok);
        CHECK(!r.accessChanged); // the same people may sign in
        CHECK(s.clientSecret.isEmpty());

        // What changes who may sign in, and what does not.
        Oidc::Settings t = s;
        CHECK(Oidc::applyUpdate(t, {{"admin_groups", "others"}}).accessChanged);
        t = s;
        CHECK(Oidc::applyUpdate(t, {{"allowed_groups", QJsonArray{}}}).accessChanged);
        t = s;
        CHECK(Oidc::applyUpdate(t, {{"groups_claim", "roles"}}).accessChanged);
        t = s;
        CHECK(Oidc::applyUpdate(t, {{"client_id", "other"}}).accessChanged);
        t = s;
        CHECK(Oidc::applyUpdate(t, {{"enabled", false}}).accessChanged);
        t = s;
        CHECK(!Oidc::applyUpdate(t, {{"session_ttl_hours", 2}, {"disable_pin", false}})
                   .accessChanged);
        t = s;
        CHECK(!Oidc::applyUpdate(t, {{"allowed_groups", "streamers, family, streamers"}})
                   .accessChanged); // the same list, however it is typed

        // Each invalid value names itself and changes nothing.
        const auto refused = [&s](const QJsonObject& body, const char* error) {
            Oidc::Settings before = s;
            const Oidc::UpdateResult res = Oidc::applyUpdate(s, body);
            const bool unchanged = before.issuer == s.issuer && before.publicUrls == s.publicUrls &&
                                   before.sessionTtlHours == s.sessionTtlHours &&
                                   before.scopes == s.scopes && before.enabled == s.enabled;
            return !res.ok && res.error == QLatin1String(error) && unchanged;
        };
        CHECK(refused({{"issuer", "http://idp.example.com/"}}, "invalid_issuer"));
        CHECK(refused({{"issuer", 42}}, "invalid_issuer"));
        CHECK(refused({{"public_url", "http://mw.example.com"}}, "invalid_public_url"));
        CHECK(refused({{"public_url", "https://mw.example.com/sub"}}, "invalid_public_url"));
        CHECK(refused({{"public_url", QJsonArray{"https://mw.example.com", "http://192.0.2.10"}}},
                      "invalid_public_url"));
        CHECK(refused({{"public_url", 7}}, "invalid_public_url"));
        CHECK(refused({{"session_ttl_hours", 0}}, "invalid_session_ttl_hours"));
        CHECK(refused({{"session_ttl_hours", 721}}, "invalid_session_ttl_hours"));
        CHECK(refused({{"session_ttl_hours", 1.5}}, "invalid_session_ttl_hours"));
        CHECK(refused({{"session_ttl_hours", "8"}}, "invalid_session_ttl_hours"));
        CHECK(refused({{"scopes", "profile email"}}, "invalid_scopes"));
        CHECK(refused({{"enabled", "yes"}}, "invalid_enabled"));
        CHECK(refused({{"disable_pin", 1}}, "invalid_disable_pin"));
        CHECK(refused({{"client_secret", 5}}, "invalid_client_secret"));
        // A valid key alongside an invalid one is not half-applied.
        CHECK(refused({{"session_ttl_hours", 6}, {"issuer", "ftp://x"}}, "invalid_issuer"));
        // Emptying the issuer of an enabled login is "incomplete".
        r = Oidc::applyUpdate(s, {{"issuer", ""}});
        CHECK(!r.ok);
        CHECK_EQ(r.error, QString("incomplete"));
        // ...but fine once it is switched off in the same request.
        CHECK(Oidc::applyUpdate(s, {{"issuer", ""}, {"enabled", false}}).ok);
        CHECK(!s.enabled);
        // Empty scopes and claim fall back to their defaults.
        CHECK(Oidc::applyUpdate(s, {{"scopes", ""}, {"groups_claim", " "}}).ok);
        CHECK_EQ(s.scopes, QString("openid profile email"));
        CHECK_EQ(s.groupsClaim, QString("groups"));
    }

    // ── Display name ────────────────────────────────────────────────────────
    {
        CHECK_EQ(Oidc::displayName({{"name", "Alice Liddell"}, {"preferred_username", "alice"}}),
                 QString("Alice Liddell"));
        CHECK_EQ(Oidc::displayName({{"preferred_username", "alice"}, {"email", "a@example.com"}}),
                 QString("alice"));
        CHECK_EQ(Oidc::displayName({{"email", "a@example.com"}, {"sub", "123"}}),
                 QString("a@example.com"));
        CHECK_EQ(Oidc::displayName({{"sub", "123"}}), QString("123"));
        CHECK_EQ(Oidc::displayName({{"name", "  "}, {"sub", "123"}}), QString("123"));
        CHECK_EQ(Oidc::displayName({{"name", QString("Eve\nAdmin‮")}}), QString("EveAdmin"));
        CHECK_EQ(Oidc::displayName({{"name", QString(200, 'a')}}).size(), 64);
        CHECK(Oidc::displayName({}).isEmpty());
    }
}
