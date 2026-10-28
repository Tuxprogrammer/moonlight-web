/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, describe, expect, it, vi } from 'vitest';
import { BackendClient } from '../js/api/BackendClient.js';
import { resetWebrtcProbe } from '../js/api/WebRtcSupport.js';

/**
 * /start and a guest's join say whether this browser has WebRTC
 * (`capabilities: { webrtc }`): a backend that reads it drops every webrtc-*
 * rung, so a browser without WebRTC never even tries one. An older backend
 * ignores the field.
 */

class GoodPc {
    close() {}
}

afterEach(() => {
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
    resetWebrtcProbe();
});

function capture() {
    return vi.spyOn(BackendClient, 'post').mockResolvedValue({ status: 'streaming' });
}

describe('launch body capabilities', () => {
    it('says webrtc: false where the browser has none', async () => {
        vi.stubGlobal('RTCPeerConnection', undefined);
        resetWebrtcProbe();
        const post = capture();
        await BackendClient.launchApp('host-1', 7, { video_codec: 'h264' });
        expect(post.mock.calls[0][0]).toBe('/api/hosts/host-1/start');
        expect(post.mock.calls[0][1].capabilities).toEqual({ webrtc: false });
        expect(post.mock.calls[0][1].video_codec).toBe('h264');
    });

    it('says webrtc: true where it has it', async () => {
        vi.stubGlobal('RTCPeerConnection', GoodPc);
        resetWebrtcProbe();
        const post = capture();
        await BackendClient.launchApp('host-1', 7, {});
        expect(post.mock.calls[0][1].capabilities).toEqual({ webrtc: true });
    });

    it('is sent on every relaunch too (the same call, a transport_index added)', async () => {
        vi.stubGlobal('RTCPeerConnection', undefined);
        resetWebrtcProbe();
        const post = capture();
        await BackendClient.launchApp('host-1', 7, { transport_index: 3 });
        expect(post.mock.calls[0][1]).toMatchObject({
            transport_index: 3,
            capabilities: { webrtc: false },
        });
    });

    it("is in a guest's join", async () => {
        vi.stubGlobal('RTCPeerConnection', undefined);
        resetWebrtcProbe();
        const post = capture();
        await BackendClient.playerJoin('tok', 720, undefined, 2);
        expect(post.mock.calls[0][0]).toBe('/api/share/player/join');
        expect(post.mock.calls[0][1]).toMatchObject({
            token: 'tok',
            transport_index: 2,
            capabilities: { webrtc: false },
        });
    });
});
