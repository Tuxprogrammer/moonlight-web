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

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimeZone>

#include <functional>

class QTextStream;

/**
 * The operator commands that manage sessions and settings from a terminal:
 * `--list-sessions`, `--revoke-session`, `--set` and `--get`.
 *
 * Like `--status` and `--new-pin` (main.cpp), they drive the RUNNING server
 * through the loopback endpoints the admin page calls, and never touch its
 * files: settings.json is the server's while it runs, and a write behind its
 * back would be overwritten by the next one it makes. Everything here is pure
 * or talks through an Instance, so the whole flow is testable against a fake.
 *
 * Exit codes: 0 done, 1 the server is not running or refused, 2 the command
 * line itself is wrong (nothing was sent).
 */
namespace mw::cli {

enum Exit : int
{
    kExitOk = 0,
    kExitFailed = 1,
    kExitUsage = 2
};

/// True when a raw argv entry is an operator command (`--status`,
/// `--set=k=v`…): main() routes the log echo to stderr and opens no log file
/// for those. Matches the bare flag or `flag=`, never a longer flag.
bool isOperatorCommandArg(const char* arg);

// ── Settings ────────────────────────────────────────────────────────────────

enum class ValueType
{
    Bool,
    Int,
    String,
    Enum
};

/// Which admin route pair a key belongs to.
enum class SettingGroup
{
    Streaming,
    Admin
};

struct SettingKey
{
    QString key;
    ValueType type = ValueType::Bool;
    SettingGroup group = SettingGroup::Admin;
    int min = 0;         // Int: lowest value
    int max = 0;         // Int: highest value; String: longest value (0 = none)
    QStringList choices; // Enum: accepted values, as stored
    QString summary;     // one line for the key list
};

/// The allow-list: every key `--set` / `--get` accept, nothing else.
const QList<SettingKey>& settingKeys();
const SettingKey* findSettingKey(const QString& key);

QString settingsGetPath(SettingGroup group);
QString settingsPostPath(SettingGroup group);

/// What a key accepts, for messages and the key list ("true|false", "1-65535").
QString describeValues(const SettingKey& key);
/// The supported keys, one per line with what they accept.
QString supportedKeysText();

struct ValueResult
{
    bool ok = false;
    QJsonValue value; // as the route expects it
    QString error;
};
ValueResult parseValue(const SettingKey& key, const QString& raw);

struct Assignment
{
    QString key;
    QString value;
};
/// `key=value`, split at the first '='; the value may be empty or hold '='.
bool splitAssignment(const QString& arg, Assignment* out, QString* error);

struct SettingsRequest
{
    SettingGroup group = SettingGroup::Admin;
    QJsonObject body;
    QStringList keys; // in the order given on the command line
};

struct SetPlan
{
    bool ok = false;
    bool unknownKey = false; // the error names a key outside the allow-list
    QString error;
    // At most one request per group, streaming first: a port change in the
    // admin group moves the listener the next request would have to reach.
    QList<SettingsRequest> requests;
};
SetPlan planSet(const QStringList& assignments);

struct GetPlan
{
    bool ok = false;
    bool unknownKey = false;
    QString error;
    QStringList keys; // de-duplicated, in the order given; "all" expanded
};
GetPlan planGet(const QStringList& keys);

/// A value as printed: true/false, digits, or the text itself.
QString formatValue(const QJsonValue& value);

// ── Sessions ────────────────────────────────────────────────────────────────

/// Characters of the opaque id the table shows; any unique prefix of at
/// least kMinIdPrefix characters is accepted back.
constexpr int kShortIdLength = 12;
constexpr int kMinIdPrefix = 4;
/// Longer than any id (43 base64url characters today), so a future longer
/// hash still passes, and a pasted paragraph does not.
constexpr int kMaxIdLength = 128;

enum class MatchKind
{
    Found,
    NotFound,
    Ambiguous,
    Malformed
};

struct SessionMatch
{
    MatchKind kind = MatchKind::NotFound;
    QJsonObject session;    // Found
    QStringList candidates; // Ambiguous: the matching ids
};
SessionMatch matchSession(const QJsonArray& sessions, const QString& idOrPrefix);

/// host, admin, tunnel, oidc, oidc-admin, ephemeral, streaming, current.
QStringList sessionFlags(const QJsonObject& session);
/// "just now", "5 min ago", "3 h ago", "2 d ago".
QString formatAge(qint64 seconds);
QString formatSessionsTable(const QJsonArray& sessions, qint64 nowSecs, const QTimeZone& zone);

// ── Commands ────────────────────────────────────────────────────────────────

struct Reply
{
    bool ok = false;    // transport-level success AND HTTP 2xx
    int httpStatus = 0; // 0 when the request never completed
    QJsonObject json;   // parsed body (empty when not JSON)
    QString error;      // human-readable transport error
};

/// The API's own message, else the transport's, else the HTTP status.
QString replyError(const Reply& reply);

/// The running server, reached over loopback (main.cpp) or a fake (tests).
struct Instance
{
    std::function<bool()> connect; // false: nothing answers
    std::function<Reply(const QString& path)> get;
    std::function<Reply(const QString& path, const QByteArray& body)> adminPost;
};

QString notRunningMessage();

int runListSessions(const Instance& server, bool json, qint64 nowSecs, const QTimeZone& zone,
                    QTextStream& out, QTextStream& err);
int runRevokeSession(const Instance& server, const QString& idOrPrefix, QTextStream& out,
                     QTextStream& err);
int runSet(const Instance& server, const QStringList& assignments, QTextStream& out,
           QTextStream& err);
int runGet(const Instance& server, const QStringList& keys, bool json, QTextStream& out,
           QTextStream& err);

} // namespace mw::cli
