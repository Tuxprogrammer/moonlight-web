/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// The admin's "Advanced" section: the reverse proxies whose X-Forwarded-For
// the server believes (trusted_proxies). One address or network per line,
// saved with its own button, shown back as the server stored it, read-only
// when MW_TRUSTED_PROXIES sets it, and absent for a backend without it.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: { getAdminSettings: vi.fn(), saveAdminSettings: vi.fn() },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

/** A rejection shaped like BackendClient._handleError's. */
function backendError(status, body) {
    const err = new Error((body && body.error) || `Request failed (${status})`);
    err.statusCode = status;
    err.responseBody = body;
    return err;
}

describe('AdminView — trusted reverse proxies (Advanced)', () => {
    let view;

    const area = () => document.querySelector('#admin-trusted-proxies');
    const saveBtn = () => document.querySelector('#btn-trusted-proxies-save');
    const mount = () => {
        document.body.innerHTML = `<div>${view._renderAdvanced()}</div>`;
        view.container = document.body;
        view.bindEvents();
    };
    const type = (text) => {
        area().value = text;
        area().dispatchEvent(new Event('input'));
    };

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        BackendClient.getAdminSettings.mockReset();
        BackendClient.saveAdminSettings.mockReset();
        Toast.success.mockReset();
        Toast.error.mockReset();
    });

    afterEach(() => {
        view.destroy();
    });

    it('reads the list, whether the environment holds it, and the cap', async () => {
        BackendClient.getAdminSettings.mockResolvedValue({
            trusted_proxies: ['192.0.2.10/32', '2001:db8::/48'],
            trusted_proxies_env: false,
            trusted_proxies_max: 64,
        });
        await view._loadState();
        expect(view._trustedProxiesSupported).toBe(true);
        expect(view._trustedProxies).toEqual(['192.0.2.10/32', '2001:db8::/48']);
        expect(view._trustedProxiesEnv).toBe(false);
        expect(view._trustedProxiesMax).toBe(64);
    });

    it('is absent for a backend that does not know the setting', async () => {
        BackendClient.getAdminSettings.mockResolvedValue({ https_port: 443 });
        await view._loadState();
        expect(view._trustedProxiesSupported).toBe(false);
        const html = view._renderAdvanced();
        expect(html).not.toContain('admin-trusted-proxies');
        // The rest of the section is still there.
        expect(html).toContain('btn-download-logs');
    });

    it('shows one entry per line, the save button off until something changes', () => {
        view._trustedProxiesSupported = true;
        view._trustedProxies = ['192.0.2.10/32', '10.0.0.0/8'];
        mount();
        expect(area().value).toBe('192.0.2.10/32\n10.0.0.0/8');
        expect(area().readOnly).toBe(false);
        expect(saveBtn().disabled).toBe(true);
        expect(document.body.innerHTML).toContain('admin.trustedProxiesDesc');
        expect(document.body.innerHTML).toContain('admin.trustedProxiesHint');

        type('192.0.2.10/32\n10.0.0.0/8\n');
        // Spacing alone is not a change.
        expect(saveBtn().disabled).toBe(true);
        type('192.0.2.10/32, 10.0.0.0/8 192.0.2.11');
        expect(saveBtn().disabled).toBe(false);
        type('192.0.2.10/32 10.0.0.0/8');
        expect(saveBtn().disabled).toBe(true);
    });

    it('escapes what a hand-edited settings file holds', () => {
        view._trustedProxiesSupported = true;
        view._trustedProxies = ['</textarea><img src=x onerror=alert(1)>'];
        mount();
        expect(document.querySelector('img')).toBeNull();
        expect(area().value).toBe('</textarea><img src=x onerror=alert(1)>');
    });

    it('is read-only, with no save button, when MW_TRUSTED_PROXIES sets it', () => {
        view._trustedProxiesSupported = true;
        view._trustedProxiesEnv = true;
        view._trustedProxies = ['203.0.113.7'];
        mount();
        expect(area().readOnly).toBe(true);
        expect(saveBtn()).toBeNull();
        expect(document.body.innerHTML).toContain('admin.trustedProxiesEnv');
    });

    it('saves the entries as a list and shows what the server kept', async () => {
        BackendClient.saveAdminSettings.mockResolvedValue({
            trusted_proxies: ['192.0.2.10/32', '2001:db8::/48'],
        });
        view._trustedProxiesSupported = true;
        mount();
        type(' 192.0.2.10,\n2001:db8::1/48 ');
        saveBtn().click();
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledWith({
            trusted_proxies: ['192.0.2.10', '2001:db8::1/48'],
        });
        expect(view._trustedProxies).toEqual(['192.0.2.10/32', '2001:db8::/48']);
        expect(area().value).toBe('192.0.2.10/32\n2001:db8::/48');
        expect(Toast.success).toHaveBeenCalledWith('admin.trustedProxiesSaved');
        expect(saveBtn().disabled).toBe(true);
    });

    it('says so when the list is emptied', async () => {
        BackendClient.saveAdminSettings.mockResolvedValue({ trusted_proxies: [] });
        view._trustedProxiesSupported = true;
        view._trustedProxies = ['192.0.2.10/32'];
        mount();
        type('');
        saveBtn().click();
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(BackendClient.saveAdminSettings).toHaveBeenCalledWith({ trusted_proxies: [] });
        expect(view._trustedProxies).toEqual([]);
        expect(Toast.success).toHaveBeenCalledWith('admin.trustedProxiesCleared');
    });

    it('names the refused entries and keeps what was typed', async () => {
        BackendClient.saveAdminSettings.mockRejectedValue(
            backendError(400, {
                error: 'invalid_trusted_proxies',
                invalid: ['proxy.example', '0.0.0.0/0'],
                max: 64,
            }),
        );
        view._trustedProxiesSupported = true;
        view._trustedProxies = ['192.0.2.10/32'];
        mount();
        type('192.0.2.10/32\nproxy.example\n0.0.0.0/0');
        saveBtn().click();
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(Toast.error).toHaveBeenCalledWith('admin.trustedProxiesInvalid');
        expect(view._trustedProxies).toEqual(['192.0.2.10/32']);
        expect(area().value).toBe('192.0.2.10/32\nproxy.example\n0.0.0.0/0');
        expect(saveBtn().disabled).toBe(false);
        expect(Toast.success).not.toHaveBeenCalled();
    });

    it('turns read-only when the server says the environment holds the list', async () => {
        BackendClient.saveAdminSettings.mockRejectedValue(
            backendError(409, { error: 'set_by_environment' }),
        );
        view._trustedProxiesSupported = true;
        mount();
        type('192.0.2.10');
        saveBtn().click();
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(Toast.error).toHaveBeenCalledWith('admin.trustedProxiesEnv');
        expect(view._trustedProxiesEnv).toBe(true);
        expect(area().readOnly).toBe(true);
    });

    it('reports any other failure and keeps the stored list', async () => {
        BackendClient.saveAdminSettings.mockRejectedValue(new Error('backend busy'));
        view._trustedProxiesSupported = true;
        view._trustedProxies = ['192.0.2.10/32'];
        mount();
        type('192.0.2.11');
        saveBtn().click();
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(Toast.error).toHaveBeenCalledWith('admin.saveFailed');
        expect(view._trustedProxies).toEqual(['192.0.2.10/32']);
        expect(saveBtn().disabled).toBe(false);
    });

    it('does nothing when save is pressed with nothing changed', async () => {
        view._trustedProxiesSupported = true;
        view._trustedProxies = ['192.0.2.10/32'];
        mount();
        await view._saveTrustedProxies();
        expect(BackendClient.saveAdminSettings).not.toHaveBeenCalled();
    });

    it('splits on commas, spaces and new lines, dropping empties', () => {
        expect(AdminView.parseTrustedProxies(' a,b\n\nc  d,, ')).toEqual(['a', 'b', 'c', 'd']);
        expect(AdminView.parseTrustedProxies('')).toEqual([]);
        expect(AdminView.parseTrustedProxies(' \n , ')).toEqual([]);
    });
});
