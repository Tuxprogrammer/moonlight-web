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

#include "ClipboardAgentServer.h"

#include "ClipboardAgentRegistry.h"
#include "clipboard/ClipboardAgentProtocol.h"

#include <QDateTime>
#include <QNetworkRequest>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QtDebug>

namespace P = ClipboardAgent;

namespace {
qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}
} // namespace

ClipboardAgentServer::ClipboardAgentServer(ClipboardAgentRegistry& registry, QObject* parent)
    : QObject(parent)
    , m_Registry(registry)
{
    // A rotated or revoked token takes its agent with it, at once.
    connect(&m_Registry, &ClipboardAgentRegistry::credentialsRevoked, this,
            [this](const QString& host) {
                closeHost(host, P::CloseRevoked, QStringLiteral("token revoked"));
            });
    // The admin's switch decides whether the agent watches at all: a disabled
    // host's clipboard does not leave the host, session or not.
    connect(&m_Registry, &ClipboardAgentRegistry::availabilityChanged, this,
            [this](const QString& host, bool) { refreshWatch(host); });
}

ClipboardAgentServer::~ClipboardAgentServer()
{
    // Nobody is left to call back once the server goes.
    for (auto it = m_Pending.begin(); it != m_Pending.end(); ++it)
        if (it->timer) it->timer->stop();
    m_Pending.clear();
}

bool ClipboardAgentServer::listen(const QHostAddress& address, quint16 port)
{
    if (!m_Server) {
        m_Server = new QWebSocketServer(QStringLiteral("MoonlightWeb clipboard agents"),
                                        QWebSocketServer::NonSecureMode, this);
        m_Server->setMaxPendingConnections(16);
        connect(m_Server, &QWebSocketServer::newConnection, this,
                &ClipboardAgentServer::onNewConnection);
    }
    if (!m_Server->listen(address, port)) {
        qWarning().noquote() << "[ClipboardAgent] Cannot listen:" << m_Server->errorString();
        return false;
    }
    qInfo() << "[ClipboardAgent] Agent endpoint on" << address.toString() << m_Server->serverPort();
    return true;
}

quint16 ClipboardAgentServer::port() const
{
    return m_Server ? m_Server->serverPort() : 0;
}

QString ClipboardAgentServer::bearerToken(const QByteArray& header)
{
    const QByteArray h = header.trimmed();
    if (h.size() <= 7 || qstrnicmp(h.constData(), "Bearer ", 7) != 0) return QString();
    return QString::fromLatin1(h.mid(7).trimmed());
}

void ClipboardAgentServer::onNewConnection()
{
    while (QWebSocket* socket = m_Server->nextPendingConnection()) {
        socket->setParent(this);
        socket->setMaxAllowedIncomingMessageSize(P::kMaxFrameBytes);
        socket->setMaxAllowedIncomingFrameSize(P::kMaxFrameBytes);

        const QString host = m_Registry.hostForToken(
            bearerToken(socket->request().rawHeader(QByteArrayLiteral("Authorization"))));
        if (host.isEmpty()) {
            // HttpServer refuses these before the upgrade; only a local process
            // talking to this port directly gets this far.
            qWarning() << "[ClipboardAgent] Connection without a valid token refused";
            socket->close(QWebSocketProtocol::CloseCodePolicyViolated,
                          QStringLiteral("unauthorized"));
            socket->deleteLater();
            continue;
        }

        // One agent per host: the newcomer wins (a restarted agent, typically,
        // whose old socket has not timed out yet).
        if (QWebSocket* old = m_ByHost.value(host).data()) {
            qInfo().noquote() << "[ClipboardAgent] A new agent replaces the one for host" << host;
            closeSocket(old, P::CloseReplaced, QStringLiteral("replaced"));
        }

        Conn conn;
        conn.host = host;
        conn.idle = new QTimer(socket);
        conn.idle->setSingleShot(true);
        conn.idle->setInterval(m_HelloTimeoutMs);
        connect(conn.idle, &QTimer::timeout, socket, [this, socket]() {
            const bool ready = m_Conns.value(socket).ready;
            qWarning() << "[ClipboardAgent]"
                       << (ready ? "Agent went silent — closing" : "No hello in time — closing");
            closeSocket(socket, P::CloseProtocolError,
                        ready ? QStringLiteral("idle") : QStringLiteral("no hello"));
        });
        conn.idle->start();
        m_Conns.insert(socket, conn);
        m_ByHost.insert(host, socket);
        m_Registry.markConnected(host, nowMs());

        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket](const QString& message) { onTextMessage(socket, message); });
        connect(socket, &QWebSocket::binaryMessageReceived, this, [this, socket]() {
            closeSocket(socket, P::CloseProtocolError, QStringLiteral("binary frame"));
        });
        connect(socket, &QWebSocket::disconnected, this,
                [this, socket]() { onDisconnected(socket); });
    }
}

void ClipboardAgentServer::onTextMessage(QWebSocket* socket, const QString& message)
{
    auto it = m_Conns.find(socket);
    if (it == m_Conns.end()) return;
    const P::Message m = P::decode(message.toUtf8());
    const QString host = it->host;

    if (m.type == P::Type::Invalid) {
        qWarning().noquote() << "[ClipboardAgent] Unreadable frame from host" << host << ":"
                             << m.error;
        closeSocket(socket, P::CloseProtocolError, m.error);
        return;
    }

    if (!it->ready) {
        if (m.type != P::Type::Hello) {
            closeSocket(socket, P::CloseProtocolError, QStringLiteral("hello first"));
            return;
        }
        if (m.version != P::kProtocolVersion) {
            qWarning() << "[ClipboardAgent] Agent speaks protocol" << m.version << ", not"
                       << P::kProtocolVersion;
            closeSocket(socket, P::CloseVersion, QStringLiteral("unsupported version"));
            return;
        }
        it->ready = true;
        it->idle->setInterval(m_IdleTimeoutMs);
        it->idle->start();
        const bool watchOn = wantsWatch(host);
        if (watchOn)
            m_WatchSent.insert(host);
        else
            m_WatchSent.remove(host);
        send(socket, P::welcome(watchOn));
        m_Registry.markReady(host, m.platform, m.backend, m.agentVersion, nowMs());
        return;
    }

    it->idle->start(); // any frame proves it is alive
    switch (m.type) {
    case P::Type::Ping: send(socket, P::pong()); return;
    case P::Type::Pong: return;
    case P::Type::Ack: {
        const auto p = m_Pending.constFind(m.id);
        // Only the host the set went to may answer it.
        if (p != m_Pending.constEnd() && p->host == host) resolve(m.id, m.ok);
        return;
    }
    case P::Type::Changed:
        // Never asked for, never passed on: the agent should not have sent it.
        if (!wantsWatch(host)) return;
        if (m.truncated) qInfo() << "[ClipboardAgent] Host clipboard truncated to the cap";
        emit agentText(host, m.text);
        return;
    default:
        closeSocket(socket, P::CloseProtocolError,
                    QStringLiteral("unexpected %1").arg(P::typeName(m.type)));
        return;
    }
}

void ClipboardAgentServer::onDisconnected(QWebSocket* socket)
{
    const auto it = m_Conns.constFind(socket);
    if (it != m_Conns.constEnd()) {
        const QString host = it->host;
        m_Conns.erase(it);
        if (m_ByHost.value(host).data() == socket) {
            qInfo().noquote() << "[ClipboardAgent] Agent for host" << host << "disconnected";
            detachHost(host);
        }
    }
    socket->deleteLater();
}

void ClipboardAgentServer::closeHost(const QString& host, int code, const QString& reason)
{
    if (QWebSocket* s = m_ByHost.value(host).data()) closeSocket(s, code, reason);
}

void ClipboardAgentServer::closeSocket(QWebSocket* socket, int code, const QString& reason)
{
    // Detach first so the host is free for a replacement the moment we decide,
    // not when the close handshake finishes.
    const auto it = m_Conns.constFind(socket);
    if (it != m_Conns.constEnd() && m_ByHost.value(it->host).data() == socket) detachHost(it->host);
    socket->close(static_cast<QWebSocketProtocol::CloseCode>(code), reason);
}

void ClipboardAgentServer::detachHost(const QString& host)
{
    m_ByHost.remove(host);
    m_WatchSent.remove(host);
    m_Registry.markDisconnected(host, nowMs());
    // Whatever was waiting on this agent will not be answered.
    QList<qint64> orphans;
    for (auto p = m_Pending.constBegin(); p != m_Pending.constEnd(); ++p)
        if (p->host == host) orphans.append(p.key());
    for (qint64 id : orphans)
        resolve(id, false);
}

QWebSocket* ClipboardAgentServer::readySocket(const QString& host) const
{
    QWebSocket* s = m_ByHost.value(host).data();
    if (!s) return nullptr;
    const auto it = m_Conns.constFind(s);
    return it != m_Conns.constEnd() && it->ready ? s : nullptr;
}

void ClipboardAgentServer::send(QWebSocket* socket, const QByteArray& frame)
{
    socket->sendTextMessage(QString::fromUtf8(frame));
}

bool ClipboardAgentServer::isAvailable(const QString& host) const
{
    return m_Registry.isAvailable(host);
}

void ClipboardAgentServer::setWatching(const QString& host, bool on)
{
    if (on)
        m_Watching.insert(host);
    else
        m_Watching.remove(host);
    refreshWatch(host);
}

bool ClipboardAgentServer::wantsWatch(const QString& host) const
{
    return m_Watching.contains(host) && m_Registry.isEnabled(host);
}

void ClipboardAgentServer::refreshWatch(const QString& host)
{
    QWebSocket* s = readySocket(host);
    if (!s) return;
    const bool want = wantsWatch(host);
    if (want == m_WatchSent.contains(host)) return;
    if (want)
        m_WatchSent.insert(host);
    else
        m_WatchSent.remove(host);
    send(s, P::watch(want));
}

void ClipboardAgentServer::requestAnnounce(const QString& host)
{
    if (!m_Registry.isEnabled(host)) return;
    if (QWebSocket* s = readySocket(host)) send(s, P::announce());
}

void ClipboardAgentServer::sendSet(const QString& host, const QString& text,
                                   std::function<void(bool ok)> done)
{
    QWebSocket* s = m_Registry.isAvailable(host) ? readySocket(host) : nullptr;
    if (!s || text.isEmpty()) {
        if (done) done(false);
        return;
    }
    const qint64 id = ++m_NextId;
    Pending p;
    p.host = host;
    p.done = std::move(done);
    p.timer = new QTimer(this);
    p.timer->setSingleShot(true);
    connect(p.timer, &QTimer::timeout, this, [this, id]() {
        qWarning() << "[ClipboardAgent] The agent did not confirm a clipboard write in time";
        resolve(id, false);
    });
    p.timer->start(m_AckTimeoutMs);
    m_Pending.insert(id, std::move(p));
    send(s, P::set(id, text));
}

void ClipboardAgentServer::resolve(qint64 id, bool ok)
{
    auto it = m_Pending.find(id);
    if (it == m_Pending.end()) return;
    Pending p = std::move(*it);
    m_Pending.erase(it);
    if (p.timer) {
        p.timer->stop();
        p.timer->deleteLater();
    }
    if (p.done) p.done(ok);
}
