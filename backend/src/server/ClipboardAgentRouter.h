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

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <functional>

class ClipboardAgentLink;

/// Joins the stream workers that may use a host's clipboard agent to that
/// agent (main process).
///
/// The supervisor attaches a worker once it decided the session may go
/// through an agent (ClipboardRoute: the owner's own, unrestricted session on
/// a remote host). From then on:
///  - the worker hears whether the agent is usable (clipboardAgent), now and
///    each time that changes — the browser's clipboardcaps follow;
///  - the agent watches its clipboard while at least one worker is attached
///    to its host, and every change goes to all of them (clipboardText);
///  - a worker's write (clipboardSet) goes to the agent, and its answer comes
///    back as clipboardAck — false at once when no agent is there.
///
/// Workers are reached through a send function, so the router is tested
/// without processes.
class ClipboardAgentRouter
{
public:
    using SendToWorker = std::function<void(const QJsonObject& command)>;

    explicit ClipboardAgentRouter(ClipboardAgentLink& link);

    /// Start routing for a worker streaming @p host. Returns its handle and
    /// tells it at once whether the agent is usable.
    int attach(const QString& host, SendToWorker send);
    /// Stop routing for @p handle (its worker ended). Unknown handles are
    /// ignored, so it is safe to call more than once.
    void detach(int handle);

    /// A clipboard event the worker sent (ClipboardIpc::Event).
    void onWorkerEvent(int handle, const QJsonObject& event);
    /// @p host's agent reported its clipboard.
    void onAgentText(const QString& host, const QString& text);
    /// @p host's agent became usable, or stopped being.
    void onAvailabilityChanged(const QString& host, bool available);

    /// What a worker about to start should begin with (its config).
    bool isAvailable(const QString& host) const;
    /// Workers attached to @p host.
    int attachedCount(const QString& host) const;

private:
    struct Attachment
    {
        QString host;
        SendToWorker send;
    };

    ClipboardAgentLink& m_Link;
    QHash<int, Attachment> m_Attached;
    int m_NextHandle = 0;
};
