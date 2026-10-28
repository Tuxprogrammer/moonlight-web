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

/// KDE's clipboard manager over D-Bus (org.kde.klipper, /klipper):
/// getClipboardContents, setClipboardContents, and clipboardHistoryUpdated
/// for changes. The fallback on Plasma when wl-clipboard is not installed.
class KlipperBackend : public ClipboardBackend
{
    Q_OBJECT

public:
    explicit KlipperBackend(QObject* parent = nullptr);

    QString name() const override { return QStringLiteral("klipper"); }
    void start() override;
    void setWatching(bool on) override;
    void read() override;
    void write(const QString& text, std::function<void(bool ok)> done) override;

private slots:
    void onHistoryUpdated();

private:
    bool m_Watching = false;
    bool m_Connected = false;
};
