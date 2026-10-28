/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';
import { t } from '../js/i18n/i18n.js';
import { IDENTITY_REFUSED } from '../js/util/pairingCrypto.js';
import { WEBRTC_UNAVAILABLE, resetWebrtcProbe } from '../js/api/WebRtcSupport.js';

/**
 * The transport chain as MoonlightApp walks it when a rung fails
 * (_onTransportFailed). A browser without WebRTC skips every webrtc-* rung —
 * the decoder's jump to a native-media rung included, which is WebRTC too —
 * and when none is left, or the backend answers 409 no_usable_transport, the
 * user is told why and nothing is relaunched.
 */

const CHAIN = ['webrtc-dc-udp', 'webrtc-dc-tcp', 'webrtc-media-udp', 'webrtc-media-tcp', 'wss'];
const WSS_FIRST = ['wss', 'webrtc-dc-udp', 'webrtc-dc-tcp', 'webrtc-media-udp', 'webrtc-media-tcp'];
const NO_WSS = CHAIN.slice(0, 4);

const KEYS = [
    'streamView',
    '_nav',
    '_startupAborted',
    '_transportChain',
    '_transportIndex',
    '_firstTransportRetried',
    '_launchRetried',
    '_webrtcUnavailable',
    '_lastStreamHost',
    '_lastStreamApp',
    '_lastStreamingSettings',
    '_degradeOverrides',
    '_reconnectOf',
    '_relaunching',
    '_carryViewState',
    'state',
    'hostListView',
];
let saved;
let relaunch;
let end;
let warn;
let error;

/** The app in the middle of a launch, on rung @p cur of @p chain. */
function onRung(chain, cur, extra = {}) {
    Object.assign(MoonlightApp, {
        streamView: null,
        _nav: { overlay: 'streaming' },
        _startupAborted: false,
        _transportChain: chain,
        _transportIndex: cur,
        _firstTransportRetried: false,
        _launchRetried: false,
        _webrtcUnavailable: false,
        ...extra,
    });
}

beforeEach(() => {
    saved = Object.fromEntries(KEYS.map((k) => [k, MoonlightApp[k]]));
    relaunch = vi.spyOn(MoonlightApp, '_relaunchTransport').mockImplementation(() => {});
    end = vi.spyOn(MoonlightApp, '_endAfterFailedLaunch').mockImplementation(() => {});
    vi.spyOn(MoonlightApp, '_abortStandby').mockImplementation(() => {});
    warn = vi.spyOn(Toast, 'warning').mockImplementation(() => {});
    error = vi.spyOn(Toast, 'error').mockImplementation(() => {});
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    vi.spyOn(console, 'error').mockImplementation(() => {});
});

afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    resetWebrtcProbe();
    Object.assign(MoonlightApp, saved);
});

describe('MoonlightApp: a rung that fails for want of WebRTC', () => {
    it('skips every other webrtc-* rung, straight to wss, without the silent retry', () => {
        onRung(CHAIN, 0);
        MoonlightApp._onTransportFailed(WEBRTC_UNAVAILABLE);
        expect(relaunch).toHaveBeenCalledTimes(1);
        expect(relaunch).toHaveBeenCalledWith(4);
        expect(MoonlightApp._webrtcUnavailable).toBe(true);
        expect(MoonlightApp._firstTransportRetried).toBe(true);
        expect(warn).toHaveBeenCalledWith(t('transport.webrtcOffNext', { to: t('transport.wss') }));
        expect(end).not.toHaveBeenCalled();
    });

    it('does the same from a later rung', () => {
        onRung(WSS_FIRST, 1, { _firstTransportRetried: true });
        MoonlightApp._onTransportFailed(WEBRTC_UNAVAILABLE);
        // wss was rung 0 and already failed: nothing usable after rung 1.
        expect(relaunch).not.toHaveBeenCalled();
        expect(end).toHaveBeenCalledTimes(1);
        expect(error).toHaveBeenCalledWith(t('transport.noUsable.webrtc_unavailable'));
    });

    it('says why and stops when the server offers no wss', () => {
        onRung(NO_WSS, 0);
        MoonlightApp._onTransportFailed(WEBRTC_UNAVAILABLE);
        expect(relaunch).not.toHaveBeenCalled();
        expect(end).toHaveBeenCalledTimes(1);
        expect(error).toHaveBeenCalledWith(t('transport.noUsable.webrtc_unavailable'));
    });

    it('says why on the last rung too', () => {
        onRung(['webrtc-dc-udp'], 0);
        MoonlightApp._onTransportFailed(WEBRTC_UNAVAILABLE);
        expect(relaunch).not.toHaveBeenCalled();
        expect(error).toHaveBeenCalledWith(t('transport.noUsable.webrtc_unavailable'));
    });
});

describe('MoonlightApp: a launch that already knew there is no WebRTC', () => {
    it('walks past the webrtc-* rungs after wss fails, and ends', () => {
        onRung(WSS_FIRST, 0, { _webrtcUnavailable: true, _firstTransportRetried: true });
        MoonlightApp._onTransportFailed('closed before connect');
        expect(relaunch).not.toHaveBeenCalled();
        expect(end).toHaveBeenCalledTimes(1);
        // wss was tried and failed: that is "nothing works", not "WebRTC off".
        expect(error).toHaveBeenCalledWith(t('transport.allFailed'));
    });

    it('still retries a first wss rung once, silently, as any first rung', () => {
        onRung(WSS_FIRST, 0, { _webrtcUnavailable: true });
        MoonlightApp._onTransportFailed('closed before connect');
        expect(relaunch).toHaveBeenCalledWith(0);
        expect(warn).not.toHaveBeenCalled();
    });

    it('has no native-media rung to jump to after the decoder failed', () => {
        onRung(WSS_FIRST, 0, { _webrtcUnavailable: true });
        MoonlightApp._onTransportFailed('decoder_unsupported');
        expect(relaunch).not.toHaveBeenCalled();
        expect(end).toHaveBeenCalledTimes(1);
        expect(error).toHaveBeenCalledWith(t('transport.allFailed'));
    });

    it('moves past webrtc-* rungs when a launch fails after a transport failure', () => {
        onRung(['webrtc-dc-udp', 'webrtc-dc-tcp', 'wss'], 0, {
            _webrtcUnavailable: true,
            _firstTransportRetried: true,
            _launchRetried: true,
        });
        MoonlightApp._onTransportFailed('launch_failed');
        expect(relaunch).toHaveBeenCalledWith(2);
        expect(warn).toHaveBeenCalledWith(
            t('transport.launchFailedNext', {
                from: t('transport.webrtcDcUdp'),
                to: t('transport.wss'),
            }),
        );
    });
});

describe('MoonlightApp: with WebRTC, the chain walks as it did', () => {
    it('jumps to a native-media rung after the decoder failed', () => {
        onRung(CHAIN, 0);
        MoonlightApp._onTransportFailed('decoder_unsupported');
        expect(relaunch).toHaveBeenCalledWith(2);
    });

    it('retries the first rung once, silently, then moves down', () => {
        onRung(CHAIN, 0);
        MoonlightApp._onTransportFailed('ICE failed');
        expect(relaunch).toHaveBeenLastCalledWith(0);
        MoonlightApp._onTransportFailed('ICE failed');
        expect(relaunch).toHaveBeenLastCalledWith(1);
        expect(warn).toHaveBeenCalledTimes(1);
    });

    it('ends with "all failed" at the end of the chain', () => {
        onRung(CHAIN, 4, { _firstTransportRetried: true });
        MoonlightApp._onTransportFailed('closed before connect');
        expect(relaunch).not.toHaveBeenCalled();
        expect(error).toHaveBeenCalledWith(t('transport.allFailed'));
    });

    it('ends on a refused identity, whatever is left', () => {
        onRung(CHAIN, 0);
        MoonlightApp._onTransportFailed(IDENTITY_REFUSED);
        expect(relaunch).not.toHaveBeenCalled();
        expect(error).toHaveBeenCalledWith(t('transport.identityRefused'));
    });
});

describe('MoonlightApp: the connect watchdog', () => {
    it('is a spent deadline: no silent retry of the first rung', () => {
        onRung(CHAIN, 0);
        MoonlightApp._onTransportFailed('connect_timeout');
        expect(relaunch).toHaveBeenCalledWith(1);
        expect(MoonlightApp._firstTransportRetried).toBe(true);
    });

    it('skips WebRTC rungs after it when WebRTC is known missing', () => {
        onRung(WSS_FIRST, 0, { _webrtcUnavailable: true });
        MoonlightApp._onTransportFailed('connect_timeout');
        expect(relaunch).not.toHaveBeenCalled();
        expect(end).toHaveBeenCalledTimes(1);
    });
});

describe('MoonlightApp: the backend says no transport is usable', () => {
    it.each([
        ['webrtc_unavailable', 'transport.noUsable.webrtc_unavailable'],
        ['policy', 'transport.noUsable.policy'],
        ['tunnel', 'transport.noUsable.tunnel'],
        ['udp_blocked', 'transport.noUsable.udp_blocked'],
        ['', 'transport.noUsable.generic'],
    ])('ends a relaunch answered "%s" with its message', (why, key) => {
        onRung(CHAIN, 1, { _firstTransportRetried: true });
        MoonlightApp._onTransportFailed('no_usable_transport:' + why);
        expect(relaunch).not.toHaveBeenCalled();
        expect(end).toHaveBeenCalledTimes(1);
        expect(error).toHaveBeenCalledWith(t(key));
    });

    it('is told by a relaunch that got the 409', async () => {
        onRung(CHAIN, 0, {
            _lastStreamHost: { uuid: 'host-1' },
            _lastStreamApp: { id: 7 },
            _lastStreamingSettings: {},
            _degradeOverrides: {},
            _reconnectOf: null,
        });
        vi.spyOn(MoonlightApp, '_showRelaunchLoader').mockImplementation(() => {});
        const err = Object.assign(new Error('no_usable_transport'), {
            statusCode: 409,
            responseBody: { error: 'no_usable_transport', reason: 'policy' },
        });
        const launch = vi.spyOn(BackendClient, 'launchApp').mockRejectedValue(err);
        const reason = await MoonlightApp._doRelaunchTransport(4, { uuid: 'host-1' }, { id: 7 });
        expect(launch).toHaveBeenCalledTimes(1);
        expect(launch.mock.calls[0][2].transport_index).toBe(4);
        expect(reason).toBe('no_usable_transport:policy');
    });

    it('still charges an ordinary relaunch failure to the host', async () => {
        onRung(CHAIN, 0, {
            _lastStreamingSettings: {},
            _degradeOverrides: {},
            _reconnectOf: null,
        });
        vi.spyOn(MoonlightApp, '_showRelaunchLoader').mockImplementation(() => {});
        const err = Object.assign(new Error('Bad gateway'), { statusCode: 502 });
        vi.spyOn(BackendClient, 'launchApp').mockRejectedValue(err);
        expect(await MoonlightApp._doRelaunchTransport(1, { uuid: 'h' }, { id: 7 })).toBe(
            'launch_failed',
        );
    });
});

describe('MoonlightApp: a first launch answered 409 no_usable_transport', () => {
    it('says why, goes back to the app list and relaunches nothing', async () => {
        vi.stubGlobal('RTCPeerConnection', undefined);
        resetWebrtcProbe();
        Object.assign(MoonlightApp, {
            state: 'app_list',
            streamView: null,
            hostListView: null,
            _nav: { overlay: null, mainView: 'apps', mainState: {} },
            _udpProbeCache: null,
        });
        vi.spyOn(MoonlightApp, '_enterLaunchFullscreen').mockImplementation(() => {});
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        vi.spyOn(Toast, 'success').mockImplementation(() => {});
        vi.spyOn(Toast, 'dismissAll').mockResolvedValue(undefined);
        const err = Object.assign(new Error('no_usable_transport'), {
            statusCode: 409,
            responseBody: { error: 'no_usable_transport', reason: 'webrtc_unavailable' },
        });
        const launch = vi.spyOn(BackendClient, 'launchApp').mockRejectedValue(err);
        await MoonlightApp.launchApp(
            { uuid: 'host-1', displayName: 'Host', backendType: 'sunshine' },
            { id: 7, name: 'Desktop' },
            undefined,
            undefined,
            { skipSelfStreamWarn: true },
        );
        expect(launch).toHaveBeenCalledTimes(1);
        expect(MoonlightApp._webrtcUnavailable).toBe(true);
        expect(error).toHaveBeenCalledWith(t('transport.noUsable.webrtc_unavailable'));
        expect(MoonlightApp.state).toBe('app_list');
        expect(relaunch).not.toHaveBeenCalled();
    });
});
