/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// The admin's « Allowed transports » (allowed_transports): All / WebRTC only /
// WebSocket only, beside « Transport Mode », which orders what it allows. Read
// from /api/internet/status, saved the moment it changes, and the transport
// modes on offer redrawn from the status that follows; a refusal puts the
// stored value back.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getInternetStatus: vi.fn(),
        saveStreamingSettings: vi.fn(),
        getStreamingSettings: vi.fn(async () => ({})),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

describe('AdminView — allowed transports', () => {
    let view;

    const allowed = () => document.querySelector('#select-allowed-transports');
    const mode = () => document.querySelector('#select-transport-mode');
    const values = (sel) => Array.from(sel.options).map((o) => o.value);
    const mount = () => {
        document.body.innerHTML = `<div>${view._renderTransportFields()}</div>`;
        view.container = document.body;
        view.bindEvents();
    };

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        BackendClient.getInternetStatus.mockReset();
        BackendClient.saveStreamingSettings.mockReset();
        Toast.success.mockReset();
        Toast.error.mockReset();
        // The full page is not under test here: a save redraws only the fields.
        view.render = vi.fn(() => mount());
    });

    afterEach(() => {
        view.destroy();
    });

    it('reads the policy from the internet status', async () => {
        BackendClient.getInternetStatus.mockResolvedValue({
            allowed_transports: 'wss',
            available_transports: ['wss'],
            transport_mode: 'auto',
        });
        await view._loadInternetState();
        expect(view._allowedTransports).toBe('wss');
        expect(view._availableTransports).toEqual(['wss']);
    });

    it('reads "all" from an older backend, or a value it does not know', async () => {
        BackendClient.getInternetStatus.mockResolvedValue({ available_transports: [] });
        await view._loadInternetState();
        expect(view._allowedTransports).toBe('all');
        BackendClient.getInternetStatus.mockResolvedValue({ allowed_transports: 'udp' });
        await view._loadInternetState();
        expect(view._allowedTransports).toBe('all');
    });

    it('offers All, WebRTC only and WebSocket only, the stored one selected, with its hint', () => {
        view._allowedTransports = 'webrtc';
        mount();
        expect(values(allowed())).toEqual(['all', 'webrtc', 'wss']);
        expect(allowed().value).toBe('webrtc');
        const labels = Array.from(allowed().options).map((o) => o.textContent);
        expect(labels).toEqual([
            'admin.allowedTransportsAll',
            'admin.allowedTransportsWebrtc',
            'admin.allowedTransportsWss',
        ]);
        expect(document.body.textContent).toContain('admin.allowedTransportsHint');
        // Next to the transport mode, which keeps its place and its hint.
        expect(mode()).not.toBeNull();
        expect(document.body.textContent).toContain('admin.transportModeHint');
    });

    it('offers the transport modes the policy leaves', () => {
        view._availableTransports = ['webrtc-dc-udp', 'webrtc-dc-tcp'];
        view._transportMode = 'auto';
        mount();
        expect(values(mode())).toEqual(['auto', 'webrtc-dc-udp', 'webrtc-dc-tcp']);
        // WebSocket only: wss is offered, so Auto saves "all" (the list survives).
        view._availableTransports = ['wss'];
        view._transportMode = 'auto';
        mount();
        expect(values(mode())).toEqual(['all', 'wss']);
        expect(mode().value).toBe('all');
        expect(mode().options[1].textContent).toContain('admin.transportLanIpOnly');
    });

    it('keeps a stored mode the policy no longer offers in the list', () => {
        view._availableTransports = ['webrtc-dc-udp'];
        view._transportMode = 'wss';
        mount();
        expect(values(mode())).toEqual(['auto', 'webrtc-dc-udp', 'wss']);
        expect(mode().value).toBe('wss');
    });

    it('saves a change at once, then redraws from the status that follows', async () => {
        BackendClient.saveStreamingSettings.mockResolvedValue({ allowed_transports: 'wss' });
        BackendClient.getInternetStatus.mockResolvedValue({
            allowed_transports: 'wss',
            available_transports: ['wss'],
            transport_mode: 'auto',
        });
        view._availableTransports = ['webrtc-dc-udp', 'webrtc-dc-tcp'];
        mount();
        allowed().value = 'wss';
        allowed().dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(view.render).toHaveBeenCalled());
        expect(BackendClient.saveStreamingSettings).toHaveBeenCalledWith({
            allowed_transports: 'wss',
        });
        expect(Toast.success).toHaveBeenCalled();
        expect(view._allowedTransports).toBe('wss');
        expect(allowed().value).toBe('wss');
        expect(values(mode())).toEqual(['all', 'wss']);
    });

    it('puts the stored value back when the server refuses the change', async () => {
        BackendClient.saveStreamingSettings.mockRejectedValue(new Error('400'));
        view._allowedTransports = 'all';
        mount();
        allowed().value = 'webrtc';
        allowed().dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(view._allowedTransports).toBe('all');
        expect(allowed().value).toBe('all');
        expect(Toast.success).not.toHaveBeenCalled();
        expect(BackendClient.getInternetStatus).not.toHaveBeenCalled();
    });

    it('saves the transport mode on its own, as before', async () => {
        BackendClient.saveStreamingSettings.mockResolvedValue({});
        view._availableTransports = ['webrtc-dc-udp', 'webrtc-dc-tcp'];
        mount();
        mode().value = 'webrtc-dc-tcp';
        mode().dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(BackendClient.saveStreamingSettings).toHaveBeenCalledWith({
            transport_mode: 'webrtc-dc-tcp',
        });
    });
});
