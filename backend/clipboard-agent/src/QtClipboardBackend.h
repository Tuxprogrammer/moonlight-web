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

class QTimer;

/// QClipboard: X11 (XFixes tells Qt of every owner change, window or not),
/// Windows (clipboard format listener) and macOS. macOS announces nothing to
/// a program in the background, so there the clipboard is polled while
/// watching. Needs a QGuiApplication.
class QtClipboardBackend : public ClipboardBackend
{
    Q_OBJECT

public:
    explicit QtClipboardBackend(const QString& name, QObject* parent = nullptr);

    QString name() const override { return m_Name; }
    void start() override;
    void setWatching(bool on) override;
    void read() override;
    void write(const QString& text, std::function<void(bool ok)> done) override;

    static constexpr int kPollMs = 500;

private:
    void onChanged();

    QString m_Name;
    bool m_Watching = false;
    QTimer* m_Poll = nullptr;
    QString m_Polled;
};
