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

#include <QJsonObject>

class HttpServer;
class AuthManager;
class AppSettings;

/// Register the OpenID Connect login: the browser routes GET /auth/oidc/start
/// and GET /auth/oidc/callback (dispatched by HttpServer ahead of the static
/// files, outside /api/), and the admin page's GET/POST /api/admin/oidc and
/// POST /api/admin/oidc/test. See docs/design/oidc-login.md.
void registerOidcRoutes(HttpServer& server, AuthManager& authManager, AppSettings& settings);

/// What GET /api/admin/oidc answers: every setting but the client secret, of
/// which only whether one is set and where it comes from.
QJsonObject oidcAdminSettingsJson(AppSettings& settings);
