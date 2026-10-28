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

#include "RemoteClipboardProvider.h"
#include "IMediaEngine.h"

#include <QTimer>
#include <QtDebug>

namespace {
RemoteClipboardProvider* g_Instance = nullptr;
}

RemoteClipboardProvider::RemoteClipboardProvider(EmitEvent emitEvent, bool available,
                                                 QObject* parent)
    : ClipboardProvider(parent)
    , m_EmitEvent(std::move(emitEvent))
    , m_Available(available)
{}

RemoteClipboardProvider::~RemoteClipboardProvider()
{
    if (g_Instance == this) g_Instance = nullptr;
}

RemoteClipboardProvider* RemoteClipboardProvider::instance()
{
    return g_Instance;
}

void RemoteClipboardProvider::install(RemoteClipboardProvider* provider)
{
    g_Instance = provider;
}

void RemoteClipboardProvider::pasteFromClient(IMediaEngine* engine, const QString& text,
                                              bool wrapCtrl)
{
    // The engine dies on the relay thread; the guard turns a teardown racing
    // this queued call into a no-op.
    QPointer<IMediaEngine> guard(engine);
    QMetaObject::invokeMethod(
        this,
        [this, guard, text, wrapCtrl]() {
            const QString t = ClipboardMsg::capText(text);
            if (t.isEmpty() || !available()) {
                // Nothing to write, or nowhere to write it: the chord alone,
                // and the host pastes its own clipboard.
                injectPasteChord(guard.data(), wrapCtrl);
                return;
            }
            write(t, true, guard.data(), wrapCtrl);
        },
        Qt::QueuedConnection);
}

void RemoteClipboardProvider::setFromClient(const QString& text)
{
    QMetaObject::invokeMethod(
        this,
        [this, text]() {
            const QString t = ClipboardMsg::capText(text);
            if (t.isEmpty()) return;
            if (!available()) {
                qDebug() << "[RemoteClipboard] Clipboard set dropped: no agent right now";
                return;
            }
            write(t, false, nullptr, false);
        },
        Qt::QueuedConnection);
}

void RemoteClipboardProvider::requestAnnounce()
{
    QMetaObject::invokeMethod(
        this,
        [this]() {
            if (available() && m_EmitEvent) m_EmitEvent(ClipboardIpc::announceEvent());
        },
        Qt::QueuedConnection);
}

void RemoteClipboardProvider::write(const QString& text, bool paste, IMediaEngine* engine,
                                    bool wrapCtrl)
{
    const qint64 id = ++m_NextId;
    Pending p;
    p.engine = engine;
    p.paste = paste;
    p.wrapCtrl = wrapCtrl;
    p.timer = new QTimer(this);
    p.timer->setSingleShot(true);
    connect(p.timer, &QTimer::timeout, this, [this, id]() { finish(id, false, true); });
    p.timer->start(m_AckTimeoutMs);
    m_Pending.insert(id, p);
    if (m_EmitEvent) m_EmitEvent(ClipboardIpc::setEvent(id, text, paste));
}

void RemoteClipboardProvider::finish(qint64 id, bool ok, bool timedOut)
{
    auto it = m_Pending.find(id);
    if (it == m_Pending.end()) return; // already settled (an ack after its timeout)
    const Pending p = *it;
    m_Pending.erase(it);
    if (p.timer) {
        p.timer->stop();
        p.timer->deleteLater();
    }
    if (!ok)
        qInfo() << "[RemoteClipboard] Host clipboard write"
                << (timedOut ? "not confirmed in time" : "refused")
                << (p.paste ? "— pasting what the host already holds" : "");
    if (p.paste) injectPasteChord(p.engine.data(), p.wrapCtrl);
}

void RemoteClipboardProvider::handleCommand(const ClipboardIpc::Command& c)
{
    switch (c.kind) {
    case ClipboardIpc::Command::Kind::None: return;
    case ClipboardIpc::Command::Kind::Available:
        if (m_Available.exchange(c.available) != c.available) {
            qInfo() << "[RemoteClipboard] Host clipboard agent"
                    << (c.available ? "available" : "unavailable");
            emit availabilityChanged(c.available);
        }
        return;
    case ClipboardIpc::Command::Kind::Text:
        // Late text after the agent went away is not for the browser anymore.
        if (available()) emit hostTextChanged(c.text);
        return;
    case ClipboardIpc::Command::Kind::Ack: finish(c.id, c.ok, false); return;
    }
}
