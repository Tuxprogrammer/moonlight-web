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

#include "QtClipboardBackend.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QTimer>

QtClipboardBackend::QtClipboardBackend(const QString& name, QObject* parent)
    : ClipboardBackend(parent)
    , m_Name(name)
{}

void QtClipboardBackend::start()
{
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        emit failed(QStringLiteral("no display connection"));
        return;
    }
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this,
            &QtClipboardBackend::onChanged);
#ifdef Q_OS_MACOS
    m_Poll = new QTimer(this);
    m_Poll->setInterval(kPollMs);
    connect(m_Poll, &QTimer::timeout, this, [this]() {
        const QString t = QGuiApplication::clipboard()->text();
        if (t == m_Polled) return;
        m_Polled = t;
        emit observed(t);
    });
#endif
    QMetaObject::invokeMethod(this, &ClipboardBackend::ready, Qt::QueuedConnection);
}

void QtClipboardBackend::setWatching(bool on)
{
    m_Watching = on;
    if (!m_Poll) return;
    if (on) {
        m_Polled = QGuiApplication::clipboard()->text();
        m_Poll->start();
    } else {
        m_Poll->stop();
        m_Polled.clear();
    }
}

void QtClipboardBackend::onChanged()
{
    if (!m_Watching) return;
    emit observed(QGuiApplication::clipboard()->text());
}

void QtClipboardBackend::read()
{
    const QString t = QGuiApplication::clipboard()->text();
    // Async like the other backends: the caller never sees observed() from
    // inside its own call.
    QMetaObject::invokeMethod(this, [this, t]() { emit observed(t); }, Qt::QueuedConnection);
}

void QtClipboardBackend::write(const QString& text, std::function<void(bool ok)> done)
{
    QGuiApplication::clipboard()->setText(text, QClipboard::Clipboard);
    if (m_Poll) m_Polled = text;
    if (done) done(true);
}
