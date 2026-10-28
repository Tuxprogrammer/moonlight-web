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

#include "ClipboardProvider.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <functional>

class QObject;

/// One relay's end of host clipboard sync, the same for all three relays.
///
/// Holds the relay's provider (none, the local bridge, or a remote agent) and
/// whether it works right now, and turns the provider's signals into the
/// browser's messages: 'clipboard' for host text, 'clipboardcaps' when an
/// agent comes or goes. The browser's own clipboard messages go through
/// handle(). Each relay only supplies how it sends a message.
class RelayClipboard
{
public:
    using Send = std::function<void(const QByteArray& json)>;

    /// Main thread, before the relay moves to its thread. @p relay is the
    /// signals' context, so @p send runs on the relay's thread; it must drop
    /// the message itself when no browser is connected. Null @p provider:
    /// clipboard sync off.
    void attach(ClipboardProvider* provider, QObject* relay, Send send);

    ClipboardProvider* provider() const { return m_Provider; }
    /// A provider that works right now. Any thread.
    bool active() const { return m_Provider && m_Available.load(); }

    /// The browser's channel just opened: advertise sync and push the current
    /// host clipboard once, so copy-before-connect pastes locally. Any thread;
    /// @p sendNow sends on the channel that opened.
    void clientConnected(const Send& sendNow);

    /// A message of @p type from the browser: clipboardpaste / clipboardset
    /// act (or are dropped while inactive); any other type is left alone.
    /// Returns whether it was a clipboard message.
    bool handle(IMediaEngine* engine, const QString& type, const QJsonObject& msg,
                const char* logTag);

    static QByteArray capsMessage(bool available);
    static QByteArray textMessage(const QString& text);

private:
    ClipboardProvider* m_Provider = nullptr;
    std::atomic<bool> m_Available{false};
};
