/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WebRtcDataChannel } from '../js/api/WebRtcDataChannel.js';
import { WebRtcMedia } from '../js/api/WebRtcMedia.js';
import { StreamView } from '../js/ui/StreamView.js';
import { WEBRTC_UNAVAILABLE, resetWebrtcProbe, webrtcAvailable } from '../js/api/WebRtcSupport.js';

/**
 * Firefox with media.peerconnection.enabled=false: no RTCPeerConnection. The
 * first rung threw a TypeError inside the signalling handler's try/catch,
 * which swallowed it; the offer's own retry threw outside its try (an
 * unhandled rejection); nothing called onError and no timer was armed. Input
 * then went out on the signalling socket, and the host answered "Unknown
 * message type" to every mouse move, for as long as the page stayed open.
 */

/** A WebSocket that records what is sent and never connects by itself. */
class FakeWs {
    static CONNECTING = 0;
    static OPEN = 1;
    static CLOSING = 2;
    static CLOSED = 3;
    static made = [];
    constructor(url) {
        this.url = url;
        this.readyState = FakeWs.CONNECTING;
        this.sent = [];
        FakeWs.made.push(this);
    }
    send(data) {
        this.sent.push(data);
    }
    close() {
        this.readyState = FakeWs.CLOSED;
    }
}

class GoodPc {
    close() {}
}

class ForbiddenPc {
    constructor() {
        throw new DOMException('blocked by policy', 'NotAllowedError');
    }
}

const OFFER = { type: 'sdp', sdp: 'v=0\r\na=fingerprint:sha-256 AA:BB\r\n' };

/** The browser said it had WebRTC at page load; the constructor is @p PC now. */
function webrtcThen(PC) {
    vi.stubGlobal('RTCPeerConnection', GoodPc);
    resetWebrtcProbe();
    expect(webrtcAvailable()).toBe(true);
    vi.stubGlobal('RTCPeerConnection', PC);
}

function noWebrtc() {
    vi.stubGlobal('RTCPeerConnection', undefined);
    resetWebrtcProbe();
}

let unhandled;
const onUnhandled = (reason) => unhandled.push(reason);

beforeEach(() => {
    FakeWs.made = [];
    unhandled = [];
    process.on('unhandledRejection', onUnhandled);
    vi.stubGlobal('WebSocket', FakeWs);
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    vi.spyOn(console, 'error').mockImplementation(() => {});
});

afterEach(() => {
    process.off('unhandledRejection', onUnhandled);
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
    vi.useRealTimers();
    resetWebrtcProbe();
});

describe.each([
    ['WebRtcDataChannel', () => new WebRtcDataChannel('ws://test.invalid/ws')],
    ['WebRtcMedia', () => new WebRtcMedia('ws://test.invalid/ws')],
])('%s without WebRTC', (_name, make) => {
    it('ends the rung at connect, and never opens the signalling socket', () => {
        vi.useFakeTimers();
        noWebrtc();
        const t = make();
        const onError = vi.fn();
        const onClose = vi.fn();
        t.connect();
        // Said once the caller had a chance to listen: connect() runs from
        // StreamView's constructor, before the app wires its callbacks.
        t.onError = onError;
        t.onClose = onClose;
        expect(onError).not.toHaveBeenCalled();
        vi.advanceTimersByTime(1);
        expect(onError).toHaveBeenCalledTimes(1);
        expect(onError.mock.calls[0][0].message).toBe(WEBRTC_UNAVAILABLE);
        expect(onClose).toHaveBeenCalledTimes(1);
        expect(FakeWs.made).toHaveLength(0);
        expect(t._stopping).toBe(true);
    });

    it('says nothing once closed before the deferred report', () => {
        vi.useFakeTimers();
        noWebrtc();
        const t = make();
        const onError = vi.fn();
        t.onError = onError;
        t.connect();
        t.close();
        vi.advanceTimersByTime(10);
        expect(onError).not.toHaveBeenCalled();
    });

    it('reports an offer it cannot answer, once, with nothing left unhandled', async () => {
        vi.stubGlobal('RTCPeerConnection', undefined);
        const t = make();
        const onError = vi.fn();
        t.onError = onError;
        await t._handleSdpOffer(OFFER);
        await new Promise((r) => setTimeout(r, 0));
        expect(onError).toHaveBeenCalledTimes(1);
        expect(onError.mock.calls[0][0].message).toBe(WEBRTC_UNAVAILABLE);
        expect(t.pc).toBeNull();
        expect(unhandled).toEqual([]);
    });

    it('reports a constructor that refuses', async () => {
        webrtcThen(ForbiddenPc);
        const t = make();
        const onError = vi.fn();
        t.onError = onError;
        expect(t._createPeerConnection()).toBe(false);
        expect(onError).toHaveBeenCalledTimes(1);
        expect(onError.mock.calls[0][0].message).toBe(WEBRTC_UNAVAILABLE);
        expect(t.pc).toBeNull();
    });

    it('does not answer an offer once closed', async () => {
        const t = make();
        const pcSpy = vi.spyOn(t, '_createPeerConnection');
        t.close();
        await t._handleSdpOffer(OFFER);
        expect(pcSpy).not.toHaveBeenCalled();
    });
});

describe('WebRtcDataChannel: the peer connection made on ice-config', () => {
    it('reports a constructor that refuses, through onError', () => {
        webrtcThen(ForbiddenPc);
        const t = new WebRtcDataChannel('ws://test.invalid/ws');
        const onError = vi.fn();
        t.onError = onError;
        t.connect();
        const onmessage = FakeWs.made[0].onmessage;
        // Through the socket, as the host sends it: the swallowing catch used
        // to be here.
        onmessage({ data: JSON.stringify({ type: 'ice-config', iceServers: [] }) });
        expect(onError).toHaveBeenCalledTimes(1);
        expect(onError.mock.calls[0][0].message).toBe(WEBRTC_UNAVAILABLE);
        // Closed: a later offer is ignored rather than reported again.
        onmessage({ data: JSON.stringify(OFFER) });
        expect(onError).toHaveBeenCalledTimes(1);
    });
});

describe('WebRtcMedia: the peer connection made on open', () => {
    it('reports a constructor that refuses, and sends no hello', () => {
        webrtcThen(ForbiddenPc);
        const t = new WebRtcMedia('ws://test.invalid/ws');
        const onError = vi.fn();
        t.onError = onError;
        t.connect();
        const ws = FakeWs.made[0];
        ws.readyState = FakeWs.OPEN;
        expect(() => ws.onopen()).not.toThrow();
        expect(onError).toHaveBeenCalledTimes(1);
        expect(onError.mock.calls[0][0].message).toBe(WEBRTC_UNAVAILABLE);
        expect(t._mwBind).toBeUndefined();
    });
});

describe('WebRtcDataChannel.send: input never goes out on the signalling socket', () => {
    const openWs = () => {
        const ws = new FakeWs('ws://test.invalid/ws');
        ws.readyState = FakeWs.OPEN;
        return ws;
    };

    it('drops input while the input channel is not open', () => {
        const t = new WebRtcDataChannel('ws://test.invalid/ws');
        t.signalingWs = openWs();
        t.send({ type: 'mousemove', x: 1, y: 2 });
        t.dataChannels.input = { readyState: 'connecting', send: vi.fn() };
        t.send({ type: 'mousemove', x: 3, y: 4 });
        expect(t.signalingWs.sent).toEqual([]);
        expect(t.dataChannels.input.send).not.toHaveBeenCalled();
    });

    it('uses the input channel once it is open', () => {
        const t = new WebRtcDataChannel('ws://test.invalid/ws');
        t.signalingWs = openWs();
        t.dataChannels.input = { readyState: 'open', send: vi.fn() };
        t.send({ type: 'ping', seq: 1 });
        expect(t.dataChannels.input.send).toHaveBeenCalledWith('{"type":"ping","seq":1}');
        expect(t.signalingWs.sent).toEqual([]);
    });

    it('uses the socket where it is the transport: wss, and WS fallback', () => {
        const wss = new WebRtcDataChannel('ws://test.invalid/ws', { wssMode: true });
        wss.signalingWs = openWs();
        wss.send({ type: 'key', code: 65 });
        expect(wss.signalingWs.sent).toEqual(['{"type":"key","code":65}']);

        const fb = new WebRtcDataChannel('ws://test.invalid/ws');
        fb.signalingWs = openWs();
        fb._wsFallback = true;
        fb.send({ type: 'key', code: 66 });
        expect(fb.signalingWs.sent).toEqual(['{"type":"key","code":66}']);
    });

    it('drops it in wss mode while the socket is not open', () => {
        const wss = new WebRtcDataChannel('ws://test.invalid/ws', { wssMode: true });
        wss.signalingWs = new FakeWs('ws://test.invalid/ws');
        wss.send({ type: 'key', code: 65 });
        expect(wss.signalingWs.sent).toEqual([]);
        wss.signalingWs = null;
        expect(() => wss.send({ type: 'key', code: 65 })).not.toThrow();
    });
});

/** A StreamView reduced to its transport handlers (setupWebRtc). */
function connectingView(webrtc, overrides = {}) {
    const sv = {
        webrtc,
        _transport: 'webrtc-dc-udp',
        _nativeAudio: true,
        _everConnected: false,
        _quitting: false,
        _manualQuitting: false,
        _takenOver: false,
        _standby: false,
        connected: false,
        onConnectionFailed: vi.fn(),
        setStatus: vi.fn(),
        quit: vi.fn(async () => {}),
        _reportConnectionFailed: StreamView.prototype._reportConnectionFailed,
        _handOffLinkLoss: StreamView.prototype._handOffLinkLoss,
        _armConnectWatchdog: StreamView.prototype._armConnectWatchdog,
        _clearConnectWatchdog: StreamView.prototype._clearConnectWatchdog,
        ...overrides,
    };
    return sv;
}

describe('StreamView: a rung that cannot run hands the reason to the chain', () => {
    it.each([
        ['webrtc-dc-udp', () => new WebRtcDataChannel('ws://test.invalid/ws')],
        ['webrtc-media-udp', () => new WebRtcMedia('ws://test.invalid/ws')],
    ])('%s', (transport, make) => {
        vi.useFakeTimers();
        noWebrtc();
        // As in the app: the view connects from its constructor, and the
        // chain's callback is wired only afterwards (_wireLiveView).
        const sv = connectingView(make(), { _transport: transport, onConnectionFailed: null });
        StreamView.prototype.setupWebRtc.call(sv);
        const failed = vi.fn();
        sv.onConnectionFailed = failed;
        vi.advanceTimersByTime(1);
        vi.advanceTimersByTime(1);
        expect(failed).toHaveBeenCalledTimes(1);
        expect(failed).toHaveBeenCalledWith(WEBRTC_UNAVAILABLE);
        // The watchdog has nothing left to do: the failure was handed off.
        vi.advanceTimersByTime(60000);
        expect(failed).toHaveBeenCalledTimes(1);
        expect(FakeWs.made).toHaveLength(0);
    });

    it('a wss rung still connects without WebRTC', () => {
        vi.useFakeTimers();
        noWebrtc();
        const t = new WebRtcDataChannel('ws://test.invalid/ws', { wssMode: true });
        const sv = connectingView(t, { _transport: 'wss', _nativeAudio: false });
        StreamView.prototype.setupWebRtc.call(sv);
        expect(FakeWs.made).toHaveLength(1);
        expect(sv.onConnectionFailed).not.toHaveBeenCalled();
    });
});
