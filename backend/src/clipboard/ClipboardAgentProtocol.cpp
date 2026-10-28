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

#include "ClipboardAgentProtocol.h"

#include "streaming/ClipboardMessage.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>

namespace ClipboardAgent {

namespace {

struct TypeName
{
    Type type;
    const char* name;
};

constexpr TypeName kTypes[] = {
    {Type::Hello, "hello"},       {Type::Welcome, "welcome"}, {Type::Watch, "watch"},
    {Type::Announce, "announce"}, {Type::Set, "set"},         {Type::Ack, "ack"},
    {Type::Changed, "changed"},   {Type::Ping, "ping"},       {Type::Pong, "pong"},
};

Type typeFromName(const QString& name)
{
    for (const TypeName& t : kTypes)
        if (name == QLatin1String(t.name)) return t.type;
    return Type::Invalid;
}

Message invalid(const QString& why)
{
    Message m;
    m.error = why;
    return m;
}

/// A JSON number that is a positive integer JSON can carry exactly (2^53).
bool readId(const QJsonValue& v, qint64* out)
{
    if (!v.isDouble()) return false;
    const double d = v.toDouble();
    if (!(d >= 1.0) || d > 9007199254740992.0 || std::floor(d) != d) return false;
    *out = static_cast<qint64>(d);
    return true;
}

/// A required non-empty string, capped like the relays cap the browser's.
bool readText(const QJsonObject& o, Message* m)
{
    const QJsonValue v = o.value(QLatin1String("text"));
    if (!v.isString()) return false;
    const QString text = v.toString();
    if (text.isEmpty()) return false;
    m->text = ClipboardMsg::capText(text);
    m->truncated = m->text.size() < text.size();
    return true;
}

QByteArray frame(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

} // namespace

QString typeName(Type type)
{
    for (const TypeName& t : kTypes)
        if (t.type == type) return QString::fromLatin1(t.name);
    return QString();
}

QString sanitizeLabel(const QString& label)
{
    QString out;
    out.reserve(qMin(label.size(), kMaxLabelChars));
    for (const QChar c : label) {
        if (out.size() >= kMaxLabelChars) break;
        const ushort u = c.unicode();
        if (u >= 0x20 && u < 0x7f) out.append(c);
    }
    return out;
}

Message decode(const QByteArray& bytes)
{
    if (bytes.size() > kMaxFrameBytes) return invalid(QStringLiteral("frame too large"));
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return invalid(QStringLiteral("not a JSON object"));
    const QJsonObject o = doc.object();

    const QJsonValue typeValue = o.value(QLatin1String("type"));
    if (!typeValue.isString()) return invalid(QStringLiteral("no type"));
    Message m;
    m.type = typeFromName(typeValue.toString());

    switch (m.type) {
    case Type::Invalid: return invalid(QStringLiteral("unknown type"));
    case Type::Hello:
    case Type::Welcome: {
        const QJsonValue v = o.value(QLatin1String("version"));
        if (!v.isDouble() || v.toDouble() < 1 || v.toDouble() > 1000000 ||
            std::floor(v.toDouble()) != v.toDouble())
            return invalid(QStringLiteral("bad version"));
        m.version = v.toInt();
        if (m.type == Type::Hello) {
            m.platform = sanitizeLabel(o.value(QLatin1String("platform")).toString());
            m.backend = sanitizeLabel(o.value(QLatin1String("backend")).toString());
            m.agentVersion = sanitizeLabel(o.value(QLatin1String("agent")).toString());
        } else {
            m.watch = o.value(QLatin1String("watch")).toBool(false);
        }
        return m;
    }
    case Type::Watch: {
        const QJsonValue v = o.value(QLatin1String("on"));
        if (!v.isBool()) return invalid(QStringLiteral("watch without on"));
        m.watch = v.toBool();
        return m;
    }
    case Type::Set:
        if (!readId(o.value(QLatin1String("id")), &m.id)) return invalid(QStringLiteral("bad id"));
        if (!readText(o, &m)) return invalid(QStringLiteral("set without text"));
        return m;
    case Type::Ack: {
        if (!readId(o.value(QLatin1String("id")), &m.id)) return invalid(QStringLiteral("bad id"));
        // Absent reads as success: an older agent that only echoes the id.
        const QJsonValue ok = o.value(QLatin1String("ok"));
        if (!ok.isUndefined() && !ok.isBool()) return invalid(QStringLiteral("bad ok"));
        m.ok = ok.toBool(true);
        return m;
    }
    case Type::Changed:
        if (!readText(o, &m)) return invalid(QStringLiteral("changed without text"));
        return m;
    case Type::Announce:
    case Type::Ping:
    case Type::Pong: return m;
    }
    return invalid(QStringLiteral("unknown type"));
}

QByteArray encode(const Message& m)
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), typeName(m.type));
    switch (m.type) {
    case Type::Invalid: return QByteArray();
    case Type::Hello:
        o.insert(QStringLiteral("version"), m.version);
        o.insert(QStringLiteral("platform"), sanitizeLabel(m.platform));
        o.insert(QStringLiteral("backend"), sanitizeLabel(m.backend));
        o.insert(QStringLiteral("agent"), sanitizeLabel(m.agentVersion));
        break;
    case Type::Welcome:
        o.insert(QStringLiteral("version"), m.version);
        o.insert(QStringLiteral("watch"), m.watch);
        break;
    case Type::Watch: o.insert(QStringLiteral("on"), m.watch); break;
    case Type::Set:
        o.insert(QStringLiteral("id"), static_cast<double>(m.id));
        o.insert(QStringLiteral("text"), ClipboardMsg::capText(m.text));
        break;
    case Type::Ack:
        o.insert(QStringLiteral("id"), static_cast<double>(m.id));
        o.insert(QStringLiteral("ok"), m.ok);
        break;
    case Type::Changed: o.insert(QStringLiteral("text"), ClipboardMsg::capText(m.text)); break;
    case Type::Announce:
    case Type::Ping:
    case Type::Pong: break;
    }
    return frame(o);
}

QByteArray hello(const QString& platform, const QString& backend, const QString& agentVersion)
{
    Message m;
    m.type = Type::Hello;
    m.version = kProtocolVersion;
    m.platform = platform;
    m.backend = backend;
    m.agentVersion = agentVersion;
    return encode(m);
}

QByteArray welcome(bool watchOn)
{
    Message m;
    m.type = Type::Welcome;
    m.version = kProtocolVersion;
    m.watch = watchOn;
    return encode(m);
}

QByteArray watch(bool on)
{
    Message m;
    m.type = Type::Watch;
    m.watch = on;
    return encode(m);
}

QByteArray announce()
{
    Message m;
    m.type = Type::Announce;
    return encode(m);
}

QByteArray set(qint64 id, const QString& text)
{
    Message m;
    m.type = Type::Set;
    m.id = id;
    m.text = text;
    return encode(m);
}

QByteArray ack(qint64 id, bool ok)
{
    Message m;
    m.type = Type::Ack;
    m.id = id;
    m.ok = ok;
    return encode(m);
}

QByteArray changed(const QString& text)
{
    Message m;
    m.type = Type::Changed;
    m.text = text;
    return encode(m);
}

QByteArray ping()
{
    Message m;
    m.type = Type::Ping;
    return encode(m);
}

QByteArray pong()
{
    Message m;
    m.type = Type::Pong;
    return encode(m);
}

} // namespace ClipboardAgent
