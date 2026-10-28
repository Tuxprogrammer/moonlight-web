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

/// Keeps a clipboard watcher from announcing what it just wrote, or the same
/// text twice.
///
/// Every backend sees its own writes come back as a change (QClipboard's
/// dataChanged, wl-paste --watch, Klipper's history signal). Announcing them
/// would send the browser's own text back to the browser, and two watchers
/// facing each other would loop. ClipboardBridge does the same with
/// m_LastText; this is that rule, on its own so it can be tested.
class ClipboardEcho
{
public:
    /// We are about to write @p text: its echo must not be announced.
    void noteWritten(const QString& text) { m_Last = text; }

    /// The clipboard now holds @p text. True when it is news worth
    /// announcing: not empty (cleared, or not text) and not what we last wrote
    /// or announced. Remembers it either way.
    bool shouldAnnounce(const QString& text)
    {
        if (text.isEmpty() || text == m_Last) return false;
        m_Last = text;
        return true;
    }

    /// An announce was asked for: the next reading goes out even if unchanged.
    void forget() { m_Last.clear(); }

    const QString& last() const { return m_Last; }

private:
    QString m_Last;
};
