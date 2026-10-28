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

#include "ClipboardAgentRouter.h"

#include "ClipboardAgentServer.h"
#include "clipboard/ClipboardIpc.h"

ClipboardAgentRouter::ClipboardAgentRouter(ClipboardAgentLink& link)
    : m_Link(link)
{}

int ClipboardAgentRouter::attach(const QString& host, SendToWorker send)
{
    const int handle = ++m_NextHandle;
    const bool first = attachedCount(host) == 0;
    m_Attached.insert(handle, Attachment{host, std::move(send)});
    const Attachment& a = m_Attached[handle];
    if (a.send) a.send(ClipboardIpc::availableCommand(m_Link.isAvailable(host)));
    // The first listener turns the agent's watch on; it then reports the
    // current clipboard by itself. Later ones ask when their browser connects.
    if (first) m_Link.setWatching(host, true);
    return handle;
}

void ClipboardAgentRouter::detach(int handle)
{
    const auto it = m_Attached.constFind(handle);
    if (it == m_Attached.constEnd()) return;
    const QString host = it->host;
    m_Attached.erase(it);
    // Nobody left who may read it: the host's clipboard stays on the host.
    if (attachedCount(host) == 0) m_Link.setWatching(host, false);
}

void ClipboardAgentRouter::onWorkerEvent(int handle, const QJsonObject& event)
{
    const auto it = m_Attached.constFind(handle);
    if (it == m_Attached.constEnd()) return;
    const QString host = it->host;
    const ClipboardIpc::Event e = ClipboardIpc::decodeEvent(event);
    switch (e.kind) {
    case ClipboardIpc::Event::Kind::None: return;
    case ClipboardIpc::Event::Kind::Announce: m_Link.requestAnnounce(host); return;
    case ClipboardIpc::Event::Kind::Set: {
        const qint64 id = e.id;
        // Looked up again when the answer comes: the worker may be gone.
        m_Link.sendSet(host, e.text, [this, handle, id](bool ok) {
            const auto a = m_Attached.constFind(handle);
            if (a != m_Attached.constEnd() && a->send) a->send(ClipboardIpc::ackCommand(id, ok));
        });
        return;
    }
    }
}

void ClipboardAgentRouter::onAgentText(const QString& host, const QString& text)
{
    if (text.isEmpty()) return;
    const QJsonObject cmd = ClipboardIpc::textCommand(text);
    for (const Attachment& a : std::as_const(m_Attached))
        if (a.host == host && a.send) a.send(cmd);
}

void ClipboardAgentRouter::onAvailabilityChanged(const QString& host, bool available)
{
    const QJsonObject cmd = ClipboardIpc::availableCommand(available);
    for (const Attachment& a : std::as_const(m_Attached))
        if (a.host == host && a.send) a.send(cmd);
}

bool ClipboardAgentRouter::isAvailable(const QString& host) const
{
    return m_Link.isAvailable(host);
}

int ClipboardAgentRouter::attachedCount(const QString& host) const
{
    int n = 0;
    for (const Attachment& a : m_Attached)
        if (a.host == host) ++n;
    return n;
}
