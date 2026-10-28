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

#include "BackendSelect.h"

#include <QStandardPaths>
#include <QtGlobal>

namespace BackendSelect {

namespace {
struct Named
{
    Backend backend;
    const char* name;
};
constexpr Named kNames[] = {
    {Backend::WlClipboard, "wl-clipboard"},
    {Backend::Klipper, "klipper"},
    {Backend::Qt, "qt"},
    {Backend::QtXWayland, "qt-xwayland"},
};
} // namespace

QList<Backend> candidates(const Environment& env)
{
    const QString forced = env.forced.trimmed().toLower();
    if (!forced.isEmpty() && forced != QLatin1String("auto")) {
        const std::optional<Backend> b = fromName(forced);
        if (!b) return {};
        if (*b == Backend::Klipper && !env.dbusBuilt) return {};
        return {*b};
    }

    if (env.os == QLatin1String("windows") || env.os == QLatin1String("macos"))
        return {Backend::Qt};

    QList<Backend> out;
    if (env.wayland) {
        if (env.wlPaste && env.wlCopy) out.append(Backend::WlClipboard);
        if (env.kde && env.dbusBuilt) out.append(Backend::Klipper);
        if (env.x11) out.append(Backend::QtXWayland);
        return out;
    }
    if (env.x11) out.append(Backend::Qt);
    return out;
}

QString name(Backend b)
{
    for (const Named& n : kNames)
        if (n.backend == b) return QString::fromLatin1(n.name);
    return QString();
}

std::optional<Backend> fromName(const QString& s)
{
    for (const Named& n : kNames)
        if (s.compare(QLatin1String(n.name), Qt::CaseInsensitive) == 0) return n.backend;
    return std::nullopt;
}

QStringList allNames()
{
    QStringList out;
    for (const Named& n : kNames)
        out.append(QString::fromLatin1(n.name));
    return out;
}

QString describe(Backend b)
{
    switch (b) {
    case Backend::WlClipboard:
        return QStringLiteral("wl-clipboard (Wayland data-control: wl-paste --watch, wl-copy)");
    case Backend::Klipper: return QStringLiteral("Klipper over D-Bus (org.kde.klipper)");
    case Backend::Qt: return QStringLiteral("QClipboard");
    case Backend::QtXWayland:
        return QStringLiteral("QClipboard through XWayland (best effort on Wayland)");
    }
    return QString();
}

bool needsGui(Backend b)
{
    return b == Backend::Qt || b == Backend::QtXWayland;
}

Environment detect(const QString& forced)
{
    Environment env;
#if defined(Q_OS_WIN)
    env.os = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    env.os = QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    env.os = QStringLiteral("linux");
#else
    env.os = QStringLiteral("unix");
#endif
    env.wayland = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
    env.x11 = !qEnvironmentVariableIsEmpty("DISPLAY");
    env.wlPaste = !QStandardPaths::findExecutable(QStringLiteral("wl-paste")).isEmpty();
    env.wlCopy = !QStandardPaths::findExecutable(QStringLiteral("wl-copy")).isEmpty();
    env.kde = qEnvironmentVariable("XDG_CURRENT_DESKTOP")
                  .contains(QLatin1String("KDE"), Qt::CaseInsensitive);
#ifdef MW_AGENT_HAVE_DBUS
    env.dbusBuilt = true;
#endif
    env.forced = forced;
    return env;
}

} // namespace BackendSelect
