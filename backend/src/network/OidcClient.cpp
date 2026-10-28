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

#include "OidcClient.h"

#include "server/OidcFlow.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

#include <memory>

namespace {

qint64 nowSecs()
{
    return QDateTime::currentSecsSinceEpoch();
}

QString withoutTrailingSlash(QString s)
{
    if (s.endsWith(QLatin1Char('/'))) s.chop(1);
    return s;
}

QByteArray form(const QString& v)
{
    return QUrl::toPercentEncoding(v);
}

} // namespace

OidcClient::OidcClient(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{}

// ── Pure parts ───────────────────────────────────────────────────────────────

QString OidcClient::discoveryUrl(const QString& issuer)
{
    return withoutTrailingSlash(issuer.trimmed()) +
           QStringLiteral("/.well-known/openid-configuration");
}

std::optional<OidcClient::Provider>
OidcClient::parseDiscovery(const QByteArray& body, const QString& configuredIssuer, QString* error)
{
    const auto fail = [error](const char* why) -> std::optional<Provider> {
        if (error) *error = QString::fromLatin1(why);
        return std::nullopt;
    };
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return fail("discovery_not_json");
    const QJsonObject o = doc.object();

    Provider p;
    p.issuer = o.value(QStringLiteral("issuer")).toString();
    // OpenID Connect Discovery §4.3: the document speaks for the issuer it was
    // fetched for, or for nobody. A trailing slash is the one difference
    // forgiven, since the admin page cannot tell which one a provider uses.
    if (p.issuer.isEmpty() ||
        withoutTrailingSlash(p.issuer) != withoutTrailingSlash(configuredIssuer.trimmed()))
        return fail("issuer_mismatch");

    p.authorizationEndpoint = o.value(QStringLiteral("authorization_endpoint")).toString();
    p.tokenEndpoint = o.value(QStringLiteral("token_endpoint")).toString();
    p.jwksUri = o.value(QStringLiteral("jwks_uri")).toString();
    for (const QString* url : {&p.authorizationEndpoint, &p.tokenEndpoint, &p.jwksUri})
        if (url->isEmpty() || !Oidc::isAllowedProviderUrl(QUrl(*url)))
            return fail("endpoint_not_allowed");

    // We always send PKCE S256; a provider that says it cannot check it would
    // silently drop the protection.
    const QJsonValue methods = o.value(QStringLiteral("code_challenge_methods_supported"));
    if (methods.isArray() && !methods.toArray().contains(QStringLiteral("S256")))
        return fail("pkce_s256_unsupported");

    if (error) error->clear();
    return p;
}

QByteArray OidcClient::parseTokenResponse(const QByteArray& body, QString* error)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    const QString idToken = o.value(QStringLiteral("id_token")).toString();
    if (idToken.isEmpty()) {
        // The provider's own error code, when it gave one (invalid_grant...).
        const QString code = o.value(QStringLiteral("error")).toString();
        if (error) *error = code.isEmpty() ? QStringLiteral("no_id_token") : code.left(64);
        return {};
    }
    if (error) error->clear();
    return idToken.toLatin1();
}

QByteArray OidcClient::tokenRequestBody(const TokenRequest& req)
{
    QByteArray body = "grant_type=authorization_code&code=" + form(req.code) +
                      "&redirect_uri=" + form(req.redirectUri) +
                      "&code_verifier=" + form(req.codeVerifier);
    // A public client names itself in the body; a confidential one does so in
    // the Authorization header instead, and only there.
    if (req.clientSecret.isEmpty()) body += "&client_id=" + form(req.clientId);
    return body;
}

QByteArray OidcClient::basicAuthorization(const QString& clientId, const QString& clientSecret)
{
    if (clientSecret.isEmpty()) return {};
    return "Basic " + (form(clientId) + ':' + form(clientSecret)).toBase64();
}

// ── Network ──────────────────────────────────────────────────────────────────

QNetworkRequest OidcClient::request(const QString& url)
{
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Accept", "application/json");
    req.setRawHeader("User-Agent", "MoonlightWeb");
    req.setTransferTimeout(TIMEOUT_MS);
    // A redirect would take us to a URL nobody checked against the rules.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    return req;
}

void OidcClient::finish(QNetworkReply* reply, FetchCallback cb)
{
    auto oversized = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply, oversized](qint64 received, qint64) {
                if (received > MAX_BODY_BYTES && !*oversized) {
                    *oversized = true;
                    reply->abort();
                }
            });
    connect(reply, &QNetworkReply::finished, this, [reply, cb, oversized]() {
        reply->deleteLater();
        Fetched f;
        f.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (*oversized) {
            f.error = QStringLiteral("reply_too_large");
        } else if (f.status == 0) {
            // No HTTP answer at all: refused, timed out, TLS failed.
            f.error = QStringLiteral("unreachable");
        } else {
            f.body = reply->read(MAX_BODY_BYTES + 1);
            if (f.body.size() > MAX_BODY_BYTES) {
                f.body.clear();
                f.error = QStringLiteral("reply_too_large");
            }
        }
        cb(f);
    });
}

void OidcClient::fetchJson(const QString& url, FetchCallback cb)
{
    if (!Oidc::isAllowedProviderUrl(QUrl(url))) {
        Fetched f;
        f.error = QStringLiteral("url_not_allowed");
        cb(f);
        return;
    }
    finish(m_nam->get(request(url)), std::move(cb));
}

void OidcClient::discover(const QString& issuer, ProviderCallback cb, bool fresh)
{
    const QString key = issuer.trimmed();
    if (!fresh) {
        const auto it = m_providers.constFind(key);
        if (it != m_providers.constEnd() && nowSecs() - it->at < CACHE_SECS) {
            cb(it->provider, QString());
            return;
        }
    }
    QPointer<OidcClient> self(this);
    fetchJson(discoveryUrl(key), [self, key, cb](const Fetched& f) {
        if (!self) return;
        if (!f.error.isEmpty()) return cb(std::nullopt, f.error);
        if (f.status != 200) return cb(std::nullopt, QStringLiteral("http_%1").arg(f.status));
        QString error;
        const auto provider = parseDiscovery(f.body, key, &error);
        if (provider) self->m_providers.insert(key, {*provider, nowSecs()});
        cb(provider, error);
    });
}

void OidcClient::exchangeCode(const TokenRequest& req, TokenCallback cb)
{
    if (!Oidc::isAllowedProviderUrl(QUrl(req.tokenEndpoint))) {
        cb({}, Failure::Unreachable, QStringLiteral("url_not_allowed"));
        return;
    }
    QNetworkRequest r = request(req.tokenEndpoint);
    r.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    const QByteArray auth = basicAuthorization(req.clientId, req.clientSecret);
    if (!auth.isEmpty()) r.setRawHeader("Authorization", auth);
    r.setRawHeader("Cache-Control", "no-store");

    finish(m_nam->post(r, tokenRequestBody(req)), [cb](const Fetched& f) {
        if (!f.error.isEmpty()) return cb({}, Failure::Unreachable, f.error);
        QString error;
        const QByteArray idToken = parseTokenResponse(f.body, &error);
        if (f.status != 200 || idToken.isEmpty()) {
            if (error.isEmpty()) error = QStringLiteral("http_%1").arg(f.status);
            // 4xx is the provider saying no (a spent code, a wrong secret); a
            // 5xx or a 200 without a token is the provider being broken.
            const Failure kind =
                f.status >= 400 && f.status < 500 ? Failure::Refused : Failure::Unreachable;
            return cb({}, kind, error);
        }
        cb(idToken, Failure::None, QString());
    });
}

void OidcClient::fetchJwks(const QString& uri,
                           std::function<void(std::optional<QJsonObject>, QString)> cb)
{
    QPointer<OidcClient> self(this);
    fetchJson(uri, [self, uri, cb](const Fetched& f) {
        if (!self) return;
        if (!f.error.isEmpty()) return cb(std::nullopt, f.error);
        if (f.status != 200) return cb(std::nullopt, QStringLiteral("http_%1").arg(f.status));
        const QJsonDocument doc = QJsonDocument::fromJson(f.body);
        if (!doc.isObject() || !doc.object().value(QStringLiteral("keys")).isArray())
            return cb(std::nullopt, QStringLiteral("jwks_not_json"));
        self->m_jwks.insert(uri, {doc.object(), nowSecs()});
        cb(doc.object(), QString());
    });
}

void OidcClient::verifyIdToken(const Provider& provider, const QByteArray& idToken,
                               const JwtVerifier::Expectations& expect, VerifyCallback cb)
{
    JwtVerifier::Expectations e = expect;
    e.issuer = provider.issuer;
    const QString uri = provider.jwksUri;

    QPointer<OidcClient> self(this);
    const auto fetchAndVerify = [self, uri, idToken, e, cb]() {
        if (!self) return;
        self->fetchJwks(uri, [idToken, e, cb](std::optional<QJsonObject> jwks, QString error) {
            if (!jwks) return cb({}, Failure::Unreachable, error);
            const JwtVerifier::Result r = JwtVerifier::verify(idToken, *jwks, e);
            cb(r, Failure::None, QString());
        });
    };

    const auto it = m_jwks.constFind(uri);
    if (it == m_jwks.constEnd() || nowSecs() - it->at >= CACHE_SECS) {
        fetchAndVerify();
        return;
    }
    const JwtVerifier::Result r = JwtVerifier::verify(idToken, it->jwks, e);
    // One more look, only for a kid we have never seen: the provider rotated.
    // Any other refusal stands — refetching would not change a bad signature.
    if (r.error == JwtVerifier::Error::UnknownKey) {
        fetchAndVerify();
        return;
    }
    cb(r, Failure::None, QString());
}

void OidcClient::countKeys(const QString& jwksUri, std::function<void(int, QString)> cb)
{
    fetchJwks(jwksUri, [cb](std::optional<QJsonObject> jwks, QString error) {
        if (!jwks) return cb(0, error);
        int n = 0;
        for (const QJsonValue& k : jwks->value(QStringLiteral("keys")).toArray())
            if (!JwtVerifier::jwkToSpki(k.toObject()).isEmpty()) ++n;
        cb(n, QString());
    });
}

void OidcClient::clearCache()
{
    m_providers.clear();
    m_jwks.clear();
}
