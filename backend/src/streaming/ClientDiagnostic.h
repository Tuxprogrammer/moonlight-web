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

// Messages the page sends on the input channel only to be logged.
//
// The three relays (DataChannelRelay, MediaTrackRelay, StreamRelay) each own
// their input switch; a diagnostic has nothing to act on, so all of them
// recognise it here and log one info line instead of "Unknown input type".
// What gets logged comes from a page that can write anything: only short plain
// tokens of it are kept, so no field can break the line or fake another one.

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace InputMsg {

namespace detail {

/// A field as a log token: letters, digits and . _ + - only, at most 24 of
/// them, '?' when it is absent, not a string, or nothing is left.
inline QString diagnosticToken(const QJsonValue& v)
{
    if (!v.isString()) return QStringLiteral("?");
    QString out;
    for (const QChar c : v.toString()) {
        if (out.size() >= 24) break;
        const char16_t u = c.unicode();
        const bool plain = (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') ||
                           (u >= u'0' && u <= u'9') || u == u'.' || u == u'_' || u == u'+' ||
                           u == u'-';
        if (plain) out.append(c);
    }
    return out.isEmpty() ? QStringLiteral("?") : out;
}

inline QString diagnosticFlag(const QJsonValue& v)
{
    if (!v.isBool()) return QStringLiteral("?");
    return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
}

} // namespace detail

/// Whether @p type is a diagnostic: logged, never input. Today only
/// 'codec_fallback', sent just before the page quits to relaunch in another
/// codec (StreamView._requestCodecFallback).
inline bool isClientDiagnostic(const QString& type)
{
    return type == QLatin1String("codec_fallback");
}

/// The info line for a diagnostic, e.g. "Client codec fallback: from=hevc
/// to=h264 hdr=false"; empty for any other type.
inline QString describeClientDiagnostic(const QString& type, const QJsonObject& msg)
{
    if (type == QLatin1String("codec_fallback")) {
        return QStringLiteral("Client codec fallback: from=%1 to=%2 hdr=%3")
            .arg(detail::diagnosticToken(msg.value(QLatin1String("from"))),
                 detail::diagnosticToken(msg.value(QLatin1String("to"))),
                 detail::diagnosticFlag(msg.value(QLatin1String("hdr"))));
    }
    return QString();
}

} // namespace InputMsg
