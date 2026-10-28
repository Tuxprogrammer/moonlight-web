/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Who sent a request behind a reverse proxy (trusted_proxies). The parser and
 * the decision are pure, so every precedence rule, spoof attempt and junk
 * header is pinned here rather than read off HttpServer.
 */
#include "test_framework.h"

#include "server/ClientAddress.h"

namespace {

using H = QMap<QString, QString>;
namespace CA = ClientAddress;

CA::TrustedProxies trust(std::initializer_list<const char*> entries)
{
    QStringList list;
    for (const char* e : entries)
        list << QString::fromLatin1(e);
    QStringList invalid;
    const CA::TrustedProxies out = CA::parseList(list, &invalid);
    if (!invalid.isEmpty()) std::fprintf(stderr, "  (test list had invalid entries)\n");
    return out;
}

void testAddresses()
{
    // IPv4, IPv6 canonicalised, IPv4-mapped IPv6 as IPv4, zone ids dropped.
    CHECK_EQ(CA::normalize("192.0.2.7"), QString("192.0.2.7"));
    CHECK_EQ(CA::normalize(" 192.0.2.7 "), QString("192.0.2.7"));
    CHECK_EQ(CA::normalize("2001:DB8:0:0::1"), QString("2001:db8::1"));
    CHECK_EQ(CA::normalize("::ffff:198.51.100.4"), QString("198.51.100.4"));
    CHECK_EQ(CA::normalize("::FFFF:198.51.100.4"), QString("198.51.100.4"));
    CHECK_EQ(CA::normalize("fe80::1%eth0"), QString("fe80::1"));
    CHECK_EQ(CA::normalize("::1"), QString("::1"));
    // Not addresses.
    CHECK_EQ(CA::normalize(""), QString());
    CHECK_EQ(CA::normalize("proxy.example"), QString());
    CHECK_EQ(CA::normalize("unknown"), QString());
    CHECK_EQ(CA::normalize("192.0.2"), QString());
    CHECK_EQ(CA::normalize("192.0.2.256"), QString());
    CHECK_EQ(CA::normalize("peer-4f2a"), QString());
    // A label survives normalizeOrKeep, an address is canonicalised.
    CHECK_EQ(CA::normalizeOrKeep("peer-4f2a"), QString("peer-4f2a"));
    CHECK_EQ(CA::normalizeOrKeep("::ffff:192.0.2.1"), QString("192.0.2.1"));
}

void testCidr()
{
    CA::CidrError err = CA::CidrError::None;

    // A bare address is a single host.
    auto c = CA::parseCidr("192.0.2.10", &err);
    CHECK(c.has_value());
    CHECK(err == CA::CidrError::None);
    CHECK_EQ(c->toString(), QString("192.0.2.10/32"));
    c = CA::parseCidr("2001:db8::1");
    CHECK(c && c->toString() == QString("2001:db8::1/128"));
    c = CA::parseCidr("[2001:db8::1]");
    CHECK(c && c->toString() == QString("2001:db8::1/128"));

    // Networks, host bits cleared, white space trimmed.
    c = CA::parseCidr(" 10.0.0.0/8 ");
    CHECK(c && c->toString() == QString("10.0.0.0/8"));
    c = CA::parseCidr("192.0.2.77/24");
    CHECK(c && c->toString() == QString("192.0.2.0/24"));
    c = CA::parseCidr("2001:db8:1234::5/32");
    CHECK(c && c->toString() == QString("2001:db8::/32"));
    c = CA::parseCidr("[2001:db8::]/48");
    CHECK(c && c->toString() == QString("2001:db8::/48"));
    c = CA::parseCidr("fc00::/7");
    CHECK(c && c->toString() == QString("fc00::/7"));
    c = CA::parseCidr("192.0.2.1/31");
    CHECK(c && c->toString() == QString("192.0.2.0/31"));
    c = CA::parseCidr("192.0.2.1/1");
    CHECK(c && c->toString() == QString("128.0.0.0/1"));

    // IPv4-mapped networks are the IPv4 network they name.
    c = CA::parseCidr("::ffff:192.0.2.0/120");
    CHECK(c && c->toString() == QString("192.0.2.0/24"));
    c = CA::parseCidr("::ffff:192.0.2.9");
    CHECK(c && c->toString() == QString("192.0.2.9/32"));
    CHECK(!CA::parseCidr("::ffff:192.0.2.0/95", &err));
    CHECK(err == CA::CidrError::BadPrefix);

    // Refusals, each with its reason.
    CHECK(!CA::parseCidr("", &err));
    CHECK(err == CA::CidrError::Empty);
    CHECK(!CA::parseCidr("   ", &err));
    CHECK(err == CA::CidrError::Empty);
    CHECK(!CA::parseCidr("proxy.example", &err));
    CHECK(err == CA::CidrError::NotAnIp);
    CHECK(!CA::parseCidr("localhost", &err));
    CHECK(err == CA::CidrError::NotAnIp);
    CHECK(!CA::parseCidr("192.0.2/24", &err));
    CHECK(err == CA::CidrError::NotAnIp);
    CHECK(!CA::parseCidr("/24", &err));
    CHECK(err == CA::CidrError::NotAnIp);
    CHECK(!CA::parseCidr("192.0.2.0/", &err));
    CHECK(err == CA::CidrError::BadPrefix);
    CHECK(!CA::parseCidr("192.0.2.0/33", &err));
    CHECK(err == CA::CidrError::BadPrefix);
    CHECK(!CA::parseCidr("192.0.2.0/-1", &err));
    CHECK(err == CA::CidrError::BadPrefix);
    CHECK(!CA::parseCidr("192.0.2.0/+24", &err));
    CHECK(err == CA::CidrError::BadPrefix);
    CHECK(!CA::parseCidr("192.0.2.0/ 24", &err));
    CHECK(err == CA::CidrError::BadPrefix);
    CHECK(!CA::parseCidr("192.0.2.0/24/1", &err));
    CHECK(err == CA::CidrError::BadPrefix);
    CHECK(!CA::parseCidr("192.0.2.0/abc", &err));
    CHECK(err == CA::CidrError::BadPrefix);
    CHECK(!CA::parseCidr("2001:db8::/129", &err));
    CHECK(err == CA::CidrError::BadPrefix);
    CHECK(!CA::parseCidr("fe80::1%eth0/64", &err));
    CHECK(err == CA::CidrError::NotAnIp);
    // Trusting every address would believe every header: refused.
    CHECK(!CA::parseCidr("0.0.0.0/0", &err));
    CHECK(err == CA::CidrError::TooBroad);
    CHECK(!CA::parseCidr("::/0", &err));
    CHECK(err == CA::CidrError::TooBroad);
    CHECK(!CA::parseCidr("192.0.2.1/0", &err));
    CHECK(err == CA::CidrError::TooBroad);

    CHECK_EQ(CA::cidrErrorName(CA::CidrError::None), QString());
    CHECK_EQ(CA::cidrErrorName(CA::CidrError::Empty), QString("empty"));
    CHECK_EQ(CA::cidrErrorName(CA::CidrError::NotAnIp), QString("not_an_ip"));
    CHECK_EQ(CA::cidrErrorName(CA::CidrError::BadPrefix), QString("bad_prefix"));
    CHECK_EQ(CA::cidrErrorName(CA::CidrError::TooBroad), QString("too_broad"));
}

void testMatching()
{
    const auto v4 = CA::parseCidr("192.0.2.0/24");
    CHECK(v4->contains(QHostAddress("192.0.2.0")));
    CHECK(v4->contains(QHostAddress("192.0.2.255")));
    CHECK(!v4->contains(QHostAddress("192.0.3.0")));
    CHECK(!v4->contains(QHostAddress("2001:db8::1")));
    // An IPv4-mapped peer (a dual-stack listener) matches the IPv4 network.
    CHECK(v4->contains(QHostAddress("::ffff:192.0.2.9")));
    CHECK(!v4->contains(QHostAddress()));

    const auto v6 = CA::parseCidr("2001:db8::/32");
    CHECK(v6->contains(QHostAddress("2001:db8:ffff::1")));
    CHECK(!v6->contains(QHostAddress("2001:db9::1")));
    CHECK(!v6->contains(QHostAddress("192.0.2.1")));

    // Odd prefixes cut inside a byte.
    const auto odd = CA::parseCidr("10.0.0.0/9");
    CHECK(odd->contains(QHostAddress("10.127.255.255")));
    CHECK(!odd->contains(QHostAddress("10.128.0.0")));
    const auto ula = CA::parseCidr("fc00::/7");
    CHECK(ula->contains(QHostAddress("fd12:3456::1")));
    CHECK(!ula->contains(QHostAddress("fe80::1")));

    const CA::TrustedProxies list = trust({"192.0.2.10", "2001:db8::/32"});
    CHECK(CA::isTrusted("192.0.2.10", list));
    CHECK(CA::isTrusted("::ffff:192.0.2.10", list));
    CHECK(CA::isTrusted("2001:db8::abcd", list));
    CHECK(!CA::isTrusted("192.0.2.11", list));
    CHECK(!CA::isTrusted("", list));
    CHECK(!CA::isTrusted("peer-4f2a", list));
    CHECK(!CA::isTrusted("192.0.2.10", CA::TrustedProxies{}));
}

void testLists()
{
    QStringList invalid;
    const CA::TrustedProxies list = CA::parseList(
        {"192.0.2.10", "junk", "10.0.0.0/8", "192.0.2.10/32", "0.0.0.0/0", "2001:db8::/32"},
        &invalid);
    // Bad entries reported as given, a duplicate kept once, order kept.
    CHECK_EQ(invalid, QStringList({"junk", "0.0.0.0/0"}));
    CHECK_EQ(CA::toStrings(list), QStringList({"192.0.2.10/32", "10.0.0.0/8", "2001:db8::/32"}));
    CHECK(CA::parseList({}).isEmpty());

    // Too many entries: everything past the cap is refused.
    QStringList many;
    for (int i = 0; i < CA::kMaxTrustedProxies + 3; ++i)
        many << QStringLiteral("198.51.100.%1").arg(i);
    invalid.clear();
    CHECK_EQ(CA::parseList(many, &invalid).size(), CA::kMaxTrustedProxies);
    CHECK_EQ(invalid.size(), 3);

    CHECK_EQ(CA::splitList(" 192.0.2.1, 10.0.0.0/8\t2001:db8::/32 ,,"),
             QStringList({"192.0.2.1", "10.0.0.0/8", "2001:db8::/32"}));
    CHECK(CA::splitList("").isEmpty());
    CHECK(CA::splitList(" , ").isEmpty());
}

void testNodes()
{
    CHECK_EQ(CA::parseNode("203.0.113.9"), QString("203.0.113.9"));
    CHECK_EQ(CA::parseNode(" 203.0.113.9 "), QString("203.0.113.9"));
    CHECK_EQ(CA::parseNode("203.0.113.9:4711"), QString("203.0.113.9"));
    CHECK_EQ(CA::parseNode("2001:db8::7"), QString("2001:db8::7"));
    CHECK_EQ(CA::parseNode("[2001:db8::7]"), QString("2001:db8::7"));
    CHECK_EQ(CA::parseNode("[2001:db8::7]:4711"), QString("2001:db8::7"));
    CHECK_EQ(CA::parseNode("\"[2001:db8::7]:4711\""), QString("2001:db8::7"));
    CHECK_EQ(CA::parseNode("\"192.0.2.60\""), QString("192.0.2.60"));
    CHECK_EQ(CA::parseNode("::ffff:203.0.113.9"), QString("203.0.113.9"));
    // Not addresses.
    CHECK_EQ(CA::parseNode(""), QString());
    CHECK_EQ(CA::parseNode("unknown"), QString());
    CHECK_EQ(CA::parseNode("_hidden"), QString());
    CHECK_EQ(CA::parseNode("\"_gazonk\""), QString());
    CHECK_EQ(CA::parseNode("proxy.example"), QString());
    CHECK_EQ(CA::parseNode("[2001:db8::7"), QString());
    CHECK_EQ(CA::parseNode("203.0.113.9:port"), QString());
    CHECK_EQ(CA::parseNode("203.0.113.9:4711:1"), QString());
    CHECK_EQ(CA::parseNode("\""), QString());
    CHECK_EQ(CA::parseNode("<script>"), QString());

    CHECK_EQ(CA::xForwardedFor("198.51.100.1, 203.0.113.9 ,,10.0.0.2"),
             QStringList({"198.51.100.1", "203.0.113.9", "10.0.0.2"}));
    CHECK(CA::xForwardedFor("").isEmpty());

    // RFC 7239: elements by comma, pairs by semicolon, names case-insensitive,
    // quoted values kept quoted, separators inside quotes ignored.
    CHECK_EQ(CA::forwardedFor("for=192.0.2.43, for=\"[2001:db8:cafe::17]:4711\""),
             QStringList({"192.0.2.43", "\"[2001:db8:cafe::17]:4711\""}));
    CHECK_EQ(CA::forwardedFor("proto=https;For=192.0.2.60;by=203.0.113.43"),
             QStringList({"192.0.2.60"}));
    CHECK_EQ(CA::forwardedFor("for=\"a,b;c\";proto=http, for=198.51.100.17"),
             QStringList({"\"a,b;c\"", "198.51.100.17"}));
    // An element without for= is kept as a hole that parseNode refuses.
    CHECK_EQ(CA::forwardedFor("proto=https, for=198.51.100.17"),
             QStringList({"", "198.51.100.17"}));
    CHECK(CA::forwardedFor("").isEmpty());
}

void testRightmostUntrusted()
{
    const CA::TrustedProxies t = trust({"192.0.2.10", "192.0.2.11", "10.0.0.0/8"});

    // One proxy: its own entry is the client.
    CHECK_EQ(CA::rightmostUntrusted({"203.0.113.9"}, t), QString("203.0.113.9"));
    // Whatever the client wrote before the proxy's entry is ignored.
    CHECK_EQ(CA::rightmostUntrusted({"127.0.0.1", "198.51.100.66", "203.0.113.9"}, t),
             QString("203.0.113.9"));
    // Two proxies in a row: skip the inner one, stop at the client.
    CHECK_EQ(CA::rightmostUntrusted({"6.6.6.6", "203.0.113.9", "192.0.2.11"}, t),
             QString("203.0.113.9"));
    CHECK_EQ(CA::rightmostUntrusted({"203.0.113.9", "10.1.2.3", "192.0.2.11"}, t),
             QString("203.0.113.9"));
    // Every hop trusted: the request started inside, the left-most is it.
    CHECK_EQ(CA::rightmostUntrusted({"10.0.0.5", "192.0.2.11"}, t), QString("10.0.0.5"));
    // A private untrusted client is a client like any other.
    CHECK_EQ(CA::rightmostUntrusted({"192.168.1.20"}, t), QString("192.168.1.20"));
    // Junk on the way: no answer, the caller falls back to the peer.
    CHECK_EQ(CA::rightmostUntrusted({"203.0.113.9", "garbage"}, t), QString());
    CHECK_EQ(CA::rightmostUntrusted({"203.0.113.9", "unknown", "192.0.2.11"}, t), QString());
    // Junk left of the client is the client's own business.
    CHECK_EQ(CA::rightmostUntrusted({"garbage", "203.0.113.9"}, t), QString("203.0.113.9"));
    CHECK_EQ(CA::rightmostUntrusted({}, t), QString());
    // Ports and brackets are parsed on the way.
    CHECK_EQ(CA::rightmostUntrusted({"[2001:db8::7]:4711", "192.0.2.10:80"}, t),
             QString("2001:db8::7"));
}

void testResolveDefault()
{
    // trusted_proxies empty: exactly today's behaviour.
    const CA::TrustedProxies none;

    auto r = CA::resolve("203.0.113.9", H{{"x-forwarded-for", "198.51.100.1"}}, none);
    CHECK_EQ(r.client, QString("203.0.113.9"));
    CHECK_EQ(r.attempt, QString("203.0.113.9"));
    CHECK_EQ(r.peer, QString("203.0.113.9"));
    CHECK(r.via == CA::Via::Peer);
    CHECK(!r.forwarded());
    CHECK_EQ(r.header, QString());

    // A LAN peer forging the headers buys nothing.
    r = CA::resolve("192.168.1.20",
                    H{{"x-forwarded-for", "9.9.9.9"},
                      {"forwarded", "for=9.9.9.9"},
                      {"cf-connecting-ip", "9.9.9.9"}},
                    none);
    CHECK_EQ(r.client, QString("192.168.1.20"));
    CHECK_EQ(r.attempt, QString("192.168.1.20"));

    // Loopback (a tunnel on this machine): only the attempt bucket follows the
    // headers, as AuthManager::attemptAddress always did. The client stays the
    // peer, so ConnectionGuard and the sessions list are untouched.
    r = CA::resolve("127.0.0.1", H{{"x-forwarded-for", "6.6.6.6, 203.0.113.9"}}, none);
    CHECK_EQ(r.client, QString("127.0.0.1"));
    CHECK_EQ(r.attempt, QString("203.0.113.9"));
    CHECK(r.via == CA::Via::LocalProxy);
    CHECK_EQ(r.header, QString("x-forwarded-for"));
    r = CA::resolve("127.0.0.1",
                    H{{"cf-connecting-ip", "198.51.100.7"}, {"x-forwarded-for", "10.0.0.1"}}, none);
    CHECK_EQ(r.attempt, QString("198.51.100.7"));
    CHECK_EQ(r.header, QString("cf-connecting-ip"));
    r = CA::resolve("::1", H{{"forwarded", "for=1.1.1.1, for=\"[2001:db8::7]:4711\""}}, none);
    CHECK_EQ(r.attempt, QString("2001:db8::7"));
    CHECK_EQ(r.header, QString("forwarded"));
    r = CA::resolve("::ffff:127.0.0.1", H{{"x-forwarded-for", "203.0.113.9"}}, none);
    CHECK_EQ(r.attempt, QString("203.0.113.9"));
    r = CA::resolve("127.0.0.1", H{{"x-forwarded-for", "not-an-ip"}}, none);
    CHECK_EQ(r.attempt, QString("127.0.0.1"));
    CHECK(r.via == CA::Via::Peer);
    r = CA::resolve("127.0.0.1", H{}, none);
    CHECK_EQ(r.attempt, QString("127.0.0.1"));
    CHECK(r.via == CA::Via::Peer);
}

void testResolveTrusted()
{
    const CA::TrustedProxies t = trust({"192.0.2.10", "2001:db8:aaaa::/48"});

    // The proxy appends the client: believed.
    auto r = CA::resolve("192.0.2.10", H{{"x-forwarded-for", "203.0.113.9"}}, t);
    CHECK_EQ(r.client, QString("203.0.113.9"));
    CHECK_EQ(r.attempt, QString("203.0.113.9"));
    CHECK_EQ(r.peer, QString("192.0.2.10"));
    CHECK(r.via == CA::Via::TrustedProxy);
    CHECK(r.forwarded());
    CHECK_EQ(r.header, QString("x-forwarded-for"));

    // A dual-stack listener reports the proxy as IPv4-mapped: still trusted.
    r = CA::resolve("::ffff:192.0.2.10", H{{"x-forwarded-for", "203.0.113.9"}}, t);
    CHECK_EQ(r.client, QString("203.0.113.9"));

    // The client wrote an X-Forwarded-For of its own: the proxy appended to
    // it, and the right-most entry is still the real one.
    r = CA::resolve("192.0.2.10", H{{"x-forwarded-for", "127.0.0.1, 203.0.113.9"}}, t);
    CHECK_EQ(r.client, QString("203.0.113.9"));

    // IPv6 proxy, IPv6 client with port in brackets.
    r = CA::resolve("2001:db8:aaaa::2", H{{"x-forwarded-for", "[2001:db8:bbbb::9]:51000"}}, t);
    CHECK_EQ(r.client, QString("2001:db8:bbbb::9"));

    // Standard Forwarded when there is no X-Forwarded-For.
    r = CA::resolve("192.0.2.10",
                    H{{"forwarded", "for=198.51.100.66, for=\"203.0.113.9:4711\";proto=https"}}, t);
    CHECK_EQ(r.client, QString("203.0.113.9"));
    CHECK_EQ(r.header, QString("forwarded"));
    r = CA::resolve("192.0.2.10", H{{"forwarded", "for=\"[2001:db8:bbbb::9]\""}}, t);
    CHECK_EQ(r.client, QString("2001:db8:bbbb::9"));

    // X-Forwarded-For wins when both are present (the proxy that writes only
    // Forwarded is documented as having to strip it).
    r = CA::resolve("192.0.2.10",
                    H{{"x-forwarded-for", "203.0.113.9"}, {"forwarded", "for=198.51.100.66"}}, t);
    CHECK_EQ(r.client, QString("203.0.113.9"));

    // CF-Connecting-IP is a single value a client can send through a proxy
    // that does not strip it: never believed from a trusted proxy.
    r = CA::resolve("192.0.2.10", H{{"cf-connecting-ip", "198.51.100.66"}}, t);
    CHECK_EQ(r.client, QString("192.0.2.10"));
    CHECK_EQ(r.attempt, QString("192.0.2.10"));
    CHECK(r.via == CA::Via::Peer);

    // Missing or malformed headers: the peer.
    r = CA::resolve("192.0.2.10", H{}, t);
    CHECK_EQ(r.client, QString("192.0.2.10"));
    CHECK(r.via == CA::Via::Peer);
    r = CA::resolve("192.0.2.10", H{{"x-forwarded-for", ""}}, t);
    CHECK_EQ(r.client, QString("192.0.2.10"));
    r = CA::resolve("192.0.2.10", H{{"x-forwarded-for", "203.0.113.9, junk"}}, t);
    CHECK_EQ(r.client, QString("192.0.2.10"));
    CHECK_EQ(r.attempt, QString("192.0.2.10"));
    r = CA::resolve("192.0.2.10", H{{"x-forwarded-for", "unknown"}}, t);
    CHECK_EQ(r.client, QString("192.0.2.10"));
    r = CA::resolve("192.0.2.10", H{{"forwarded", "for=_hidden"}}, t);
    CHECK_EQ(r.client, QString("192.0.2.10"));
    r = CA::resolve("192.0.2.10", H{{"forwarded", "proto=https"}}, t);
    CHECK_EQ(r.client, QString("192.0.2.10"));
    // A malformed X-Forwarded-For is not rescued by a Forwarded header.
    r = CA::resolve("192.0.2.10",
                    H{{"x-forwarded-for", "garbage"}, {"forwarded", "for=198.51.100.66"}}, t);
    CHECK_EQ(r.client, QString("192.0.2.10"));

    // Every hop trusted: the left-most, which is itself a trusted address.
    r = CA::resolve("192.0.2.10", H{{"x-forwarded-for", "2001:db8:aaaa::7"}}, t);
    CHECK_EQ(r.client, QString("2001:db8:aaaa::7"));
    CHECK(r.forwarded());
}

void testResolveSpoof()
{
    const CA::TrustedProxies t = trust({"192.0.2.10"});

    // An untrusted peer's headers are ignored entirely, whatever they say.
    for (const char* peer : {"203.0.113.50", "192.168.1.20", "10.0.0.7", "2001:db8:ffff::1"}) {
        const auto r = CA::resolve(peer,
                                   H{{"x-forwarded-for", "198.51.100.1"},
                                     {"forwarded", "for=198.51.100.1"},
                                     {"cf-connecting-ip", "198.51.100.1"}},
                                   t);
        CHECK_EQ(r.client, CA::normalize(peer));
        CHECK_EQ(r.attempt, CA::normalize(peer));
        CHECK(!r.forwarded());
    }

    // A client claiming to BE the trusted proxy in its own header gets nowhere:
    // the proxy's appended entry is untrusted, and it is the answer.
    auto r = CA::resolve("192.0.2.10", H{{"x-forwarded-for", "192.0.2.10, 203.0.113.9"}}, t);
    CHECK_EQ(r.client, QString("203.0.113.9"));
    // …but a client whose request comes from the proxy's own address, saying
    // "the proxy forwarded me from 127.0.0.1", is read as 127.0.0.1. That is
    // bookkeeping only: privilege reads the peer (see test_api_csrf).
    r = CA::resolve("192.0.2.10", H{{"x-forwarded-for", "127.0.0.1"}}, t);
    CHECK_EQ(r.client, QString("127.0.0.1"));
    CHECK_EQ(r.peer, QString("192.0.2.10"));

    // Over the rendezvous tunnel no header is ever read, even when the ICE
    // address happens to sit inside a trusted network.
    const CA::TrustedProxies lan = trust({"192.168.1.0/24"});
    r = CA::resolve("192.168.1.50", H{{"x-forwarded-for", "203.0.113.9"}}, lan, true);
    CHECK_EQ(r.client, QString("192.168.1.50"));
    CHECK_EQ(r.attempt, QString("192.168.1.50"));
    CHECK(r.via == CA::Via::Tunnel);
    r = CA::resolve("peer-4f2a", H{{"x-forwarded-for", "203.0.113.9"}}, lan, true);
    CHECK_EQ(r.client, QString("peer-4f2a"));
    CHECK_EQ(r.attempt, QString("peer-4f2a"));
    r = CA::resolve("127.0.0.1", H{{"cf-connecting-ip", "203.0.113.9"}}, {}, true);
    CHECK_EQ(r.attempt, QString("127.0.0.1"));

    // A loopback proxy in the list uses the trusted-proxy rule, not the
    // local-tunnel one: the right-most untrusted entry, CF-Connecting-IP not
    // believed, and the client (not only the attempt) follows.
    const CA::TrustedProxies lo = trust({"127.0.0.1/32", "::1"});
    r = CA::resolve("127.0.0.1", H{{"x-forwarded-for", "203.0.113.9"}}, lo);
    CHECK_EQ(r.client, QString("203.0.113.9"));
    CHECK_EQ(r.attempt, QString("203.0.113.9"));
    CHECK(r.forwarded());
    r = CA::resolve("127.0.0.1", H{{"x-forwarded-for", "203.0.113.9, 127.0.0.1"}}, lo);
    CHECK_EQ(r.client, QString("203.0.113.9"));
    // Malformed from a trusted loopback peer: the local-tunnel rule takes over
    // for the attempt, exactly as without the list.
    r = CA::resolve("127.0.0.1",
                    H{{"x-forwarded-for", "junk"}, {"cf-connecting-ip", "198.51.100.7"}}, lo);
    CHECK_EQ(r.client, QString("127.0.0.1"));
    CHECK_EQ(r.attempt, QString("198.51.100.7"));
}

void testHints()
{
    CHECK(!CA::hasForwardingHeaders(H{}));
    CHECK(!CA::hasForwardingHeaders(H{{"host", "proxy.example"}}));
    CHECK(CA::hasForwardingHeaders(H{{"x-forwarded-for", "203.0.113.9"}}));
    CHECK(CA::hasForwardingHeaders(H{{"forwarded", "for=203.0.113.9"}}));
    CHECK(CA::hasForwardingHeaders(H{{"cf-connecting-ip", "203.0.113.9"}}));
    CHECK(CA::hasForwardingHeaders(H{{"tailscale-funnel-request", "?1"}}));

    const CA::TrustedProxies t = trust({"203.0.113.10"});
    const H h{{"host", "192.168.1.5:8443"}, {"x-forwarded-host", "proxy.example, other.example"}};
    // A loopback or LAN proxy names what the browser typed.
    CHECK_EQ(CA::forwardedHost("127.0.0.1", h, {}), QString("proxy.example"));
    CHECK_EQ(CA::forwardedHost("192.168.1.9", h, {}), QString("proxy.example"));
    // So does a trusted proxy on a public address.
    CHECK_EQ(CA::forwardedHost("203.0.113.10", h, t), QString("proxy.example"));
    // Anyone else's word for it is the Host itself.
    CHECK_EQ(CA::forwardedHost("203.0.113.11", h, t), QString("192.168.1.5:8443"));
    CHECK_EQ(CA::forwardedHost("127.0.0.1", H{{"host", "localhost:8443"}}, {}),
             QString("localhost:8443"));
    CHECK_EQ(
        CA::forwardedHost("127.0.0.1", H{{"host", "localhost"}, {"x-forwarded-host", " "}}, {}),
        QString("localhost"));
}

} // namespace

void run_client_address_tests()
{
    SECTION("ClientAddress (trusted_proxies)");
    testAddresses();
    testCidr();
    testMatching();
    testLists();
    testNodes();
    testRightmostUntrusted();
    testResolveDefault();
    testResolveTrusted();
    testResolveSpoof();
    testHints();
}
