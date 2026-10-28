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

#pragma once

#include "common/JwtVerifier.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

/**
 * The three HTTP exchanges of an OpenID Connect login, against the provider
 * the operator configured: discovery, the key set (JWKS), and the code-for-
 * token exchange. Everything it decides is in static functions tested on
 * their own; the network half is tested against a local stand-in.
 *
 * No redirect is ever followed, no reply over MAX_BODY_BYTES is read, every
 * URL — the issuer's and those the discovery document names — must pass
 * Oidc::isAllowedProviderUrl, and nothing a reply carries is logged.
 */
class OidcClient : public QObject
{
    Q_OBJECT

public:
    explicit OidcClient(QObject* parent = nullptr);

    struct Provider
    {
        QString issuer; ///< as the discovery document states it: what `iss` must equal
        QString authorizationEndpoint;
        QString tokenEndpoint;
        QString jwksUri;
    };

    struct TokenRequest
    {
        QString tokenEndpoint;
        QString code;
        QString redirectUri;
        QString codeVerifier;
        QString clientId;
        QString clientSecret; ///< empty: a public client, authenticated by PKCE alone
    };

    /// Failure kinds a caller tells apart: the provider could not be reached or
    /// answered nonsense, or it refused what we sent.
    enum class Failure
    {
        None,
        Unreachable,
        Refused,
    };

    using ProviderCallback = std::function<void(std::optional<Provider>, QString error)>;
    using TokenCallback = std::function<void(QByteArray idToken, Failure failure, QString error)>;
    using VerifyCallback =
        std::function<void(JwtVerifier::Result result, Failure failure, QString error)>;

    static constexpr int TIMEOUT_MS = 6000; // four exchanges fit in HttpServer's 30 s
    static constexpr qint64 MAX_BODY_BYTES = 512 * 1024;
    static constexpr qint64 CACHE_SECS = 3600;

    /// The provider's endpoints, cached per issuer for CACHE_SECS. @p fresh
    /// skips the cache (the admin page's "Test discovery").
    void discover(const QString& issuer, ProviderCallback cb, bool fresh = false);

    /// Spend an authorization code for an ID token.
    void exchangeCode(const TokenRequest& req, TokenCallback cb);

    /// Verify @p idToken against the provider's keys, fetching them when not
    /// cached (or cached over CACHE_SECS ago), and fetching them once more when
    /// the token names a key the cached set does not hold: that is how a
    /// provider's key rotation reaches us.
    void verifyIdToken(const Provider& provider, const QByteArray& idToken,
                       const JwtVerifier::Expectations& expect, VerifyCallback cb);

    /// Forget every cached document and key set (the settings changed).
    void clearCache();

    /// Fetch @p jwksUri and report how many usable keys it holds (admin test).
    void countKeys(const QString& jwksUri, std::function<void(int keys, QString error)> cb);

    // ── Pure parts ───────────────────────────────────────────────────────────

    /// <issuer>/.well-known/openid-configuration, a trailing slash tolerated.
    static QString discoveryUrl(const QString& issuer);

    /// Parse a discovery document fetched for @p configuredIssuer. Its issuer
    /// must be the configured one (a trailing slash aside), and each endpoint
    /// an allowed provider URL.
    static std::optional<Provider> parseDiscovery(const QByteArray& body,
                                                  const QString& configuredIssuer, QString* error);

    /// The id_token of a token endpoint reply, or empty with @p error set.
    static QByteArray parseTokenResponse(const QByteArray& body, QString* error);

    /// application/x-www-form-urlencoded body of the code exchange.
    static QByteArray tokenRequestBody(const TokenRequest& req);

    /// client_secret_basic: "Basic " + base64(urlencode(id) ":" urlencode(secret)),
    /// RFC 6749 §2.3.1. Empty for a public client.
    static QByteArray basicAuthorization(const QString& clientId, const QString& clientSecret);

private:
    struct Fetched
    {
        QByteArray body;
        int status = 0;
        QString error; ///< non-empty on a transport failure or an oversized reply
    };
    using FetchCallback = std::function<void(const Fetched&)>;

    static QNetworkRequest request(const QString& url);
    void finish(QNetworkReply* reply, FetchCallback cb);
    void fetchJson(const QString& url, FetchCallback cb);
    void fetchJwks(const QString& uri, std::function<void(std::optional<QJsonObject>, QString)> cb);

    struct CachedProvider
    {
        Provider provider;
        qint64 at = 0;
    };
    struct CachedJwks
    {
        QJsonObject jwks;
        qint64 at = 0;
    };

    QNetworkAccessManager* m_nam = nullptr;
    QHash<QString, CachedProvider> m_providers; // issuer → document
    QHash<QString, CachedJwks> m_jwks;          // jwks_uri → key set
};
