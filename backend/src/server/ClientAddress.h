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

#include "common/Types.h"

#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

/**
 * Who sent a request, as opposed to where its socket came from.
 *
 * Behind a reverse proxy every request arrives from the proxy's address, so
 * the rate-limit buckets, ConnectionGuard, the sessions list and the logs all
 * see one caller. `trusted_proxies` names the proxies whose forwarding headers
 * are believed; for those, the client is read from X-Forwarded-For (else the
 * standard Forwarded header), right-most untrusted entry first.
 *
 * The answer is for bookkeeping only. It never grants anything: every check of
 * where the caller sits (local privilege, the admin unlock, LAN-only routes)
 * keeps reading the socket peer, `Resolution::peer`. Pure Qt Core/Network, no
 * state, so the whole precedence is covered by tests/test_client_address.cpp.
 */
namespace ClientAddress {

/// One trusted_proxies entry: an IPv4 or IPv6 network. An IPv4-mapped IPv6
/// network (::ffff:192.0.2.0/120) is stored as the IPv4 network it names.
struct Cidr
{
    QHostAddress network; ///< host bits cleared
    int prefix = -1;      ///< 0..32 for IPv4, 0..128 for IPv6

    bool contains(const QHostAddress& addr) const;
    /// Canonical text, always with its prefix: "192.0.2.0/24", "2001:db8::/32".
    QString toString() const;
};

using TrustedProxies = QList<Cidr>;

/// Most entries a list may hold. A longer list is refused rather than walked
/// on every request.
constexpr int kMaxTrustedProxies = 64;

/// An address as written anywhere: IPv4, IPv6, IPv4-mapped IPv6 (returned as
/// the IPv4 address), an IPv6 zone id dropped. Null when it is not an address.
QHostAddress parseAddress(const QString& text);

/// parseAddress() as canonical text ("2001:db8::1", "192.0.2.7"), or "" when
/// it is not an address.
QString normalize(const QString& text);

/// normalize(), or @p text unchanged when it is not an address (a tunnel peer
/// is a label like "peer-4f2a", and still has to name somebody).
QString normalizeOrKeep(const QString& text);

/// Why an entry was refused. "" when it was accepted.
enum class CidrError
{
    None,
    Empty,     ///< nothing but spaces
    NotAnIp,   ///< a hostname, junk, a malformed address
    BadPrefix, ///< not a number, out of range for the family
    TooBroad,  ///< /0: trusting every address means believing every header
};

/// "192.0.2.10" (a /32), "192.0.2.0/24", "2001:db8::/32", "[2001:db8::1]",
/// "::ffff:192.0.2.0/120". Host bits are cleared, not refused.
std::optional<Cidr> parseCidr(const QString& text, CidrError* error = nullptr);

/// A machine-readable name for @p error ("not_an_ip", "too_broad", …).
QString cidrErrorName(CidrError error);

/// Parse a whole list. Every refused entry lands in @p invalid (as given) and
/// is left out of the result; a duplicate is kept once.
TrustedProxies parseList(const QStringList& entries, QStringList* invalid = nullptr);

/// Split an environment value ("192.0.2.1, 10.0.0.0/8 2001:db8::/32") on
/// commas and white space.
QStringList splitList(const QString& value);

/// Canonical text of each entry.
QStringList toStrings(const TrustedProxies& list);

/// Whether @p ip falls inside one of @p list.
bool isTrusted(const QString& ip, const TrustedProxies& list);
bool isTrusted(const QHostAddress& ip, const TrustedProxies& list);

/// One node of a forwarding header as a proxy writes it: maybe quoted, maybe
/// with a port, IPv6 in brackets when it carries one ("[2001:db8::1]:4711").
/// "" for anything that is not an address: "unknown", an obfuscated
/// "_hidden" identifier, junk.
QString parseNode(const QString& node);

/// The `for=` value of each element of a Forwarded header (RFC 7239), left to
/// right, as written (quotes kept, so parseNode() decides). An element with no
/// `for=` yields "", which parseNode() refuses. Commas and semicolons inside a
/// quoted string do not split.
QStringList forwardedFor(const QString& header);

/// The entries of an X-Forwarded-For header, left to right, empty ones dropped.
QStringList xForwardedFor(const QString& header);

/// Right-most-untrusted: walk @p nodes from the right, skip the ones that are
/// themselves trusted proxies, and return the first that is not. When every
/// node is trusted, the left-most (the request started inside the trusted
/// set). "" when the list is empty or a node on the way is not an address.
QString rightmostUntrusted(const QStringList& nodes, const TrustedProxies& trusted);

/// How the client was decided.
enum class Via
{
    Peer,         ///< the socket peer, nothing else believed
    Tunnel,       ///< carried over the rendezvous tunnel: the ICE address
    LocalProxy,   ///< loopback peer not in trusted_proxies: only the attempt
                  ///< bucket follows the headers (see Resolution::attempt)
    TrustedProxy, ///< peer in trusted_proxies, client read from its headers
};

struct Resolution
{
    /// The socket peer as the server saw it. The only address a privilege
    /// check may read.
    QString peer;
    /// Who the request is accounted to: rate limits, bans, sessions list,
    /// geolocation, logs. The forwarded client behind a trusted proxy, else
    /// the peer.
    QString client;
    /// The address a PIN or password attempt is counted against: the client,
    /// except for a loopback peer that is not a trusted proxy, where the
    /// local-tunnel rule applies (see resolve()).
    QString attempt;
    Via via = Via::Peer;
    /// The header the client was read from ("x-forwarded-for", "forwarded",
    /// "cf-connecting-ip"), "" when none was believed.
    QString header;

    bool forwarded() const { return via == Via::TrustedProxy; }
};

/**
 * Decide who sent a request. One precedence, in this order:
 *
 *  1. Over the rendezvous tunnel (@p viaTunnel): the peer is the address ICE
 *     settled on. No header is read.
 *  2. Peer inside @p trusted: X-Forwarded-For when the request has one, else
 *     Forwarded `for=`, walked right-most-untrusted. The result is both the
 *     client and the attempt address. A header that is malformed on the way
 *     falls through to 3, as if there were none.
 *  3. Otherwise the client is the peer. The attempt address is the peer too,
 *     except for a loopback peer (a tunnel such as cloudflared or Tailscale
 *     Funnel running on this machine): CF-Connecting-IP, else the LAST
 *     X-Forwarded-For entry, else the last Forwarded `for=`, else the peer.
 *     This is the rule AuthManager::attemptAddress has always applied, kept
 *     as it was so a default install behaves exactly as before.
 *
 * Headers are the request's, with lower-case names (HttpParser's form).
 */
Resolution resolve(const QString& peer, const QMap<QString, QString>& headers,
                   const TrustedProxies& trusted, bool viaTunnel = false);

/// resolve() for a request, written into its sourceAddress and attemptAddress.
/// clientAddress (the peer) is left exactly as it was: privilege reads it, and
/// nothing here may move it.
Resolution stamp(HttpRequest& req, const TrustedProxies& trusted, bool viaTunnel = false);

/// The caller for a log line: "198.51.100.23 via 192.0.2.10" behind a trusted
/// proxy, else the peer.
QString describe(const HttpRequest& req);

/// Whether a request carries any header a proxy adds on its way through. Only
/// a hint (a browser on the LAN never sends one); it grants nothing.
bool hasForwardingHeaders(const QMap<QString, QString>& headers);

/// The authority the browser typed, as a reverse proxy in front of us passes
/// it in X-Forwarded-Host (first entry), when the peer is loopback, private or
/// a trusted proxy; else, or when there is none, the Host header. Used to name
/// a link, never to grant anything.
QString forwardedHost(const QString& peer, const QMap<QString, QString>& headers,
                      const TrustedProxies& trusted);

} // namespace ClientAddress
