/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/ConnectionGuard.h"

void run_connection_guard_tests()
{
    SECTION("ConnectionGuard");

    using CG = ConnectionGuard;
    const qint64 t0 = 1'000'000; // arbitrary fixed epoch (ms)

    // ── Exemptions: loopback / RFC1918 / IPv6 ULA / link-local never limited ──
    CHECK(CG::isExempt("127.0.0.1"));
    CHECK(CG::isExempt("::1"));
    CHECK(CG::isExempt("::ffff:127.0.0.1"));
    CHECK(CG::isExempt("10.0.0.5"));
    CHECK(CG::isExempt("172.16.4.1"));
    CHECK(CG::isExempt("10.0.0.50"));
    CHECK(CG::isExempt("fe80::1%eth0"));
    CHECK(CG::isExempt("fc00::1234"));
    CHECK(!CG::isExempt("8.8.8.8"));
    CHECK(!CG::isExempt("2001:db8::1"));
    CHECK(!CG::isExempt(""));

    // ── A normal public client well under the threshold is always allowed ────
    {
        CG g;
        const QString ip = "203.0.113.10";
        bool allAllowed = true;
        for (int i = 0; i < CG::CONN_MAX_PER_WINDOW; ++i)
            if (!g.allowConnection(ip, t0 + i)) allAllowed = false;
        CHECK(allAllowed);
        CHECK(!g.isBanned(ip, t0 + CG::CONN_MAX_PER_WINDOW));
    }

    // ── Connection flood: crossing the cap within the window bans the IP ─────
    {
        CG g;
        const QString ip = "203.0.113.20";
        bool banned = false;
        // All timestamps inside one window so they accumulate.
        for (int i = 0; i <= CG::CONN_MAX_PER_WINDOW; ++i)
            if (!g.allowConnection(ip, t0 + i)) banned = true;
        CHECK(banned);
        CHECK(g.isBanned(ip, t0 + CG::CONN_MAX_PER_WINDOW + 1));
        CHECK(g.banSecondsRemaining(ip, t0 + CG::CONN_MAX_PER_WINDOW + 1) > 0);
        // Still banned just before expiry, free once the ban elapses.
        CHECK(g.isBanned(ip, t0 + CG::BAN_MS - 1));
        CHECK(!g.isBanned(ip, t0 + CG::BAN_MS + CG::CONN_MAX_PER_WINDOW + 10));
    }

    // ── Sliding window: connections spread beyond the window never ban ───────
    {
        CG g;
        const QString ip = "203.0.113.30";
        bool everBanned = false;
        // One connection every (window/2) ms over many iterations: at most ~2
        // ever coexist in a window, far below the cap.
        for (int i = 0; i < CG::CONN_MAX_PER_WINDOW * 4; ++i) {
            qint64 now = t0 + static_cast<qint64>(i) * (CG::CONN_WINDOW_MS / 2 + 1);
            if (!g.allowConnection(ip, now)) everBanned = true;
        }
        CHECK(!everBanned);
    }

    // ── Auth-failure flood (brute force) bans the IP ─────────────────────────
    {
        CG g;
        const QString ip = "203.0.113.40";
        for (int i = 0; i < CG::AUTHFAIL_MAX; ++i)
            g.reportAuthFailure(ip, t0 + i);
        CHECK(g.isBanned(ip, t0 + CG::AUTHFAIL_MAX));
        // A banned IP is refused at connection time.
        CHECK(!g.allowConnection(ip, t0 + CG::AUTHFAIL_MAX + 1));
    }

    // ── Exempt IPs never get banned, even when flooded ───────────────────────
    {
        CG g;
        const QString lan = "192.168.0.42";
        bool everRefused = false;
        for (int i = 0; i <= CG::CONN_MAX_PER_WINDOW * 2; ++i)
            if (!g.allowConnection(lan, t0 + i)) everRefused = true;
        for (int i = 0; i < CG::AUTHFAIL_MAX * 2; ++i)
            g.reportAuthFailure(lan, t0 + i);
        CHECK(!everRefused);
        CHECK(!g.isBanned(lan, t0));
        CHECK_EQ(g.trackedIpCount(), 0); // exempt IPs are never tracked
    }

    // ── Purge drops idle, unbanned entries but keeps active bans ─────────────
    {
        CG g;
        g.allowConnection("203.0.113.50", t0);             // idle, not banned
        for (int i = 0; i <= CG::CONN_MAX_PER_WINDOW; ++i) // banned
            g.allowConnection("203.0.113.51", t0 + i);
        CHECK_EQ(g.trackedIpCount(), 2);
        g.purge(t0 + CG::IDLE_PURGE_MS + 1);
        CHECK_EQ(g.trackedIpCount(), 1); // idle one dropped, banned one kept
        CHECK(g.isBanned("203.0.113.51", t0 + CG::IDLE_PURGE_MS + 1));
    }

    // ── Behind a reverse proxy (trusted_proxies) ─────────────────────────────
    const ClientAddress::TrustedProxies proxies =
        ClientAddress::parseList({"203.0.113.200", "2001:db8:aaaa::/48"});
    // The proxy is exempt from both detectors once named — even on a public
    // address, where every client's connections and failures would add up.
    {
        CG g;
        g.setTrustedProxies(proxies);
        CHECK_EQ(g.trustedProxies().size(), 2);
        CHECK(g.isExemptHere("203.0.113.200"));
        CHECK(g.isExemptHere("::ffff:203.0.113.200"));
        CHECK(g.isExemptHere("2001:db8:aaaa::5"));
        CHECK(g.isExemptHere("10.0.0.5")); // the LAN, as before
        CHECK(!g.isExemptHere("203.0.113.201"));
        CHECK(!CG::isExempt("203.0.113.200")); // the static rule is unchanged

        bool refused = false;
        for (int i = 0; i <= CG::CONN_MAX_PER_WINDOW * 2; ++i)
            if (!g.allowConnection("203.0.113.200", t0 + i)) refused = true;
        for (int i = 0; i < CG::AUTHFAIL_MAX * 2; ++i)
            g.reportAuthFailure("203.0.113.200", t0 + i);
        CHECK(!refused);
        CHECK(!g.isBanned("203.0.113.200", t0 + 1));
        CHECK_EQ(g.trackedIpCount(), 0);
    }
    // Without the list the same public proxy would be banned, as today.
    {
        CG g;
        for (int i = 0; i < CG::AUTHFAIL_MAX; ++i)
            g.reportAuthFailure("203.0.113.200", t0 + i);
        CHECK(g.isBanned("203.0.113.200", t0 + CG::AUTHFAIL_MAX));
    }
    // Failures attributed to the forwarded client ban that client alone, and
    // it is refused per request while the proxy's socket stays welcome.
    {
        CG g;
        g.setTrustedProxies(proxies);
        const QString proxy = "203.0.113.200";
        const QString client = "198.51.100.23";
        const QString other = "198.51.100.24";
        for (int i = 0; i < CG::AUTHFAIL_MAX; ++i)
            g.reportAuthFailure(client, t0 + i);
        const qint64 later = t0 + CG::AUTHFAIL_MAX;
        CHECK(g.isBanned(client, later));
        CHECK(!g.isBanned(other, later));
        CHECK(!g.isBanned(proxy, later));
        CHECK(g.allowConnection(proxy, later)); // the proxy's socket
        CHECK(!g.allowRequest(client, proxy, later));
        CHECK(g.allowRequest(other, proxy, later));
        CHECK(g.banSecondsRemaining(client, later) > 0);
        // The ban runs out like any other.
        CHECK(g.allowRequest(client, proxy, t0 + CG::BAN_MS + CG::AUTHFAIL_MAX + 1));
    }
    // allowRequest leaves direct peers to accept(): a request from the banned
    // peer's own socket is not judged twice, and no client means no verdict.
    {
        CG g;
        for (int i = 0; i < CG::AUTHFAIL_MAX; ++i)
            g.reportAuthFailure("198.51.100.30", t0 + i);
        const qint64 later = t0 + CG::AUTHFAIL_MAX;
        CHECK(g.allowRequest("198.51.100.30", "198.51.100.30", later));
        CHECK(g.allowRequest("198.51.100.30", "::ffff:198.51.100.30", later));
        CHECK(g.allowRequest("", "203.0.113.200", later));
        CHECK(!g.allowConnection("198.51.100.30", later)); // accept() still refuses
    }
    // A forwarded client on a private address stays exempt, as on the LAN:
    // the proxy wrote that address, so that client really is on the LAN.
    {
        CG g;
        g.setTrustedProxies(proxies);
        for (int i = 0; i < CG::AUTHFAIL_MAX * 2; ++i)
            g.reportAuthFailure("192.168.1.77", t0 + i);
        CHECK(!g.isBanned("192.168.1.77", t0 + CG::AUTHFAIL_MAX * 2));
        CHECK(g.allowRequest("192.168.1.77", "203.0.113.200", t0 + CG::AUTHFAIL_MAX * 2));
        CHECK_EQ(g.trackedIpCount(), 0);
    }
    // One key per address: an IPv4-mapped accept() and a plain forwarded entry
    // count together, so a ban earned through the proxy also holds when the
    // same client connects directly.
    {
        CG g;
        for (int i = 0; i < CG::AUTHFAIL_MAX / 2; ++i)
            g.reportAuthFailure("::ffff:198.51.100.40", t0 + i);
        for (int i = 0; i < CG::AUTHFAIL_MAX / 2; ++i)
            g.reportAuthFailure("198.51.100.40", t0 + 100 + i);
        CHECK_EQ(g.trackedIpCount(), 1);
        CHECK(g.isBanned("198.51.100.40", t0 + 200));
        CHECK(!g.allowConnection("::ffff:198.51.100.40", t0 + 201));
        // IPv6 spelled two ways is one address too.
        g.reportAuthFailure("2001:DB8:0:0::9", t0);
        g.reportAuthFailure("2001:db8::9", t0 + 1);
        CHECK_EQ(g.trackedIpCount(), 2);
    }
    // An empty list changes nothing.
    {
        CG g;
        g.setTrustedProxies({});
        CHECK(!g.isExemptHere("203.0.113.200"));
        CHECK(g.isExemptHere("127.0.0.1"));
    }
}
