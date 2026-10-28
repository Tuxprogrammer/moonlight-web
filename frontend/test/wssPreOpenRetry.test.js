/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WebRtcDataChannel } from '../js/api/WebRtcDataChannel.js';

/**
 * The relaunch race: a relaunch reaches /ws/stream while the previous attempt's
 * worker is still going away, and the socket closes before it ever opened (a
 * proxy turned the refusal into a 502). That cost the whole rung. The host now
 * answers "retry in a second", and the client opens the socket once more
 * before giving the rung up.
 */

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
        this.closeCalls = 0;
        FakeWs.made.push(this);
    }
    send(data) {
        this.sent.push(data);
    }
    close() {
        this.closeCalls++;
        this.readyState = FakeWs.CLOSED;
    }
    open() {
        this.readyState = FakeWs.OPEN;
        this.onopen?.();
    }
    fail(code = 1006) {
        this.readyState = FakeWs.CLOSED;
        this.onerror?.(new Event('error'));
        this.onclose?.({ code, reason: '' });
    }
}

function wss() {
    const t = new WebRtcDataChannel('wss://test.invalid/ws/stream', { wssMode: true });
    t.onOpen = vi.fn();
    t.onError = vi.fn();
    t.onClose = vi.fn();
    return t;
}

beforeEach(() => {
    vi.useFakeTimers();
    FakeWs.made = [];
    vi.stubGlobal('WebSocket', FakeWs);
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    vi.spyOn(console, 'error').mockImplementation(() => {});
});

afterEach(() => {
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
    vi.useRealTimers();
});

describe('WSS: a socket that closes before it opened', () => {
    it('is opened once more a second later, and the rung carries on', () => {
        const t = wss();
        t.connect();
        FakeWs.made[0].fail(1006);
        expect(t.onError).not.toHaveBeenCalled();
        expect(FakeWs.made).toHaveLength(1);
        vi.advanceTimersByTime(t.WSS_RETRY_DELAY_MS - 1);
        expect(FakeWs.made).toHaveLength(1);
        vi.advanceTimersByTime(1);
        expect(FakeWs.made).toHaveLength(2);
        FakeWs.made[1].open();
        expect(t.onOpen).toHaveBeenCalledTimes(1);
        expect(t.connected).toBe(true);
        expect(t.onError).not.toHaveBeenCalled();
    });

    it('fails the rung when the second one closes before opening too', () => {
        const t = wss();
        t.connect();
        FakeWs.made[0].fail(1006);
        vi.advanceTimersByTime(t.WSS_RETRY_DELAY_MS);
        FakeWs.made[1].fail(1006);
        expect(t.onError).toHaveBeenCalledTimes(1);
        expect(t.onError.mock.calls[0][0].message).toMatch(/impossible/);
        expect(t.onClose).toHaveBeenCalledTimes(1);
        vi.advanceTimersByTime(10000);
        expect(FakeWs.made).toHaveLength(2);
    });

    it('counts an error and its close as one failure', () => {
        const t = wss();
        t.connect();
        const first = FakeWs.made[0];
        first.onerror(new Event('error'));
        first.onclose?.({ code: 1006, reason: '' });
        vi.advanceTimersByTime(t.WSS_RETRY_DELAY_MS);
        expect(FakeWs.made).toHaveLength(2);
        expect(t.onError).not.toHaveBeenCalled();
    });

    it('retries on an error alone, when no close follows', () => {
        const t = wss();
        t.connect();
        FakeWs.made[0].onerror(new Event('error'));
        vi.advanceTimersByTime(t.WSS_RETRY_DELAY_MS);
        expect(FakeWs.made).toHaveLength(2);
    });

    it('retries on a clean close before open, too (a proxy that answered and hung up)', () => {
        const t = wss();
        t.connect();
        FakeWs.made[0].onclose({ code: 1000, reason: '' });
        vi.advanceTimersByTime(t.WSS_RETRY_DELAY_MS);
        expect(FakeWs.made).toHaveLength(2);
    });

    it('does not retry a TLS failure', () => {
        const t = wss();
        t.connect();
        FakeWs.made[0].onclose({ code: 1015, reason: '' });
        expect(t.onError).toHaveBeenCalledTimes(1);
        expect(t.onError.mock.calls[0][0].message).toMatch(/TLS/);
        vi.advanceTimersByTime(10000);
        expect(FakeWs.made).toHaveLength(1);
    });

    it('opens nothing more once closed while the retry waits', () => {
        const t = wss();
        t.connect();
        FakeWs.made[0].fail(1006);
        t.close();
        vi.advanceTimersByTime(10000);
        expect(FakeWs.made).toHaveLength(1);
        expect(t.onError).not.toHaveBeenCalled();
    });

    it('opens nothing more once stopping while the retry waits', () => {
        const t = wss();
        t.connect();
        FakeWs.made[0].fail(1006);
        t.markStopping();
        vi.advanceTimersByTime(10000);
        expect(FakeWs.made).toHaveLength(1);
    });

    it('lets go of the first socket, whose late events change nothing', () => {
        const t = wss();
        t.connect();
        const first = FakeWs.made[0];
        const lateOpen = first.onopen;
        first.onclose({ code: 1006, reason: '' });
        expect(first.onopen).toBeNull();
        vi.advanceTimersByTime(t.WSS_RETRY_DELAY_MS);
        lateOpen();
        expect(t.onOpen).not.toHaveBeenCalled();
        FakeWs.made[1].open();
        expect(t.onOpen).toHaveBeenCalledTimes(1);
    });
});

describe('WSS: a socket that had opened', () => {
    it('is not retried when it goes: the stream was interrupted', () => {
        const t = wss();
        t.connect();
        FakeWs.made[0].open();
        FakeWs.made[0].onclose({ code: 1006, reason: '' });
        expect(t.onError).toHaveBeenCalledTimes(1);
        expect(t.onError.mock.calls[0][0].message).toMatch(/interrompue/);
        vi.advanceTimersByTime(10000);
        expect(FakeWs.made).toHaveLength(1);
    });

    it('reports an error after open once, not again on its close', () => {
        const t = wss();
        t.connect();
        const ws = FakeWs.made[0];
        ws.open();
        const onclose = ws.onclose;
        ws.onerror(new Event('error'));
        onclose({ code: 1006, reason: '' });
        expect(t.onError).toHaveBeenCalledTimes(1);
    });

    it('carries frames and input once open', () => {
        const t = wss();
        const msg = vi.spyOn(t, '_onWssMessage').mockImplementation(() => {});
        t.connect();
        const ws = FakeWs.made[0];
        expect(ws.binaryType).toBe('arraybuffer');
        ws.open();
        ws.onmessage({ data: new ArrayBuffer(4) });
        expect(msg).toHaveBeenCalledTimes(1);
        t.send({ type: 'key', code: 65 });
        expect(ws.sent).toEqual(['{"type":"key","code":65}']);
    });

    it('is closed by close(), with its handlers dropped', () => {
        const t = wss();
        t.connect();
        const ws = FakeWs.made[0];
        ws.open();
        t.close();
        expect(ws.closeCalls).toBe(1);
        expect(ws.onclose).toBeNull();
        expect(t.signalingWs).toBeNull();
    });
});
