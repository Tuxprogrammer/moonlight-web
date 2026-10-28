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

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QString>
#include <QUrl>

/// What the clipboard agent runs with, worked out from its arguments, its
/// environment and its token file. Pure: main() does the reading.
struct AgentConfig
{
    /// wss://host:port/ws/clipboard-agent (ws:// only towards loopback).
    QUrl url;
    QString token;
    /// SHA-256 digests (32 bytes) of the server key(s) to accept. Empty: the
    /// certificate must verify against the system's CAs and the host name.
    QList<QByteArray> pins;
    /// "auto", or one backend's name (see BackendSelect).
    QString backend = QStringLiteral("auto");
};

namespace AgentConfigParse {

/// The agent's endpoint for @p server: https://h:p or wss://h:p (the path is
/// added), a bare h:p meaning https. Plain http:// and ws:// are accepted for
/// a loopback server only: the token travels in the handshake. Empty with
/// @p error set when unusable.
QUrl agentUrl(const QString& server, QString* error);

/// "sha256/<base64>" pins, comma-separated; a bare base64 digest is accepted
/// too. False with @p error set on anything that is not a SHA-256 digest.
bool parsePins(const QString& spec, QList<QByteArray>* out, QString* error);

/// The token in a token file: its first non-empty line, trimmed.
QString tokenFromFile(const QByteArray& contents);

/// KEY=VALUE lines (the settings file, clipboard-agent.env): blank lines and
/// # comments skipped, surrounding quotes dropped.
QMap<QString, QString> parseEnvFile(const QByteArray& contents);

/// The token looks like one the server issues (mwca_ + base64url).
bool looksLikeToken(const QString& token);

struct Inputs
{
    QString server;       ///< --server, else MW_CLIPBOARD_AGENT_SERVER
    QString pins;         ///< --pin, else MW_CLIPBOARD_AGENT_PIN
    QString envToken;     ///< MW_CLIPBOARD_AGENT_TOKEN
    QByteArray tokenFile; ///< the token file's contents, when it was read
    bool tokenFileRead = false;
    QString backend; ///< --backend
};

/// Everything above together. The environment's token wins over the file's.
bool resolve(const Inputs& in, AgentConfig* out, QString* error);

} // namespace AgentConfigParse
