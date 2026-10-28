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

// The wire protocol between the server and a clipboard agent running on a
// streamed host (docs/design/clipboard-agent.md).
//
// The agent dials out to wss://<server>/ws/clipboard-agent with its host's
// token as a bearer, then both sides exchange one JSON object per text frame:
//
//   agent → server   hello    {version, platform, backend, agent}
//   server → agent   welcome  {version, watch}
//   server → agent   watch    {on}     start / stop reporting clipboard changes
//   server → agent   announce {}       report the current clipboard once
//   server → agent   set      {id, text}
//   agent → server   ack      {id, ok}
//   agent → server   changed  {text}
//   either way       ping {} / pong {}
//
// The agent reports nothing unless the server asked it to watch, so the
// host's clipboard only leaves the host while a session that may read it is
// streaming. Text only, capped at ClipboardMsg::kMaxTextChars.
//
// Shared by the server and the agent executable: Qt Core only, no sockets.

#include <QByteArray>
#include <QString>

namespace ClipboardAgent {

/// Bumped on an incompatible change. The server refuses a hello it does not
/// speak; the agent refuses a welcome it does not speak.
constexpr int kProtocolVersion = 1;

/// Where the agent connects, under the server's HTTPS origin.
inline QString path()
{
    return QStringLiteral("/ws/clipboard-agent");
}

/// Upper bound for one frame. 256 Ki UTF-16 units at the worst JSON escape
/// (\uXXXX, 6 bytes) is 1.5 MiB; anything larger is not ours.
constexpr qint64 kMaxFrameBytes = 2 * 1024 * 1024;

/// Longest free-form identity string (platform, backend, agent version) kept
/// from a hello: they end up in logs and the admin page.
constexpr int kMaxLabelChars = 64;

/// WebSocket close codes the server uses (4000-4999 is the private range).
enum CloseCode : int
{
    CloseProtocolError = 4400, ///< A frame we could not read, or no hello in time.
    CloseRevoked = 4401,       ///< The token was revoked or rotated.
    CloseReplaced = 4409,      ///< Another agent authenticated for the same host.
    CloseVersion = 4426,       ///< A protocol version this side does not speak.
};

enum class Type
{
    Invalid,
    Hello,
    Welcome,
    Watch,
    Announce,
    Set,
    Ack,
    Changed,
    Ping,
    Pong,
};

struct Message
{
    Type type = Type::Invalid;
    /// hello, welcome.
    int version = 0;
    /// hello: what the agent runs on and how it reaches the clipboard.
    QString platform;
    QString backend;
    QString agentVersion;
    /// welcome (initial state), watch.
    bool watch = false;
    /// set, ack. Positive.
    qint64 id = 0;
    /// ack: the agent's backend took the text.
    bool ok = false;
    /// set, changed. Never empty once decoded; capped at kMaxTextChars.
    QString text;
    /// The text arrived longer than the cap and was cut.
    bool truncated = false;
    /// Invalid only: why, for a log line (never contains the payload).
    QString error;
};

/// Read one frame. Anything malformed comes back as Type::Invalid with a
/// reason; a valid message has every field it needs.
Message decode(const QByteArray& frame);

/// Write one frame (compact JSON). Fields that do not belong to the type are
/// left out; text is capped.
QByteArray encode(const Message& message);

/// Name on the wire ("hello", "set"…), empty for Invalid.
QString typeName(Type type);

// Builders for the frames each side sends.
QByteArray hello(const QString& platform, const QString& backend, const QString& agentVersion);
QByteArray welcome(bool watch);
QByteArray watch(bool on);
QByteArray announce();
QByteArray set(qint64 id, const QString& text);
QByteArray ack(qint64 id, bool ok);
QByteArray changed(const QString& text);
QByteArray ping();
QByteArray pong();

/// A label from a hello as it is safe to log and show: printable ASCII only,
/// at most kMaxLabelChars.
QString sanitizeLabel(const QString& label);

} // namespace ClipboardAgent
