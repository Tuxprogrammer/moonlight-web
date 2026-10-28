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

// The clipboard lines between a stream worker and the server, on the worker's
// stdin/stdout JSON-line protocol (worker/StreamWorkerMain.h).
//
// The agent's socket lives in the server, the relays in the worker, so a
// session whose clipboard goes through an agent speaks to it through these:
//
//   server → worker  {"cmd":"clipboardAgent","available":b}   agent usable or not
//   server → worker  {"cmd":"clipboardText","text":s}         host clipboard changed
//   server → worker  {"cmd":"clipboardAck","id":n,"ok":b}     a write landed (or not)
//   worker → server  {"event":"clipboardSet","id":n,"text":s,"paste":b}
//   worker → server  {"event":"clipboardAnnounce"}            a browser connected
//
// Header-only and pure, so the mapping is tested on its own.

#include "streaming/ClipboardMessage.h"

#include <QJsonObject>
#include <QString>

#include <cmath>

namespace ClipboardIpc {

namespace detail {
inline bool readId(const QJsonValue& v, qint64* out)
{
    if (!v.isDouble()) return false;
    const double d = v.toDouble();
    if (!(d >= 1.0) || d > 9007199254740992.0 || std::floor(d) != d) return false;
    *out = static_cast<qint64>(d);
    return true;
}
} // namespace detail

// ── server → worker ─────────────────────────────────────────────────────────

inline QJsonObject availableCommand(bool available)
{
    return {{QStringLiteral("cmd"), QStringLiteral("clipboardAgent")},
            {QStringLiteral("available"), available}};
}

inline QJsonObject textCommand(const QString& text)
{
    return {{QStringLiteral("cmd"), QStringLiteral("clipboardText")},
            {QStringLiteral("text"), ClipboardMsg::capText(text)}};
}

inline QJsonObject ackCommand(qint64 id, bool ok)
{
    return {{QStringLiteral("cmd"), QStringLiteral("clipboardAck")},
            {QStringLiteral("id"), static_cast<double>(id)},
            {QStringLiteral("ok"), ok}};
}

struct Command
{
    enum class Kind
    {
        None, ///< Not a clipboard command (or a malformed one).
        Available,
        Text,
        Ack,
    };
    Kind kind = Kind::None;
    bool available = false;
    QString text;
    qint64 id = 0;
    bool ok = false;
};

inline Command decodeCommand(const QJsonObject& msg)
{
    Command c;
    const QString cmd = msg.value(QLatin1String("cmd")).toString();
    if (cmd == QLatin1String("clipboardAgent")) {
        const QJsonValue v = msg.value(QLatin1String("available"));
        if (!v.isBool()) return c;
        c.kind = Command::Kind::Available;
        c.available = v.toBool();
    } else if (cmd == QLatin1String("clipboardText")) {
        const QString text = msg.value(QLatin1String("text")).toString();
        if (text.isEmpty()) return c;
        c.kind = Command::Kind::Text;
        c.text = ClipboardMsg::capText(text);
    } else if (cmd == QLatin1String("clipboardAck")) {
        if (!detail::readId(msg.value(QLatin1String("id")), &c.id)) return c;
        c.kind = Command::Kind::Ack;
        c.ok = msg.value(QLatin1String("ok")).toBool(false);
    }
    return c;
}

// ── worker → server ─────────────────────────────────────────────────────────

inline QJsonObject setEvent(qint64 id, const QString& text, bool paste)
{
    return {{QStringLiteral("event"), QStringLiteral("clipboardSet")},
            {QStringLiteral("id"), static_cast<double>(id)},
            {QStringLiteral("text"), ClipboardMsg::capText(text)},
            {QStringLiteral("paste"), paste}};
}

inline QJsonObject announceEvent()
{
    return {{QStringLiteral("event"), QStringLiteral("clipboardAnnounce")}};
}

/// Whether @p name is one of the events above (StreamWorkerHost hands them on
/// as they are, and must never log their payload).
inline bool isEvent(const QString& name)
{
    return name == QLatin1String("clipboardSet") || name == QLatin1String("clipboardAnnounce");
}

struct Event
{
    enum class Kind
    {
        None,
        Set,
        Announce,
    };
    Kind kind = Kind::None;
    qint64 id = 0;
    QString text;
    bool paste = false;
};

inline Event decodeEvent(const QJsonObject& msg)
{
    Event e;
    const QString name = msg.value(QLatin1String("event")).toString();
    if (name == QLatin1String("clipboardAnnounce")) {
        e.kind = Event::Kind::Announce;
    } else if (name == QLatin1String("clipboardSet")) {
        if (!detail::readId(msg.value(QLatin1String("id")), &e.id)) return e;
        e.text = ClipboardMsg::capText(msg.value(QLatin1String("text")).toString());
        e.paste = msg.value(QLatin1String("paste")).toBool(false);
        e.kind = Event::Kind::Set;
    }
    return e;
}

} // namespace ClipboardIpc
