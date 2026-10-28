/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A guest of the shared feed whose browser failed HEVC before asks for H.264
 * when joining, as one whose browser says outright that it cannot: the whole
 * feed goes H.264 once instead of this guest failing its first picture again.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

const trusted = vi.fn();
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { playerInfo: vi.fn(), playerPin: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
}));
vi.mock('../js/util/BrowserDetect.js', () => ({
    IS_MOBILE_OR_TABLET: false,
    IS_TOUCH_DEVICE: false,
    hevcDecodeTrusted: () => trusted(),
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

describe('PlayerJoinView: the codec a guest asks for', () => {
    let container;

    const join = async (info) => {
        const onJoin = vi.fn(async () => {});
        const view = new PlayerJoinView(container, 'tok', onJoin);
        BackendClient.playerInfo.mockResolvedValue({ ...BASE, ...info });
        await view.refresh();
        container.querySelector('.player-join-btn').click();
        await vi.waitFor(() => expect(onJoin).toHaveBeenCalled());
        return onJoin.mock.calls[0];
    };

    beforeEach(() => {
        document.body.innerHTML = '<div id="player"></div>';
        container = document.getElementById('player');
        localStorage.clear();
        vi.clearAllMocks();
    });

    it('is H.264 on the shared feed when HEVC is not trusted here', async () => {
        trusted.mockResolvedValue(false);
        const [, index, codec] = await join({ shared_feed: true });
        expect(index).toBe(0);
        expect(codec).toBe('h264');
    });

    it("is the backend's pick when HEVC is trusted", async () => {
        trusted.mockResolvedValue(true);
        const [, , codec] = await join({ shared_feed: true });
        expect(codec).toBeUndefined();
    });

    it('is not asked off the shared feed', async () => {
        trusted.mockResolvedValue(false);
        const [, , codec] = await join({ shared_feed: false });
        expect(codec).toBeUndefined();
        expect(trusted).not.toHaveBeenCalled();
    });
});
