/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';
import { t } from '../js/i18n/i18n.js';
import { resetWebrtcProbe } from '../js/api/WebRtcSupport.js';

/**
 * A launch that ends in an error takes down the toast that said it was under
 * way. After a 409 no_usable_transport, "Launching Desktop..." stayed up
 * beside the error that said nothing could stream (real-host E2E, Firefox
 * without WebRTC); the generic launch error had the same leftover.
 */

const HOST = { uuid: 'host-1', displayName: 'Host', backendType: 'sunshine' };
const APP = { id: 7, name: 'Desktop' };
const CHAIN = ['webrtc-dc-udp', 'webrtc-dc-tcp', 'webrtc-media-udp', 'webrtc-media-tcp', 'wss'];
const KEYS = [
    'state',
    'streamView',
    'hostListView',
    '_nav',
    '_udpProbeCache',
    '_transportChain',
    '_transportIndex',
    '_firstTransportRetried',
    '_launchRetried',
    '_webrtcUnavailable',
    '_startupAborted',
    '_chainToast',
];
let saved;

/** The toasts on screen and leaving it: [text, leaving]. */
const toasts = () =>
    [...document.querySelectorAll('#toast-container .toast')].map((el) => [
        el.querySelector('.toast-text').textContent,
        el.classList.contains('toast-exit'),
    ]);
const showing = () =>
    toasts()
        .filter(([, leaving]) => !leaving)
        .map(([text]) => text);

const refusal = (statusCode, responseBody, message = 'refused') =>
    Object.assign(new Error(message), { statusCode, responseBody });

beforeEach(() => {
    saved = Object.fromEntries(KEYS.map((k) => [k, MoonlightApp[k]]));
    document.getElementById('toast-container')?.remove();
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    vi.spyOn(console, 'error').mockImplementation(() => {});
});

afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    resetWebrtcProbe();
    document.getElementById('toast-container')?.remove();
    Object.assign(MoonlightApp, saved);
});

/** An owner launch whose /start answers @p err. */
async function launchFailing(err) {
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
    vi.spyOn(BackendClient, 'launchApp').mockRejectedValue(err);
    await MoonlightApp.launchApp(HOST, APP, undefined, undefined, {
        skipSelfStreamWarn: true,
        runningChecked: true,
    });
}

describe('An owner launch that fails takes "Launching…" down', () => {
    const launching = () => t('launch.launching', { name: APP.name });

    it('on a 409 no_usable_transport, leaving only the reason', async () => {
        await launchFailing(
            refusal(409, { error: 'no_usable_transport', reason: 'webrtc_unavailable' }),
        );
        expect(showing()).toEqual([t('transport.noUsable.webrtc_unavailable')]);
        expect(toasts()).toContainEqual([launching(), true]);
    });

    it('on any other launch error too (the generic path)', async () => {
        await launchFailing(refusal(502, null, 'Bad gateway'));
        expect(showing()).toEqual(['Bad gateway']);
        expect(toasts()).toContainEqual([launching(), true]);
    });

    it('on a launch that answered a status this build does not stream on', async () => {
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
        vi.spyOn(BackendClient, 'launchApp').mockResolvedValue({ status: 'queued' });
        await MoonlightApp.launchApp(HOST, APP, undefined, undefined, {
            skipSelfStreamWarn: true,
            runningChecked: true,
        });
        expect(showing()).toEqual([t('launch.failed')]);
    });
});

describe('A relaunch the backend answers 409 takes the "trying …" toast down', () => {
    it('leaves only the reason', () => {
        Object.assign(MoonlightApp, {
            streamView: null,
            _nav: { overlay: 'streaming' },
            _startupAborted: false,
            _transportChain: CHAIN,
            _transportIndex: 0,
            _firstTransportRetried: true,
            _launchRetried: false,
            _webrtcUnavailable: false,
        });
        vi.spyOn(MoonlightApp, '_abortStandby').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_relaunchTransport').mockImplementation(() => {});
        const end = vi.spyOn(MoonlightApp, '_endAfterFailedLaunch').mockImplementation(() => {});
        // Rung 0 failed: "… failed — trying …", and the relaunch is refused.
        MoonlightApp._onTransportFailed('ICE failed');
        expect(showing()).toHaveLength(1);
        MoonlightApp._onTransportFailed('no_usable_transport:policy');
        expect(showing()).toEqual([t('transport.noUsable.policy')]);
        expect(end).toHaveBeenCalledTimes(1);
    });
});

describe('_sayNoUsableTransport (owner and guest paths)', () => {
    it('takes down every toast it is given, then says why', () => {
        const a = Toast.info('Launching Desktop...');
        const b = Toast.warning('trying wss…');
        MoonlightApp._sayNoUsableTransport('tunnel', a, b);
        expect(showing()).toEqual([t('transport.noUsable.tunnel')]);
    });

    it('takes a missing or already gone toast in its stride', () => {
        const gone = Toast.info('gone');
        gone.dismiss();
        expect(() =>
            MoonlightApp._sayNoUsableTransport('', null, undefined, {}, gone),
        ).not.toThrow();
        expect(showing()).toEqual([t('transport.noUsable.generic')]);
    });
});
