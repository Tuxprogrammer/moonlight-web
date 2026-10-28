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

#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

/// Which way the agent reaches the clipboard of the session it runs in.
///
///  - Wayland does not let a background program read the clipboard, except
///    through a data-control protocol (ext-data-control-v1, or
///    wlr-data-control). wl-clipboard speaks both: `wl-paste --watch` tells
///    us of each change, `wl-paste` reads it and `wl-copy` writes it. KDE
///    Plasma and wlroots compositors offer it; GNOME does not.
///  - On KDE, Klipper's D-Bus interface is the fallback (org.kde.klipper).
///  - X11, Windows and macOS: QClipboard. A Wayland session that also runs
///    XWayland gets it last, through XWayland's clipboard (best effort: what
///    the compositor chooses to mirror there).
///
/// Candidates come in order; the agent starts the first, and moves on when
/// one fails to start (wl-paste exits at once without data-control, Klipper
/// is not running…). Pure, so the choice is tested over made-up environments.
namespace BackendSelect {

enum class Backend
{
    WlClipboard,
    Klipper,
    Qt,
    /// QClipboard through XWayland (the xcb platform) in a Wayland session.
    QtXWayland,
};

struct Environment
{
    /// "linux", "windows", "macos", or another Unix.
    QString os;
    bool wayland = false;   ///< WAYLAND_DISPLAY set
    bool x11 = false;       ///< DISPLAY set
    bool wlPaste = false;   ///< wl-paste on PATH
    bool wlCopy = false;    ///< wl-copy on PATH
    bool kde = false;       ///< XDG_CURRENT_DESKTOP names KDE
    bool dbusBuilt = false; ///< built with Qt D-Bus (Klipper possible)
    QString forced;         ///< --backend, "auto" or empty for automatic
};

/// The backends to try, best first. Empty: none can work here.
QList<Backend> candidates(const Environment& env);

QString name(Backend b);
std::optional<Backend> fromName(const QString& name);
QStringList allNames();

/// A line for the log saying what the backend is.
QString describe(Backend b);

/// Needs a QGuiApplication (and a display connection).
bool needsGui(Backend b);

/// The environment of this process (variables, PATH).
Environment detect(const QString& forced);

} // namespace BackendSelect
