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

#include "RelayClipboard.h"

#include <QJsonDocument>
#include <QObject>

void RelayClipboard::attach(ClipboardProvider* provider, QObject* relay, Send send)
{
    m_Provider = provider;
    m_Available.store(provider && provider->available());
    if (!provider) return;
    // Context 'relay': the slots run on the relay's (future) thread and are
    // cut when the relay dies.
    QObject::connect(provider, &ClipboardProvider::hostTextChanged, relay,
                     [this, send](const QString& text) {
                         if (m_Available.load()) send(textMessage(text));
                     });
    QObject::connect(provider, &ClipboardProvider::availabilityChanged, relay,
                     [this, send](bool available) {
                         if (m_Available.exchange(available) == available) return;
                         send(capsMessage(available));
                         if (available) m_Provider->requestAnnounce();
                     });
}

void RelayClipboard::clientConnected(const Send& sendNow)
{
    if (!active()) return;
    sendNow(capsMessage(true));
    m_Provider->requestAnnounce();
}

bool RelayClipboard::handle(IMediaEngine* engine, const QString& type, const QJsonObject& msg,
                            const char* logTag)
{
    if (!ClipboardMsg::isClipboardType(type)) return false;
    // 'clipboardpaste' (Ctrl/Cmd+V: commit the text, then inject the chord)
    // and 'clipboardset' (the clipboard panel: commit only). Dropped unless
    // sync works right now.
    ClipboardProvider::dispatch(m_Provider, engine, ClipboardMsg::decode(type, msg, active()),
                                logTag);
    return true;
}

QByteArray RelayClipboard::capsMessage(bool available)
{
    return available ? QByteArrayLiteral("{\"type\":\"clipboardcaps\",\"available\":true}")
                     : QByteArrayLiteral("{\"type\":\"clipboardcaps\",\"available\":false}");
}

QByteArray RelayClipboard::textMessage(const QString& text)
{
    QJsonObject m;
    m.insert(QStringLiteral("type"), QStringLiteral("clipboard"));
    m.insert(QStringLiteral("text"), text);
    return QJsonDocument(m).toJson(QJsonDocument::Compact);
}
