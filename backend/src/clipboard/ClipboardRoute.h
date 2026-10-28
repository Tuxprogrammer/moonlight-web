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

// Where a session's host clipboard comes from, decided in one place.
//
//  - Local: the streamed host is this machine, so this process's QClipboard is
//    the host's (ClipboardBridge). Unchanged from before the agent existed.
//  - Agent: the host runs a clipboard agent that the admin enabled and that is
//    connected now (ClipboardAgentServer).
//  - None: the browser gets no clipboardcaps, and Ctrl+V is a plain keystroke.
//
// The host clipboard is an exfiltration channel. A restricted session never
// reads it, and an invited player never reaches it through an agent, whatever
// the owner ticked: an agent is an opt-in for the owner's own use of a remote
// machine. Pure, so every combination is tested.

namespace ClipboardRoute {

enum class Route
{
    None,
    Local,
    Agent,
};

struct Inputs
{
    /// The streamed host's address is this machine.
    bool hostIsSelf = false;
    /// The session's input policy is unrestricted (keyboard, mouse, gamepad).
    bool unrestricted = false;
    /// The session belongs to an invited player rather than the owner.
    bool guest = false;
    /// The admin turned the agent on for this host.
    bool agentEnabled = false;
    /// The host's agent is connected and said hello.
    bool agentConnected = false;
};

/// Whether a session may ever go through its host's agent: the owner's own,
/// unrestricted session on a host that is not this machine. The server then
/// installs a remote provider in its worker, usable while the agent is.
inline bool mayUseAgent(bool hostIsSelf, bool unrestricted, bool guest)
{
    return unrestricted && !hostIsSelf && !guest;
}

/// The server's decision for one session (main process), right now.
inline Route decide(const Inputs& in)
{
    if (!in.unrestricted) return Route::None;
    // The local rule is today's: host is this machine, session unrestricted.
    if (in.hostIsSelf) return Route::Local;
    if (mayUseAgent(in.hostIsSelf, in.unrestricted, in.guest) && in.agentEnabled &&
        in.agentConnected)
        return Route::Agent;
    return Route::None;
}

/// The stream worker's choice of provider. It cannot see the agent: the
/// server installs a remote provider only for sessions it may route through
/// one, and switches it on and off as the agent comes and goes.
inline Route workerProvider(bool hostIsSelf, bool unrestricted, bool remoteInstalled)
{
    if (!unrestricted) return Route::None;
    if (hostIsSelf) return Route::Local;
    return remoteInstalled ? Route::Agent : Route::None;
}

inline const char* name(Route r)
{
    switch (r) {
    case Route::None: return "none";
    case Route::Local: return "local";
    case Route::Agent: return "agent";
    }
    return "none";
}

} // namespace ClipboardRoute
