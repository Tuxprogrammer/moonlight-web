/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { CONNECT_WATCHDOG_MS } from '../js/api/WebRtcSupport.js';

/**
 * Nothing ever timed out: a rung whose peer connection never came, or whose
 * socket a proxy held open, left the page on "Connecting…" for good. The view
 * now gives a rung a deadline from /start to its onOpen, above every deadline
 * the transports keep on their own, and hands a rung that misses it to the
 * transport chain like any other failure.
 */

/** A transport that never opens unless told to. */
function idleTransport() {
    return { connect: vi.fn(), send: vi.fn() };
}

/** A StreamView reduced to its transport handlers (setupWebRtc). */
function view(overrides = {}) {
    const sv = {
        webrtc: idleTransport(),
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
    StreamView.prototype.setupWebRtc.call(sv);
    return sv;
}

beforeEach(() => {
    vi.useFakeTimers();
    vi.spyOn(console, 'error').mockImplementation(() => {});
    vi.spyOn(console, 'warn').mockImplementation(() => {});
});

afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
});

describe('StreamView connect watchdog', () => {
    it('fails a WebRTC rung that has not opened in time, and not before', () => {
        const sv = view();
        const failed = sv.onConnectionFailed;
        expect(sv.webrtc.connect).toHaveBeenCalledTimes(1);
        vi.advanceTimersByTime(CONNECT_WATCHDOG_MS.webrtc - 1);
        expect(failed).not.toHaveBeenCalled();
        vi.advanceTimersByTime(1);
        vi.advanceTimersByTime(1);
        expect(failed).toHaveBeenCalledTimes(1);
        expect(failed).toHaveBeenCalledWith('connect_timeout');
    });

    it('gives a wss rung its own, shorter deadline', () => {
        const sv = view({ _transport: 'wss', _nativeAudio: false });
        const failed = sv.onConnectionFailed;
        vi.advanceTimersByTime(CONNECT_WATCHDOG_MS.wss);
        vi.advanceTimersByTime(1);
        expect(failed).toHaveBeenCalledWith('connect_timeout');
    });

    it('arms its deadline before the transport starts connecting', () => {
        const order = [];
        const sv = {
            webrtc: { connect: vi.fn(() => order.push('connect')) },
            _transport: 'webrtc-media-udp',
            _nativeAudio: true,
            _armConnectWatchdog: vi.fn(() => order.push('watchdog')),
        };
        StreamView.prototype.setupWebRtc.call(sv);
        expect(order).toEqual(['watchdog', 'connect']);
    });

    it('stands down once the rung opens', () => {
        const sv = view({
            // Everything onOpen touches past the guard, stubbed.
            _updateStartupStep: vi.fn(),
            _sendInputState: vi.fn(),
            _sendCursorMode: vi.fn(),
            _checkLinkStall: vi.fn(),
            stats: { decoded: 0 },
        });
        const failed = sv.onConnectionFailed;
        try {
            sv.webrtc.onOpen();
        } catch (_e) {
            // onOpen goes on to start the session; only the watchdog matters here.
        }
        expect(sv._connectWatchdog).toBeNull();
        vi.advanceTimersByTime(CONNECT_WATCHDOG_MS.webrtc * 2);
        expect(failed).not.toHaveBeenCalledWith('connect_timeout');
        vi.clearAllTimers();
    });

    it('stands down when the rung is reported failed some other way', () => {
        const sv = view();
        const failed = sv.onConnectionFailed;
        sv.webrtc.onError(new Error('ICE timeout'));
        vi.advanceTimersByTime(CONNECT_WATCHDOG_MS.webrtc);
        expect(failed).toHaveBeenCalledTimes(1);
        expect(failed).toHaveBeenCalledWith('ICE timeout');
    });

    it('does nothing after Stop or once the view is going', () => {
        for (const flags of [{ _manualQuitting: true }, { _quitting: true }]) {
            const sv = view();
            const failed = sv.onConnectionFailed;
            Object.assign(sv, flags);
            vi.advanceTimersByTime(CONNECT_WATCHDOG_MS.webrtc);
            expect(failed, JSON.stringify(flags)).not.toHaveBeenCalled();
            expect(sv.quit).not.toHaveBeenCalled();
        }
    });

    it('ends the view itself when nobody walks the chain', () => {
        const sv = view({ onConnectionFailed: null });
        vi.advanceTimersByTime(CONNECT_WATCHDOG_MS.webrtc);
        expect(sv.setStatus).toHaveBeenCalledWith('error', 'Connection timed out');
        expect(sv.quit).toHaveBeenCalledTimes(1);
    });

    it('is cleared on demand, and re-arming replaces the old deadline', () => {
        const sv = view();
        const failed = sv.onConnectionFailed;
        vi.advanceTimersByTime(CONNECT_WATCHDOG_MS.webrtc - 1000);
        sv._armConnectWatchdog();
        vi.advanceTimersByTime(2000);
        expect(failed).not.toHaveBeenCalled();
        sv._clearConnectWatchdog();
        vi.advanceTimersByTime(CONNECT_WATCHDOG_MS.webrtc * 2);
        expect(failed).not.toHaveBeenCalled();
        // Clearing twice is harmless.
        expect(() => sv._clearConnectWatchdog()).not.toThrow();
    });

    it("hands a standby leg's timeout to the standby path", () => {
        const sv = view({ _standby: true });
        const failed = sv.onConnectionFailed;
        vi.advanceTimersByTime(CONNECT_WATCHDOG_MS.webrtc);
        vi.advanceTimersByTime(1);
        expect(failed).toHaveBeenCalledWith('connect_timeout');
    });
});
