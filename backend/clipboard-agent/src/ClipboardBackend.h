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

#include <QObject>
#include <QString>

#include <functional>

/// One way of reaching the clipboard of the session the agent runs in.
///
/// Deliberately plain: it reads, writes and reports raw changes. Echo
/// suppression and the cap live in AgentClient, so every backend behaves the
/// same and the rules are tested once.
class ClipboardBackend : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QString name() const = 0;

    /// Get ready; ends with ready() or failed().
    virtual void start() = 0;

    /// Report changes (observed) while on; read nothing while off.
    virtual void setWatching(bool on) = 0;

    /// Read the clipboard now: observed() follows, empty for no text.
    virtual void read() = 0;

    /// Put @p text on the clipboard; @p done says whether it took.
    virtual void write(const QString& text, std::function<void(bool ok)> done) = 0;

signals:
    void ready();
    void failed(const QString& reason);
    /// The clipboard's text, read after a change (while watching) or after
    /// read(). Empty when it holds no text.
    void observed(const QString& text);
};
