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

#include "server/routes/OidcRoutes.h"

#include "common/Logger.h"
#include "network/OidcClient.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/HttpServer.h"
#include "server/OidcFlow.h"
#include "server/OidcLogin.h"
#include "server/RestRouter.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace {

QString secretSourceName(Oidc::SecretSource source)
{
    switch (source) {
    case Oidc::SecretSource::None: return QStringLiteral("none");
    case Oidc::SecretSource::Settings: return QStringLiteral("settings");
    case Oidc::SecretSource::Env: return QStringLiteral("env");
    case Oidc::SecretSource::EnvFile: return QStringLiteral("env_file");
    }
    return QStringLiteral("none");
}

OidcLogin::Request loginRequest(const HttpRequest& req)
{
    OidcLogin::Request r;
    r.host = req.headers.value(QStringLiteral("host"));
    // Bookkeeping only (session IP, logs, lockout): the client a trusted proxy
    // forwards, as for a PIN login. No privilege reads it.
    r.clientAddress = req.source();
    r.attemptAddress = AuthManager::attemptAddress(req);
    // Booth mode lets no new device in, through this door either.
    r.closed = req.viaTunnel || req.adminLocked;
    r.query = req.queryParams;
    r.bindingCookie = HttpServer::cookieFromRequest(req, QStringLiteral("mw_oidc"));
    return r;
}

HttpResponse httpReply(const OidcLogin::Reply& reply)
{
    HttpResponse resp;
    if (reply.status == 404) {
        resp = HttpResponse::error(404, "Not Found");
    } else if (reply.status == 403) {
        resp = HttpResponse::error(403, "OpenID Connect sign-in is not offered at this address "
                                        "(it is not one of the configured public_url origins)");
    } else if (reply.status == 200) {
        resp.statusCode = 200;
        resp.contentType = QStringLiteral("text/html; charset=utf-8");
        resp.body = OidcLogin::landingPage();
    } else {
        resp.statusCode = reply.status;
        resp.headers["Location"] = reply.location;
    }
    if (!reply.setCookie.isEmpty()) resp.headers["Set-Cookie"] = reply.setCookie;
    // The callback's URL carries the code and state: keep it out of any
    // Referer, and the answer out of every cache.
    resp.headers["Referrer-Policy"] = QStringLiteral("no-referrer");
    resp.headers["Cache-Control"] = QStringLiteral("no-store");
    return resp;
}

} // namespace

QJsonObject oidcAdminSettingsJson(AppSettings& settings)
{
    const Oidc::Settings s = settings.oidcSettings();
    Oidc::SecretSource source{};
    const QString secret = Oidc::effectiveClientSecret(s, Oidc::processEnv(), &source);

    QJsonObject o;
    o["enabled"] = s.enabled;
    o["issuer"] = s.issuer;
    o["client_id"] = s.clientId;
    // Write-only: whether there is one and where it comes from, never the value.
    o["client_secret_set"] = !secret.isEmpty();
    o["client_secret_source"] = secretSourceName(source);
    o["scopes"] = s.scopes;
    o["groups_claim"] = s.groupsClaim;
    o["allowed_groups"] = QJsonArray::fromStringList(s.allowedGroups);
    o["admin_groups"] = QJsonArray::fromStringList(s.adminGroups);
    o["button_label"] = s.buttonLabel;
    o["disable_pin"] = s.disablePin;
    o["session_ttl_hours"] = s.sessionTtlHours;
    o["public_url"] = QJsonArray::fromStringList(s.publicUrls);
    // What the operator registers with the provider: one per origin.
    QJsonArray redirects;
    for (const QString& origin : Oidc::publicOrigins(s))
        redirects.append(Oidc::redirectUriFor(origin));
    o["redirect_uris"] = redirects;
    o["usable"] = Oidc::isUsable(s);
    o["problem"] = s.enabled ? Oidc::configProblem(s) : QString();
    return o;
}

void registerOidcRoutes(HttpServer& server, AuthManager& authManager, AppSettings& settings)
{
    auto* client = new OidcClient(&server);
    auto* login = new OidcLogin(settings, authManager, *client, &server);

    // ── The browser's half ──────────────────────────────────────────────────
    // GET /auth/oidc/start — off to the provider, with state, nonce and PKCE.
    server.router()->getAsync(
        "/auth/oidc/start", [login](const HttpRequest& req, ResponseCallback respond) {
            login->start(loginRequest(req),
                         [respond](const OidcLogin::Reply& reply) { respond(httpReply(reply)); });
        });

    // GET /auth/oidc/callback — back from the provider: verify, then a session.
    server.router()->getAsync("/auth/oidc/callback", [&server, login](const HttpRequest& req,
                                                                      ResponseCallback respond) {
        const QString peer = req.source();
        login->callback(loginRequest(req), [&server, peer, respond](const OidcLogin::Reply& reply) {
            if (reply.authFailure) server.reportAuthFailure(peer);
            respond(httpReply(reply));
        });
    });

    // ── The admin page's half ───────────────────────────────────────────────
    server.router()->get("/api/admin/oidc", [&settings](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available to an administrator");
        return HttpResponse::json(oidcAdminSettingsJson(settings));
    });

    // POST /api/admin/oidc — change any of the settings (admin key required, as
    // for every admin write). A change to who may sign in ends every OIDC
    // session, the caller's own included: the old rules admitted them.
    server.router()->post("/api/admin/oidc", [&settings, &authManager,
                                              client](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Only available to an administrator");
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();

        Oidc::Settings s = settings.oidcSettings();
        const Oidc::UpdateResult res = Oidc::applyUpdate(s, body);
        if (!res.ok) {
            QJsonObject err{{"status", "error"}, {"error", res.error}};
            if (!res.problem.isEmpty()) err["problem"] = res.problem;
            return HttpResponse::json(err, 400);
        }
        settings.setOidcSettings(s);
        client->clearCache();
        const int ended = res.accessChanged ? authManager.destroyOidcSessions() : 0;
        Logger::info(QStringLiteral("[OIDC] Settings changed from the admin page (%1)")
                         .arg(s.enabled ? QStringLiteral("enabled") : QStringLiteral("disabled")));

        QJsonObject obj = oidcAdminSettingsJson(settings);
        obj["status"] = "ok";
        obj["sessions_ended"] = ended;
        return HttpResponse::json(obj);
    });

    // POST /api/admin/oidc/test — fetch the discovery document and the key set
    // of {issuer} (or of the saved issuer), bypassing the cache, and say what
    // came back. A diagnostic: always 200, with ok and error.
    server.router()->postAsync(
        "/api/admin/oidc/test",
        [&settings, client](const HttpRequest& req, ResponseCallback respond) {
            if (!req.isLocal) {
                respond(HttpResponse::error(403, "Only available to an administrator"));
                return;
            }
            QString issuer = QJsonDocument::fromJson(req.body).object().value("issuer").toString();
            if (issuer.trimmed().isEmpty()) issuer = settings.oidcSettings().issuer;
            if (!Oidc::isAllowedProviderUrl(QUrl(issuer.trimmed()))) {
                respond(HttpResponse::json({{"ok", false}, {"error", "invalid_issuer"}}));
                return;
            }
            client->discover(
                issuer,
                [client, respond](std::optional<OidcClient::Provider> p, QString error) {
                    if (!p) {
                        respond(HttpResponse::json({{"ok", false}, {"error", error}}));
                        return;
                    }
                    QJsonObject obj{{"issuer", p->issuer},
                                    {"authorization_endpoint", p->authorizationEndpoint},
                                    {"token_endpoint", p->tokenEndpoint},
                                    {"jwks_uri", p->jwksUri}};
                    client->countKeys(p->jwksUri, [obj, respond](int keys, QString error) mutable {
                        obj["keys"] = keys;
                        obj["ok"] = error.isEmpty() && keys > 0;
                        if (!error.isEmpty())
                            obj["error"] = error;
                        else if (keys == 0)
                            obj["error"] = "no_usable_keys";
                        respond(HttpResponse::json(obj));
                    });
                },
                true);
        });
}
