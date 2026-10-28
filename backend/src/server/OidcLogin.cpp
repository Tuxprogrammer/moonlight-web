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

#include "OidcLogin.h"

#include "common/Logger.h"
#include "network/OidcClient.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"

#include <QDateTime>
#include <QPointer>

OidcLogin::OidcLogin(AppSettings& settings, AuthManager& auth, OidcClient& client, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_auth(auth)
    , m_client(client)
    , m_now([] { return QDateTime::currentSecsSinceEpoch(); })
    , m_env(Oidc::processEnv())
{}

QByteArray OidcLogin::landingPage()
{
    // No script: the CSP allows none inline. A meta refresh is a navigation
    // this page starts, so it is same-site and carries the SameSite=Strict
    // session cookie a redirect at the end of the provider's chain would not.
    return QByteArrayLiteral("<!doctype html><html><head><meta charset=\"utf-8\">"
                             "<meta http-equiv=\"refresh\" content=\"0;url=/\">"
                             "<title>MoonlightWeb</title></head>"
                             "<body><p><a href=\"/\">MoonlightWeb</a></p></body></html>\n");
}

void OidcLogin::refuse(const Request& req, const QString& code, bool counted,
                       const ReplyCallback& done)
{
    // Counted refusals are the ones a caller can cause on purpose: a forged or
    // replayed callback, a token that does not verify, a user outside the
    // allowed groups. The provider being down is nobody's attempt.
    if (counted) m_auth.recordOidcAttempt(req.attemptAddress, false);
    Logger::warning(QStringLiteral("[OIDC] Sign-in refused for %1: %2")
                        .arg(AuthManager::cleanClientAddress(req.clientAddress), code));
    Reply r;
    r.status = 302;
    r.location = QStringLiteral("/?oidc_error=") + code;
    r.error = code;
    r.authFailure = counted;
    done(r);
}

void OidcLogin::start(const Request& req, ReplyCallback done)
{
    const Oidc::Settings s = m_settings.oidcSettings();
    // Not offered through the tunnel (those visitors keep the PIN) nor in a booth.
    if (req.closed || !Oidc::isUsable(s)) {
        Reply r;
        r.status = 404;
        done(r);
        return;
    }

    // Offered only on a configured origin: the provider sends the browser back
    // to the redirect_uri registered for it, and both cookies live there.
    const QString origin = Oidc::originFor(s, req.host);
    if (origin.isEmpty()) {
        Logger::warning(QStringLiteral("[OIDC] Sign-in asked for under '%1', not a public_url")
                            .arg(req.host.left(128)));
        Reply r;
        r.status = 403;
        r.error = QStringLiteral("origin_not_configured");
        done(r);
        return;
    }

    if (m_auth.oidcLockout(req.attemptAddress) > 0) {
        refuse(req, QStringLiteral("rate_limited"), false, done);
        return;
    }

    QPointer<OidcLogin> self(this);
    m_client.discover(s.issuer, [self, s, req, origin, done](
                                    std::optional<OidcClient::Provider> provider, QString error) {
        if (!self) return;
        if (!provider) {
            Logger::warning(QStringLiteral("[OIDC] Discovery failed: %1").arg(error));
            self->refuse(req, QStringLiteral("idp_unreachable"), false, done);
            return;
        }
        QString binding;
        Oidc::Attempt attempt;
        const QString redirectUri = Oidc::redirectUriFor(origin);
        const QString state = self->m_pending.begin(self->m_now(), redirectUri, &binding, &attempt);

        Reply r;
        r.location = Oidc::authorizationUrl(provider->authorizationEndpoint, s, redirectUri, state,
                                            attempt.nonce, Oidc::pkceChallenge(attempt.verifier))
                         .toString(QUrl::FullyEncoded);
        // Lax, so the provider's top-level redirect back carries it; scoped to
        // the two routes, so it travels with nothing else.
        r.setCookie = QStringLiteral("mw_oidc=%1; HttpOnly; Secure; SameSite=Lax; "
                                     "Path=/auth/oidc; Max-Age=%2")
                          .arg(binding)
                          .arg(Oidc::PendingLogins::TTL_SECS);
        done(r);
    });
}

void OidcLogin::callback(const Request& req, ReplyCallback done)
{
    const Oidc::Settings s = m_settings.oidcSettings();
    if (req.closed || !Oidc::isUsable(s)) {
        Reply r;
        r.status = 404;
        done(r);
        return;
    }
    if (m_auth.oidcLockout(req.attemptAddress) > 0) {
        refuse(req, QStringLiteral("rate_limited"), false, done);
        return;
    }

    // Spent first, whatever follows: a callback URL works once, in the browser
    // that started it, within ten minutes.
    Oidc::TakeError takeError{};
    const auto attempt = m_pending.take(req.query.value(QStringLiteral("state")), req.bindingCookie,
                                        m_now(), &takeError);
    if (!attempt) {
        Logger::warning(QStringLiteral("[OIDC] Callback state refused (%1)")
                            .arg(takeError == Oidc::TakeError::Expired        ? "expired"
                                 : takeError == Oidc::TakeError::WrongBrowser ? "wrong browser"
                                                                              : "unknown"));
        refuse(req, QStringLiteral("invalid_state"), true, done);
        return;
    }
    // Finished on the origin it started on, and nowhere else: the code was
    // issued for that redirect_uri, and the session cookie must land there.
    if (Oidc::redirectUriFor(Oidc::originFor(s, req.host)) != attempt->redirectUri) {
        refuse(req, QStringLiteral("wrong_origin"), true, done);
        return;
    }
    // The user declined at the provider, or the provider could not serve us.
    const QString code = req.query.value(QStringLiteral("code"));
    if (req.query.contains(QStringLiteral("error")) || code.isEmpty()) {
        Logger::warning(QStringLiteral("[OIDC] Provider answered '%1'")
                            .arg(req.query.value(QStringLiteral("error")).left(64)));
        refuse(req, QStringLiteral("idp_error"), false, done);
        return;
    }

    QPointer<OidcLogin> self(this);
    const Oidc::Attempt a = *attempt;
    m_client.discover(s.issuer, [self, s, req, done, a, code](
                                    std::optional<OidcClient::Provider> provider, QString error) {
        if (!self) return;
        if (!provider) {
            Logger::warning(QStringLiteral("[OIDC] Discovery failed: %1").arg(error));
            self->refuse(req, QStringLiteral("idp_unreachable"), false, done);
            return;
        }

        OidcClient::TokenRequest tr;
        tr.tokenEndpoint = provider->tokenEndpoint;
        tr.code = code;
        tr.redirectUri = a.redirectUri;
        tr.codeVerifier = a.verifier;
        tr.clientId = s.clientId;
        tr.clientSecret = Oidc::effectiveClientSecret(s, self->m_env);

        const OidcClient::Provider p = *provider;
        self->m_client.exchangeCode(tr, [self, s, req, done, a, p](QByteArray idToken,
                                                                   OidcClient::Failure failure,
                                                                   QString error) {
            if (!self) return;
            if (failure != OidcClient::Failure::None) {
                Logger::warning(QStringLiteral("[OIDC] Code exchange failed: %1").arg(error));
                const bool refused = failure == OidcClient::Failure::Refused;
                self->refuse(req,
                             refused ? QStringLiteral("token_exchange")
                                     : QStringLiteral("idp_unreachable"),
                             refused, done);
                return;
            }

            JwtVerifier::Expectations e;
            e.audience = s.clientId;
            e.nonce = a.nonce;
            e.now = self->m_now();
            self->m_client.verifyIdToken(
                p, idToken, e,
                [self, s, req, done](JwtVerifier::Result r, OidcClient::Failure failure,
                                     QString error) {
                    if (!self) return;
                    if (failure != OidcClient::Failure::None) {
                        Logger::warning(
                            QStringLiteral("[OIDC] Key set unavailable: %1").arg(error));
                        self->refuse(req, QStringLiteral("idp_unreachable"), false, done);
                        return;
                    }
                    if (!r.ok()) {
                        Logger::warning(QStringLiteral("[OIDC] ID token refused: %1")
                                            .arg(JwtVerifier::errorName(r.error)));
                        self->refuse(req, QStringLiteral("invalid_token"), true, done);
                        return;
                    }

                    const QString subject = r.claims.value(QStringLiteral("sub")).toString();
                    const QString name = Oidc::displayName(r.claims);
                    const Oidc::Access access = Oidc::decideAccess(r.claims, s);
                    if (!access.allowed) {
                        Logger::warning(QStringLiteral("[OIDC] '%1' (%2) is in no allowed group")
                                            .arg(name, subject));
                        self->refuse(req, QStringLiteral("not_allowed"), true, done);
                        return;
                    }

                    const qint64 ttl = static_cast<qint64>(s.sessionTtlHours) * 3600;
                    const QString token = self->m_auth.createOidcSession(req.clientAddress, subject,
                                                                         name, access.admin, ttl);
                    self->m_auth.recordOidcAttempt(req.attemptAddress, true);

                    Reply ok;
                    ok.status = 200;
                    ok.sessionToken = token;
                    // Same cookie as a PIN login's, its lifetime the session's own.
                    ok.setCookie = QStringLiteral("mw_session=%1; HttpOnly; Secure; Path=/; "
                                                  "SameSite=Strict; Max-Age=%2")
                                       .arg(token)
                                       .arg(ttl);
                    done(ok);
                });
        });
    });
}
