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

#include "StreamConfig.h"
#include "TransportPriorities.h"

#include <Limelight.h>

#include <QString>
#include <QStringList>

/// The ordered transport fallback chain of one stream start: the owner's /start
/// and a player's join both build it here, from what the caller read off the
/// request and the settings. Pure, so every combination is tested
/// (tests/test_transport_chain.cpp); main.cpp only logs and answers.
namespace transportchain {

/// Everything the chain depends on.
struct Inputs
{
    QString adminMode = QStringLiteral("auto"); ///< AppSettings::transportMode()
    QString bodyMode;                           ///< the request's transport_mode; empty when absent
    bool viaTunnel = false;                     ///< the browser came through the rendezvous
    bool dropUdp = false;                       ///< udpRungsDropped()'s verdict
    bool videoEnhancement = false;              ///< canvas rungs before MediaTrack
    bool mediaTrackAllowed = true;              ///< mediaTrackAllowed()'s verdict for the codec
};

struct Chain
{
    QStringList rungs;
    QString mode;            ///< "auto", or the transport asked for by name
    QString dropped;         ///< asked for by name and impossible here ("wss" via the rendezvous)
    bool udpDropped = false; ///< the UDP rungs were skipped
};

/// Does a host advertising @p support (SCM_* bits) carry codec @p c?
/// The canonical SCM_MASK_* values: Sunshine reports HEVC at 0x100, AV1 at 0x10000.
inline bool hostSupportsCodec(int support, VideoCodec c)
{
    switch (c) {
    case VideoCodec::H264: return (support & SCM_MASK_H264) != 0;
    case VideoCodec::HEVC: return (support & SCM_MASK_HEVC) != 0;
    case VideoCodec::AV1: return (support & SCM_MASK_AV1) != 0;
    default: return true;
    }
}

/// The codec the host will actually be asked for: Auto is HEVC when the host
/// has it, and a codec the host lacks falls back to H.264. Auto when the host
/// advertises no codec at all (nothing is filtered then; the launch fails).
inline VideoCodec effectiveCodec(VideoCodec requested, int support)
{
    VideoCodec effective = requested;
    if (effective == VideoCodec::Auto)
        effective =
            hostSupportsCodec(support, VideoCodec::HEVC) ? VideoCodec::HEVC : VideoCodec::H264;
    if (hostSupportsCodec(support, effective)) return effective;
    return hostSupportsCodec(support, VideoCodec::H264) ? VideoCodec::H264 : VideoCodec::Auto;
}

/// MediaTrack rungs carry H.264 only (<video> codec support varies across
/// browsers; the DataChannel's WebCodecs is more reliable for HEVC and AV1).
inline bool mediaTrackAllowed(VideoCodec requested, int support)
{
    const VideoCodec e = effectiveCodec(requested, support);
    return e != VideoCodec::HEVC && e != VideoCodec::AV1;
}

/// A browser whose STUN probe got no answer sits on a network that lets no UDP
/// out, and each UDP rung would run to its ICE deadline. Its word only ever
/// shortens a chain planned for the internet (a public browser, or one through
/// the rendezvous): on a LAN, STUN can fail while the host is one UDP hop away.
/// The bench switch debug_ice_drop_udp drops them for every browser.
inline bool udpRungsDropped(bool debugDrop, bool udpBlocked, bool viaTunnel, bool publicClient)
{
    if (debugDrop) return true;
    return udpBlocked && (viaTunnel || publicClient);
}

/// The priority order for this stream, without the MediaTrack rungs when the
/// codec is not H.264.
inline QStringList autoOrder(bool videoEnhancement, bool mediaTrack)
{
    QStringList order;
    for (const QString& t : TransportPriorities::orderedTransports(videoEnhancement))
        if (mediaTrack || !t.startsWith(QStringLiteral("webrtc-media"))) order.append(t);
    return order;
}

inline Chain build(const Inputs& in)
{
    Chain out;

    // A per-request transport_mode overrides the admin setting; "all" is the
    // auto chain (it only makes the admin selector offer wss as well).
    QString mode = in.adminMode.isEmpty() ? QStringLiteral("auto") : in.adminMode;
    if (!in.bodyMode.isEmpty()) mode = in.bodyMode;
    if (mode == QStringLiteral("all")) mode = QStringLiteral("auto");
    out.mode = mode;

    // auto → the priority order. A forced X → X first; a "-udp" X promotes its
    // "-tcp" sibling to second (same family before switching), then the rest.
    const QStringList order = autoOrder(in.videoEnhancement, in.mediaTrackAllowed);
    if (mode == QStringLiteral("auto")) {
        out.rungs = order;
    } else {
        out.rungs.append(mode);
        QString sibling;
        if (mode.endsWith(QStringLiteral("-udp")))
            sibling = mode.left(mode.length() - 4) + QStringLiteral("-tcp");
        if (!sibling.isEmpty() && order.contains(sibling)) out.rungs.append(sibling);
        for (const QString& m : order)
            if (m != mode && m != sibling) out.rungs.append(m);
    }

    // wss carries video over the TLS connection that served the page; through
    // the rendezvous there is none, so it leaves the chain even when named.
    if (in.viaTunnel && out.rungs.contains(QStringLiteral("wss"))) {
        out.rungs.removeAll(QStringLiteral("wss"));
        if (mode == QStringLiteral("wss")) out.dropped = QStringLiteral("wss");
    }

    // Blocked UDP: skip the UDP rungs, never down to nothing.
    if (in.dropUdp) {
        QStringList kept;
        for (const QString& m : out.rungs)
            if (!m.endsWith(QStringLiteral("-udp"))) kept.append(m);
        if (!kept.isEmpty() && kept.size() != out.rungs.size()) {
            out.rungs = kept;
            out.udpDropped = true;
        }
    }
    return out;
}

} // namespace transportchain
