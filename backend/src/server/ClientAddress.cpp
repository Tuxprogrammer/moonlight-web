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

#include "ClientAddress.h"

#include "NetClassify.h"

#include <QRegularExpression>

namespace ClientAddress {

namespace {

const QString kXff = QStringLiteral("x-forwarded-for");
const QString kForwarded = QStringLiteral("forwarded");
const QString kCf = QStringLiteral("cf-connecting-ip");

/// IPv4-mapped IPv6 (::ffff:a.b.c.d) as the IPv4 address it carries.
/// Read off the bytes: QHostAddress::toIPv4Address() also converts "::".
bool isMapped(const QHostAddress& addr)
{
    if (addr.protocol() != QAbstractSocket::IPv6Protocol) return false;
    const Q_IPV6ADDR raw = addr.toIPv6Address();
    for (int i = 0; i < 10; ++i)
        if (raw[i] != 0) return false;
    return raw[10] == 0xFF && raw[11] == 0xFF;
}

QHostAddress unmapped(const QHostAddress& addr)
{
    if (!isMapped(addr)) return addr;
    const Q_IPV6ADDR raw = addr.toIPv6Address();
    return QHostAddress((quint32(raw[12]) << 24) | (quint32(raw[13]) << 16) |
                        (quint32(raw[14]) << 8) | quint32(raw[15]));
}

bool allDigits(const QString& s)
{
    if (s.isEmpty()) return false;
    for (const QChar c : s)
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
    return true;
}

QString unquote(QString v)
{
    v = v.trimmed();
    if (v.size() >= 2 && v.startsWith(QLatin1Char('"')) && v.endsWith(QLatin1Char('"')))
        v = v.mid(1, v.size() - 2);
    return v;
}

/// The local-tunnel rule (step 3 of resolve()) for a loopback peer: the
/// header a proxy on this machine passed along, or "" when none parses.
QString localTunnelCandidate(const QMap<QString, QString>& headers, QString* header)
{
    const QString cf = parseNode(headers.value(kCf));
    if (!cf.isEmpty()) {
        *header = kCf;
        return cf;
    }
    const QStringList xff = xForwardedFor(headers.value(kXff));
    if (!xff.isEmpty()) {
        const QString last = parseNode(xff.last());
        if (!last.isEmpty()) {
            *header = kXff;
            return last;
        }
    }
    const QStringList fwd = forwardedFor(headers.value(kForwarded));
    if (!fwd.isEmpty()) {
        const QString last = parseNode(fwd.last());
        if (!last.isEmpty()) {
            *header = kForwarded;
            return last;
        }
    }
    return {};
}

} // namespace

// ── Addresses ───────────────────────────────────────────────────────────────

QHostAddress parseAddress(const QString& text)
{
    QString t = text.trimmed();
    const int zone = t.indexOf(QLatin1Char('%'));
    if (zone >= 0) t = t.left(zone);
    if (t.isEmpty()) return {};
    // QHostAddress also takes forms no proxy writes (a bare "1" is 0.0.0.1);
    // require the shape of a dotted quad or an IPv6 literal.
    if (!t.contains(QLatin1Char(':')) && t.count(QLatin1Char('.')) != 3) return {};
    const QHostAddress addr(t);
    if (addr.isNull()) return {};
    return unmapped(addr);
}

QString normalize(const QString& text)
{
    const QHostAddress addr = parseAddress(text);
    return addr.isNull() ? QString() : addr.toString();
}

QString normalizeOrKeep(const QString& text)
{
    const QString n = normalize(text);
    return n.isEmpty() ? text : n;
}

// ── CIDR ────────────────────────────────────────────────────────────────────

bool Cidr::contains(const QHostAddress& raw) const
{
    if (prefix < 0 || network.isNull() || raw.isNull()) return false;
    const QHostAddress addr = unmapped(raw);
    if (addr.protocol() != network.protocol()) return false;
    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 mask = prefix == 0 ? 0u : ~quint32(0) << (32 - prefix);
        return (addr.toIPv4Address() & mask) == (network.toIPv4Address() & mask);
    }
    const Q_IPV6ADDR a = addr.toIPv6Address();
    const Q_IPV6ADDR n = network.toIPv6Address();
    int bits = prefix;
    for (int i = 0; i < 16 && bits > 0; ++i, bits -= 8) {
        const quint8 mask = bits >= 8 ? 0xFF : static_cast<quint8>(0xFF << (8 - bits));
        if ((a[i] & mask) != (n[i] & mask)) return false;
    }
    return true;
}

QString Cidr::toString() const
{
    return network.toString() + QLatin1Char('/') + QString::number(prefix);
}

std::optional<Cidr> parseCidr(const QString& text, CidrError* error)
{
    auto fail = [error](CidrError e) -> std::optional<Cidr> {
        if (error) *error = e;
        return std::nullopt;
    };
    if (error) *error = CidrError::None;

    const QString t = text.trimmed();
    if (t.isEmpty()) return fail(CidrError::Empty);

    const int slash = t.indexOf(QLatin1Char('/'));
    QString host = slash >= 0 ? t.left(slash) : t;
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.mid(1, host.size() - 2);
    // A zone id names an interface of whoever wrote it, never a network here.
    if (host.contains(QLatin1Char('%'))) return fail(CidrError::NotAnIp);
    if (!host.contains(QLatin1Char(':')) && host.count(QLatin1Char('.')) != 3)
        return fail(CidrError::NotAnIp);
    const QHostAddress parsed(host);
    if (parsed.isNull()) return fail(CidrError::NotAnIp);

    const bool mapped = isMapped(parsed);
    const bool v4 = parsed.protocol() == QAbstractSocket::IPv4Protocol;
    const int max = v4 ? 32 : 128;

    int prefix = max;
    if (slash >= 0) {
        const QString p = t.mid(slash + 1);
        if (!allDigits(p) || p.size() > 3) return fail(CidrError::BadPrefix);
        prefix = p.toInt();
        if (prefix > max) return fail(CidrError::BadPrefix);
    }
    if (mapped) {
        // ::ffff:0:0/96 is the IPv4 space; a shorter prefix is not a network
        // of IPv4 addresses at all.
        if (prefix < 96) return fail(CidrError::BadPrefix);
        prefix -= 96;
    }
    if (prefix == 0) return fail(CidrError::TooBroad);

    Cidr c;
    c.prefix = prefix;
    const QHostAddress addr = unmapped(parsed);
    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 mask = ~quint32(0) << (32 - prefix);
        c.network = QHostAddress(addr.toIPv4Address() & mask);
    } else {
        Q_IPV6ADDR raw = addr.toIPv6Address();
        int bits = prefix;
        for (int i = 0; i < 16; ++i, bits -= 8) {
            if (bits >= 8) continue;
            raw[i] &= bits <= 0 ? 0 : static_cast<quint8>(0xFF << (8 - bits));
        }
        c.network = QHostAddress(raw);
    }
    return c;
}

QString cidrErrorName(CidrError error)
{
    switch (error) {
    case CidrError::None: return {};
    case CidrError::Empty: return QStringLiteral("empty");
    case CidrError::NotAnIp: return QStringLiteral("not_an_ip");
    case CidrError::BadPrefix: return QStringLiteral("bad_prefix");
    case CidrError::TooBroad: return QStringLiteral("too_broad");
    }
    return {};
}

TrustedProxies parseList(const QStringList& entries, QStringList* invalid)
{
    TrustedProxies out;
    QStringList seen;
    for (const QString& e : entries) {
        const std::optional<Cidr> c = parseCidr(e);
        if (!c || out.size() >= kMaxTrustedProxies) {
            if (invalid) invalid->append(e);
            continue;
        }
        const QString key = c->toString();
        if (seen.contains(key)) continue;
        seen.append(key);
        out.append(*c);
    }
    return out;
}

QStringList splitList(const QString& value)
{
    static const QRegularExpression sep(QStringLiteral("[,\\s]+"));
    return value.split(sep, Qt::SkipEmptyParts);
}

QStringList toStrings(const TrustedProxies& list)
{
    QStringList out;
    for (const Cidr& c : list)
        out.append(c.toString());
    return out;
}

bool isTrusted(const QHostAddress& ip, const TrustedProxies& list)
{
    if (ip.isNull()) return false;
    for (const Cidr& c : list)
        if (c.contains(ip)) return true;
    return false;
}

bool isTrusted(const QString& ip, const TrustedProxies& list)
{
    if (list.isEmpty()) return false;
    return isTrusted(parseAddress(ip), list);
}

// ── Forwarding headers ──────────────────────────────────────────────────────

QString parseNode(const QString& node)
{
    QString v = unquote(node);
    if (v.startsWith(QLatin1Char('['))) {
        const int end = v.indexOf(QLatin1Char(']'));
        if (end < 0) return {};
        const QString rest = v.mid(end + 1);
        if (!rest.isEmpty() && !(rest.startsWith(QLatin1Char(':')) && allDigits(rest.mid(1))))
            return {};
        v = v.mid(1, end - 1);
    } else if (v.count(QLatin1Char(':')) == 1) {
        // IPv4 with a port; a bare IPv6 address has at least two colons.
        const int colon = v.indexOf(QLatin1Char(':'));
        if (!allDigits(v.mid(colon + 1))) return {};
        v = v.left(colon);
    }
    return normalize(v);
}

QStringList forwardedFor(const QString& header)
{
    QStringList out;

    // One element at a time, honouring quoted strings (RFC 7230 quoted-string,
    // with backslash escapes) so a separator inside one does not split.
    QString forValue;
    bool haveFor = false;
    bool content = false; // an empty element (", ,") is list syntax, not a hop
    QString pair;
    bool quoted = false;
    auto endPair = [&]() {
        const QString p = pair.trimmed();
        if (!p.isEmpty()) content = true;
        const int eq = p.indexOf(QLatin1Char('='));
        if (!haveFor && eq > 0 &&
            p.left(eq).trimmed().compare(QLatin1String("for"), Qt::CaseInsensitive) == 0) {
            forValue = p.mid(eq + 1).trimmed();
            haveFor = true;
        }
        pair.clear();
    };
    auto endElement = [&]() {
        endPair();
        if (content) out.append(haveFor ? forValue : QString());
        forValue.clear();
        haveFor = false;
        content = false;
    };
    for (int i = 0; i < header.size(); ++i) {
        const QChar c = header.at(i);
        if (quoted) {
            pair.append(c);
            if (c == QLatin1Char('\\') && i + 1 < header.size()) {
                pair.append(header.at(++i));
            } else if (c == QLatin1Char('"')) {
                quoted = false;
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            quoted = true;
            pair.append(c);
        } else if (c == QLatin1Char(';')) {
            endPair();
        } else if (c == QLatin1Char(',')) {
            endElement();
        } else {
            pair.append(c);
        }
    }
    endElement();
    return out;
}

QStringList xForwardedFor(const QString& header)
{
    QStringList out;
    for (const QString& part : header.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString p = part.trimmed();
        if (!p.isEmpty()) out.append(p);
    }
    return out;
}

QString rightmostUntrusted(const QStringList& nodes, const TrustedProxies& trusted)
{
    QString leftmost;
    for (int i = nodes.size() - 1; i >= 0; --i) {
        const QString addr = parseNode(nodes.at(i));
        if (addr.isEmpty()) return {};
        if (!isTrusted(QHostAddress(addr), trusted)) return addr;
        leftmost = addr;
    }
    return leftmost;
}

// ── The decision ────────────────────────────────────────────────────────────

Resolution resolve(const QString& peer, const QMap<QString, QString>& headers,
                   const TrustedProxies& trusted, bool viaTunnel)
{
    Resolution r;
    r.peer = peer;
    r.client = normalizeOrKeep(peer);
    r.attempt = r.client;

    // 1. The tunnel's peer is ICE's own conclusion; nothing the browser sent
    //    is read.
    if (viaTunnel) {
        r.via = Via::Tunnel;
        return r;
    }

    const QHostAddress peerAddr = parseAddress(peer);

    // 2. A proxy we were told to believe.
    if (isTrusted(peerAddr, trusted)) {
        QString client;
        QString header;
        if (!headers.value(kXff).trimmed().isEmpty()) {
            header = kXff;
            client = rightmostUntrusted(xForwardedFor(headers.value(kXff)), trusted);
        } else if (!headers.value(kForwarded).trimmed().isEmpty()) {
            header = kForwarded;
            client = rightmostUntrusted(forwardedFor(headers.value(kForwarded)), trusted);
        }
        if (!client.isEmpty()) {
            r.client = client;
            r.attempt = client;
            r.via = Via::TrustedProxy;
            r.header = header;
            return r;
        }
    }

    // 3. The peer; a loopback peer's attempts follow the local-tunnel rule.
    if (peerAddr.isLoopback()) {
        QString header;
        const QString candidate = localTunnelCandidate(headers, &header);
        if (!candidate.isEmpty()) {
            r.attempt = candidate;
            r.via = Via::LocalProxy;
            r.header = header;
        }
    }
    return r;
}

Resolution stamp(HttpRequest& req, const TrustedProxies& trusted, bool viaTunnel)
{
    const Resolution r = resolve(req.clientAddress, req.headers, trusted, viaTunnel);
    req.sourceAddress = r.client;
    req.attemptAddress = r.attempt;
    return r;
}

QString describe(const HttpRequest& req)
{
    const QString& source = req.source();
    if (source == req.clientAddress || normalizeOrKeep(req.clientAddress) == source)
        return req.clientAddress;
    return source + QStringLiteral(" via ") + req.clientAddress;
}

bool hasForwardingHeaders(const QMap<QString, QString>& headers)
{
    return headers.contains(kXff) || headers.contains(kForwarded) || headers.contains(kCf) ||
           headers.contains(QStringLiteral("tailscale-funnel-request"));
}

QString forwardedHost(const QString& peer, const QMap<QString, QString>& headers,
                      const TrustedProxies& trusted)
{
    // A reverse proxy in front of this PC names the address the browser used;
    // anyone else's word for it is the Host itself.
    const NetClassify::Kind kind = NetClassify::classify(peer);
    if (kind == NetClassify::Kind::Loopback || kind == NetClassify::Kind::Private ||
        isTrusted(peer, trusted)) {
        const QString fwd = headers.value(QStringLiteral("x-forwarded-host"))
                                .section(QLatin1Char(','), 0, 0)
                                .trimmed();
        if (!fwd.isEmpty()) return fwd;
    }
    return headers.value(QStringLiteral("host"));
}

} // namespace ClientAddress
