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

class HttpServer;
class ComputerManager;
class ClipboardAgentRegistry;

/// Register the admin routes of a host's clipboard agent, all admin-only:
///   GET    /api/hosts/:id/clipboard-agent          status
///   POST   /api/hosts/:id/clipboard-agent/enabled  {enabled}
///   POST   /api/hosts/:id/clipboard-agent/token    issue or rotate (shown once)
///   DELETE /api/hosts/:id/clipboard-agent/token    revoke
void registerClipboardAgentRoutes(HttpServer& server, ComputerManager& computerManager,
                                  ClipboardAgentRegistry& registry);
