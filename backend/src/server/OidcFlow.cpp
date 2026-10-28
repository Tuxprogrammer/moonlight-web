/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "OidcFlow.h"

#include "RequestGuard.h"

#include <QCryptographicHash>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace Oidc {

namespace {

QByteArray sha256(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QString b64u(const QByteArray& data)
{
    return QString::fromLatin1(
        data.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

bool isLoopbackHost(const QString& host)
{
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) return true;
    const QHostAddress addr(host);
    return !addr.isNull() && addr.isLoopback();
}

QString enc(const QString& v)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(v));
}

} // namespace

// ── Settings ─────────────────────────────────────────────────────────────────

Settings Settings::fromJson(const QJsonObject& o)
{
    Settings s;
    s.enabled = o.value(QStringLiteral("oidc_enabled")).toBool(false);
    s.issuer = o.value(QStringLiteral("oidc_issuer")).toString().trimmed();
    s.clientId = o.value(QStringLiteral("oidc_client_id")).toString().trimmed();
    s.clientSecret = o.value(QStringLiteral("oidc_client_secret")).toString();
    const QString scopes = o.value(QStringLiteral("oidc_scopes")).toString().simplified();
    if (!scopes.isEmpty()) s.scopes = scopes;
    const QString claim = o.value(QStringLiteral("oidc_groups_claim")).toString().trimmed();
    if (!claim.isEmpty()) s.groupsClaim = claim;
    s.allowedGroups = parseList(o.value(QStringLiteral("oidc_allowed_groups")));
    s.adminGroups = parseList(o.value(QStringLiteral("oidc_admin_groups")));
    s.buttonLabel =
        o.value(QStringLiteral("oidc_button_label")).toString().trimmed().left(MAX_LABEL);
    s.disablePin = o.value(QStringLiteral("oidc_disable_pin")).toBool(false);
    const int ttl = o.value(QStringLiteral("oidc_session_ttl_hours")).toInt(24);
    s.sessionTtlHours = (ttl >= MIN_TTL_HOURS && ttl <= MAX_TTL_HOURS) ? ttl : 24;
    // One origin as a string, as written before it took a list, or several.
    for (QString url : parseList(o.value(QStringLiteral("public_url")))) {
        if (url.endsWith(QLatin1Char('/'))) url.chop(1);
        s.publicUrls << url;
    }
    return s;
}

void Settings::writeTo(QJsonObject& o) const
{
    o[QStringLiteral("oidc_enabled")] = enabled;
    o[QStringLiteral("oidc_issuer")] = issuer;
    o[QStringLiteral("oidc_client_id")] = clientId;
    o[QStringLiteral("oidc_client_secret")] = clientSecret;
    o[QStringLiteral("oidc_scopes")] = scopes;
    o[QStringLiteral("oidc_groups_claim")] = groupsClaim;
    o[QStringLiteral("oidc_allowed_groups")] = QJsonArray::fromStringList(allowedGroups);
    o[QStringLiteral("oidc_admin_groups")] = QJsonArray::fromStringList(adminGroups);
    o[QStringLiteral("oidc_button_label")] = buttonLabel;
    o[QStringLiteral("oidc_disable_pin")] = disablePin;
    o[QStringLiteral("oidc_session_ttl_hours")] = sessionTtlHours;
    o[QStringLiteral("public_url")] = QJsonArray::fromStringList(publicUrls);
}

EnvReader processEnv()
{
    return [](const char* name) { return qEnvironmentVariable(name); };
}

QString effectiveClientSecret(const Settings& s, const EnvReader& env, SecretSource* source)
{
    const auto said = [source](SecretSource v) {
        if (source) *source = v;
    };
    const QString direct = env ? env("MW_OIDC_CLIENT_SECRET") : QString();
    if (!direct.isEmpty()) {
        said(SecretSource::Env);
        return direct;
    }
    const QString path = env ? env("MW_OIDC_CLIENT_SECRET_FILE") : QString();
    if (!path.isEmpty()) {
        QFile f(path);
        // Docker secrets end with a newline the provider did not issue.
        const QString fromFile =
            f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(4096)).trimmed() : QString();
        if (!fromFile.isEmpty()) {
            said(SecretSource::EnvFile);
            return fromFile;
        }
    }
    said(s.clientSecret.isEmpty() ? SecretSource::None : SecretSource::Settings);
    return s.clientSecret;
}

bool isAllowedProviderUrl(const QUrl& url)
{
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()) return false;
    if (url.scheme() == QLatin1String("https")) return true;
    return url.scheme() == QLatin1String("http") && isLoopbackHost(url.host());
}

bool isAllowedPublicUrl(const QUrl& url)
{
    if (!url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty())
        return false;
    if (!url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return false;
    return url.path().isEmpty() || url.path() == QLatin1String("/");
}

QString originOf(const QString& value)
{
    const QUrl url(value.trimmed());
    if (!isAllowedPublicUrl(url)) return {};
    QString host = url.host().toLower();
    if (host.contains(QLatin1Char(':'))) host = QLatin1Char('[') + host + QLatin1Char(']'); // IPv6
    const int port = url.port();
    return port > 0 && port != 443 ? QStringLiteral("https://%1:%2").arg(host).arg(port)
                                   : QStringLiteral("https://%1").arg(host);
}

QStringList publicOrigins(const Settings& s)
{
    QStringList out;
    for (const QString& url : s.publicUrls) {
        const QString origin = originOf(url);
        if (!origin.isEmpty() && !out.contains(origin)) out << origin;
    }
    return out;
}

QString originFor(const Settings& s, const QString& hostHeader)
{
    const QString host = RequestGuard::normalizeAuthority(hostHeader);
    if (host.isEmpty()) return {};
    for (const QString& origin : publicOrigins(s))
        if (RequestGuard::normalizeAuthority(origin) == host) return origin;
    return {};
}

QString redirectUriFor(const QString& origin)
{
    return origin.isEmpty() ? QString() : origin + QStringLiteral("/auth/oidc/callback");
}

QString configProblem(const Settings& s)
{
    if (!s.enabled) return QStringLiteral("disabled");
    if (s.issuer.isEmpty() || !isAllowedProviderUrl(QUrl(s.issuer)))
        return QStringLiteral("issuer");
    if (s.clientId.isEmpty()) return QStringLiteral("client_id");
    // Every entry must be one: a typo would otherwise silently drop an origin.
    if (s.publicUrls.isEmpty()) return QStringLiteral("public_url");
    for (const QString& url : s.publicUrls)
        if (originOf(url).isEmpty()) return QStringLiteral("public_url");
    if (!s.scopes.split(QLatin1Char(' ')).contains(QStringLiteral("openid")))
        return QStringLiteral("scopes");
    return {};
}

QStringList parseList(const QJsonValue& v)
{
    QStringList raw;
    if (v.isArray()) {
        for (const QJsonValue& e : v.toArray())
            if (e.isString()) raw << e.toString();
    } else if (v.isString()) {
        static const QRegularExpression sep(QStringLiteral("[,\\n]"));
        raw = v.toString().split(sep);
    }
    QStringList out;
    for (const QString& r : raw) {
        const QString g = r.trimmed();
        if (g.isEmpty() || out.contains(g)) continue;
        out << g;
        if (out.size() >= MAX_GROUPS) break;
    }
    return out;
}

UpdateResult applyUpdate(Settings& s, const QJsonObject& body)
{
    UpdateResult res;
    Settings next = s;
    const auto bad = [&res](const char* key) {
        res.error = QStringLiteral("invalid_") + QLatin1String(key);
        return res;
    };
    const auto has = [&body](const char* key) { return body.contains(QLatin1String(key)); };
    const auto value = [&body](const char* key) { return body.value(QLatin1String(key)); };

    for (const char* key : {"enabled", "disable_pin", "clear_client_secret"})
        if (has(key) && !value(key).isBool()) return bad(key);
    for (const char* key :
         {"issuer", "client_id", "client_secret", "scopes", "groups_claim", "button_label"})
        if (has(key) && !value(key).isString()) return bad(key);

    if (has("enabled")) next.enabled = value("enabled").toBool();
    if (has("disable_pin")) next.disablePin = value("disable_pin").toBool();
    if (has("issuer")) {
        next.issuer = value("issuer").toString().trimmed();
        if (!next.issuer.isEmpty() && !isAllowedProviderUrl(QUrl(next.issuer)))
            return bad("issuer");
    }
    if (has("client_id")) next.clientId = value("client_id").toString().trimmed();
    if (value("clear_client_secret").toBool(false)) next.clientSecret.clear();
    if (has("client_secret") && !value("client_secret").toString().isEmpty())
        next.clientSecret = value("client_secret").toString();
    if (has("scopes")) {
        const QString scopes = value("scopes").toString().simplified();
        next.scopes = scopes.isEmpty() ? Settings().scopes : scopes;
        if (!next.scopes.split(QLatin1Char(' ')).contains(QStringLiteral("openid")))
            return bad("scopes");
    }
    if (has("groups_claim")) {
        const QString claim = value("groups_claim").toString().trimmed();
        next.groupsClaim = claim.isEmpty() ? Settings().groupsClaim : claim;
    }
    if (has("allowed_groups")) next.allowedGroups = parseList(value("allowed_groups"));
    if (has("admin_groups")) next.adminGroups = parseList(value("admin_groups"));
    if (has("button_label"))
        next.buttonLabel = value("button_label").toString().trimmed().left(MAX_LABEL);
    if (has("session_ttl_hours")) {
        const QJsonValue v = value("session_ttl_hours");
        const double h = v.toDouble(-1);
        if (!v.isDouble() || h != static_cast<int>(h) || h < MIN_TTL_HOURS || h > MAX_TTL_HOURS)
            return bad("session_ttl_hours");
        next.sessionTtlHours = static_cast<int>(h);
    }
    if (has("public_url")) {
        if (!value("public_url").isString() && !value("public_url").isArray())
            return bad("public_url");
        next.publicUrls.clear();
        for (QString url : parseList(value("public_url"))) {
            if (url.endsWith(QLatin1Char('/'))) url.chop(1);
            if (!isAllowedPublicUrl(QUrl(url))) return bad("public_url");
            if (!next.publicUrls.contains(url)) next.publicUrls << url;
        }
    }

    if (next.enabled && !isUsable(next)) {
        res.error = QStringLiteral("incomplete");
        res.problem = configProblem(next);
        return res;
    }

    res.accessChanged = (s.enabled && !next.enabled) || s.issuer != next.issuer ||
                        s.clientId != next.clientId || s.groupsClaim != next.groupsClaim ||
                        s.allowedGroups != next.allowedGroups || s.adminGroups != next.adminGroups;
    s = next;
    res.ok = true;
    return res;
}

// ── The authorization request ────────────────────────────────────────────────

QString randomToken(int bytes)
{
    QByteArray buf(bytes, Qt::Uninitialized);
    // system() is the platform CSPRNG.
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(buf.data()), bytes / 4);
    for (int i = bytes - bytes % 4; i < bytes; ++i)
        buf[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
    return b64u(buf);
}

QString pkceChallenge(const QString& verifier)
{
    return b64u(sha256(verifier.toLatin1()));
}

QUrl authorizationUrl(const QString& authorizationEndpoint, const Settings& s,
                      const QString& redirectUri, const QString& state, const QString& nonce,
                      const QString& challenge)
{
    QUrl url(authorizationEndpoint);
    // Each value percent-encoded here, so a client id or scope carrying '&' or
    // '=' cannot add a parameter of its own.
    QStringList items;
    const QString existing = url.query(QUrl::FullyEncoded);
    if (!existing.isEmpty()) items << existing;
    items << QStringLiteral("response_type=code") << QStringLiteral("client_id=") + enc(s.clientId)
          << QStringLiteral("redirect_uri=") + enc(redirectUri)
          << QStringLiteral("scope=") + enc(s.scopes) << QStringLiteral("state=") + enc(state)
          << QStringLiteral("nonce=") + enc(nonce)
          << QStringLiteral("code_challenge=") + enc(challenge)
          << QStringLiteral("code_challenge_method=S256");
    url.setQuery(items.join(QLatin1Char('&')), QUrl::StrictMode);
    return url;
}

// ── Login attempts ───────────────────────────────────────────────────────────

void PendingLogins::prune(qint64 now)
{
    for (auto it = m_attempts.begin(); it != m_attempts.end();) {
        if (now - it->createdAt > TTL_SECS)
            it = m_attempts.erase(it);
        else
            ++it;
    }
}

QString PendingLogins::begin(qint64 now, const QString& redirectUri, QString* binding,
                             Attempt* attempt)
{
    prune(now);
    // Unauthenticated callers can start attempts, so the table is bounded: the
    // oldest one gives way. A flood costs its own attempts, nobody else's for
    // long — a real user starts again with one click.
    while (m_attempts.size() >= MAX_PENDING) {
        auto oldest = m_attempts.begin();
        for (auto it = m_attempts.begin(); it != m_attempts.end(); ++it)
            if (it->createdAt < oldest->createdAt) oldest = it;
        m_attempts.erase(oldest);
    }

    const QString cookie = randomToken();
    Attempt a;
    a.bindingHash = sha256(cookie.toLatin1());
    a.nonce = randomToken();
    a.verifier = randomToken(); // 43 characters, inside RFC 7636's 43..128
    a.redirectUri = redirectUri;
    a.createdAt = now;

    QString state = randomToken();
    while (m_attempts.contains(state))
        state = randomToken();
    m_attempts.insert(state, a);

    if (binding) *binding = cookie;
    if (attempt) *attempt = a;
    return state;
}

std::optional<Attempt> PendingLogins::take(const QString& state, const QString& binding, qint64 now,
                                           TakeError* error)
{
    const auto fail = [error](TakeError e) -> std::optional<Attempt> {
        if (error) *error = e;
        return std::nullopt;
    };
    auto it = m_attempts.find(state);
    if (state.isEmpty() || it == m_attempts.end()) return fail(TakeError::UnknownState);
    const Attempt a = *it;
    m_attempts.erase(it); // spent, whatever is decided below

    if (now - a.createdAt > TTL_SECS) return fail(TakeError::Expired);
    if (binding.isEmpty() || !constantTimeEquals(sha256(binding.toLatin1()), a.bindingHash))
        return fail(TakeError::WrongBrowser);
    if (error) *error = TakeError::None;
    return a;
}

// ── What the ID token grants ─────────────────────────────────────────────────

QStringList groupsFromClaims(const QJsonObject& claims, const QString& claimName)
{
    const QJsonValue v = claims.value(claimName);
    QStringList out;
    if (v.isString()) {
        out << v.toString();
    } else if (v.isArray()) {
        for (const QJsonValue& g : v.toArray())
            if (g.isString()) out << g.toString();
    }
    return out;
}

Access decideAccess(const QJsonObject& claims, const Settings& s)
{
    Access a;
    a.groups = groupsFromClaims(claims, s.groupsClaim);
    const auto inAny = [&a](const QStringList& list) {
        for (const QString& g : a.groups)
            if (list.contains(g)) return true;
        return false;
    };
    a.admin = inAny(s.adminGroups);
    a.allowed = s.allowedGroups.isEmpty() || inAny(s.allowedGroups) || a.admin;
    return a;
}

QString displayName(const QJsonObject& claims)
{
    for (const char* key : {"name", "preferred_username", "email", "sub"}) {
        QString v = claims.value(QLatin1String(key)).toString();
        v.removeIf([](QChar c) {
            return c.category() == QChar::Other_Control || c.category() == QChar::Other_Format;
        });
        v = v.simplified().left(64);
        if (!v.isEmpty()) return v;
    }
    return {};
}

} // namespace Oidc
