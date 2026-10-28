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

#include "ClipboardAgentRegistry.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtDebug>

namespace {

const QLatin1String kTokenPrefix("mwca_");

/// Equal byte strings, in time that depends only on the lengths.
bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    const int n = qMax(a.size(), b.size());
    quint8 diff = static_cast<quint8>(a.size() ^ b.size());
    for (int i = 0; i < n; ++i) {
        const quint8 x = i < a.size() ? static_cast<quint8>(a[i]) : 0;
        const quint8 y = i < b.size() ? static_cast<quint8>(b[i]) : 0;
        diff |= static_cast<quint8>(x ^ y);
    }
    return diff == 0;
}

} // namespace

ClipboardAgentRegistry::ClipboardAgentRegistry(const QString& filePath, QObject* parent)
    : QObject(parent)
    , m_FilePath(filePath)
{
    load();
}

QString ClipboardAgentRegistry::defaultFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/clipboard-agents.json");
}

QString ClipboardAgentRegistry::generateToken()
{
    QByteArray bytes(32, '\0');
    QRandomGenerator* rng = QRandomGenerator::system();
    for (int i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(rng->bounded(256));
    return kTokenPrefix + QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                                             QByteArray::OmitTrailingEquals));
}

QString ClipboardAgentRegistry::hashToken(const QString& token)
{
    const QByteArray h = QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(
        h.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

template <class F> void ClipboardAgentRegistry::mutate(const QString& host, F change)
{
    const bool before = isAvailable(host);
    change(m_Entries[host]);
    const bool after = isAvailable(host);
    if (before != after) emit availabilityChanged(host, after);
}

QString ClipboardAgentRegistry::issueToken(const QString& host, qint64 nowMs)
{
    if (host.isEmpty()) return QString();
    const QString token = generateToken();
    const bool hadToken = hasToken(host);
    m_Entries[host].tokenHash = hashToken(token);
    m_Entries[host].tokenCreatedMs = nowMs;
    save();
    qInfo().noquote() << "[ClipboardAgent]" << (hadToken ? "Token rotated" : "Token issued")
                      << "for host" << host;
    // Whatever authenticated with the previous token goes.
    if (hadToken) emit credentialsRevoked(host);
    return token;
}

bool ClipboardAgentRegistry::revokeToken(const QString& host)
{
    if (!hasToken(host)) return false;
    m_Entries[host].tokenHash.clear();
    m_Entries[host].tokenCreatedMs = 0;
    save();
    qInfo().noquote() << "[ClipboardAgent] Token revoked for host" << host;
    emit credentialsRevoked(host);
    return true;
}

void ClipboardAgentRegistry::forgetHost(const QString& host)
{
    if (!m_Entries.contains(host)) return;
    const bool hadToken = hasToken(host);
    const bool wasAvailable = isAvailable(host);
    m_Entries.remove(host);
    save();
    if (hadToken) emit credentialsRevoked(host);
    if (wasAvailable) emit availabilityChanged(host, false);
}

void ClipboardAgentRegistry::setEnabled(const QString& host, bool enabled)
{
    if (host.isEmpty() || isEnabled(host) == enabled) return;
    mutate(host, [enabled](Entry& e) { e.enabled = enabled; });
    save();
    qInfo().noquote() << "[ClipboardAgent] Host" << host << (enabled ? "enabled" : "disabled");
}

bool ClipboardAgentRegistry::isEnabled(const QString& host) const
{
    const auto it = m_Entries.constFind(host);
    return it != m_Entries.constEnd() && it->enabled;
}

bool ClipboardAgentRegistry::hasToken(const QString& host) const
{
    const auto it = m_Entries.constFind(host);
    return it != m_Entries.constEnd() && !it->tokenHash.isEmpty();
}

QString ClipboardAgentRegistry::hostForToken(const QString& token) const
{
    if (token.isEmpty()) return QString();
    const QByteArray presented = hashToken(token).toLatin1();
    QString found;
    // No early exit: every host is compared, so the time taken does not say
    // whether, or where, a match was.
    for (auto it = m_Entries.constBegin(); it != m_Entries.constEnd(); ++it) {
        if (it->tokenHash.isEmpty()) continue;
        if (constantTimeEquals(presented, it->tokenHash.toLatin1())) found = it.key();
    }
    return found;
}

void ClipboardAgentRegistry::markConnected(const QString& host, qint64 nowMs)
{
    mutate(host, [nowMs](Entry& e) {
        e.state = State::Connected;
        e.lastSeenMs = nowMs;
        e.platform.clear();
        e.backend.clear();
        e.agentVersion.clear();
    });
}

void ClipboardAgentRegistry::markReady(const QString& host, const QString& platform,
                                       const QString& backend, const QString& agentVersion,
                                       qint64 nowMs)
{
    mutate(host, [&](Entry& e) {
        e.state = State::Ready;
        e.lastSeenMs = nowMs;
        e.platform = platform;
        e.backend = backend;
        e.agentVersion = agentVersion;
    });
    save();
    qInfo().noquote() << "[ClipboardAgent] Agent ready for host" << host << "(" << platform << ","
                      << backend << "," << agentVersion << ")";
}

void ClipboardAgentRegistry::markDisconnected(const QString& host, qint64 nowMs)
{
    if (state(host) == State::Disconnected) return;
    mutate(host, [nowMs](Entry& e) {
        e.state = State::Disconnected;
        e.lastSeenMs = nowMs;
    });
    save();
}

ClipboardAgentRegistry::State ClipboardAgentRegistry::state(const QString& host) const
{
    const auto it = m_Entries.constFind(host);
    return it == m_Entries.constEnd() ? State::Disconnected : it->state;
}

bool ClipboardAgentRegistry::isAvailable(const QString& host) const
{
    const auto it = m_Entries.constFind(host);
    return it != m_Entries.constEnd() && it->enabled && it->state == State::Ready;
}

QJsonObject ClipboardAgentRegistry::statusJson(const QString& host) const
{
    const Entry e = m_Entries.value(host);
    QJsonObject o;
    o.insert(QStringLiteral("enabled"), e.enabled);
    o.insert(QStringLiteral("has_token"), !e.tokenHash.isEmpty());
    o.insert(QStringLiteral("token_created"), static_cast<double>(e.tokenCreatedMs));
    o.insert(QStringLiteral("connected"), e.state == State::Ready);
    o.insert(QStringLiteral("available"), e.enabled && e.state == State::Ready);
    o.insert(QStringLiteral("last_seen"), static_cast<double>(e.lastSeenMs));
    o.insert(QStringLiteral("platform"), e.platform);
    o.insert(QStringLiteral("backend"), e.backend);
    o.insert(QStringLiteral("agent_version"), e.agentVersion);
    return o;
}

void ClipboardAgentRegistry::load()
{
    if (m_FilePath.isEmpty()) return;
    QFile file(m_FilePath);
    if (!file.open(QIODevice::ReadOnly)) return; // nothing yet
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject hosts = root.value(QLatin1String("hosts")).toObject();
    for (auto it = hosts.constBegin(); it != hosts.constEnd(); ++it) {
        const QJsonObject h = it.value().toObject();
        Entry e;
        e.enabled = h.value(QLatin1String("enabled")).toBool(false);
        e.tokenHash = h.value(QLatin1String("token_sha256")).toString();
        e.tokenCreatedMs = static_cast<qint64>(h.value(QLatin1String("token_created")).toDouble());
        e.lastSeenMs = static_cast<qint64>(h.value(QLatin1String("last_seen")).toDouble());
        if (!it.key().isEmpty()) m_Entries.insert(it.key(), e);
    }
}

void ClipboardAgentRegistry::save() const
{
    if (m_FilePath.isEmpty()) return;
    QJsonObject hosts;
    for (auto it = m_Entries.constBegin(); it != m_Entries.constEnd(); ++it) {
        QJsonObject h;
        h.insert(QStringLiteral("enabled"), it->enabled);
        h.insert(QStringLiteral("token_sha256"), it->tokenHash);
        h.insert(QStringLiteral("token_created"), static_cast<double>(it->tokenCreatedMs));
        h.insert(QStringLiteral("last_seen"), static_cast<double>(it->lastSeenMs));
        hosts.insert(it.key(), h);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("hosts"), hosts);

    QDir().mkpath(QFileInfo(m_FilePath).absolutePath());
    QSaveFile file(m_FilePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning().noquote() << "[ClipboardAgent] Cannot write" << m_FilePath;
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!file.commit()) qWarning().noquote() << "[ClipboardAgent] Cannot commit" << m_FilePath;
}
