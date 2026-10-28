/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Where the identity provider is the only door (oidc_disable_pin), a page with
 * no session goes straight back to it rather than showing a login page with a
 * single button — except right after a refused sign-in, which would loop, and
 * right after Log out, which the provider's open session would undo.
 */

// @vitest-environment-options {"url": "https://mw.example.com/"}

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

vi.mock('../js/util/oidcLogin.js', async (importOriginal) => ({
    ...(await importOriginal()),
    startOidcLogin: vi.fn(),
}));

import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { markSignedOut, startOidcLogin } from '../js/util/oidcLogin.js';

describe('an expired session where the provider is the only door', () => {
    let saved;

    const status = (oidc, extra = {}) =>
        vi.spyOn(BackendClient, 'getAuthStatus').mockResolvedValue({
            authenticated: false,
            is_localhost: false,
            remaining: 3,
            oidc,
            ...extra,
        });

    beforeEach(() => {
        saved = { loginView: MoonlightApp.loginView, state: MoonlightApp.state };
        vi.clearAllMocks();
        sessionStorage.clear();
        window.history.replaceState(null, '', '/');
        document.body.innerHTML = '<div id="main-content"></div>';
        vi.spyOn(console, 'log').mockImplementation(() => {});
    });
    afterEach(() => {
        vi.restoreAllMocks();
        MoonlightApp.loginView?.destroy?.();
        Object.assign(MoonlightApp, saved);
        document.body.innerHTML = '';
    });

    it('goes straight to the provider', async () => {
        status({ enabled: true, label: 'Authentik', pin_disabled: true });
        expect(await MoonlightApp._checkAuth()).toBe(false);
        expect(startOidcLogin).toHaveBeenCalledTimes(1);
        expect(MoonlightApp.loginView).toBe(saved.loginView);
    });

    it('shows the login page while the PIN is still a door', async () => {
        status({ enabled: true, label: 'Authentik', pin_disabled: false });
        expect(await MoonlightApp._checkAuth()).toBe(false);
        expect(startOidcLogin).not.toHaveBeenCalled();
        expect(MoonlightApp.loginView).toBeTruthy();
    });

    it('does not loop on a refusal', async () => {
        window.history.replaceState(null, '', '/?oidc_error=not_allowed');
        status({ enabled: true, label: 'Authentik', pin_disabled: true });
        await MoonlightApp._checkAuth();
        expect(startOidcLogin).not.toHaveBeenCalled();
    });

    it('does not sign back in right after Log out', async () => {
        markSignedOut();
        status({ enabled: true, label: 'Authentik', pin_disabled: true });
        await MoonlightApp._checkAuth();
        expect(startOidcLogin).not.toHaveBeenCalled();
    });

    it('leaves a signed-in page alone', async () => {
        status({ enabled: true, label: 'Authentik', pin_disabled: true }, { authenticated: true });
        vi.spyOn(BackendClient, 'ensurePairingKey').mockResolvedValue(null);
        expect(await MoonlightApp._checkAuth()).toBe(true);
        expect(startOidcLogin).not.toHaveBeenCalled();
        // MW-BIND-v1: an OIDC login binds its key on this first authenticated load.
        expect(BackendClient.ensurePairingKey).toHaveBeenCalled();
    });
});
