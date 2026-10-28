/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Admin page → sign-in with an identity provider: every setting on screen, the
 * client secret write-only (said to be set, never shown, sent only when typed),
 * the redirect URIs to register, a save the server takes all or nothing, and
 * "Test discovery".
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getOidcSettings: vi.fn(),
        saveOidcSettings: vi.fn(),
        testOidcDiscovery: vi.fn(),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, vars) => (vars ? `${key}:${JSON.stringify(vars)}` : key),
}));

import { AdminOidcSection, saveErrorMessage, splitList } from '../js/ui/AdminOidcSection.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const saved = () => ({
    enabled: true,
    issuer: 'https://idp.example.com/application/o/mw/',
    client_id: 'moonlightweb',
    client_secret_set: true,
    client_secret_source: 'settings',
    scopes: 'openid profile email',
    groups_claim: 'groups',
    allowed_groups: ['streamers', 'family'],
    admin_groups: ['mw-admins'],
    button_label: 'Authentik',
    disable_pin: false,
    session_ttl_hours: 24,
    public_url: ['https://mw.example.com', 'https://192.0.2.10:8443'],
    redirect_uris: [
        'https://mw.example.com/auth/oidc/callback',
        'https://192.0.2.10:8443/auth/oidc/callback',
    ],
    usable: true,
    problem: '',
});

describe('AdminOidcSection', () => {
    let section;
    let root;
    const $ = (sel) => root.querySelector(sel);

    const mount = async (state = saved()) => {
        BackendClient.getOidcSettings.mockResolvedValue(state);
        section = new AdminOidcSection();
        await section.load();
        document.body.innerHTML = `<div id="root">${section.render()}</div>`;
        root = document.getElementById('root');
        section.bind(root);
    };

    beforeEach(() => vi.clearAllMocks());

    it('splits lists the way the server reads them', () => {
        expect(splitList('a, b\nc,,a')).toEqual(['a', 'b', 'c']);
        expect(splitList('')).toEqual([]);
        expect(splitList(undefined)).toEqual([]);
    });

    it('is absent on a server without the route', async () => {
        BackendClient.getOidcSettings.mockRejectedValue(new Error('404'));
        section = new AdminOidcSection();
        await section.load();
        expect(section.state).toBe(null);
        expect(section.render()).toBe('');
        document.body.innerHTML = '<div id="root"></div>';
        expect(() => section.bind(document.getElementById('root'))).not.toThrow();
    });

    it('shows every setting, and the redirect URIs to register', async () => {
        await mount();
        expect($('#oidc-enabled').checked).toBe(true);
        expect($('#oidc-issuer').value).toBe('https://idp.example.com/application/o/mw/');
        expect($('#oidc-client-id').value).toBe('moonlightweb');
        expect($('#oidc-public-urls').value).toBe(
            'https://mw.example.com\nhttps://192.0.2.10:8443',
        );
        expect($('#oidc-allowed-groups').value).toBe('streamers, family');
        expect($('#oidc-admin-groups').value).toBe('mw-admins');
        expect($('#oidc-groups-claim').value).toBe('groups');
        expect($('#oidc-scopes').value).toBe('openid profile email');
        expect($('#oidc-label').value).toBe('Authentik');
        expect($('#oidc-ttl').value).toBe('24');
        expect($('#oidc-disable-pin').checked).toBe(false);
        expect($('#oidc-redirect-uris').textContent).toContain(
            'https://192.0.2.10:8443/auth/oidc/callback',
        );
    });

    it('never shows the secret, only that one is set and where it comes from', async () => {
        await mount();
        expect($('#oidc-client-secret').value).toBe('');
        expect($('#oidc-client-secret').type).toBe('password');
        expect(root.textContent).toContain('admin.oidcSecretSet');
        expect($('#oidc-clear-secret')).not.toBe(null);

        await mount({ ...saved(), client_secret_source: 'env' });
        expect(root.textContent).toContain('admin.oidcSecretEnv');
        expect($('#oidc-clear-secret')).toBe(null); // the file holds nothing to clear

        await mount({ ...saved(), client_secret_set: false, client_secret_source: 'none' });
        expect(root.textContent).toContain('admin.oidcSecretNone');
    });

    it('says what is missing when enabled but incomplete', async () => {
        await mount({ ...saved(), usable: false, problem: 'public_url' });
        expect(root.textContent).toContain('admin.oidcIncomplete');
        expect(root.textContent).toContain('admin.oidcPublicUrls');
    });

    it('escapes what the server stored', async () => {
        await mount({ ...saved(), button_label: '"><img src=x>', issuer: '<b>x</b>' });
        expect(root.querySelector('img')).toBe(null);
        expect(root.querySelector('b')).toBe(null);
        expect($('#oidc-label').value).toBe('"><img src=x>');
    });

    it('sends the form, the secret only when typed', async () => {
        await mount();
        let body = section.collect(root);
        expect(body).toEqual({
            enabled: true,
            issuer: 'https://idp.example.com/application/o/mw/',
            client_id: 'moonlightweb',
            public_url: ['https://mw.example.com', 'https://192.0.2.10:8443'],
            allowed_groups: ['streamers', 'family'],
            admin_groups: ['mw-admins'],
            groups_claim: 'groups',
            scopes: 'openid profile email',
            button_label: 'Authentik',
            session_ttl_hours: 24,
            disable_pin: false,
        });
        $('#oidc-client-secret').value = 'n3w';
        $('#oidc-clear-secret').checked = true;
        $('#oidc-disable-pin').checked = true;
        body = section.collect(root);
        expect(body.client_secret).toBe('n3w');
        expect(body.clear_client_secret).toBe(true);
        expect(body.disable_pin).toBe(true);
    });

    it('saves, re-renders what the server kept, and says if sessions ended', async () => {
        await mount();
        BackendClient.saveOidcSettings.mockResolvedValue({
            ...saved(),
            button_label: 'Keycloak',
            status: 'ok',
            sessions_ended: 2,
        });
        $('#oidc-label').value = 'Keycloak';
        $('#btn-oidc-save').click();
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalled());
        expect(BackendClient.saveOidcSettings.mock.calls[0][0].button_label).toBe('Keycloak');
        expect(Toast.success.mock.calls[0][0]).toBe('admin.oidcSavedEnded:{"count":2}');
        expect($('#oidc-label').value).toBe('Keycloak');

        BackendClient.saveOidcSettings.mockResolvedValue({ ...saved(), sessions_ended: 0 });
        $('#btn-oidc-save').click();
        await vi.waitFor(() => expect(Toast.success).toHaveBeenCalledTimes(2));
        expect(Toast.success.mock.calls[1][0]).toBe('admin.oidcSaved');
    });

    it('explains a refused save', async () => {
        await mount();
        const err = new Error('400');
        err.responseBody = { status: 'error', error: 'invalid_public_url' };
        BackendClient.saveOidcSettings.mockRejectedValue(err);
        $('#btn-oidc-save').click();
        await vi.waitFor(() => expect(Toast.error).toHaveBeenCalled());
        expect(Toast.error.mock.calls[0][0]).toBe(
            'admin.oidcInvalid:{"field":"admin.oidcPublicUrls"}',
        );
        expect(saveErrorMessage({ error: 'incomplete', problem: 'issuer' })).toBe(
            'admin.oidcIncomplete:{"field":"admin.oidcIssuer"}',
        );
        expect(saveErrorMessage({ error: 'incomplete', problem: 'weird' })).toBe(
            'admin.oidcIncomplete:{"field":"weird"}',
        );
        expect(saveErrorMessage({ error: 'invalid_session_ttl_hours' })).toBe(
            'admin.oidcInvalid:{"field":"admin.oidcTtl"}',
        );
        expect(saveErrorMessage({ error: 'invalid_enabled' })).toBe('admin.oidcSaveFailed');
        expect(saveErrorMessage(undefined)).toBe('admin.oidcSaveFailed');
    });

    it('tests discovery against what is typed, keeping the form', async () => {
        await mount();
        $('#oidc-issuer').value = 'https://other.example.com/';
        $('#oidc-label').value = 'typed, not saved';
        BackendClient.testOidcDiscovery.mockResolvedValue({
            ok: true,
            issuer: 'https://other.example.com/',
            keys: 2,
        });
        $('#btn-oidc-test').click();
        await vi.waitFor(() =>
            expect($('#oidc-test-result').textContent).toContain('admin.oidcTestOk'),
        );
        expect(BackendClient.testOidcDiscovery).toHaveBeenCalledWith('https://other.example.com/');
        expect($('#oidc-test-result').textContent).toContain('"keys":2');
        expect($('#oidc-label').value).toBe('typed, not saved');

        BackendClient.testOidcDiscovery.mockResolvedValue({ ok: false, error: 'issuer_mismatch' });
        $('#btn-oidc-test').click();
        await vi.waitFor(() =>
            expect($('#oidc-test-result').textContent).toContain('issuer_mismatch'),
        );

        BackendClient.testOidcDiscovery.mockRejectedValue(new Error('offline'));
        $('#btn-oidc-test').click();
        await vi.waitFor(() => expect($('#oidc-test-result').textContent).toContain('offline'));
        expect($('#btn-oidc-test').disabled).toBe(false);
        // A full re-render shows the last result too.
        expect(section.render()).toContain('admin.oidcTestFailed');
    });
});
