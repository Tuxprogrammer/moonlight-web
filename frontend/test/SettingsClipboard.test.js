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
import { readClipboardPrefs, readTypeMethod } from '../js/ui/ClipboardPanel.js';

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

    describe('Type using key presses', () => {
        it('is a choice of three, Automatic by default, with its explanation', () => {
            mount();
            const sel = $('#settings-clipboard-type-method');
            expect(sel.tagName).toBe('SELECT');
            expect([...sel.options].map((o) => o.value)).toEqual(['auto', 'keys', 'text']);
            expect([...sel.options].map((o) => o.textContent)).toEqual([
                'text:settings.clipboardTypeMethodAuto',
                'text:settings.clipboardTypeMethodKeys',
                'text:settings.clipboardTypeMethodText',
            ]);
            expect(sel.value).toBe('auto');
            expect($('label[for="settings-clipboard-type-method"]').textContent.trim()).toBe(
                'text:settings.clipboardTypeMethod',
            );
            expect(view.container.innerHTML).toContain('text:settings.clipboardTypeMethodDesc');
        });

        it('saves a choice where the clipboard panel reads it', async () => {
            mount();
            const sel = $('#settings-clipboard-type-method');
            sel.value = 'keys';
            sel.dispatchEvent(new Event('change'));
            await vi.waitFor(() => expect(stored().clipboard_type_method).toBe('keys'));
            expect(readTypeMethod()).toBe('keys');
            sel.value = 'text';
            sel.dispatchEvent(new Event('change'));
            await vi.waitFor(() => expect(stored().clipboard_type_method).toBe('text'));
            expect(readTypeMethod()).toBe('text');
        });

        it('reads it back from storage, and keeps it through another save', async () => {
            view._applySettings({ clipboard_type_method: 'text' });
            mount();
            expect($('#settings-clipboard-type-method').value).toBe('text');
            await view._saveToStorage();
            expect(stored().clipboard_type_method).toBe('text');
        });

        it('an unknown value reads as Automatic', () => {
            view._applySettings({ clipboard_type_method: 'morse' });
            expect(view._clipboardTypeMethod).toBe('auto');
            view._applySettings({ clipboard_type_method: 1 });
            expect(view._clipboardTypeMethod).toBe('auto');
        });

        it('a value picked outside the list is not saved', async () => {
            mount();
            const sel = $('#settings-clipboard-type-method');
            const opt = document.createElement('option');
            opt.value = 'bogus';
            sel.appendChild(opt);
            sel.value = 'bogus';
            sel.dispatchEvent(new Event('change'));
            await view._saveToStorage();
            expect(stored().clipboard_type_method).toBe('auto');
        });

        it('reset puts it back on Automatic', async () => {
            view._applySettings({ clipboard_type_method: 'keys' });
            await view._resetDefaults();
            expect(stored().clipboard_type_method).toBe('auto');
        });
    });
});
