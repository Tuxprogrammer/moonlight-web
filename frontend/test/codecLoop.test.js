/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';
import { t } from '../js/i18n/i18n.js';
import {
    AUTO_RELAUNCH_MAX,
    CODEC_RELAUNCH_MAX,
    STABLE_STREAM_MS,
    freshRelaunchBudget,
} from '../js/stream/RelaunchBudget.js';
import {
    hevcDecodeRefused,
    noteHevcDecodeRefused,
    resetHevcProbe,
} from '../js/util/BrowserDetect.js';
import { resetWebrtcProbe } from '../js/api/WebRtcSupport.js';

/**
 * The relaunch loop: a browser that says it decodes HEVC and does not falls
 * back to H.264, and the relaunches of every kind that follow — transport
 * rungs, launch retries, restarts after a crash — each kept a count of their
 * own, if any, while a codec relaunch started the chain over. Now one budget
 * counts them all (stream/RelaunchBudget.js), codec relaunches capped on
 * their own inside it, and a browser that failed HEVC is asked for H.264 at
 * once on its next launches.
 */

const CHAIN = ['webrtc-dc-udp', 'webrtc-dc-tcp', 'webrtc-media-udp', 'webrtc-media-tcp', 'wss'];
const HOST = { uuid: 'host-1', displayName: 'Host', backendType: 'sunshine' };
const APP = { id: 7, name: 'Desktop' };
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
    '_relaunchBudget',
    '_crashRelaunches',
    '_reconnectOf',
    '_relaunching',
    'state',
    'hostListView',
    '_udpProbeCache',
];

let saved;
let error;

beforeEach(() => {
    saved = Object.fromEntries(KEYS.map((k) => [k, MoonlightApp[k]]));
    error = vi.spyOn(Toast, 'error').mockImplementation(() => {});
    vi.spyOn(Toast, 'warning').mockImplementation(() => {});
    vi.spyOn(Toast, 'info').mockImplementation(() => {});
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    vi.spyOn(console, 'error').mockImplementation(() => {});
    localStorage.removeItem('mw_hevc_refused_v1');
});

afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    resetHevcProbe();
    resetWebrtcProbe();
    localStorage.removeItem('mw_hevc_refused_v1');
    localStorage.removeItem('mw-streaming-settings');
    Object.assign(MoonlightApp, saved);
});

/**
 * Drive the real failure handlers against an adversary that fails every
 * launch, in the order @p failures gives (cycled). Relaunches are recorded,
 * never performed.
 */
async function simulate(failures, { maxSteps = 200, firstFrameAt } = {}) {
    const log = [];
    let pending = null;
    let step = 0;
    const nextFailure = () => failures[step++ % failures.length];

    Object.assign(MoonlightApp, {
        _nav: { overlay: 'streaming', mainView: 'apps', mainState: {} },
        _startupAborted: false,
        _transportChain: CHAIN,
        _transportIndex: 0,
        _firstTransportRetried: false,
        _launchRetried: false,
        _webrtcUnavailable: false,
        _lastStreamHost: HOST,
        _lastStreamApp: APP,
        _relaunchBudget: freshRelaunchBudget(),
        _crashRelaunches: [],
        _reconnectOf: null,
        _relaunching: false,
    });
    for (const m of [
        '_abortStandby',
        '_showRelaunchLoader',
        '_hideRelaunchLoader',
        '_stopUpgradeTimer',
        '_stopAspectProbe',
        '_renderMainView',
        '_noteSupportSession',
        'transition',
    ])
        vi.spyOn(MoonlightApp, m).mockImplementation(() => {});
    // A codec relaunch: the new launch starts the chain over, as launchApp does.
    vi.spyOn(MoonlightApp, 'launchApp').mockImplementation((host, app, codec) => {
        log.push('codec:' + codec);
        MoonlightApp._restartChainForCodec();
        MoonlightApp._transportIndex = 0;
        MoonlightApp._nav.overlay = 'streaming';
        pending = nextFailure();
    });
    vi.spyOn(MoonlightApp, '_relaunchTransport').mockImplementation((index) => {
        log.push('rung:' + index);
        MoonlightApp._transportIndex = index;
        pending = nextFailure();
    });
    vi.spyOn(MoonlightApp, '_endAfterFailedLaunch').mockImplementation(() => {
        log.push('end');
    });
    vi.spyOn(BackendClient, 'streamState').mockResolvedValue('crashed');

    const view = (over = {}) => ({
        streamId: 's',
        videoCodec: 'h264',
        _quitting: false,
        _manualQuitting: false,
        _takenOver: false,
        _firstFrameAt: firstFrameAt,
        _linkLostDefault: vi.fn(() => log.push('end')),
        ...over,
    });

    pending = nextFailure();
    while (pending && log.length < maxSteps) {
        const kind = pending;
        pending = null;
        if (kind === 'codec') {
            // The decoder went silent: the same codec again, on keyframes.
            MoonlightApp.streamView = view({
                _codecFallbackRequested: true,
                _codecFallback: { codec: 'h264', hdr: false, reason: 'silent-decoder' },
            });
            MoonlightApp._nav.overlay = 'streaming';
            MoonlightApp._onStreamingQuit();
            if (!pending && !log.includes('end') && error.mock.calls.length) log.push('end');
        } else if (kind === 'crash') {
            const sv = view();
            MoonlightApp.streamView = sv;
            await MoonlightApp._onLiveLinkLost(sv);
        } else {
            MoonlightApp.streamView = kind === 'launch' ? null : view({ _firstFrameAt: undefined });
            MoonlightApp._onTransportFailed(kind === 'launch' ? 'launch_failed' : 'ICE failed');
        }
    }
    const relaunches = log.filter((e) => e !== 'end');
    return { log, relaunches, codec: relaunches.filter((e) => e.startsWith('codec')).length };
}

describe('The relaunch loop is bounded, whatever the mix of failures', () => {
    it.each([
        ['codec and transport failures taking turns', ['codec', 'transport']],
        ['a codec failure after every two transport ones', ['transport', 'transport', 'codec']],
        ['codec, launch and transport failures', ['codec', 'launch', 'transport', 'launch']],
        ['codec failures and crashes', ['codec', 'crash']],
        ['every kind', ['transport', 'codec', 'launch', 'crash', 'transport']],
    ])('%s', async (_name, failures) => {
        const { log, relaunches, codec } = await simulate(failures);
        expect(log.at(-1), log.join(' ')).toBe('end');
        expect(relaunches.length).toBeLessThanOrEqual(AUTO_RELAUNCH_MAX);
        expect(codec).toBeLessThanOrEqual(CODEC_RELAUNCH_MAX);
    });

    it('stops codec relaunches at their own cap, with the codec message', async () => {
        const { codec, log } = await simulate(['codec']);
        expect(codec).toBe(CODEC_RELAUNCH_MAX);
        expect(log.at(-1)).toBe('end');
        expect(error).toHaveBeenLastCalledWith(t('launch.hevcH264Unsupported'));
    });

    it('says the launch kept failing when the whole budget ran out', async () => {
        // Three rungs fail between codec relaunches: the total binds before
        // the codec cap does.
        const { relaunches, codec } = await simulate([
            'transport',
            'transport',
            'transport',
            'codec',
        ]);
        expect(codec).toBeLessThan(CODEC_RELAUNCH_MAX);
        expect(relaunches.length).toBe(AUTO_RELAUNCH_MAX);
        expect(error).toHaveBeenLastCalledWith(t('launch.relaunchLimit'));
    });

    it('gives the first rung its silent retry again after each codec relaunch', async () => {
        const { log } = await simulate(['transport', 'codec']);
        // rung:0 (silent retry) comes back after every codec relaunch.
        const retries = log.filter(
            (e, i) => e === 'rung:0' && (i === 0 || log[i - 1].startsWith('codec')),
        );
        expect(retries.length).toBeGreaterThan(1);
    });

    it('walks a plain transport failure down the chain and ends there', async () => {
        const { log } = await simulate(['transport']);
        expect(log).toEqual(['rung:0', 'rung:1', 'rung:2', 'rung:3', 'rung:4', 'end']);
        expect(error).toHaveBeenLastCalledWith(t('transport.allFailed'));
    });

    it('starts over after a stream that played for a minute', async () => {
        // Every view showed pictures a minute ago: each failure is a new trouble.
        vi.spyOn(performance, 'now').mockReturnValue(1e7);
        const { relaunches } = await simulate(['codec'], {
            firstFrameAt: 1e7 - STABLE_STREAM_MS,
            maxSteps: 30,
        });
        expect(relaunches.length).toBe(30);
    });

    it('never asks the budget for a relaunch it does not perform (Stop pressed)', () => {
        Object.assign(MoonlightApp, {
            _nav: { overlay: 'streaming' },
            _transportChain: CHAIN,
            _transportIndex: 1,
            _relaunchBudget: freshRelaunchBudget(),
            streamView: { _manualQuitting: true },
            _startupAborted: false,
        });
        vi.spyOn(MoonlightApp, '_abortStandby').mockImplementation(() => {});
        MoonlightApp._onTransportFailed('ICE failed');
        expect(MoonlightApp._relaunchBudget).toEqual({ codec: 0, total: 0 });
    });
});

describe('MoonlightApp._takeRelaunch', () => {
    it('charges the budget and refuses once it is spent', () => {
        MoonlightApp._relaunchBudget = { codec: 0, total: AUTO_RELAUNCH_MAX - 1 };
        expect(MoonlightApp._takeRelaunch('transport', null)).toBe(true);
        expect(MoonlightApp._relaunchBudget.total).toBe(AUTO_RELAUNCH_MAX);
        expect(MoonlightApp._takeRelaunch('crash', null)).toBe(false);
        expect(MoonlightApp._relaunchBudget.total).toBe(AUTO_RELAUNCH_MAX);
    });

    it('starts from a fresh budget when there is none', () => {
        MoonlightApp._relaunchBudget = null;
        expect(MoonlightApp._takeRelaunch('codec', null)).toBe(true);
        expect(MoonlightApp._relaunchBudget).toEqual({ codec: 1, total: 1 });
    });

    it('a crash after a long, healthy stream is not refused', () => {
        MoonlightApp._relaunchBudget = { codec: 4, total: AUTO_RELAUNCH_MAX };
        vi.spyOn(performance, 'now').mockReturnValue(1e7);
        const sv = { _firstFrameAt: 1e7 - 2 * STABLE_STREAM_MS };
        expect(MoonlightApp._takeRelaunch('crash', sv)).toBe(true);
        expect(MoonlightApp._relaunchBudget).toEqual({ codec: 0, total: 1 });
    });
});

describe('A codec fallback from HEVC is remembered', () => {
    beforeEach(() => {
        for (const m of [
            '_abortStandby',
            '_showRelaunchLoader',
            '_hideRelaunchLoader',
            '_stopUpgradeTimer',
            '_stopAspectProbe',
            '_persistHdrDisabled',
            'transition',
        ])
            vi.spyOn(MoonlightApp, m).mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_persistChroma444Disabled').mockReturnValue(false);
        vi.spyOn(MoonlightApp, 'launchApp').mockImplementation(() => {});
        Object.assign(MoonlightApp, {
            _nav: { overlay: 'streaming' },
            _lastStreamHost: HOST,
            _lastStreamApp: APP,
            _lastStreamingSettings: {},
            _relaunchBudget: freshRelaunchBudget(),
        });
    });

    const quitWith = (view) => {
        MoonlightApp.streamView = { _codecFallbackRequested: true, ...view };
        MoonlightApp._onStreamingQuit();
    };

    it('when HEVC SDR failed to decode and fell back to H.264', () => {
        quitWith({ videoCodec: 'hevc', _codecFallback: { codec: 'h264', hdr: false } });
        expect(hevcDecodeRefused()).toBe(true);
        expect(MoonlightApp.launchApp).toHaveBeenCalledWith(HOST, APP, 'h264', false);
    });

    it('not when HDR, a silent decoder or AV1 was what failed', () => {
        quitWith({
            videoCodec: 'hevc',
            _hdrEnabled: true,
            _codecFallback: { codec: 'av1', hdr: true },
        });
        MoonlightApp._nav.overlay = 'streaming';
        quitWith({
            videoCodec: 'hevc',
            _codecFallback: { codec: 'hevc', hdr: false, reason: 'silent-decoder' },
        });
        MoonlightApp._nav.overlay = 'streaming';
        quitWith({ videoCodec: 'av1', _codecFallback: { codec: 'h264', hdr: false } });
        expect(hevcDecodeRefused()).toBe(false);
        expect(MoonlightApp.launchApp).toHaveBeenCalledTimes(3);
    });
});

describe('An owner launch asks HEVC only from a browser that decodes it', () => {
    /** Run launchApp up to /start and return the body it sent. */
    async function launchBody(stored, { supported = true, codecOverride, hdrOverride } = {}) {
        vi.stubGlobal('RTCPeerConnection', undefined);
        resetWebrtcProbe();
        vi.stubGlobal('VideoDecoder', {
            isConfigSupported: vi.fn(async (cfg) => ({
                supported: /^(hev1|hvc1)/.test(cfg.codec) ? supported : true,
            })),
        });
        localStorage.setItem('mw-streaming-settings', JSON.stringify(stored));
        Object.assign(MoonlightApp, {
            state: 'app_list',
            streamView: null,
            hostListView: null,
            _nav: { overlay: null, mainView: 'apps', mainState: {} },
            _udpProbeCache: null,
            _firstTransportRetried: true,
            _launchRetried: true,
            _transportChain: CHAIN,
            _transportIndex: 0,
        });
        vi.spyOn(MoonlightApp, '_enterLaunchFullscreen').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_hideRelaunchLoader').mockImplementation(() => {});
        const launch = vi
            .spyOn(BackendClient, 'launchApp')
            .mockRejectedValue(Object.assign(new Error('stop here'), { statusCode: 500 }));
        await MoonlightApp.launchApp(HOST, APP, codecOverride, hdrOverride, {
            skipSelfStreamWarn: true,
            runningChecked: true,
        });
        expect(launch).toHaveBeenCalledTimes(1);
        return launch.mock.calls[0][2];
    }

    it('asks HEVC where the browser decodes it', async () => {
        expect((await launchBody({ video_codec: 'hevc' })).video_codec).toBe('hevc');
    });

    it('asks H.264 where the probe says it does not', async () => {
        expect((await launchBody({ video_codec: 'hevc' }, { supported: false })).video_codec).toBe(
            'h264',
        );
    });

    it('asks H.264 where the browser said it does and was seen failing', async () => {
        noteHevcDecodeRefused();
        expect((await launchBody({ video_codec: 'hevc' })).video_codec).toBe('h264');
    });

    it('applies to Auto, which asks HEVC', async () => {
        noteHevcDecodeRefused();
        expect((await launchBody({ video_codec: 'auto' })).video_codec).toBe('h264');
    });

    it('leaves another codec alone', async () => {
        noteHevcDecodeRefused();
        expect((await launchBody({ video_codec: 'av1' })).video_codec).toBe('av1');
    });

    it('leaves a fallback relaunch its forced codec, and starts the chain over for it', async () => {
        noteHevcDecodeRefused();
        const body = await launchBody(
            { video_codec: 'h264' },
            { codecOverride: 'hevc', hdrOverride: false },
        );
        expect(body.video_codec).toBe('hevc');
        expect(MoonlightApp._firstTransportRetried).toBe(false);
        expect(MoonlightApp._launchRetried).toBe(false);
    });

    it('gives a fresh budget to a launch the user starts', async () => {
        MoonlightApp._relaunchBudget = { codec: 4, total: 12 };
        await launchBody({ video_codec: 'h264' });
        expect(MoonlightApp._relaunchBudget).toEqual({ codec: 0, total: 0 });
    });
});

describe('_trustHevc leaves what has its own probe alone', () => {
    it.each([
        [{ video_codec: 'hevc', hdr_enabled: true }],
        [{ video_codec: 'hevc', chroma_444_enabled: true }],
        [{ video_codec: 'hevc', ultra_codec: 'pyrowave' }],
    ])('%j', async (settings) => {
        noteHevcDecodeRefused();
        const s = { ...settings };
        await MoonlightApp._trustHevc(s, undefined);
        expect(s.video_codec).toBe('hevc');
    });
});
