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
#include <QJsonObject>
#include <QObject>
#include <QString>

/// Which streamed hosts may run a clipboard agent, with what credential, and
/// whether one is connected right now (docs/design/clipboard-agent.md).
///
/// Per host: an admin switch (off until the admin turns it on), at most one
/// token, and the live connection's state. The token is random, shown to the
/// admin once, and kept only as its SHA-256 — the way AuthManager keeps
/// session tokens — so a leaked file names no usable credential. Rotating or
/// revoking it drops the agent that used it.
///
/// Connection states, per host:
///   Disconnected → Connected (authenticated socket, no hello yet)
///                → Ready (hello read; counts as connected for routing)
///                → Disconnected
/// A second agent authenticating for the same host replaces the first.
///
/// Persistence: one JSON file, written whole (QSaveFile) and owner-only.
/// Runtime state (connected, platform…) is never written, except last_seen.
class ClipboardAgentRegistry : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Disconnected,
        Connected,
        Ready,
    };

    /// @p filePath: where the state lives (empty = memory only, for tests).
    explicit ClipboardAgentRegistry(const QString& filePath, QObject* parent = nullptr);

    /// AppData/clipboard-agents.json, next to sessions.json.
    static QString defaultFilePath();

    /// A fresh token: "mwca_" + 43 characters of base64url (256 random bits).
    static QString generateToken();
    /// What the file keeps of a token: base64url(SHA-256(token)).
    static QString hashToken(const QString& token);

    /// Create or rotate @p host's token and return it — the only time it is
    /// ever readable. An agent connected with the old one is dropped.
    QString issueToken(const QString& host, qint64 nowMs);
    /// Forget @p host's token. False when it had none.
    bool revokeToken(const QString& host);
    /// Forget everything about @p host (it was removed from the host list).
    void forgetHost(const QString& host);

    void setEnabled(const QString& host, bool enabled);
    bool isEnabled(const QString& host) const;
    bool hasToken(const QString& host) const;

    /// The host @p token belongs to, or empty. Compares hashes in constant
    /// time against every host, so the answer's timing names none of them.
    QString hostForToken(const QString& token) const;

    // ── Live connection (driven by ClipboardAgentServer) ────────────────────
    void markConnected(const QString& host, qint64 nowMs);
    void markReady(const QString& host, const QString& platform, const QString& backend,
                   const QString& agentVersion, qint64 nowMs);
    void markDisconnected(const QString& host, qint64 nowMs);
    State state(const QString& host) const;

    /// Enabled and Ready: sessions streaming this host may use the agent.
    bool isAvailable(const QString& host) const;

    /// For the admin page: enabled, has_token, token_created, connected,
    /// last_seen, platform, backend, agent_version. Never the token.
    QJsonObject statusJson(const QString& host) const;

signals:
    /// @p host's credential is gone (rotated or revoked): its live agent must
    /// be disconnected.
    void credentialsRevoked(const QString& host);
    /// isAvailable(@p host) changed.
    void availabilityChanged(const QString& host, bool available);

private:
    struct Entry
    {
        bool enabled = false;
        QString tokenHash;
        qint64 tokenCreatedMs = 0;
        qint64 lastSeenMs = 0;
        State state = State::Disconnected;
        QString platform;
        QString backend;
        QString agentVersion;
    };

    void load();
    void save() const;
    /// Run @p change on @p host's entry, then announce an availability flip.
    template <class F> void mutate(const QString& host, F change);

    QString m_FilePath;
    QHash<QString, Entry> m_Entries;
};
