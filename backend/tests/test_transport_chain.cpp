/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The transport fallback chain of a stream start (streaming/TransportChain.h):
 * the owner's /start and a player's join walk it rung by rung, so a rung in the
 * wrong place costs a relaunch and a rung that cannot run costs an ICE deadline.
 * Explicit rows for the cases that matter, then invariants over every
 * combination of the inputs.
 */
#include "test_framework.h"
#include "streaming/TransportChain.h"

#include <QSet>

using transportchain::Chain;
using transportchain::Inputs;

namespace {

const QString DCU = QStringLiteral("webrtc-dc-udp");
const QString DCT = QStringLiteral("webrtc-dc-tcp");
const QString MU = QStringLiteral("webrtc-media-udp");
const QString MT = QStringLiteral("webrtc-media-tcp");
const QString WSS = QStringLiteral("wss");

Inputs make(const QString& admin, const QString& body = QString())
{
    Inputs in;
    in.adminMode = admin;
    in.bodyMode = body;
    return in;
}

void testCodecHelpers()
{
    SECTION("TransportChain: codec helpers");
    using transportchain::effectiveCodec;
    using transportchain::hostSupportsCodec;
    using transportchain::mediaTrackAllowed;

    const int h264 = SCM_H264;
    const int hevc = SCM_HEVC;
    const int av1 = SCM_AV1_MAIN8;
    const int all = h264 | hevc | av1;

    CHECK(hostSupportsCodec(h264, VideoCodec::H264));
    CHECK(!hostSupportsCodec(h264, VideoCodec::HEVC));
    CHECK(hostSupportsCodec(hevc, VideoCodec::HEVC));
    CHECK(hostSupportsCodec(SCM_HEVC_MAIN10, VideoCodec::HEVC));
    CHECK(hostSupportsCodec(av1, VideoCodec::AV1));
    CHECK(!hostSupportsCodec(h264 | hevc, VideoCodec::AV1));
    CHECK(hostSupportsCodec(0, VideoCodec::Auto)); // nothing to check for Auto

    // Auto → HEVC when the host has it, else H.264.
    CHECK(effectiveCodec(VideoCodec::Auto, all) == VideoCodec::HEVC);
    CHECK(effectiveCodec(VideoCodec::Auto, h264) == VideoCodec::H264);
    // A codec the host lacks falls back to H.264.
    CHECK(effectiveCodec(VideoCodec::AV1, h264 | hevc) == VideoCodec::H264);
    CHECK(effectiveCodec(VideoCodec::HEVC, h264) == VideoCodec::H264);
    CHECK(effectiveCodec(VideoCodec::AV1, all) == VideoCodec::AV1);
    // A host with no codec at all: nothing to settle on.
    CHECK(effectiveCodec(VideoCodec::HEVC, 0) == VideoCodec::Auto);
    CHECK(effectiveCodec(VideoCodec::HEVC, av1) == VideoCodec::Auto);

    // MediaTrack carries H.264 only.
    CHECK(mediaTrackAllowed(VideoCodec::H264, all));
    CHECK(!mediaTrackAllowed(VideoCodec::Auto, all));
    CHECK(mediaTrackAllowed(VideoCodec::Auto, h264));
    CHECK(!mediaTrackAllowed(VideoCodec::HEVC, all));
    CHECK(!mediaTrackAllowed(VideoCodec::AV1, all));
    CHECK(mediaTrackAllowed(VideoCodec::AV1, h264));
    // No codec at all: nothing is filtered, the launch fails on its own.
    CHECK(mediaTrackAllowed(VideoCodec::HEVC, 0));
}

void testUdpVerdict()
{
    SECTION("TransportChain: blocked-UDP verdict");
    using transportchain::udpRungsDropped;
    // The bench switch drops them for everyone.
    CHECK(udpRungsDropped(true, false, false, false));
    // The browser's word counts for an internet-planned browser only.
    CHECK(udpRungsDropped(false, true, false, true));
    CHECK(udpRungsDropped(false, true, true, false));
    CHECK(!udpRungsDropped(false, true, false, false)); // LAN: STUN may fail for want of internet
    CHECK(!udpRungsDropped(false, false, true, true));
    CHECK(!udpRungsDropped(false, false, false, false));
}

void testAutoOrder()
{
    SECTION("TransportChain: auto order");
    using transportchain::autoOrder;
    CHECK_EQ(autoOrder(false, true), (QStringList{DCU, DCT, MU, MT, WSS}));
    CHECK_EQ(autoOrder(true, true), (QStringList{DCU, DCT, WSS, MU, MT}));
    CHECK_EQ(autoOrder(false, false), (QStringList{DCU, DCT, WSS}));
    CHECK_EQ(autoOrder(true, false), (QStringList{DCU, DCT, WSS}));
}

void testExplicitRows()
{
    SECTION("TransportChain: explicit rows (owner)");
    using transportchain::build;

    // auto and all are the same chain.
    Chain c = build(make(QStringLiteral("auto")));
    CHECK_EQ(c.rungs, (QStringList{DCU, DCT, MU, MT, WSS}));
    CHECK_EQ(c.mode, QStringLiteral("auto"));
    CHECK(c.dropped.isEmpty());
    CHECK(!c.udpDropped);
    CHECK_EQ(build(make(QStringLiteral("all"))).rungs, c.rungs);
    CHECK_EQ(build(make(QStringLiteral("all"))).mode, QStringLiteral("auto"));
    CHECK_EQ(build(make(QString())).rungs, c.rungs); // nothing saved reads as auto

    // A forced "-udp" mode promotes its "-tcp" sibling to second.
    CHECK_EQ(build(make(MU)).rungs, (QStringList{MU, MT, DCU, DCT, WSS}));
    CHECK_EQ(build(make(DCU)).rungs, (QStringList{DCU, DCT, MU, MT, WSS}));
    // A forced "-tcp" mode is first, the rest in priority order.
    CHECK_EQ(build(make(MT)).rungs, (QStringList{MT, DCU, DCT, MU, WSS}));
    CHECK_EQ(build(make(DCT)).rungs, (QStringList{DCT, DCU, MU, MT, WSS}));
    // wss forced: only put first, every WebRTC rung still follows it.
    CHECK_EQ(build(make(WSS)).rungs, (QStringList{WSS, DCU, DCT, MU, MT}));
    CHECK_EQ(build(make(WSS)).mode, WSS);

    // The request's transport_mode overrides the admin setting, "all" included.
    CHECK_EQ(build(make(WSS, MU)).rungs, (QStringList{MU, MT, DCU, DCT, WSS}));
    CHECK_EQ(build(make(WSS, QStringLiteral("all"))).rungs, (QStringList{DCU, DCT, MU, MT, WSS}));
    CHECK_EQ(build(make(MU, QStringLiteral("auto"))).mode, QStringLiteral("auto"));

    // HEVC/AV1: no MediaTrack in the order, but a MediaTrack mode asked for by
    // name is still put first (its sibling is not, it is not in the order).
    Inputs hevc = make(MU);
    hevc.mediaTrackAllowed = false;
    CHECK_EQ(build(hevc).rungs, (QStringList{MU, DCU, DCT, WSS}));
    hevc.adminMode = QStringLiteral("auto");
    CHECK_EQ(build(hevc).rungs, (QStringList{DCU, DCT, WSS}));

    // Enhancement: canvas rungs first, MediaTrack last.
    Inputs enh = make(QStringLiteral("auto"));
    enh.videoEnhancement = true;
    CHECK_EQ(build(enh).rungs, (QStringList{DCU, DCT, WSS, MU, MT}));
    enh.adminMode = MU;
    CHECK_EQ(build(enh).rungs, (QStringList{MU, MT, DCU, DCT, WSS}));

    // Through the rendezvous wss leaves the chain; asked for by name, it is
    // reported as dropped.
    Inputs tun = make(QStringLiteral("auto"));
    tun.viaTunnel = true;
    c = build(tun);
    CHECK_EQ(c.rungs, (QStringList{DCU, DCT, MU, MT}));
    CHECK(c.dropped.isEmpty());
    tun.adminMode = WSS;
    c = build(tun);
    CHECK_EQ(c.rungs, (QStringList{DCU, DCT, MU, MT}));
    CHECK_EQ(c.dropped, WSS);

    // Blocked UDP: UDP rungs skipped.
    Inputs udp = make(QStringLiteral("auto"));
    udp.dropUdp = true;
    c = build(udp);
    CHECK_EQ(c.rungs, (QStringList{DCT, MT, WSS}));
    CHECK(c.udpDropped);
    // A forced UDP mode goes too: its TCP sibling leads.
    udp.adminMode = MU;
    CHECK_EQ(build(udp).rungs, (QStringList{MT, DCT, WSS}));
    // Tunnel + blocked UDP: TCP WebRTC only.
    udp.adminMode = QStringLiteral("auto");
    udp.viaTunnel = true;
    CHECK_EQ(build(udp).rungs, (QStringList{DCT, MT}));
    // Enhancement + HEVC + tunnel + UDP: the shortest chain there is.
    udp.videoEnhancement = true;
    udp.mediaTrackAllowed = false;
    CHECK_EQ(build(udp).rungs, (QStringList{DCT}));
}

void testPlayerRows()
{
    SECTION("TransportChain: explicit rows (player)");
    using transportchain::build;
    // A player: always the enhancement order, never a forced mode.
    Inputs p;
    p.videoEnhancement = true;
    p.mediaTrackAllowed = false; // Auto on an HEVC host
    CHECK_EQ(build(p).rungs, (QStringList{DCU, DCT, WSS}));
    p.mediaTrackAllowed = true; // the guest's H.264 fallback
    CHECK_EQ(build(p).rungs, (QStringList{DCU, DCT, WSS, MU, MT}));
    p.viaTunnel = true;
    CHECK_EQ(build(p).rungs, (QStringList{DCU, DCT, MU, MT}));
    p.dropUdp = true;
    CHECK_EQ(build(p).rungs, (QStringList{DCT, MT}));
}

// Every combination: modes × body modes × the four booleans.
void testInvariants()
{
    SECTION("TransportChain: invariants over every combination");
    using transportchain::build;
    const QStringList modes{
        QString(), QStringLiteral("auto"), QStringLiteral("all"), DCU, DCT, MU, MT, WSS};
    int combos = 0;
    for (const QString& admin : modes)
        for (const QString& body : modes)
            for (int bits = 0; bits < 16; ++bits) {
                Inputs in = make(admin, body);
                in.viaTunnel = bits & 1;
                in.dropUdp = bits & 2;
                in.videoEnhancement = bits & 4;
                in.mediaTrackAllowed = bits & 8;
                const Chain c = build(in);
                ++combos;

                // Never empty, never a duplicate.
                CHECK(!c.rungs.isEmpty());
                CHECK_EQ(QSet<QString>(c.rungs.begin(), c.rungs.end()).size(),
                         static_cast<int>(c.rungs.size()));
                // No wss through the rendezvous.
                if (in.viaTunnel) CHECK(!c.rungs.contains(WSS));
                // Blocked UDP leaves no UDP rung (a TCP one always remains).
                if (in.dropUdp) {
                    for (const QString& r : c.rungs)
                        CHECK(!r.endsWith(QStringLiteral("-udp")));
                    CHECK(c.udpDropped);
                } else {
                    CHECK(!c.udpDropped);
                }
                // The effective mode: body over admin, "all"/empty read as auto.
                QString expected = body.isEmpty() ? (admin.isEmpty() ? "auto" : admin) : body;
                if (expected == QStringLiteral("all")) expected = QStringLiteral("auto");
                CHECK_EQ(c.mode, expected);
                // A forced mode that can run here leads the chain.
                const bool forcedRuns = expected != QStringLiteral("auto") &&
                                        !(in.viaTunnel && expected == WSS) &&
                                        !(in.dropUdp && expected.endsWith(QStringLiteral("-udp")));
                if (forcedRuns) CHECK_EQ(c.rungs.first(), expected);
                CHECK_EQ(c.dropped.isEmpty(), !(in.viaTunnel && expected == WSS));
                // MediaTrack only as the forced mode when the codec is not H.264.
                if (!in.mediaTrackAllowed)
                    for (const QString& r : c.rungs)
                        if (r.startsWith(QStringLiteral("webrtc-media"))) CHECK_EQ(r, expected);
                // DataChannel TCP is always there: the rung that works anywhere.
                CHECK(c.rungs.contains(DCT));
            }
    CHECK_EQ(combos, 8 * 8 * 16);
}

} // namespace

void run_transport_chain_tests()
{
    testCodecHelpers();
    testUdpVerdict();
    testAutoOrder();
    testExplicitRows();
    testPlayerRows();
    testInvariants();
}
