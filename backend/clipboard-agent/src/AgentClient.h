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

#include "AgentConfig.h"
#include "clipboard/ClipboardEcho.h"

#include <QObject>
#include <QPointer>
#include <QSslError>

class ClipboardBackend;
class QTimer;
class QWebSocket;

/// The agent's connection to the server (docs/design/clipboard-agent.md).
///
/// Dials wss://<server>/ws/clipboard-agent with the host's token as a bearer,
/// checks the server's key against the pin before the handshake carries the
/// token anywhere, says hello, and from then on does what the server asks:
/// watch the clipboard (and report its changes), announce it once, write to
/// it and acknowledge. Reconnects with backoff when the line drops. Gives up
/// for good — fatal() — when the server refuses the token, another agent took
/// the host over, or the protocol versions differ: retrying would only fill
/// the server's failed-login counters.
///
/// What it wrote is never reported back (ClipboardEcho), and nothing is read
/// from the clipboard unless the server asked it to watch. Clipboard text is
/// never logged.
class AgentClient : public QObject
{
    Q_OBJECT

public:
    /// Process exit codes for fatal(); the systemd unit does not restart on
    /// them (RestartPreventExitStatus).
    enum ExitCode : int
    {
        ExitTokenRefused = 2,
        ExitReplaced = 3,
        ExitVersion = 4,
    };

    AgentClient(const AgentConfig& config, ClipboardBackend* backend, QObject* parent = nullptr);
    ~AgentClient() override;

    void start();
    void stop();

    bool isWelcomed() const { return m_Welcomed; }
    bool isWatching() const { return m_Watching; }

    // Timings, shortened by the tests.
    void setReconnectDelays(int initialMs, int maxMs);
    void setPingIntervalMs(int ms) { m_PingIntervalMs = ms; }

    static constexpr int kPingIntervalMs = 30000;
    static constexpr int kWelcomeTimeoutMs = 10000;

    /// What the hello says the agent runs on.
    static QString platform();
    static QString version();

signals:
    void welcomed();
    void connectionLost();
    /// Stop for good with process exit code @p code.
    void fatal(int code, const QString& reason);

private:
    void connectNow();
    void scheduleReconnect();
    void onSslErrors(QWebSocket* ws, const QList<QSslError>& errors);
    void onConnected(QWebSocket* ws);
    void onDisconnected(QWebSocket* ws);
    void onError(QWebSocket* ws);
    void onText(QWebSocket* ws, const QString& frame);
    void onObserved(const QString& text);
    void setWatch(bool on);
    void send(const QByteArray& frame);
    void giveUp(int code, const QString& reason);

    AgentConfig m_Config;
    QPointer<ClipboardBackend> m_Backend;
    QPointer<QWebSocket> m_Ws;
    QTimer* m_Reconnect = nullptr;
    QTimer* m_Ping = nullptr;
    QTimer* m_WelcomeDeadline = nullptr;
    ClipboardEcho m_Echo;
    bool m_Stopped = true;
    bool m_Welcomed = false;
    bool m_Watching = false;
    /// This attempt was dropped over the server's certificate (said once).
    bool m_TlsRefused = false;
    int m_Attempt = 0;
    /// Bumped per connection, so an ack finishing after a reconnect is dropped.
    quint64 m_Generation = 0;
    qint64 m_LastRxMs = 0;
    int m_InitialDelayMs = 1000;
    int m_MaxDelayMs = 60000;
    int m_PingIntervalMs = kPingIntervalMs;
};
