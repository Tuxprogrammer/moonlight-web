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

#include <QString>
#include <QStringList>

/// Which transport families a stream may use: the admin's `allowed_transports`
/// ("all" | "webrtc" | "wss") and the browser's `transport_pref`
/// ("auto" | "webrtc" | "wss"). Both filter the chain (TransportChain.h); the
/// admin's transport_mode only orders what they leave.
namespace transportpolicy {

/// The policy name in canonical form, or empty when @p value is none.
inline QString normalizePolicy(const QString& value)
{
    const QString v = value.trimmed().toLower();
    return v == QLatin1String("all") || v == QLatin1String("webrtc") || v == QLatin1String("wss")
               ? v
               : QString();
}

/// The preference in canonical form; anything else is "auto".
inline QString normalizePref(const QString& value)
{
    const QString v = value.trimmed().toLower();
    return v == QLatin1String("webrtc") || v == QLatin1String("wss") ? v : QStringLiteral("auto");
}

/// Does family @p family ("all"/"auto", "webrtc", "wss") let rung @p rung run?
inline bool familyAllows(const QString& family, const QString& rung)
{
    if (family == QLatin1String("webrtc")) return rung.startsWith(QLatin1String("webrtc-"));
    if (family == QLatin1String("wss")) return rung == QLatin1String("wss");
    return true;
}

/// The admin selector's transports: the ones the policy allows, in @p order.
/// Under "all", release builds leave wss out unless it is the saved mode or the
/// saved mode is "all" (InternetAccessManager says why); "wss" is the admin
/// choosing it, so it is always offered there.
inline QStringList availableTransports(const QStringList& order, const QString& policy,
                                       const QString& savedMode, bool debugBuild)
{
    QStringList out;
    for (const QString& t : order)
        if (familyAllows(policy, t)) out.append(t);
    if (policy != QLatin1String("wss") && !debugBuild && savedMode != QLatin1String("all") &&
        savedMode != QLatin1String("wss"))
        out.removeAll(QStringLiteral("wss"));
    return out;
}

} // namespace transportpolicy
