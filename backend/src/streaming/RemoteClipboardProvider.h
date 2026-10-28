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

#include "ClipboardProvider.h"
#include "clipboard/ClipboardIpc.h"

#include <QHash>
#include <QJsonObject>
#include <QPointer>

#include <atomic>
#include <functional>

class QTimer;

/// The host clipboard of a remote host, through its clipboard agent — the
/// stream worker's end (docs/design/clipboard-agent.md).
///
/// The agent's socket is the server's; this provider talks to the server over
/// the worker's stdout/stdin (ClipboardIpc):
///  - a write goes out as clipboardSet; a paste injects its chord only once
///    the server answers clipboardAck, so the keystrokes never overtake the
///    write. An answer that does not come within kAckTimeoutMs, or a refusal,
///    still injects the chord: the host then pastes what it already held,
///    which is what Ctrl+V did before any clipboard sync existed;
///  - clipboardText from the server becomes hostTextChanged;
///  - clipboardAgent switches available() on and off, and the relays tell the
///    browser (clipboardcaps).
///
/// Installed by StreamWorkerMain only when the server said this session may
/// use an agent; Session hands it to the relays then. Main thread.
class RemoteClipboardProvider : public ClipboardProvider
{
    Q_OBJECT

public:
    using EmitEvent = std::function<void(const QJsonObject& event)>;

    /// Longer than the server's own wait for the agent (1.5 s), so the
    /// server's answer, even a "no", normally arrives first.
    static constexpr int kAckTimeoutMs = 2500;

    RemoteClipboardProvider(EmitEvent emitEvent, bool available, QObject* parent = nullptr);
    ~RemoteClipboardProvider() override;

    /// The worker's one provider, or null when the server installed none.
    static RemoteClipboardProvider* instance();
    static void install(RemoteClipboardProvider* provider);

    bool available() const override { return m_Available.load(); }
    void pasteFromClient(IMediaEngine* engine, const QString& text, bool wrapCtrl) override;
    void setFromClient(const QString& text) override;
    void requestAnnounce() override;

    /// A clipboard command from the server. Main thread.
    void handleCommand(const ClipboardIpc::Command& command);

    void setAckTimeoutMs(int ms) { m_AckTimeoutMs = ms; }
    int pendingCount() const { return m_Pending.size(); }

private:
    struct Pending
    {
        QPointer<IMediaEngine> engine;
        bool paste = false;
        bool wrapCtrl = false;
        QTimer* timer = nullptr;
    };

    /// Main thread: send @p text to the server, remember what to do on ack.
    void write(const QString& text, bool paste, IMediaEngine* engine, bool wrapCtrl);
    void finish(qint64 id, bool ok, bool timedOut);

    EmitEvent m_EmitEvent;
    std::atomic<bool> m_Available;
    QHash<qint64, Pending> m_Pending;
    qint64 m_NextId = 0;
    int m_AckTimeoutMs = kAckTimeoutMs;
};
