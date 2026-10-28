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

#include "ClipboardBridge.h"
#include "IMediaEngine.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QMetaMethod>
#include <QNetworkInterface>
#include <QPointer>
#include <QtDebug>

ClipboardBridge* ClipboardBridge::instance()
{
    // Parented to the app so it dies with the main event loop. Thread-safe
    // static init; first caller's thread becomes the object's thread — see
    // header contract (first call is from Session on the main thread).
    static ClipboardBridge* s_Instance = new ClipboardBridge(QCoreApplication::instance());
    return s_Instance;
}

ClipboardBridge::ClipboardBridge(QObject* parent)
    : ClipboardProvider(parent)
{
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this,
            &ClipboardBridge::onClipboardChanged);
    qInfo() << "[ClipboardBridge] Created (host clipboard monitor active)";
}

bool ClipboardBridge::isSelfAddress(const QString& address)
{
    if (address.isEmpty()) return false;
    if (address.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) return true;
    if (address.compare(QHostInfo::localHostName(), Qt::CaseInsensitive) == 0) return true;

    const QHostAddress addr(address);
    if (addr.isNull()) return false; // hostname we can't cheaply resolve — assume remote
    if (addr.isLoopback()) return true;

    const QList<QHostAddress> locals = QNetworkInterface::allAddresses();
    for (const QHostAddress& local : locals) {
        if (addr.isEqual(local, QHostAddress::TolerantConversion)) return true;
    }
    return false;
}

void ClipboardBridge::pasteFromClient(IMediaEngine* engine, const QString& text, bool wrapCtrl)
{
    // The shim is Qt-parented to its relay and dies on the relay thread; the
    // lambda runs on the main thread — guard with a QPointer so a teardown
    // racing this queued call degrades to a no-op instead of a dangling call.
    QPointer<IMediaEngine> shimGuard(engine);
    QMetaObject::invokeMethod(
        this,
        [this, shimGuard, text, wrapCtrl]() {
            commitText(ClipboardMsg::capText(text));
            // Inject the chord only AFTER the clipboard is committed — same
            // main-thread hop, so ordering is structural. Li input calls are
            // thread-safe (guarded by the shim's connected atomic).
            injectPasteChord(shimGuard.data(), wrapCtrl);
        },
        Qt::QueuedConnection);
}

void ClipboardBridge::setFromClient(const QString& text)
{
    QMetaObject::invokeMethod(
        this, [this, text]() { commitText(ClipboardMsg::capText(text)); }, Qt::QueuedConnection);
}

void ClipboardBridge::commitText(const QString& t)
{
    if (t.isEmpty() || t == m_LastText) return;
    m_LastText = t;
    QGuiApplication::clipboard()->setText(t);
}

void ClipboardBridge::requestAnnounce()
{
    QMetaObject::invokeMethod(
        this,
        [this]() {
            const QString text = ClipboardMsg::capText(QGuiApplication::clipboard()->text());
            if (text.isEmpty()) return;
            m_LastText = text;
            emit hostTextChanged(text);
        },
        Qt::QueuedConnection);
}

void ClipboardBridge::onClipboardChanged()
{
    // Don't even read the clipboard while no session is listening.
    static const QMetaMethod changedSignal =
        QMetaMethod::fromSignal(&ClipboardBridge::hostTextChanged);
    if (!isSignalConnected(changedSignal)) return;

    const QString text = ClipboardMsg::capText(QGuiApplication::clipboard()->text());
    // Empty = cleared or non-text content (image/files) — text-only bridge.
    // Identical text = echo of our own setText() or a no-op copy.
    if (text.isEmpty() || text == m_LastText) return;

    m_LastText = text;
    emit hostTextChanged(text);
}
