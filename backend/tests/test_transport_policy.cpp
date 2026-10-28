/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The admin's allowed transports and the browser's preference
 * (streaming/TransportPolicy.h): the names each accepts, which rungs each lets
 * run, and what the admin selector is offered under each policy.
 */
#include "test_framework.h"
#include "streaming/TransportPolicy.h"
#include "streaming/TransportPriorities.h"

using namespace transportpolicy;

namespace {

const QString DCU = QStringLiteral("webrtc-dc-udp");
const QString DCT = QStringLiteral("webrtc-dc-tcp");
const QString MU = QStringLiteral("webrtc-media-udp");
const QString MT = QStringLiteral("webrtc-media-tcp");
const QString WSS = QStringLiteral("wss");

void testNames()
{
    SECTION("TransportPolicy: names");
    CHECK_EQ(normalizePolicy(QStringLiteral("all")), QStringLiteral("all"));
    CHECK_EQ(normalizePolicy(QStringLiteral("webrtc")), QStringLiteral("webrtc"));
    CHECK_EQ(normalizePolicy(QStringLiteral("wss")), QStringLiteral("wss"));
    CHECK_EQ(normalizePolicy(QStringLiteral(" WebRTC ")), QStringLiteral("webrtc"));
    CHECK_EQ(normalizePolicy(QStringLiteral("WSS")), QStringLiteral("wss"));
    // Anything else is refused, not guessed.
    CHECK(normalizePolicy(QString()).isEmpty());
    CHECK(normalizePolicy(QStringLiteral("auto")).isEmpty());
    CHECK(normalizePolicy(QStringLiteral("websocket")).isEmpty());
    CHECK(normalizePolicy(QStringLiteral("webrtc-dc-udp")).isEmpty());
    CHECK(normalizePolicy(QStringLiteral("all,wss")).isEmpty());

    CHECK_EQ(normalizePref(QStringLiteral("auto")), QStringLiteral("auto"));
    CHECK_EQ(normalizePref(QStringLiteral("webrtc")), QStringLiteral("webrtc"));
    CHECK_EQ(normalizePref(QStringLiteral("wss")), QStringLiteral("wss"));
    CHECK_EQ(normalizePref(QStringLiteral(" WSS")), QStringLiteral("wss"));
    // A preference is a wish: what is not one is no preference.
    CHECK_EQ(normalizePref(QString()), QStringLiteral("auto"));
    CHECK_EQ(normalizePref(QStringLiteral("all")), QStringLiteral("auto"));
    CHECK_EQ(normalizePref(QStringLiteral("webrtc-dc-udp")), QStringLiteral("auto"));
}

void testFamilies()
{
    SECTION("TransportPolicy: families");
    for (const QString& r : {DCU, DCT, MU, MT, WSS}) {
        CHECK(familyAllows(QStringLiteral("all"), r));
        CHECK(familyAllows(QStringLiteral("auto"), r));
        CHECK_EQ(familyAllows(QStringLiteral("webrtc"), r), r != WSS);
        CHECK_EQ(familyAllows(QStringLiteral("wss"), r), r == WSS);
    }
    // A rung name that only looks like one is not let through by "webrtc".
    CHECK(!familyAllows(QStringLiteral("webrtc"), QStringLiteral("webrtcx")));
    CHECK(!familyAllows(QStringLiteral("wss"), QStringLiteral("wss2")));
}

void testAvailableTransports()
{
    SECTION("TransportPolicy: the admin selector's transports");
    const QStringList order = TransportPriorities::orderedTransports();
    const QString all = QStringLiteral("all");
    const QString auto_ = QStringLiteral("auto");

    // "all": as before — release builds hide wss unless it, or "all", is saved.
    CHECK_EQ(availableTransports(order, all, auto_, true), (QStringList{DCU, DCT, MU, MT, WSS}));
    CHECK_EQ(availableTransports(order, all, auto_, false), (QStringList{DCU, DCT, MU, MT}));
    CHECK_EQ(availableTransports(order, all, DCU, false), (QStringList{DCU, DCT, MU, MT}));
    CHECK_EQ(availableTransports(order, all, all, false), (QStringList{DCU, DCT, MU, MT, WSS}));
    CHECK_EQ(availableTransports(order, all, WSS, false), (QStringList{DCU, DCT, MU, MT, WSS}));

    // "webrtc": never wss, debug build or not.
    for (bool debug : {true, false})
        for (const QString& saved : {auto_, all, WSS, DCT})
            CHECK_EQ(availableTransports(order, QStringLiteral("webrtc"), saved, debug),
                     (QStringList{DCU, DCT, MU, MT}));

    // "wss": only wss, and always: the admin chose it.
    for (bool debug : {true, false})
        for (const QString& saved : {auto_, all, WSS, DCT})
            CHECK_EQ(availableTransports(order, QStringLiteral("wss"), saved, debug),
                     (QStringList{WSS}));
}

} // namespace

void run_transport_policy_tests()
{
    testNames();
    testFamilies();
    testAvailableTransports();
}
