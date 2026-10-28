/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, describe, expect, it, vi } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import {
    CONNECT_WATCHDOG_MS,
    NO_USABLE_TRANSPORT,
    WEBRTC_UNAVAILABLE,
    clientCapabilities,
    connectWatchdogMs,
    isWebrtcRung,
    nextUsableRung,
    noUsableTransportKey,
    noUsableTransportReason,
    probeWebrtc,
    resetWebrtcProbe,
    webrtcAvailable,
} from '../js/api/WebRtcSupport.js';

/**
 * Firefox with media.peerconnection.enabled=false has no RTCPeerConnection at
 * all. The page asks once whether WebRTC is there, tells /start, and the chain
 * skips every webrtc-* rung: before, the first rung threw inside a swallowing
 * catch and the page waited forever.
 */

const LOCALES = join(dirname(fileURLToPath(import.meta.url)), '..', 'locales');
const CHAIN = ['webrtc-dc-udp', 'webrtc-dc-tcp', 'webrtc-media-udp', 'webrtc-media-tcp', 'wss'];

class GoodPc {
    static built = 0;
    static closed = 0;
    constructor() {
        GoodPc.built++;
    }
    close() {
        GoodPc.closed++;
    }
}

describe('probeWebrtc', () => {
    it('is true for a constructor that builds, and closes what it built', () => {
        GoodPc.built = GoodPc.closed = 0;
        expect(probeWebrtc(GoodPc)).toBe(true);
        expect(GoodPc.built).toBe(1);
        expect(GoodPc.closed).toBe(1);
    });

    it('is false where the browser has none', () => {
        expect(probeWebrtc(undefined)).toBe(false);
        expect(probeWebrtc(null)).toBe(false);
        expect(probeWebrtc({})).toBe(false);
        expect(probeWebrtc('RTCPeerConnection')).toBe(false);
    });

    it('is false where the name is kept but construction is forbidden', () => {
        class Forbidden {
            constructor() {
                throw new DOMException('blocked by policy', 'NotAllowedError');
            }
        }
        expect(probeWebrtc(Forbidden)).toBe(false);
    });

    it('is still true when close() itself throws', () => {
        class BadClose {
            close() {
                throw new Error('already closed');
            }
        }
        expect(probeWebrtc(BadClose)).toBe(true);
    });
});

describe('webrtcAvailable', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        resetWebrtcProbe();
    });

    it('reads the global, once per page', () => {
        GoodPc.built = 0;
        vi.stubGlobal('RTCPeerConnection', GoodPc);
        resetWebrtcProbe();
        expect(webrtcAvailable()).toBe(true);
        expect(webrtcAvailable()).toBe(true);
        expect(GoodPc.built).toBe(1);
        // The verdict holds until it is forgotten.
        vi.stubGlobal('RTCPeerConnection', undefined);
        expect(webrtcAvailable()).toBe(true);
        resetWebrtcProbe();
        expect(webrtcAvailable()).toBe(false);
    });

    it('is told to the backend as capabilities.webrtc', () => {
        vi.stubGlobal('RTCPeerConnection', undefined);
        resetWebrtcProbe();
        expect(clientCapabilities()).toEqual({ webrtc: false });
        vi.stubGlobal('RTCPeerConnection', GoodPc);
        resetWebrtcProbe();
        expect(clientCapabilities()).toEqual({ webrtc: true });
    });
});

describe('isWebrtcRung', () => {
    it('is every webrtc-* entry and nothing else', () => {
        for (const m of CHAIN.slice(0, 4)) expect(isWebrtcRung(m), m).toBe(true);
        expect(isWebrtcRung('wss')).toBe(false);
        expect(isWebrtcRung('')).toBe(false);
        expect(isWebrtcRung(undefined)).toBe(false);
        expect(isWebrtcRung(3)).toBe(false);
    });
});

describe('nextUsableRung', () => {
    it('is the next rung when everything can run', () => {
        expect(nextUsableRung(CHAIN, 1)).toBe(1);
        expect(nextUsableRung(CHAIN, 4)).toBe(4);
        expect(nextUsableRung(CHAIN, 5)).toBe(5);
        expect(nextUsableRung(CHAIN, -3)).toBe(0);
    });

    it('skips every webrtc-* rung without WebRTC', () => {
        expect(nextUsableRung(CHAIN, 0, { webrtc: false })).toBe(4);
        expect(nextUsableRung(['wss', 'webrtc-dc-udp'], 0, { webrtc: false })).toBe(0);
        expect(nextUsableRung(['wss', 'webrtc-dc-udp'], 1, { webrtc: false })).toBe(2);
    });

    it('is the end of the chain when nothing usable is left', () => {
        const noWss = CHAIN.slice(0, 4);
        expect(nextUsableRung(noWss, 0, { webrtc: false })).toBe(noWss.length);
        expect(nextUsableRung([], 0, { webrtc: false })).toBe(0);
    });

    it('jumps to a native-media rung after the decoder failed', () => {
        expect(nextUsableRung(CHAIN, 1, { mediaOnly: true })).toBe(2);
        expect(nextUsableRung(CHAIN, 4, { mediaOnly: true })).toBe(5);
        expect(nextUsableRung(['wss', 7, 'webrtc-media-tcp'], 0, { mediaOnly: true })).toBe(2);
    });

    it('leaves no native-media rung to a browser without WebRTC', () => {
        expect(nextUsableRung(CHAIN, 1, { mediaOnly: true, webrtc: false })).toBe(CHAIN.length);
    });
});

describe('the backend saying no transport is usable', () => {
    const err = (statusCode, responseBody) =>
        Object.assign(new Error('x'), { statusCode, responseBody });

    it('reads the reason of a 409 no_usable_transport', () => {
        for (const reason of ['webrtc_unavailable', 'policy', 'tunnel', 'udp_blocked'])
            expect(noUsableTransportReason(err(409, { error: NO_USABLE_TRANSPORT, reason }))).toBe(
                reason,
            );
        expect(noUsableTransportReason(err(409, { error: NO_USABLE_TRANSPORT }))).toBe('');
        expect(noUsableTransportReason(err(409, { error: NO_USABLE_TRANSPORT, reason: 4 }))).toBe(
            '',
        );
    });

    it('is nothing for any other answer', () => {
        expect(noUsableTransportReason(err(502, { error: NO_USABLE_TRANSPORT }))).toBeNull();
        expect(noUsableTransportReason(err(409, { error: 'reconnect_refused' }))).toBeNull();
        expect(noUsableTransportReason(err(409, null))).toBeNull();
        expect(noUsableTransportReason(new Error('server_timeout'))).toBeNull();
        expect(noUsableTransportReason(null)).toBeNull();
        expect(noUsableTransportReason(undefined)).toBeNull();
    });

    it('has a message for each reason, and one for a reason it does not know', () => {
        expect(noUsableTransportKey('webrtc_unavailable')).toBe(
            'transport.noUsable.webrtc_unavailable',
        );
        expect(noUsableTransportKey('policy')).toBe('transport.noUsable.policy');
        expect(noUsableTransportKey('tunnel')).toBe('transport.noUsable.tunnel');
        expect(noUsableTransportKey('udp_blocked')).toBe('transport.noUsable.udp_blocked');
        for (const r of ['', 'later', null, undefined, 'constructor', 'toString'])
            expect(noUsableTransportKey(r), String(r)).toBe('transport.noUsable.generic');
    });

    it('says every one of them in English, French and Chinese', () => {
        for (const locale of ['en', 'fr', 'zh']) {
            const cat = JSON.parse(readFileSync(join(LOCALES, locale + '.json'), 'utf8'));
            for (const r of ['webrtc_unavailable', 'policy', 'tunnel', 'udp_blocked', 'x']) {
                const key = noUsableTransportKey(r).split('.');
                const text = key.reduce((o, k) => o && o[k], cat);
                expect(typeof text, locale + ' ' + r).toBe('string');
                expect(text.length).toBeGreaterThan(20);
            }
            expect(cat.transport.webrtcOffNext).toContain('{{to}}');
        }
    });
});

describe('connect watchdog deadlines', () => {
    it('sits above what each transport waits for on its own', () => {
        // ICE clamp (30 s, WebRtcDataChannel ice-config) + DataChannel grace (10 s).
        expect(CONNECT_WATCHDOG_MS.webrtc).toBeGreaterThan(30000 + 10000);
        // One upgrade, retried once a second later.
        expect(CONNECT_WATCHDOG_MS.wss).toBeGreaterThan(2 * 1000);
        expect(Object.isFrozen(CONNECT_WATCHDOG_MS)).toBe(true);
    });

    it('picks the deadline of the rung', () => {
        expect(connectWatchdogMs('wss')).toBe(CONNECT_WATCHDOG_MS.wss);
        for (const m of CHAIN.slice(0, 4))
            expect(connectWatchdogMs(m)).toBe(CONNECT_WATCHDOG_MS.webrtc);
        expect(connectWatchdogMs(undefined)).toBe(CONNECT_WATCHDOG_MS.webrtc);
    });

    it('names the reason the chain reads as "skip WebRTC"', () => {
        expect(WEBRTC_UNAVAILABLE).toBe('webrtc_unavailable');
    });
});
