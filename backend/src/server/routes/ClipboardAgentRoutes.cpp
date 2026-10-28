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

#include "server/routes/ClipboardAgentRoutes.h"

#include "backend/ComputerManager.h"
#include "server/ClipboardAgentRegistry.h"
#include "server/ClipboardAgentSetup.h"
#include "server/HttpServer.h"
#include "server/RestRouter.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>

namespace {

HttpResponse adminOnly()
{
    return HttpResponse::error(403, "A host's clipboard agent is managed by the administrator");
}

/// The host exists and the caller administers this server; otherwise the
/// answer to send instead.
bool refused(const HttpRequest& req, ComputerManager& computers, HttpResponse* out)
{
    if (!req.isLocal) {
        *out = adminOnly();
        return true;
    }
    const QString uuid = req.pathParams.value(QStringLiteral("id"));
    if (uuid.isEmpty() || !computers.getHost(uuid)) {
        *out = HttpResponse::error(404, "Unknown host");
        return true;
    }
    return false;
}

QList<QHostAddress> privateLanAddresses()
{
    QList<QHostAddress> out;
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && a.isPrivateUse()) out.append(a);
    return out;
}

} // namespace

void registerClipboardAgentRoutes(HttpServer& server, ComputerManager& computerManager,
                                  ClipboardAgentRegistry& registry)
{
    server.router()->get(
        "/api/hosts/:id/clipboard-agent", [&computerManager, &registry](const HttpRequest& req) {
            HttpResponse no;
            if (refused(req, computerManager, &no)) return no;
            return HttpResponse::json(registry.statusJson(req.pathParams.value("id")));
        });

    server.router()->post("/api/hosts/:id/clipboard-agent/enabled", [&computerManager, &registry](
                                                                        const HttpRequest& req) {
        HttpResponse no;
        if (refused(req, computerManager, &no)) return no;
        const QJsonValue enabled =
            QJsonDocument::fromJson(req.body).object().value(QLatin1String("enabled"));
        if (!enabled.isBool()) return HttpResponse::error(400, "Missing 'enabled' boolean");
        const QString uuid = req.pathParams.value("id");
        registry.setEnabled(uuid, enabled.toBool());
        return HttpResponse::json(registry.statusJson(uuid));
    });

    // Issue, or rotate: either way a new token, shown in this answer only, and
    // whatever agent used the previous one is disconnected.
    server.router()->post(
        "/api/hosts/:id/clipboard-agent/token",
        [&server, &computerManager, &registry](const HttpRequest& req) {
            HttpResponse no;
            if (refused(req, computerManager, &no)) return no;
            const QString uuid = req.pathParams.value("id");
            const QString token = registry.issueToken(uuid, QDateTime::currentMSecsSinceEpoch());
            const QString url = ClipboardAgentSetup::serverUrl(
                req.headers.value("host"), server.activeHttpsPort(), privateLanAddresses());
            QJsonObject body = ClipboardAgentSetup::describe(token, url, server.tlsKeyPins());
            body.insert(QStringLiteral("status"), registry.statusJson(uuid));
            HttpResponse resp = HttpResponse::json(body);
            resp.headers["Cache-Control"] = "no-store";
            return resp;
        });

    server.router()->del("/api/hosts/:id/clipboard-agent/token",
                         [&computerManager, &registry](const HttpRequest& req) {
                             HttpResponse no;
                             if (refused(req, computerManager, &no)) return no;
                             const QString uuid = req.pathParams.value("id");
                             registry.revokeToken(uuid);
                             return HttpResponse::json(registry.statusJson(uuid));
                         });
}
