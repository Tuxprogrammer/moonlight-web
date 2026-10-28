/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, describe, expect, it, vi } from 'vitest';
import {
    HEVC_REFUSED_DAYS,
    forgetHevcDecodeRefused,
    hevcClientDecodes,
    hevcDecodeRefused,
    hevcDecodeTrusted,
    noteHevcDecodeRefused,
    resetHevcProbe,
} from '../js/util/BrowserDetect.js';
import { decodeFailedOnHevc } from '../js/app.js';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * Firefox says yes to isConfigSupported for HEVC and then decodes nothing: the
 * stream fails its first picture, falls back to H.264, and the next launch
 * did the same again. The browser now keeps that verdict, under its user
 * agent, for a while, and later launches ask H.264 at once.
 */

const DAY = 24 * 3600 * 1000;
const UA = 'Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0';
const KEY = 'mw_hevc_refused_v1';

/** A Storage that can be told to fail. */
function store({ throwOnGet = false, throwOnSet = false } = {}) {
    const m = new Map();
    return {
        map: m,
        getItem: (k) => {
            if (throwOnGet) throw new Error('denied');
            return m.has(k) ? m.get(k) : null;
        },
        setItem: (k, v) => {
            if (throwOnSet) throw new Error('quota');
            m.set(k, String(v));
        },
        removeItem: (k) => m.delete(k),
    };
}

afterEach(() => {
    vi.unstubAllGlobals();
    resetHevcProbe();
});

describe('the HEVC verdict', () => {
    it('is absent until noted, and holds once noted', () => {
        const s = store();
        expect(hevcDecodeRefused(UA, s, 1000)).toBe(false);
        noteHevcDecodeRefused(UA, s, 1000);
        expect(hevcDecodeRefused(UA, s, 1000)).toBe(true);
        expect(hevcDecodeRefused(UA, s, 1000 + 29 * DAY)).toBe(true);
    });

    it(`runs out after ${HEVC_REFUSED_DAYS} days`, () => {
        const s = store();
        noteHevcDecodeRefused(UA, s, 1000);
        expect(hevcDecodeRefused(UA, s, 1000 + HEVC_REFUSED_DAYS * DAY)).toBe(false);
    });

    it('does not hold for a date ahead of the clock', () => {
        const s = store();
        noteHevcDecodeRefused(UA, s, 10 * DAY);
        expect(hevcDecodeRefused(UA, s, 5 * DAY)).toBe(false);
    });

    it('belongs to one browser version: a new user agent asks again', () => {
        const s = store();
        noteHevcDecodeRefused(UA, s, 1000);
        expect(hevcDecodeRefused(UA.replace('140.0', '141.0'), s, 1000)).toBe(false);
    });

    it('is kept under a versioned key, and drops the verdicts past their time', () => {
        const s = store();
        noteHevcDecodeRefused('old', s, 1000);
        noteHevcDecodeRefused(UA, s, 1000 + 40 * DAY);
        const kept = JSON.parse(s.map.get(KEY));
        expect(Object.keys(kept)).toEqual([UA]);
        expect(kept[UA]).toBe(1000 + 40 * DAY);
    });

    it('is forgotten on demand, the key with it when nothing is left', () => {
        const s = store();
        noteHevcDecodeRefused('other', s, 1000);
        noteHevcDecodeRefused(UA, s, 1000);
        forgetHevcDecodeRefused(UA, s);
        expect(hevcDecodeRefused(UA, s, 1000)).toBe(false);
        expect(hevcDecodeRefused('other', s, 1000)).toBe(true);
        forgetHevcDecodeRefused('other', s);
        expect(s.map.has(KEY)).toBe(false);
        // Forgetting what is not there changes nothing.
        forgetHevcDecodeRefused(UA, s);
        expect(s.map.size).toBe(0);
    });

    it('reads a damaged record as no verdict', () => {
        for (const raw of ['not json', '[1,2]', 'null', '42', '{"x": "yes"}']) {
            const s = store();
            s.map.set(KEY, raw);
            expect(hevcDecodeRefused('x', s, 1000), raw).toBe(false);
            noteHevcDecodeRefused(UA, s, 1000);
            expect(hevcDecodeRefused(UA, s, 1000), raw).toBe(true);
        }
    });

    it('survives storage that refuses (private mode, sandboxed frame)', () => {
        expect(hevcDecodeRefused(UA, store({ throwOnGet: true }), 1000)).toBe(false);
        expect(() => noteHevcDecodeRefused(UA, store({ throwOnSet: true }), 1000)).not.toThrow();
        expect(() => noteHevcDecodeRefused(UA, null, 1000)).not.toThrow();
        expect(() => forgetHevcDecodeRefused(UA, null)).not.toThrow();
        expect(() => forgetHevcDecodeRefused(UA, store({ throwOnGet: true }))).not.toThrow();
        expect(hevcDecodeRefused(UA, null, 1000)).toBe(false);
    });

    it('uses this browser and localStorage by default', () => {
        localStorage.removeItem(KEY);
        expect(hevcDecodeRefused()).toBe(false);
        noteHevcDecodeRefused();
        expect(hevcDecodeRefused()).toBe(true);
        expect(JSON.parse(localStorage.getItem(KEY))[navigator.userAgent]).toBeTypeOf('number');
        forgetHevcDecodeRefused();
        expect(hevcDecodeRefused()).toBe(false);
    });
});

describe('hevcDecodeTrusted', () => {
    const decoder = (supported) =>
        vi.stubGlobal('VideoDecoder', {
            isConfigSupported: vi.fn(async () => ({ supported })),
        });

    it("is the browser's answer when it has no verdict against it", async () => {
        decoder(true);
        await expect(hevcDecodeTrusted(UA, store(), 1000)).resolves.toBe(true);
        resetHevcProbe();
        decoder(false);
        await expect(hevcDecodeTrusted(UA, store(), 1000)).resolves.toBe(false);
    });

    it('is no once the browser failed to keep its word, whatever it answers', async () => {
        decoder(true);
        const s = store();
        noteHevcDecodeRefused(UA, s, 1000);
        await expect(hevcDecodeTrusted(UA, s, 1000)).resolves.toBe(false);
        await expect(hevcDecodeTrusted(UA, s, 1000 + 31 * DAY)).resolves.toBe(true);
    });

    it('asks the browser once per page, until reset', async () => {
        decoder(true);
        await hevcClientDecodes();
        await hevcClientDecodes();
        expect(globalThis.VideoDecoder.isConfigSupported).toHaveBeenCalledTimes(1);
        resetHevcProbe();
        await hevcClientDecodes();
        expect(globalThis.VideoDecoder.isConfigSupported).toHaveBeenCalledTimes(2);
    });
});

describe('decodeFailedOnHevc: which endings are a verdict on HEVC', () => {
    const view = (over = {}) => ({
        videoCodec: 'hevc',
        _hdrEnabled: false,
        _yuv444: false,
        ...over,
    });
    const h264 = { codec: 'h264', hdr: false };

    it('is HEVC SDR falling back to H.264 because the decoder failed', () => {
        expect(decodeFailedOnHevc(view(), h264)).toBe(true);
        expect(decodeFailedOnHevc(view({ videoCodec: 'HEVC' }), h264)).toBe(true);
        // Auto resolves to HEVC.
        expect(decodeFailedOnHevc(view({ videoCodec: 'auto' }), h264)).toBe(true);
    });

    it('is not an HDR or 4:4:4 stream failing: each has its own probe', () => {
        expect(decodeFailedOnHevc(view({ _hdrEnabled: true }), h264)).toBe(false);
        expect(decodeFailedOnHevc(view({ _yuv444: true }), h264)).toBe(false);
    });

    it('is not another codec, another target, or a fallback with a reason of its own', () => {
        expect(decodeFailedOnHevc(view({ videoCodec: 'av1' }), h264)).toBe(false);
        expect(decodeFailedOnHevc(view({ videoCodec: 'h264' }), h264)).toBe(false);
        expect(decodeFailedOnHevc(view(), { codec: 'av1', hdr: true })).toBe(false);
        for (const reason of ['silent-decoder', 'shared-feed', 'opaque-hdr-frames'])
            expect(decodeFailedOnHevc(view(), { ...h264, reason }), reason).toBe(false);
    });

    it('is nothing without a view or a target', () => {
        expect(decodeFailedOnHevc(null, h264)).toBe(false);
        expect(decodeFailedOnHevc(view(), null)).toBe(false);
        expect(decodeFailedOnHevc(view({ videoCodec: undefined }), h264)).toBe(false);
    });
});

describe('StreamView: the first picture', () => {
    /** _markFirstFrame on a view reduced to what its first lines read. */
    const markFirst = (over) => {
        const sv = {
            _firstFrameRendered: false,
            _transport: 'webrtc-dc-udp',
            videoCodec: 'hevc',
            _clearStalePoster: () => {},
            setStatus: () => {},
            _standby: true,
            ...over,
        };
        try {
            StreamView.prototype._markFirstFrame.call(sv);
        } catch (_e) {
            // The rest of the overlay work needs a real view; not asked here.
        }
        return sv;
    };

    afterEach(() => localStorage.removeItem(KEY));

    it('is when the view proved itself, for the relaunch budget', () => {
        vi.spyOn(performance, 'now').mockReturnValue(123456);
        expect(markFirst({ videoCodec: 'h264' })._firstFrameAt).toBe(123456);
        vi.restoreAllMocks();
    });

    it('drops an old HEVC verdict once HEVC decodes through WebCodecs after all', () => {
        noteHevcDecodeRefused();
        markFirst({});
        expect(hevcDecodeRefused()).toBe(false);
    });

    it('keeps it for a native <video> stream or another codec, and on later frames', () => {
        noteHevcDecodeRefused();
        markFirst({ _transport: 'webrtc-media' });
        markFirst({ videoCodec: 'h264' });
        markFirst({ _firstFrameRendered: true });
        expect(hevcDecodeRefused()).toBe(true);
    });
});
