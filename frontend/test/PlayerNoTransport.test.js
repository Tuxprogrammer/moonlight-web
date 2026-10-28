/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A guest whose browser has WebRTC off, on a server that offers no WebSocket
 * streaming: the join answers 409 no_usable_transport. The join screen says
 * why, in words the guest can act on, instead of "could not join".
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { playerInfo: vi.fn(), playerPin: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));
vi.mock('../js/util/BrowserDetect.js', () => ({
    IS_MOBILE_OR_TABLET: false,
    IS_TOUCH_DEVICE: false,
    hevcDecodeTrusted: async () => true,
}));

import { PlayerJoinView } from '../js/ui/PlayerJoinView.js';
import { BackendClient } from '../js/api/BackendClient.js';

const BASE = {
    needs_pin: false,
    machine_name: 'ATELIER',
    state: 'idle',
    cold_start: false,
    permissions: {},
    access_level: 'viewer',
};

const refusal = (statusCode, responseBody) =>
    Object.assign(new Error('refused'), { statusCode, responseBody });

describe('PlayerJoinView: no transport this browser can use', () => {
    let container;
    let onJoin;

    const join = async () => {
        const view = new PlayerJoinView(container, 'tok', onJoin);
        BackendClient.playerInfo.mockResolvedValue(BASE);
        await view.refresh();
        container.querySelector('.player-join-btn').click();
        await vi.waitFor(() => {
            if (container.querySelector('.player-join-error').hidden) throw new Error('not yet');
        });
        return container.querySelector('.player-join-error').textContent;
    };

    beforeEach(() => {
        document.body.innerHTML = '<div id="player"></div>';
        container = document.getElementById('player');
        localStorage.clear();
        vi.clearAllMocks();
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    it.each([
        ['webrtc_unavailable', 'text:transport.noUsable.webrtc_unavailable'],
        ['policy', 'text:transport.noUsable.policy'],
        ['tunnel', 'text:transport.noUsable.tunnel'],
        ['something new', 'text:transport.noUsable.generic'],
    ])('says why for "%s"', async (reason, text) => {
        onJoin = vi.fn().mockRejectedValue(refusal(409, { error: 'no_usable_transport', reason }));
        expect(await join()).toBe(text);
        expect(onJoin).toHaveBeenCalledTimes(1);
        // The button comes back: the guest may turn WebRTC on and try again.
        expect(container.querySelector('.player-join-btn').disabled).toBe(false);
    });

    it('keeps the other answers as they were', async () => {
        onJoin = vi.fn().mockRejectedValue(refusal(409, { error: 'stream_in_progress' }));
        expect(await join()).toBe('text:player.busy');
        onJoin = vi.fn().mockRejectedValue(refusal(502, { error: 'no_usable_transport' }));
        expect(await join()).toBe('text:player.joinFailed');
    });
});
