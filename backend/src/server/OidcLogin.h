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

#include "server/OidcFlow.h"

#include <QMap>
#include <QObject>
#include <QString>

#include <functional>

class AppSettings;
class AuthManager;
class OidcClient;

/**
 * The two browser routes of an OpenID Connect login, without the HTTP server:
 * GET /auth/oidc/start and GET /auth/oidc/callback take a Request and answer a
 * Reply, so the whole flow — state, cookie binding, code exchange, token
 * check, groups, session — runs in tests against a stand-in provider
 * (tests/test_oidc_flow.cpp). OidcRoutes maps them onto HttpRequest and
 * HttpResponse. See docs/design/oidc-login.md for the flow and its reasons.
 */
class OidcLogin : public QObject
{
    Q_OBJECT

public:
    OidcLogin(AppSettings& settings, AuthManager& auth, OidcClient& client,
              QObject* parent = nullptr);

    struct Request
    {
        QString host;           ///< the Host header
        QString clientAddress;  ///< socket peer, stored on the session
        QString attemptAddress; ///< AuthManager::attemptAddress, for the rate limit
        /// Through the rendezvous tunnel, or under --noadmin: the door is not
        /// there. A tunnelled page is on the introduction server's origin, where
        /// no redirect_uri can point; a booth lets no new device in.
        bool closed = false;
        QMap<QString, QString> query;
        QString bindingCookie; ///< value of mw_oidc, empty when absent
    };

    struct Reply
    {
        int status = 302;
        QString location;         ///< for a redirect
        QString setCookie;        ///< at most one: mw_oidc on start, mw_session on success
        QString error;            ///< the oidc_error code of a refusal, "" otherwise
        QString sessionToken;     ///< raw token of the session made (tests read it)
        bool authFailure = false; ///< counted against the caller: feed ConnectionGuard
    };
    using ReplyCallback = std::function<void(const Reply&)>;

    void start(const Request& req, ReplyCallback done);
    void callback(const Request& req, ReplyCallback done);

    /// The page a successful callback answers with: it navigates to "/" by
    /// meta refresh, which the CSP allows where an inline script would not.
    static QByteArray landingPage();

    /// Tests move the clock and the environment.
    void setClock(std::function<qint64()> now) { m_now = std::move(now); }
    void setEnv(Oidc::EnvReader env) { m_env = std::move(env); }
    Oidc::PendingLogins& pending() { return m_pending; }

private:
    void refuse(const Request& req, const QString& code, bool counted, const ReplyCallback& done);

    AppSettings& m_settings;
    AuthManager& m_auth;
    OidcClient& m_client;
    Oidc::PendingLogins m_pending;
    std::function<qint64()> m_now;
    Oidc::EnvReader m_env;
};
