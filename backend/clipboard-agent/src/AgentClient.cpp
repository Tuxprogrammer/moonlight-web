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

#include "AgentClient.h"

#include "ClipboardBackend.h"
#include "PinCheck.h"
#include "clipboard/ClipboardAgentProtocol.h"
#include "streaming/ClipboardMessage.h"

#include <QAuthenticator>
#include <QDateTime>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QSslConfiguration>
#include <QSysInfo>
#include <QTimer>
#include <QWebSocket>
#include <QtDebug>

namespace P = ClipboardAgent;

#ifndef MW_AGENT_VERSION
#define MW_AGENT_VERSION "1.0.0"
#endif

namespace {
qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}
} // namespace

AgentClient::AgentClient(const AgentConfig& config, ClipboardBackend* backend, QObject* parent)
    : QObject(parent)
    , m_Config(config)
    , m_Backend(backend)
{
    m_Reconnect = new QTimer(this);
    m_Reconnect->setSingleShot(true);
    connect(m_Reconnect, &QTimer::timeout, this, &AgentClient::connectNow);

    m_WelcomeDeadline = new QTimer(this);
    m_WelcomeDeadline->setSingleShot(true);
    connect(m_WelcomeDeadline, &QTimer::timeout, this, [this]() {
        qWarning() << "[agent] No welcome from the server — reconnecting";
        if (m_Ws) m_Ws->abort();
    });

    m_Ping = new QTimer(this);
    connect(m_Ping, &QTimer::timeout, this, [this]() {
        // Silent for two and a half intervals: the line is dead even if TCP
        // has not noticed yet.
        if (nowMs() - m_LastRxMs > m_PingIntervalMs * 5 / 2) {
            qWarning() << "[agent] The server stopped answering — reconnecting";
            if (m_Ws) m_Ws->abort();
            return;
        }
        send(P::ping());
    });

    if (m_Backend) connect(m_Backend, &ClipboardBackend::observed, this, &AgentClient::onObserved);
}

AgentClient::~AgentClient()
{
    m_Stopped = true;
    if (m_Ws) {
        m_Ws->disconnect(this);
        m_Ws->abort();
    }
}

QString AgentClient::platform()
{
    return P::sanitizeLabel(QSysInfo::prettyProductName());
}

QString AgentClient::version()
{
    return QStringLiteral(MW_AGENT_VERSION);
}

void AgentClient::setReconnectDelays(int initialMs, int maxMs)
{
    m_InitialDelayMs = initialMs;
    m_MaxDelayMs = maxMs;
}

void AgentClient::start()
{
    m_Stopped = false;
    m_Attempt = 0;
    connectNow();
}

void AgentClient::stop()
{
    m_Stopped = true;
    m_Reconnect->stop();
    m_Ping->stop();
    m_WelcomeDeadline->stop();
    if (m_Ws) m_Ws->close();
}

void AgentClient::connectNow()
{
    if (m_Stopped) return;
    if (m_Ws) {
        m_Ws->disconnect(this);
        m_Ws->abort();
        m_Ws->deleteLater();
    }
    ++m_Generation;
    m_Welcomed = false;
    m_TlsRefused = false;

    auto* ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_Ws = ws;
    ws->setMaxAllowedIncomingMessageSize(P::kMaxFrameBytes);
    ws->setMaxAllowedIncomingFrameSize(P::kMaxFrameBytes);

    if (m_Config.url.scheme() == QLatin1String("wss")) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
        if (!m_Config.pins.isEmpty()) {
            // No CA at all: every certificate then fails verification, so the
            // pin is checked in onSslErrors on every connection — before the
            // handshake, which carries the token, is sent.
            ssl.setCaCertificates({});
        }
        ws->setSslConfiguration(ssl);
        connect(ws, &QWebSocket::sslErrors, this,
                [this, ws](const QList<QSslError>& errors) { onSslErrors(ws, errors); });
    }
    connect(ws, &QWebSocket::connected, this, [this, ws]() { onConnected(ws); });
    connect(ws, &QWebSocket::disconnected, this, [this, ws]() { onDisconnected(ws); });
    connect(ws, &QWebSocket::errorOccurred, this, [this, ws]() { onError(ws); });
    // A 401 asks for credentials; the bearer was all there is to give.
    connect(ws, &QWebSocket::authenticationRequired, this, [this, ws](QAuthenticator*) {
        if (ws != m_Ws) return;
        giveUp(ExitTokenRefused, QStringLiteral("the server refused this agent's token"));
    });
    connect(ws, &QWebSocket::textMessageReceived, this,
            [this, ws](const QString& frame) { onText(ws, frame); });

    QNetworkRequest req(m_Config.url);
    req.setRawHeader(QByteArrayLiteral("Authorization"), "Bearer " + m_Config.token.toLatin1());
    req.setRawHeader(QByteArrayLiteral("User-Agent"),
                     "moonlightweb-clipboard-agent/" + version().toLatin1());
    qInfo().noquote() << "[agent] Connecting to"
                      << m_Config.url.toDisplayString(QUrl::RemoveUserInfo);
    ws->open(req);
}

void AgentClient::scheduleReconnect()
{
    if (m_Stopped || m_Reconnect->isActive()) return;
    const int delay = backoffDelayMs(m_Attempt++, m_InitialDelayMs, m_MaxDelayMs,
                                     QRandomGenerator::global()->generateDouble());
    qInfo() << "[agent] Reconnecting in" << delay / 1000.0 << "s";
    m_Reconnect->start(delay);
}

void AgentClient::onSslErrors(QWebSocket* ws, const QList<QSslError>& errors)
{
    if (ws != m_Ws) return;
    QSslCertificate cert = ws->sslConfiguration().peerCertificate();
    if (cert.isNull() && !errors.isEmpty()) cert = errors.first().certificate();

    if (!m_Config.pins.isEmpty()) {
        if (PinCheck::matches(cert, m_Config.pins)) {
            ws->ignoreSslErrors();
            return;
        }
        qWarning().noquote() << "[agent] The server's key does not match the pin: it presents"
                             << PinCheck::pinText(cert)
                             << "— refusing to connect (issue a new token on the admin page if "
                                "the server's certificate key really changed)";
        m_TlsRefused = true;
        ws->abort();
        return;
    }
    for (const QSslError& e : errors)
        qWarning().noquote() << "[agent] TLS:" << e.errorString();
    qWarning() << "[agent] The server's certificate does not verify; pass its --pin from the "
                  "admin page";
    m_TlsRefused = true;
    ws->abort();
}

void AgentClient::onConnected(QWebSocket* ws)
{
    if (ws != m_Ws) return;
    m_LastRxMs = nowMs();
    send(P::hello(platform(), m_Backend ? m_Backend->name() : QString(), version()));
    m_WelcomeDeadline->start(kWelcomeTimeoutMs);
}

void AgentClient::onDisconnected(QWebSocket* ws)
{
    if (ws != m_Ws) return;
    const int code = ws->closeCode();
    const QString reason = ws->closeReason();
    const bool wasWelcomed = m_Welcomed;
    m_Welcomed = false;
    m_Ping->stop();
    m_WelcomeDeadline->stop();
    if (m_Watching) {
        m_Watching = false;
        if (m_Backend) m_Backend->setWatching(false);
    }
    if (wasWelcomed) {
        qInfo() << "[agent] Disconnected (" << code << reason << ")";
        emit connectionLost();
    }
    switch (code) {
    case P::CloseRevoked:
        giveUp(ExitTokenRefused, QStringLiteral("the server revoked this agent's token"));
        return;
    case P::CloseReplaced:
        giveUp(ExitReplaced, QStringLiteral("another agent connected for this host"));
        return;
    case P::CloseVersion:
        giveUp(ExitVersion, QStringLiteral("the server speaks another protocol version"));
        return;
    default: break;
    }
    scheduleReconnect();
}

void AgentClient::onError(QWebSocket* ws)
{
    if (ws != m_Ws) return;
    const QString why = ws->errorString();
    // The upgrade's HTTP answer: 401 is a token the server does not know (or
    // a host it no longer lists), 403 a refusal no retry will change. Qt words
    // a 401 after its authentication handling ("Host requires authentication",
    // "Unsupported WWW-Authenticate challenges"), any other status by number.
    const bool unauthorized = why.contains(QLatin1String("authenticat"), Qt::CaseInsensitive) ||
                              why.contains(QLatin1String(" 401"));
    if (unauthorized || why.contains(QLatin1String(" 403"))) {
        giveUp(ExitTokenRefused, QStringLiteral("the server refused this agent (%1)")
                                     .arg(unauthorized ? QStringLiteral("unknown or revoked token")
                                                       : QStringLiteral("forbidden")));
        return;
    }
    // Already said why, in onSslErrors.
    if (!m_TlsRefused) qWarning().noquote() << "[agent] Connection error:" << why;
    scheduleReconnect();
}

void AgentClient::giveUp(int code, const QString& reason)
{
    if (m_Stopped) return;
    qCritical().noquote() << "[agent] Stopping:" << reason;
    stop();
    emit fatal(code, reason);
}

void AgentClient::onText(QWebSocket* ws, const QString& frame)
{
    if (ws != m_Ws) return;
    m_LastRxMs = nowMs();
    const P::Message m = P::decode(frame.toUtf8());
    if (m.type == P::Type::Invalid) {
        qDebug().noquote() << "[agent] Ignored an unreadable frame:" << m.error;
        return;
    }

    if (!m_Welcomed) {
        if (m.type != P::Type::Welcome) return;
        if (m.version != P::kProtocolVersion) {
            ws->close(static_cast<QWebSocketProtocol::CloseCode>(P::CloseVersion));
            giveUp(ExitVersion, QStringLiteral("the server speaks protocol %1").arg(m.version));
            return;
        }
        m_Welcomed = true;
        m_Attempt = 0;
        m_WelcomeDeadline->stop();
        m_Ping->start(m_PingIntervalMs);
        qInfo() << "[agent] Connected; the server" << (m.watch ? "is" : "is not")
                << "watching this host's clipboard";
        emit welcomed();
        setWatch(m.watch);
        return;
    }

    switch (m.type) {
    case P::Type::Watch: setWatch(m.watch); return;
    case P::Type::Announce:
        if (!m_Watching || !m_Backend) return;
        m_Echo.forget();
        m_Backend->read();
        return;
    case P::Type::Set: {
        if (!m_Backend) {
            send(P::ack(m.id, false));
            return;
        }
        // Its echo must not come back as a change.
        m_Echo.noteWritten(m.text);
        const quint64 generation = m_Generation;
        const qint64 id = m.id;
        QPointer<AgentClient> self(this);
        m_Backend->write(m.text, [self, generation, id](bool ok) {
            if (!self || self->m_Generation != generation || !self->m_Welcomed) return;
            if (!ok) qWarning() << "[agent] The clipboard did not take a write";
            self->send(P::ack(id, ok));
        });
        return;
    }
    case P::Type::Ping: send(P::pong()); return;
    default: return; // pong, and anything only the server sends
    }
}

void AgentClient::setWatch(bool on)
{
    m_Watching = on;
    if (!m_Backend) return;
    m_Backend->setWatching(on);
    if (on) {
        // Watching starts with the clipboard as it is, even if it is what we
        // last wrote: the session that asked has not seen it.
        m_Echo.forget();
        m_Backend->read();
    }
}

void AgentClient::onObserved(const QString& text)
{
    if (!m_Welcomed || !m_Watching) return;
    const QString t = ClipboardMsg::capText(text);
    if (!m_Echo.shouldAnnounce(t)) return;
    send(P::changed(t));
}

void AgentClient::send(const QByteArray& frame)
{
    if (m_Ws && m_Ws->state() == QAbstractSocket::ConnectedState)
        m_Ws->sendTextMessage(QString::fromUtf8(frame));
}
