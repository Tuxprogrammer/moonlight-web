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

#include "ClipboardBackend.h"

#include <QPointer>

class QProcess;
class QTimer;

/// The Wayland clipboard through wl-clipboard, which speaks the data-control
/// protocols (ext-data-control-v1 and wlr-data-control) a background program
/// needs: `wl-paste --watch echo` prints a line at each change, `wl-paste`
/// reads the text, `wl-copy` writes it — always through stdin and stdout,
/// never on a command line, where other users could read it.
///
/// Fails to start when the watcher exits at once: wl-paste then said why,
/// typically that the compositor offers no data-control protocol (GNOME).
class WlClipboardBackend : public ClipboardBackend
{
    Q_OBJECT

public:
    WlClipboardBackend(const QString& wlPaste, const QString& wlCopy, QObject* parent = nullptr);
    ~WlClipboardBackend() override;

    QString name() const override { return QStringLiteral("wl-clipboard"); }
    void start() override;
    void setWatching(bool on) override { m_Watching = on; }
    void read() override;
    void write(const QString& text, std::function<void(bool ok)> done) override;

    /// How long the watcher has to stay up to count as working.
    void setStartGraceMs(int ms) { m_StartGraceMs = ms; }
    static constexpr int kStartGraceMs = 1000;
    static constexpr int kWriteTimeoutMs = 5000;

private:
    void spawnWatcher();

    QString m_WlPaste;
    QString m_WlCopy;
    QPointer<QProcess> m_Watcher;
    QTimer* m_Grace = nullptr;
    bool m_Ready = false;
    bool m_Watching = false;
    bool m_Reading = false;
    bool m_ReadAgain = false;
    int m_Restarts = 0;
    int m_StartGraceMs = kStartGraceMs;
};
