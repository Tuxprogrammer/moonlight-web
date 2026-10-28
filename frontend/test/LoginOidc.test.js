/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * "Sign in with …" on the login page (OpenID Connect): offered when the server
 * says so, beside the PIN or in place of it, with the reason a refused sign-in
 * came back for — and the PIN form, the certificate toggle and the PIN hint gone
 * when the identity provider is the only door.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(),
        validatePin: vi.fn(),
        validateCertificate: vi.fn(),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, vars) => (vars ? `${key}:${JSON.stringify(vars)}` : key),
}));
vi.mock('../js/net/tunnelBridge.js', () => ({ tunnelHostId: () => null }));
vi.mock('../js/util/oidcLogin.js', async (importOriginal) => ({
    ...(await importOriginal()),
    startOidcLogin: vi.fn(),
}));

import { LoginView } from '../js/ui/LoginView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { startOidcLogin } from '../js/util/oidcLogin.js';

describe('the OpenID Connect button on the login page', () => {
    let view;
    const $ = (sel) => document.querySelector(sel);

    const open = async (oidc, extra = {}) => {
        BackendClient.getAuthStatus.mockResolvedValue({
            authenticated: false,
            remaining: 3,
            cert_auth_enabled: true,
            oidc,
            ...extra,
        });
        document.body.innerHTML = '<div id="host"></div>';
        view = new LoginView(document.getElementById('host'), () => {});
        await view.start();
    };

    beforeEach(() => {
        vi.clearAllMocks();
        localStorage.clear();
        window.history.replaceState(null, '', '/');
    });
    afterEach(() => view && view.destroy());

    it('is not there when the server offers no provider', async () => {
        await open(undefined);
        expect($('#btn-login-oidc')).toBe(null);
        expect($('#login-pin-input')).not.toBe(null);
        await open({ enabled: false, label: 'Authentik', pin_disabled: false });
        expect($('#btn-login-oidc')).toBe(null);
    });

    it('sits above the PIN, which stays, and names the provider', async () => {
        await open({ enabled: true, label: 'Authentik', pin_disabled: false });
        const btn = $('#btn-login-oidc');
        expect(btn).not.toBe(null);
        expect(btn.textContent.trim()).toBe('login.oidcSignIn:{"name":"Authentik"}');
        expect($('.login-or').textContent).toBe('login.oidcOr');
        expect($('#login-pin-input')).not.toBe(null);
        expect($('#btn-toggle-auth-method')).not.toBe(null);
        expect(document.body.textContent).toContain('login.pinHint');
    });

    it('escapes the label the operator typed', async () => {
        await open({ enabled: true, label: '<img src=x onerror=alert(1)>', pin_disabled: false });
        expect($('#btn-login-oidc img')).toBe(null);
        expect($('#btn-login-oidc').textContent).toContain('<img src=x onerror=alert(1)>');
    });

    it('leaves for the provider when clicked', async () => {
        await open({ enabled: true, label: '', pin_disabled: false });
        $('#btn-login-oidc').click();
        expect(startOidcLogin).toHaveBeenCalledTimes(1);
        expect($('#btn-login-oidc').disabled).toBe(true);
    });

    it('is alone when the provider is the only door', async () => {
        await open({ enabled: true, label: 'Authentik', pin_disabled: true });
        expect($('#btn-login-oidc')).not.toBe(null);
        expect($('#login-pin-input')).toBe(null);
        expect($('#login-machine-input')).toBe(null);
        expect($('#btn-toggle-auth-method')).toBe(null);
        expect($('.login-or')).toBe(null);
        expect(document.body.textContent).not.toContain('login.pinHint');
    });

    it('says sign-in is unavailable where neither door is open', async () => {
        await open({ enabled: false, label: '', pin_disabled: true });
        expect($('#btn-login-oidc')).toBe(null);
        expect($('#login-pin-input')).toBe(null);
        expect(document.body.textContent).toContain('login.oidcUnavailableHere');
    });

    it('explains a refused sign-in once, then cleans the address', async () => {
        window.history.replaceState(null, '', '/?oidc_error=not_allowed');
        await open({ enabled: true, label: 'Authentik', pin_disabled: false });
        expect($('#login-error').textContent).toBe('login.oidcErrorNotAllowed');
        expect(window.location.search).toBe('');
    });

    it('explains it too when the PIN form is gone', async () => {
        window.history.replaceState(null, '', '/?oidc_error=idp_unreachable');
        await open({ enabled: true, label: 'Authentik', pin_disabled: true });
        expect($('#login-error').textContent).toBe('login.oidcErrorUnreachable');
        window.history.replaceState(null, '', '/?oidc_error=invalid_state');
        await open({ enabled: false, label: '', pin_disabled: true });
        expect($('#login-error').textContent).toBe('login.oidcErrorState');
    });
});
