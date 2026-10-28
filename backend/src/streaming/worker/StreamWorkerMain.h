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

#include <QCoreApplication>

/**
 * Entry point for `MoonlightWeb --stream-worker`: hosts ONE streaming session
 * (Sunshine launch + relay graph + moonlight-common-c) in a dedicated child
 * process, so the parent server can run several concurrent sessions despite
 * moonlight-common-c being a process-global singleton.
 *
 * Protocol (JSON lines):
 *   stdin  (parent → worker):
 *     first line       — the session config (see StreamWorkerHost::buildConfig);
 *                        "upnpPublicIp"/"upnpExternalPort" carry the router hole
 *                        the parent claimed for the slot — the worker never
 *                        touches the router itself
 *     {"cmd":"quit"}      — graceful local teardown, then exit(0)
 *     {"cmd":"takenOver"} — notify the browser it was taken over, teardown, exit
 *     {"cmd":"revoked"}   — notify the browser it was revoked, teardown, exit
 *     {"cmd":"clipboardAgent"|"clipboardText"|"clipboardAck",…}
 *                         — the host's clipboard agent, relayed by the server
 *                           (clipboard/ClipboardIpc.h); config key
 *                           "clipboardAgent" installs the remote provider
 *     EOF                 — parent died: teardown, exit
 *   stdout (worker → parent):
 *     {"event":"response","code":<http>,"body":{...}} — the /start HTTP reply
 *     {"event":"captureHold","held":…,"display_mhz":…} — the capture's verdict
 *                        on its display, just before "ended" (vddmatch)
 *     {"event":"mouseKeys","on":true,"saved":{…}} / {"event":"mouseKeys","on":false}
 *                        — Mouse Keys turned on for a host with no pointing
 *                        device, or let go; put back by the parent if the
 *                        worker dies holding them
 *     {"event":"clipboardSet"|"clipboardAnnounce",…}   — a write to, or a read
 *                        of, the host clipboard through its agent (ClipboardIpc)
 *     {"event":"ended"}                               — session over (any cause)
 * All logging goes to the log file / stderr; stdout carries only the protocol.
 */
int runStreamWorker(QCoreApplication& app);
