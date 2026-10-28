/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Settings → Connection: Automatic, WebRTC or WebSocket, stored per browser as
 * `transport_pref` in mw-streaming-settings (and so sent in the launch body).
 * Offered only inside the admin's allowed transports (/api/internet/status)
 * and what this page can run: no WebRTC without RTCPeerConnection, no
 * WebSocket through the rendezvous. Nothing to choose, no field.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

const tunnel = vi.hoisted(() => ({ through: false }));
vi.mock('../js/net/tunnelBridge.js', () => ({
    pageCameThroughTunnel: () => tunnel.through,
}));
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(async () => ({ has_session: false })),
        getStreamingSettings: vi.fn(async () => ({})),
        saveStreamingSettings: vi.fn(async () => ({})),
        getInternetStatus: vi.fn(async () => ({})),
        getHosts: vi.fn(async () => ({ hosts: [] })),
        getMetricsReporting: vi.fn(),
        setMetricsReporting: vi.fn(async () => ({ enabled: true })),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
    getLanguage: () => 'en',
    setLanguage: vi.fn(),
    AVAILABLE_LANGUAGES: [{ code: 'en', label: 'English' }],
}));

import { SettingsView } from '../js/ui/SettingsView.js';
import { BackendClient } from '../js/api/BackendClient.js';

const STORAGE_KEY = 'mw-streaming-settings';

describe('SettingsView: Connection', () => {
    let view;
    const savedRtc = globalThis.RTCPeerConnection;
    const $ = (sel) => view.container.querySelector(sel);
    const select = () => $('#settings-transport-pref');
    const values = () => Array.from(select().options).map((o) => o.value);
    const mount = () => {
        view.render();
        view.bindEvents();
    };
    const stored = () => JSON.parse(localStorage.getItem(STORAGE_KEY) || '{}');

    beforeEach(() => {
        localStorage.clear();
        tunnel.through = false;
        globalThis.RTCPeerConnection = function RTCPeerConnection() {};
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
        view._codecSupport = { h264: true, hevc: true, av1: true };
    });

    afterEach(() => {
        globalThis.RTCPeerConnection = savedRtc;
        vi.useRealTimers();
    });

    it('offers Automatic, WebRTC and WebSocket under "all", Automatic selected', () => {
        mount();
        expect(values()).toEqual(['auto', 'webrtc', 'wss']);
        expect(select().value).toBe('auto');
        const labels = Array.from(select().options).map((o) => o.textContent);
        expect(labels).toEqual([
            'text:settings.connectionAuto',
            'text:settings.connectionWebrtc',
            'text:settings.connectionWss',
        ]);
        expect(view.container.textContent).toContain('text:settings.connection');
        expect(view.container.textContent).toContain('text:settings.connectionDesc');
    });

    it('shows the stored choice', () => {
        view._applySettings({ transport_pref: 'wss' });
        mount();
        expect(select().value).toBe('wss');
    });

    it('reads a stored value it does not know as Automatic', () => {
        view._applySettings({ transport_pref: 'webrtc-dc-udp' });
        expect(view._transportPref).toBe('auto');
    });

    it('has no field when only one family is left', () => {
        view._allowedTransports = 'wss';
        mount();
        expect(select()).toBeNull();
        view._allowedTransports = 'webrtc';
        mount();
        expect(select()).toBeNull();
    });

    it('offers no WebRTC to a browser without it, and so no field', () => {
        globalThis.RTCPeerConnection = undefined;
        mount();
        expect(select()).toBeNull();
    });

    it('offers no WebSocket through the rendezvous, and so no field', () => {
        tunnel.through = true;
        mount();
        expect(select()).toBeNull();
    });

    it('shows Automatic for a stored choice no longer on offer', () => {
        // WebSocket stored, then the admin allowed WebRTC only... and WebSocket
        // back: a page with two families shows the clamped value.
        view._applySettings({ transport_pref: 'wss' });
        view._allowedTransports = 'all';
        tunnel.through = false;
        mount();
        expect(select().value).toBe('wss');
        view._transportPref = 'nonsense';
        mount();
        expect(select().value).toBe('auto');
    });

    it('reads the policy from the internet status', async () => {
        BackendClient.getInternetStatus.mockResolvedValueOnce({ allowed_transports: 'webrtc' });
        await view._loadTransportPolicy();
        expect(view._allowedTransports).toBe('webrtc');
        BackendClient.getInternetStatus.mockResolvedValueOnce({ allowed_transports: 'bogus' });
        await view._loadTransportPolicy();
        expect(view._allowedTransports).toBe('all');
        BackendClient.getInternetStatus.mockResolvedValueOnce({});
        await view._loadTransportPolicy();
        expect(view._allowedTransports).toBe('all');
    });

    it('keeps "all" when the status cannot be read', async () => {
        view._allowedTransports = 'all';
        BackendClient.getInternetStatus.mockRejectedValueOnce(new Error('offline'));
        await view._loadTransportPolicy();
        expect(view._allowedTransports).toBe('all');
    });

    it('stores the choice in mw-streaming-settings as transport_pref', async () => {
        vi.useFakeTimers();
        mount();
        select().value = 'wss';
        select().dispatchEvent(new Event('change'));
        await vi.advanceTimersByTimeAsync(400);
        expect(view._transportPref).toBe('wss');
        expect(stored().transport_pref).toBe('wss');
    });

    it('keeps a stored choice when there is no field to read it from', async () => {
        vi.useFakeTimers();
        view._applySettings({ transport_pref: 'wss' });
        tunnel.through = true; // no WebSocket here: no field
        mount();
        expect(select()).toBeNull();
        view._autoSave();
        await vi.advanceTimersByTimeAsync(400);
        expect(stored().transport_pref).toBe('wss');
    });

    it('goes back to Automatic on reset', async () => {
        view._transportPref = 'webrtc';
        await view._resetDefaults();
        expect(view._transportPref).toBe('auto');
        expect(stored().transport_pref).toBe('auto');
    });

    it('reads the policy on start, before the first render', async () => {
        BackendClient.getInternetStatus.mockResolvedValue({ allowed_transports: 'wss' });
        // The browser probes are not under test here.
        view._checkCodecSupport = async () => ({ h264: true, hevc: true, av1: true });
        view._loadPyrowaveHost = async () => {};
        view._checkWebGpuSupport = async () => false;
        view._loadStatsConsent = async () => {};
        view._loadUpdateState = async () => {};
        await view.start();
        expect(BackendClient.getInternetStatus).toHaveBeenCalled();
        expect(view._allowedTransports).toBe('wss');
        expect(select()).toBeNull();
        view.destroy();
    });
});
