/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The browser's Connection choice (util/TransportChoice.js): offered only
 * inside the admin's allowed transports and what this page can run, and sent
 * at launch only when it can run here.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';

const tunnel = vi.hoisted(() => ({ through: false }));
vi.mock('../js/net/tunnelBridge.js', () => ({
    pageCameThroughTunnel: () => tunnel.through,
}));

import {
    ALLOWED_TRANSPORTS,
    TRANSPORT_PREFS,
    browserHasWebrtc,
    clampTransportPref,
    launchTransportPref,
    normalizeAllowed,
    normalizePref,
    transportChoices,
} from '../js/util/TransportChoice.js';

describe('TransportChoice: names', () => {
    it('knows the policies and preferences the backend knows', () => {
        expect(ALLOWED_TRANSPORTS).toEqual(['all', 'webrtc', 'wss']);
        expect(TRANSPORT_PREFS).toEqual(['auto', 'webrtc', 'wss']);
    });

    it('reads anything else as "all" and "auto"', () => {
        for (const v of ALLOWED_TRANSPORTS) expect(normalizeAllowed(v)).toBe(v);
        for (const v of [undefined, null, '', 'WSS', 'auto', 42])
            expect(normalizeAllowed(v)).toBe('all');
        for (const v of TRANSPORT_PREFS) expect(normalizePref(v)).toBe(v);
        for (const v of [undefined, null, '', 'all', 'webrtc-dc-udp', {}])
            expect(normalizePref(v)).toBe('auto');
    });
});

describe('TransportChoice: is there WebRTC here', () => {
    const saved = globalThis.RTCPeerConnection;
    afterEach(() => {
        globalThis.RTCPeerConnection = saved;
    });

    it('is a constructor named RTCPeerConnection', () => {
        globalThis.RTCPeerConnection = function RTCPeerConnection() {};
        expect(browserHasWebrtc()).toBe(true);
        globalThis.RTCPeerConnection = undefined;
        expect(browserHasWebrtc()).toBe(false);
        // Firefox with media.peerconnection.enabled=false leaves no constructor.
        globalThis.RTCPeerConnection = {};
        expect(browserHasWebrtc()).toBe(false);
    });
});

describe('TransportChoice: what the menu offers', () => {
    const saved = globalThis.RTCPeerConnection;
    afterEach(() => {
        tunnel.through = false;
        globalThis.RTCPeerConnection = saved;
    });

    it('offers both families under "all", on a page that can run both', () => {
        expect(transportChoices({ allowed: 'all', webrtc: true, tunnelled: false })).toEqual([
            'auto',
            'webrtc',
            'wss',
        ]);
    });

    it('offers nothing to choose when only one family is left', () => {
        // The admin allows one family: Automatic is that family.
        expect(transportChoices({ allowed: 'webrtc', webrtc: true, tunnelled: false })).toEqual([
            'auto',
        ]);
        expect(transportChoices({ allowed: 'wss', webrtc: true, tunnelled: false })).toEqual([
            'auto',
        ]);
        // No WebRTC in this browser.
        expect(transportChoices({ allowed: 'all', webrtc: false, tunnelled: false })).toEqual([
            'auto',
        ]);
        // Through the rendezvous: no WebSocket.
        expect(transportChoices({ allowed: 'all', webrtc: true, tunnelled: true })).toEqual([
            'auto',
        ]);
        // Nothing at all: still Automatic (the launch then explains).
        expect(transportChoices({ allowed: 'wss', webrtc: true, tunnelled: true })).toEqual([
            'auto',
        ]);
        expect(transportChoices({ allowed: 'webrtc', webrtc: false, tunnelled: false })).toEqual([
            'auto',
        ]);
    });

    it('reads an unknown policy as "all"', () => {
        expect(transportChoices({ allowed: 'bogus', webrtc: true, tunnelled: false })).toEqual([
            'auto',
            'webrtc',
            'wss',
        ]);
    });

    it('asks the page itself when not told', () => {
        globalThis.RTCPeerConnection = function RTCPeerConnection() {};
        tunnel.through = false;
        expect(transportChoices()).toEqual(['auto', 'webrtc', 'wss']);
        tunnel.through = true;
        expect(transportChoices()).toEqual(['auto']);
        expect(transportChoices({ allowed: 'all' })).toEqual(['auto']);
    });

    it('keeps a preference on offer and turns any other into Automatic', () => {
        const all = ['auto', 'webrtc', 'wss'];
        expect(clampTransportPref('wss', all)).toBe('wss');
        expect(clampTransportPref('webrtc', all)).toBe('webrtc');
        expect(clampTransportPref('wss', ['auto'])).toBe('auto');
        expect(clampTransportPref(undefined, all)).toBe('auto');
        expect(clampTransportPref('nonsense', all)).toBe('auto');
    });
});

describe('TransportChoice: what a launch sends', () => {
    const saved = globalThis.RTCPeerConnection;
    afterEach(() => {
        tunnel.through = false;
        globalThis.RTCPeerConnection = saved;
    });

    it('sends a preference this page can run', () => {
        expect(launchTransportPref('wss', { webrtc: true, tunnelled: false })).toBe('wss');
        expect(launchTransportPref('webrtc', { webrtc: true, tunnelled: true })).toBe('webrtc');
        expect(launchTransportPref('auto', { webrtc: false, tunnelled: true })).toBe('auto');
    });

    it('sends Automatic for one it cannot', () => {
        expect(launchTransportPref('webrtc', { webrtc: false, tunnelled: false })).toBe('auto');
        expect(launchTransportPref('wss', { webrtc: true, tunnelled: true })).toBe('auto');
        expect(launchTransportPref('garbage', { webrtc: true, tunnelled: false })).toBe('auto');
    });

    it('asks the page itself when not told', () => {
        globalThis.RTCPeerConnection = undefined;
        expect(launchTransportPref('webrtc')).toBe('auto');
        tunnel.through = true;
        expect(launchTransportPref('wss')).toBe('auto');
    });
});
