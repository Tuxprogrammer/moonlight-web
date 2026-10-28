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

#include "OperatorCli.h"

#include "AppSettings.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QTextStream>

#include <cmath>
#include <cstring>

namespace mw::cli {

namespace {

const QString kSessionsPath = QStringLiteral("/api/auth/sessions");
const QString kRevokePath = QStringLiteral("/api/auth/sessions/revoke");

// What the server does to a name before keeping it (AppSettings::setInstanceName),
// so the length refused here is the length it would have clipped.
QString tidyName(const QString& name)
{
    QString clean;
    clean.reserve(name.size());
    for (const QChar c : name) {
        if (c.isSpace())
            clean.append(QLatin1Char(' '));
        else if (c.category() != QChar::Other_Control)
            clean.append(c);
    }
    return clean.simplified();
}

// The alphabet of a session id: SHA-256 in base64url (AuthManager::hashToken).
bool isIdAlphabet(const QString& s)
{
    for (const QChar c : s) {
        const char l = c.toLatin1();
        if (!((l >= '0' && l <= '9') || (l >= 'a' && l <= 'z') || (l >= 'A' && l <= 'Z') ||
              l == '-' || l == '_'))
            return false;
    }
    return true;
}

QString shortId(const QString& id)
{
    return id.left(kShortIdLength);
}

// machine_name, else the OIDC display name, else a dash.
QString deviceLabel(const QJsonObject& s)
{
    const QString machine = s.value(QStringLiteral("machine_name")).toString().trimmed();
    if (!machine.isEmpty()) return machine;
    const QString person = s.value(QStringLiteral("display_name")).toString().trimmed();
    if (!person.isEmpty()) return person;
    return QStringLiteral("-");
}

void printUnknownKey(QTextStream& err, const QString& error)
{
    err << error << "\n\nSupported keys:\n" << supportedKeysText();
}

} // namespace

bool isOperatorCommandArg(const char* arg)
{
    if (!arg) return false;
    static const char* const kCommands[] = {"--status",
                                            "--new-pin",
                                            "--set-admin-password",
                                            "--clear-admin-password",
                                            "--enable-internet",
                                            "--list-sessions",
                                            "--revoke-session",
                                            "--set",
                                            "--get"};
    for (const char* flag : kCommands) {
        const size_t n = std::strlen(flag);
        if (std::strncmp(arg, flag, n) == 0 && (arg[n] == '\0' || arg[n] == '=')) return true;
    }
    return false;
}

// ── Settings ────────────────────────────────────────────────────────────────

const QList<SettingKey>& settingKeys()
{
    // Each entry is a key the admin page writes through that route on main;
    // the route still applies its own rules, these only refuse early and say why.
    static const QList<SettingKey> keys = [] {
        QList<SettingKey> k;
        auto add = [&k](const char* key, ValueType type, SettingGroup group, const char* summary,
                        int min = 0, int max = 0, QStringList choices = {}) {
            SettingKey s;
            s.key = QString::fromLatin1(key);
            s.type = type;
            s.group = group;
            s.min = min;
            s.max = max;
            s.choices = std::move(choices);
            s.summary = QString::fromUtf8(summary);
            k.append(s);
        };
        const auto S = SettingGroup::Streaming;
        const auto A = SettingGroup::Admin;
        add("transport_mode", ValueType::Enum, S, "the stream's transport chain", 0, 0,
            {"auto", "all", "webrtc-media-udp", "webrtc-dc-udp", "webrtc-media-tcp",
             "webrtc-dc-tcp", "wss"});
        add("upnp_enabled", ValueType::Bool, S, "ask the router to map ports (Internet Access)");
        add("video_codec", ValueType::Enum, S, "default codec of a stream", 0, 0,
            {"h264", "hevc", "av1"});
        add("native_video_pipeline", ValueType::Enum, S, "native host: capture-to-encoder chain", 0,
            0, {"auto", "d3d11", "d3d12", "vaapi", "vulkan"});
        add("cert_auth_enabled", ValueType::Bool, A, "sign-in with the certificate file");
        add("packet_marking", ValueType::Bool, A, "ask the Wi-Fi for its video queue");
        add("stream_notifications", ValueType::Bool, A, "tell the desktop when someone streams");
        add("debug_mode", ValueType::Bool, A, "debug-build options, browsers' consoles logged");
        add("verbose_logs", ValueType::Bool, A, "DEBUG lines and network diagnostics in the log");
        add("instance_name", ValueType::String, A,
            "the name browsers show for this install (empty: the PC's name)", 0,
            AppSettings::kInstanceNameMaxLength);
        add("https_port", ValueType::Int, A, "HTTPS listener; the server moves to it at once", 1,
            65535);
        return k;
    }();
    return keys;
}

const SettingKey* findSettingKey(const QString& key)
{
    for (const SettingKey& k : settingKeys())
        if (k.key == key) return &k;
    return nullptr;
}

QString settingsGetPath(SettingGroup group)
{
    return group == SettingGroup::Admin ? QStringLiteral("/api/admin/settings")
                                        : QStringLiteral("/api/settings/streaming");
}

QString settingsPostPath(SettingGroup group)
{
    // The same paths: each route reads with GET and writes with POST.
    return settingsGetPath(group);
}

QString describeValues(const SettingKey& key)
{
    switch (key.type) {
    case ValueType::Bool: return QStringLiteral("true|false");
    case ValueType::Int: return QStringLiteral("%1-%2").arg(key.min).arg(key.max);
    case ValueType::String:
        return key.max > 0 ? QStringLiteral("text, up to %1 characters").arg(key.max)
                           : QStringLiteral("text");
    case ValueType::Enum: return key.choices.join(QLatin1Char('|'));
    }
    return QString();
}

QString supportedKeysText()
{
    int width = 0;
    for (const SettingKey& k : settingKeys())
        width = qMax(width, int(k.key.size()));
    QString text;
    for (const SettingKey& k : settingKeys())
        text +=
            QStringLiteral("  %1  %2\n  %3  %4\n")
                .arg(k.key.leftJustified(width), describeValues(k), QString(width, ' '), k.summary);
    return text;
}

ValueResult parseValue(const SettingKey& key, const QString& raw)
{
    ValueResult r;
    const QString v = raw.trimmed();
    switch (key.type) {
    case ValueType::Bool: {
        const QString l = v.toLower();
        if (l == "true" || l == "on" || l == "yes" || l == "1") {
            r.ok = true;
            r.value = true;
        } else if (l == "false" || l == "off" || l == "no" || l == "0") {
            r.ok = true;
            r.value = false;
        } else {
            r.error =
                QStringLiteral("%1 takes true or false (also on/off, yes/no, 1/0), not \"%2\".")
                    .arg(key.key, raw);
        }
        return r;
    }
    case ValueType::Int: {
        bool isNumber = false;
        const int n = v.toInt(&isNumber, 10);
        if (!isNumber) {
            r.error = QStringLiteral("%1 takes a number, not \"%2\".").arg(key.key, raw);
        } else if (n < key.min || n > key.max) {
            r.error = QStringLiteral("%1 must be between %2 and %3, not %4.")
                          .arg(key.key)
                          .arg(key.min)
                          .arg(key.max)
                          .arg(n);
        } else {
            r.ok = true;
            r.value = n;
        }
        return r;
    }
    case ValueType::String: {
        // Sent as typed: the server tidies spaces itself and echoes what it kept.
        if (key.max > 0 && tidyName(raw).size() > key.max) {
            r.error = QStringLiteral("%1 is limited to %2 characters (got %3).")
                          .arg(key.key)
                          .arg(key.max)
                          .arg(tidyName(raw).size());
        } else {
            r.ok = true;
            r.value = raw;
        }
        return r;
    }
    case ValueType::Enum: {
        const QString l = v.toLower();
        if (key.choices.contains(l)) {
            r.ok = true;
            r.value = l;
        } else {
            r.error = QStringLiteral("%1 must be one of: %2 (got \"%3\").")
                          .arg(key.key, key.choices.join(QStringLiteral(", ")), raw);
        }
        return r;
    }
    }
    return r;
}

bool splitAssignment(const QString& arg, Assignment* out, QString* error)
{
    const int eq = arg.indexOf(QLatin1Char('='));
    const QString key = eq < 0 ? QString() : arg.left(eq).trimmed();
    if (eq < 0 || key.isEmpty()) {
        if (error) *error = QStringLiteral("--set takes key=value, not \"%1\".").arg(arg);
        return false;
    }
    out->key = key;
    out->value = arg.mid(eq + 1);
    return true;
}

SetPlan planSet(const QStringList& assignments)
{
    SetPlan plan;
    if (assignments.isEmpty()) {
        plan.error = QStringLiteral("--set takes key=value.");
        return plan;
    }

    SettingsRequest streaming{SettingGroup::Streaming, {}, {}};
    SettingsRequest admin{SettingGroup::Admin, {}, {}};
    for (const QString& arg : assignments) {
        Assignment a;
        if (!splitAssignment(arg, &a, &plan.error)) return plan;
        const SettingKey* key = findSettingKey(a.key);
        if (!key) {
            plan.unknownKey = true;
            plan.error = QStringLiteral("Unknown setting \"%1\".").arg(a.key);
            return plan;
        }
        SettingsRequest& req = key->group == SettingGroup::Admin ? admin : streaming;
        if (req.body.contains(key->key)) {
            plan.error = QStringLiteral("%1 is given twice.").arg(key->key);
            return plan;
        }
        const ValueResult v = parseValue(*key, a.value);
        if (!v.ok) {
            plan.error = v.error;
            return plan;
        }
        req.body.insert(key->key, v.value);
        req.keys << key->key;
    }

    if (!streaming.keys.isEmpty()) plan.requests << streaming;
    if (!admin.keys.isEmpty()) plan.requests << admin;
    plan.ok = true;
    return plan;
}

GetPlan planGet(const QStringList& keys)
{
    GetPlan plan;
    if (keys.isEmpty()) {
        plan.error = QStringLiteral("--get takes a key, or all.");
        return plan;
    }
    for (const QString& raw : keys) {
        const QString name = raw.trimmed();
        if (name == QLatin1String("all")) {
            for (const SettingKey& k : settingKeys())
                if (!plan.keys.contains(k.key)) plan.keys << k.key;
            continue;
        }
        if (!findSettingKey(name)) {
            plan.unknownKey = true;
            plan.error = QStringLiteral("Unknown setting \"%1\".").arg(name);
            plan.keys.clear();
            return plan;
        }
        if (!plan.keys.contains(name)) plan.keys << name;
    }
    plan.ok = true;
    return plan;
}

QString formatValue(const QJsonValue& value)
{
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) {
        const double d = value.toDouble();
        if (std::floor(d) == d && std::fabs(d) < 9e15) return QString::number(qint64(d));
        return QString::number(d);
    }
    if (value.isString()) return value.toString();
    return QString();
}

// ── Sessions ────────────────────────────────────────────────────────────────

SessionMatch matchSession(const QJsonArray& sessions, const QString& idOrPrefix)
{
    SessionMatch m;
    // Case-sensitive: base64url tells "Ab" from "ab". Anything outside its
    // alphabet, or longer than an id could be, is a typo.
    const QString want = idOrPrefix.trimmed();
    if (want.size() < kMinIdPrefix || want.size() > kMaxIdLength || !isIdAlphabet(want)) {
        m.kind = MatchKind::Malformed;
        return m;
    }
    QList<QJsonObject> hits;
    for (const QJsonValue& v : sessions) {
        const QJsonObject s = v.toObject();
        if (s.value(QStringLiteral("token")).toString().startsWith(want)) hits << s;
    }
    if (hits.size() == 1) {
        m.kind = MatchKind::Found;
        m.session = hits.first();
    } else if (hits.size() > 1) {
        m.kind = MatchKind::Ambiguous;
        for (const QJsonObject& s : hits)
            m.candidates << s.value(QStringLiteral("token")).toString();
    } else {
        m.kind = MatchKind::NotFound;
    }
    return m;
}

QStringList sessionFlags(const QJsonObject& s)
{
    QStringList flags;
    auto flag = [&](const char* field, const char* name) {
        if (s.value(QLatin1String(field)).toBool(false)) flags << QString::fromLatin1(name);
    };
    flag("is_host", "host");
    flag("is_admin", "admin");
    flag("via_tunnel", "tunnel");
    // How it signed in, when the server says (the OIDC sign-in adds the field).
    const QString method = s.value(QStringLiteral("auth_method")).toString();
    if (!method.isEmpty()) flags << method;
    flag("oidc_admin", "oidc-admin");
    flag("ephemeral", "ephemeral");
    flag("streaming", "streaming");
    flag("current", "current");
    return flags;
}

QString formatAge(qint64 seconds)
{
    if (seconds < 60) return QStringLiteral("just now");
    if (seconds < 3600) return QStringLiteral("%1 min ago").arg(seconds / 60);
    if (seconds < 86400) return QStringLiteral("%1 h ago").arg(seconds / 3600);
    return QStringLiteral("%1 d ago").arg(seconds / 86400);
}

QString formatSessionsTable(const QJsonArray& sessions, qint64 nowSecs, const QTimeZone& zone)
{
    if (sessions.isEmpty()) return QString();

    QList<QStringList> rows;
    rows << QStringList{"ID", "DEVICE", "ADDRESS", "CREATED", "LAST SEEN", "FLAGS"};
    for (const QJsonValue& v : sessions) {
        const QJsonObject s = v.toObject();
        const qint64 created = qint64(s.value(QStringLiteral("created_at")).toDouble());
        const qint64 seen = qint64(s.value(QStringLiteral("last_seen")).toDouble());
        const QString ip = s.value(QStringLiteral("ip")).toString();
        const QStringList flags = sessionFlags(s);
        rows << QStringList{shortId(s.value(QStringLiteral("token")).toString()),
                            deviceLabel(s),
                            ip.isEmpty() ? QStringLiteral("-") : ip,
                            created > 0 ? QDateTime::fromSecsSinceEpoch(created, zone)
                                              .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                                        : QStringLiteral("-"),
                            seen > 0 ? formatAge(nowSecs - seen) : QStringLiteral("-"),
                            flags.isEmpty() ? QStringLiteral("-")
                                            : flags.join(QStringLiteral(", "))};
    }

    QList<int> widths(rows.first().size(), 0);
    for (const QStringList& r : rows)
        for (int i = 0; i < r.size(); ++i)
            widths[i] = qMax(widths[i], int(r[i].size()));

    QString text;
    for (const QStringList& r : rows) {
        QString line;
        for (int i = 0; i < r.size(); ++i)
            line += i + 1 < r.size() ? r[i].leftJustified(widths[i] + 2) : r[i];
        text += line + QLatin1Char('\n');
    }
    return text;
}

// ── Commands ────────────────────────────────────────────────────────────────

QString replyError(const Reply& reply)
{
    const QString api = reply.json.value(QStringLiteral("error")).toString();
    if (!api.isEmpty()) return api;
    if (!reply.error.isEmpty()) return reply.error;
    return QStringLiteral("HTTP %1").arg(reply.httpStatus);
}

QString notRunningMessage()
{
    return QStringLiteral("MoonlightWeb is not running — start it first "
                          "(sudo systemctl start moonlightweb).");
}

int runListSessions(const Instance& server, bool json, qint64 nowSecs, const QTimeZone& zone,
                    QTextStream& out, QTextStream& err)
{
    if (!server.connect()) {
        err << notRunningMessage() << "\n";
        return kExitFailed;
    }
    const Reply reply = server.get(kSessionsPath);
    if (!reply.ok) {
        err << "Failed: " << replyError(reply) << "\n";
        return kExitFailed;
    }
    const QJsonArray sessions = reply.json.value(QStringLiteral("sessions")).toArray();
    if (json) {
        QJsonObject obj;
        obj[QStringLiteral("sessions")] = sessions;
        out << QJsonDocument(obj).toJson(QJsonDocument::Indented);
        return kExitOk;
    }
    if (sessions.isEmpty()) {
        out << "No sessions: no browser is signed in.\n";
        return kExitOk;
    }
    out << formatSessionsTable(sessions, nowSecs, zone) << "\n"
        << sessions.size() << (sessions.size() == 1 ? " session" : " sessions")
        << ". Revoke one with:  moonlightweb --revoke-session <ID>\n";
    return kExitOk;
}

int runRevokeSession(const Instance& server, const QString& idOrPrefix, QTextStream& out,
                     QTextStream& err)
{
    // The shape first: a typo needs no server to be pointed out.
    if (matchSession(QJsonArray(), idOrPrefix).kind == MatchKind::Malformed) {
        err << "\"" << idOrPrefix << "\" is not a session id: copy an ID from --list-sessions "
            << "(its first " << kMinIdPrefix << " characters or more).\n";
        return kExitUsage;
    }
    if (!server.connect()) {
        err << notRunningMessage() << "\n";
        return kExitFailed;
    }
    // Resolved against the list, because the route answers "revoked" for an id
    // it does not know: only the list can tell a typo from a revocation.
    const Reply list = server.get(kSessionsPath);
    if (!list.ok) {
        err << "Failed: " << replyError(list) << "\n";
        return kExitFailed;
    }
    const SessionMatch m =
        matchSession(list.json.value(QStringLiteral("sessions")).toArray(), idOrPrefix);
    if (m.kind == MatchKind::NotFound) {
        err << "No session matches \"" << idOrPrefix.trimmed()
            << "\". List them with:  moonlightweb --list-sessions\n";
        return kExitFailed;
    }
    if (m.kind == MatchKind::Ambiguous) {
        err << "\"" << idOrPrefix.trimmed() << "\" matches more than one session:\n";
        for (const QString& id : m.candidates)
            err << "  " << shortId(id) << "\n";
        err << "Give more characters of the ID.\n";
        return kExitUsage;
    }

    const QString id = m.session.value(QStringLiteral("token")).toString();
    QJsonObject body;
    body[QStringLiteral("token")] = id;
    const Reply reply =
        server.adminPost(kRevokePath, QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (!reply.ok) {
        if (reply.json.value(QStringLiteral("error")).toString() ==
            QLatin1String("cannot_revoke_self"))
            err << "Refused: that is the caller's own session (cannot_revoke_self).\n";
        else
            err << "Failed: " << replyError(reply) << "\n";
        return kExitFailed;
    }

    const QString ip = m.session.value(QStringLiteral("ip")).toString();
    out << "\n  Revoked session " << shortId(id) << " (" << deviceLabel(m.session)
        << (ip.isEmpty() ? QString() : QStringLiteral(", ") + ip) << ").\n";
    if (m.session.value(QStringLiteral("streaming")).toBool(false))
        out << "  Its stream was stopped.\n";
    if (m.session.value(QStringLiteral("is_host")).toBool(false))
        out << "  That browser loses its host access until it redeems a fresh host key.\n";
    out << "  It has to sign in again (PIN, certificate file or host key) to come back.\n\n";
    return kExitOk;
}

int runSet(const Instance& server, const QStringList& assignments, QTextStream& out,
           QTextStream& err)
{
    const SetPlan plan = planSet(assignments);
    if (!plan.ok) {
        if (plan.unknownKey)
            printUnknownKey(err, plan.error);
        else
            err << plan.error << " Nothing was changed.\n";
        return kExitUsage;
    }
    if (!server.connect()) {
        err << notRunningMessage() << "\n";
        return kExitFailed;
    }

    for (int i = 0; i < plan.requests.size(); ++i) {
        const SettingsRequest& req = plan.requests[i];
        const Reply reply = server.adminPost(
            settingsPostPath(req.group), QJsonDocument(req.body).toJson(QJsonDocument::Compact));
        if (!reply.ok) {
            err << "Failed: " << replyError(reply) << "\n";
            if (i == 0) {
                err << "Nothing was changed.\n";
            } else {
                QStringList left;
                for (int j = i; j < plan.requests.size(); ++j)
                    left << plan.requests[j].keys;
                err << "Not changed: " << left.join(QStringLiteral(", ")) << ".\n";
            }
            return kExitFailed;
        }

        for (const QString& key : req.keys) {
            const QJsonValue asked = req.body.value(key);
            const QJsonValue kept = reply.json.contains(key) ? reply.json.value(key) : asked;
            out << "  " << key << " = " << formatValue(kept);
            // Text is tidied by the server and shown as kept; a different flag or
            // number is a decision worth naming (--debug holding debug_mode on).
            if (!asked.isString() && formatValue(asked) != formatValue(kept)) {
                out << "   (asked for " << formatValue(asked);
                if (key == QLatin1String("debug_mode") || key == QLatin1String("verbose_logs"))
                    out << ": held on by --"
                        << (key == QLatin1String("debug_mode") ? "debug" : "verbose")
                        << " for this run";
                out << ")";
            }
            out << "\n";
        }
        if (reply.json.value(QStringLiteral("port_changed")).toBool(false))
            out << "  The HTTPS listener moves to port "
                << formatValue(req.body.value("https_port"))
                << " now. The operator commands find it in settings.json: give them the\n"
                << "  server's --config if it has one.\n";
    }
    return kExitOk;
}

int runGet(const Instance& server, const QStringList& keys, bool json, QTextStream& out,
           QTextStream& err)
{
    const GetPlan plan = planGet(keys);
    if (!plan.ok) {
        if (plan.unknownKey)
            printUnknownKey(err, plan.error);
        else
            err << plan.error << "\n";
        return kExitUsage;
    }
    if (!server.connect()) {
        err << notRunningMessage() << "\n";
        return kExitFailed;
    }

    QMap<SettingGroup, QJsonObject> answers;
    QJsonObject values;
    for (const QString& key : plan.keys) {
        const SettingGroup group = findSettingKey(key)->group;
        if (!answers.contains(group)) {
            const Reply reply = server.get(settingsGetPath(group));
            if (!reply.ok) {
                err << "Failed: " << replyError(reply) << "\n";
                return kExitFailed;
            }
            answers.insert(group, reply.json);
        }
        const QJsonObject& answer = answers[group];
        if (!answer.contains(key)) {
            err << "The server does not report " << key << " (an older version?).\n";
            return kExitFailed;
        }
        values.insert(key, answer.value(key));
    }

    if (json) {
        out << QJsonDocument(values).toJson(QJsonDocument::Indented);
    } else if (plan.keys.size() == 1) {
        out << formatValue(values.value(plan.keys.first())) << "\n";
    } else {
        for (const QString& key : plan.keys)
            out << key << "=" << formatValue(values.value(key)) << "\n";
    }
    return kExitOk;
}

} // namespace mw::cli
