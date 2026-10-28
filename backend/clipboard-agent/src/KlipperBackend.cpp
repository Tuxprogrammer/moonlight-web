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

#include "KlipperBackend.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace {
const QString kService = QStringLiteral("org.kde.klipper");
const QString kPath = QStringLiteral("/klipper");
const QString kInterface = QStringLiteral("org.kde.klipper.klipper");
} // namespace

KlipperBackend::KlipperBackend(QObject* parent)
    : ClipboardBackend(parent)
{}

void KlipperBackend::start()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        emit failed(QStringLiteral("no D-Bus session bus"));
        return;
    }
    if (!bus.interface() || !bus.interface()->isServiceRegistered(kService).value()) {
        emit failed(QStringLiteral("Klipper is not running"));
        return;
    }
    QMetaObject::invokeMethod(this, &ClipboardBackend::ready, Qt::QueuedConnection);
}

void KlipperBackend::setWatching(bool on)
{
    m_Watching = on;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (on && !m_Connected) {
        m_Connected =
            bus.connect(kService, kPath, kInterface, QStringLiteral("clipboardHistoryUpdated"),
                        this, SLOT(onHistoryUpdated()));
    } else if (!on && m_Connected) {
        bus.disconnect(kService, kPath, kInterface, QStringLiteral("clipboardHistoryUpdated"), this,
                       SLOT(onHistoryUpdated()));
        m_Connected = false;
    }
}

void KlipperBackend::onHistoryUpdated()
{
    if (m_Watching) read();
}

void KlipperBackend::read()
{
    const QDBusMessage call = QDBusMessage::createMethodCall(
        kService, kPath, kInterface, QStringLiteral("getClipboardContents"));
    auto* watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher* w) {
        const QDBusPendingReply<QString> reply = *w;
        w->deleteLater();
        emit observed(reply.isError() ? QString() : reply.value());
    });
}

void KlipperBackend::write(const QString& text, std::function<void(bool ok)> done)
{
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath, kInterface,
                                                       QStringLiteral("setClipboardContents"));
    call << text;
    auto* watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [done](QDBusPendingCallWatcher* w) {
        const bool ok = !w->isError();
        w->deleteLater();
        if (done) done(ok);
    });
}
