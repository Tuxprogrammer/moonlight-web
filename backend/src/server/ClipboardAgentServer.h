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

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>

class ClipboardAgentRegistry;
class QTimer;
class QWebSocket;
class QWebSocketServer;

/// What ClipboardAgentRouter needs from the agents, so it can be tested
/// against a fake.
class ClipboardAgentLink
{
public:
    virtual ~ClipboardAgentLink() = default;
    /// @p host has an enabled, ready agent.
    virtual bool isAvailable(const QString& host) const = 0;
    /// Ask @p host's agent to report clipboard changes (a session that may
    /// read them is streaming) or to stop.
    virtual void setWatching(const QString& host, bool on) = 0;
    /// Ask @p host's agent for its current clipboard, once.
    virtual void requestAnnounce(const QString& host) = 0;
    /// Put @p text on @p host's clipboard. @p done runs exactly once: true
    /// when the agent confirmed, false on refusal, timeout or no agent.
    virtual void sendSet(const QString& host, const QString& text,
                         std::function<void(bool ok)> done) = 0;
};

/// The server end of the clipboard agents' WebSockets (main process).
///
/// Listens on loopback only. Agents reach it through HttpServer's
/// /ws/clipboard-agent upgrade, which checks TLS, Origin and the token before
/// it proxies the connection here (the same shape as /ws/control). The token is
/// checked again on arrival, because this port is reachable from the machine
/// itself, and it is what binds the socket to its host.
///
/// Speaks ClipboardAgentProtocol: waits for a hello, answers welcome, relays
/// watch / announce / set to the agent and changed / ack back. Clipboard text
/// is never logged.
class ClipboardAgentServer : public QObject, public ClipboardAgentLink
{
    Q_OBJECT

public:
    explicit ClipboardAgentServer(ClipboardAgentRegistry& registry, QObject* parent = nullptr);
    ~ClipboardAgentServer() override;

    bool listen(const QHostAddress& address = QHostAddress::LocalHost, quint16 port = 0);
    quint16 port() const;

    // Timings, shortened by the tests.
    void setAckTimeoutMs(int ms) { m_AckTimeoutMs = ms; }
    void setHelloTimeoutMs(int ms) { m_HelloTimeoutMs = ms; }
    void setIdleTimeoutMs(int ms) { m_IdleTimeoutMs = ms; }

    /// How long a set waits for its ack before it counts as failed.
    static constexpr int kAckTimeoutMs = 1500;
    /// An authenticated socket that says nothing this long is closed.
    static constexpr int kHelloTimeoutMs = 10000;
    /// The agent pings every 30 s; three missed pings and it is gone.
    static constexpr int kIdleTimeoutMs = 95000;

    /// "Bearer <token>" → token; empty for anything else.
    static QString bearerToken(const QByteArray& authorizationHeader);

    // ClipboardAgentLink
    bool isAvailable(const QString& host) const override;
    void setWatching(const QString& host, bool on) override;
    void requestAnnounce(const QString& host) override;
    void sendSet(const QString& host, const QString& text,
                 std::function<void(bool ok)> done) override;

    /// Whether @p host's agent is (or, once connected, will be) asked to watch:
    /// a session may read its clipboard and the admin enabled it.
    bool wantsWatch(const QString& host) const;

signals:
    /// @p host's agent reported its clipboard (only while watching).
    void agentText(const QString& host, const QString& text);

private:
    struct Conn
    {
        QString host;
        bool ready = false;
        QTimer* idle = nullptr;
    };
    struct Pending
    {
        QString host;
        std::function<void(bool)> done;
        QTimer* timer = nullptr;
    };

    void onNewConnection();
    void onTextMessage(QWebSocket* socket, const QString& message);
    void onDisconnected(QWebSocket* socket);
    void closeHost(const QString& host, int code, const QString& reason);
    void closeSocket(QWebSocket* socket, int code, const QString& reason);
    void resolve(qint64 id, bool ok);
    /// @p host's socket no longer counts: mark it gone, fail its pending sets.
    void detachHost(const QString& host);
    /// The ready socket serving @p host, or null.
    QWebSocket* readySocket(const QString& host) const;
    void send(QWebSocket* socket, const QByteArray& frame);
    /// Tell @p host's ready agent to watch or stop, when that changed.
    void refreshWatch(const QString& host);

    ClipboardAgentRegistry& m_Registry;
    QWebSocketServer* m_Server = nullptr;
    QHash<QWebSocket*, Conn> m_Conns;
    /// The one socket per host that counts (a newer one replaces it).
    QHash<QString, QPointer<QWebSocket>> m_ByHost;
    QHash<qint64, Pending> m_Pending;
    /// Hosts the router wants watched, and hosts whose agent was told to.
    QSet<QString> m_Watching;
    QSet<QString> m_WatchSent;
    qint64 m_NextId = 0;
    int m_AckTimeoutMs = kAckTimeoutMs;
    int m_HelloTimeoutMs = kHelloTimeoutMs;
    int m_IdleTimeoutMs = kIdleTimeoutMs;
};
