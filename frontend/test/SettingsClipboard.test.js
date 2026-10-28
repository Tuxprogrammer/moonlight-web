/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The clipboard's two per-browser settings on the Settings page: "sync my
 * clipboard to the host when the stream gets focus" and "Ctrl+V types my
 * clipboard". Both off by default; both kept in mw-streaming-settings, where
 * ClipboardPanel reads them at the moment they matter.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/stream/ultra/UltraPlayer.js', () => ({
    ultraPlayerSupported: () => false,
    presentMode: () => null,
    UltraPlayer: class {},
}));
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(async () => ({ has_session: false })),
        getStreamingSettings: vi.fn(async () => ({})),
        saveStreamingSettings: vi.fn(async () => ({})),
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
import { readClipboardPrefs } from '../js/ui/ClipboardPanel.js';

const stored = () => JSON.parse(localStorage.getItem('mw-streaming-settings') || '{}');

describe('SettingsView: the clipboard settings', () => {
    let view;
    const $ = (sel) => view.container.querySelector(sel);
    const mount = () => {
        view.render();
        view.bindEvents();
    };

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
        view._codecSupport = { h264: true, hevc: true, av1: true };
    });

    it('shows both, unticked, by default', () => {
        mount();
        expect($('#settings-clipboard-sync-focus').checked).toBe(false);
        expect($('#settings-clipboard-ctrlv-types').checked).toBe(false);
        expect(view.container.innerHTML).toContain('text:settings.clipboardSyncOnFocusDesc');
        expect(view.container.innerHTML).toContain('text:settings.clipboardCtrlVTypesDesc');
    });

    it('saves a tick where the clipboard panel reads it', async () => {
        mount();
        const sync = $('#settings-clipboard-sync-focus');
        sync.checked = true;
        sync.dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(stored().clipboard_sync_on_focus).toBe(true));
        expect(readClipboardPrefs()).toEqual({ syncOnFocus: true, ctrlVTypes: false });

        const types = $('#settings-clipboard-ctrlv-types');
        types.checked = true;
        types.dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(stored().clipboard_ctrlv_types).toBe(true));
        expect(readClipboardPrefs()).toEqual({ syncOnFocus: true, ctrlVTypes: true });
    });

    it('reads them back from storage, and keeps them through another save', async () => {
        view._applySettings({ clipboard_sync_on_focus: true, clipboard_ctrlv_types: true });
        mount();
        expect($('#settings-clipboard-sync-focus').checked).toBe(true);
        expect($('#settings-clipboard-ctrlv-types').checked).toBe(true);
        await view._saveToStorage();
        expect(stored().clipboard_sync_on_focus).toBe(true);
        expect(stored().clipboard_ctrlv_types).toBe(true);
    });

    it('a value that is not true reads as off', () => {
        view._applySettings({ clipboard_sync_on_focus: 'true', clipboard_ctrlv_types: 1 });
        expect(view._clipboardSyncOnFocus).toBe(false);
        expect(view._clipboardCtrlVTypes).toBe(false);
    });

    it('reset puts both off again', async () => {
        view._applySettings({ clipboard_sync_on_focus: true, clipboard_ctrlv_types: true });
        await view._resetDefaults();
        expect(stored().clipboard_sync_on_focus).toBe(false);
        expect(stored().clipboard_ctrlv_types).toBe(false);
    });
});
