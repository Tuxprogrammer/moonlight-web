/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * OpenID Connect sign-in, browser side (util/oidcLogin.js): reading the
 * server's answer, the error a refused login lands on, and the one decision
 * the page makes on its own — leaving for the provider without a click — and
 * the two cases where it must not (right after a refusal, right after Log out).
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, vars) => (vars ? `${key}:${JSON.stringify(vars)}` : key),
}));

import {
    OIDC_START_PATH,
    clearOidcError,
    markSignedOut,
    oidcButtonText,
    oidcErrorFrom,
    oidcErrorMessage,
    oidcStatus,
    shouldStartAutomatically,
    startOidcLogin,
    wasSignedOut,
} from '../js/util/oidcLogin.js';

const onlyDoor = { oidc: { enabled: true, label: 'Authentik', pin_disabled: true } };

describe('oidcLogin', () => {
    beforeEach(() => sessionStorage.clear());

    it('reads the oidc block, defaulting every field', () => {
        expect(oidcStatus(onlyDoor)).toEqual({
            enabled: true,
            label: 'Authentik',
            pinDisabled: true,
        });
        expect(oidcStatus({})).toEqual({ enabled: false, label: '', pinDisabled: false });
        expect(oidcStatus(null)).toEqual({ enabled: false, label: '', pinDisabled: false });
        // Only real booleans count: a truthy string is not the server saying yes.
        expect(oidcStatus({ oidc: { enabled: 'true', label: 7, pin_disabled: 1 } })).toEqual({
            enabled: false,
            label: '',
            pinDisabled: false,
        });
    });

    it('reads ?oidc_error=', () => {
        expect(oidcErrorFrom('?oidc_error=not_allowed')).toBe('not_allowed');
        expect(oidcErrorFrom('?a=1&oidc_error=invalid_state')).toBe('invalid_state');
        expect(oidcErrorFrom('')).toBe('');
        expect(oidcErrorFrom(undefined)).toBe('');
        expect(oidcErrorFrom('?other=1')).toBe('');
    });

    it('says something for every code, and something generic for others', () => {
        expect(oidcErrorMessage('not_allowed')).toBe('login.oidcErrorNotAllowed');
        expect(oidcErrorMessage('invalid_state')).toBe('login.oidcErrorState');
        expect(oidcErrorMessage('wrong_origin')).toBe('login.oidcErrorState');
        expect(oidcErrorMessage('idp_error')).toBe('login.oidcErrorIdp');
        expect(oidcErrorMessage('idp_unreachable')).toBe('login.oidcErrorUnreachable');
        expect(oidcErrorMessage('token_exchange')).toBe('login.oidcErrorFailed');
        expect(oidcErrorMessage('invalid_token')).toBe('login.oidcErrorFailed');
        expect(oidcErrorMessage('rate_limited')).toBe('login.oidcErrorRateLimited');
        expect(oidcErrorMessage('<script>')).toBe('login.oidcErrorFailed');
        expect(oidcErrorMessage('')).toBe('');
    });

    it('names the provider on the button, or says single sign-on', () => {
        expect(oidcButtonText('Authentik')).toBe('login.oidcSignIn:{"name":"Authentik"}');
        expect(oidcButtonText('')).toBe('login.oidcSignIn:{"name":"login.oidcDefaultLabel"}');
    });

    it('leaves for the provider by itself only where it is the only door', () => {
        expect(shouldStartAutomatically(onlyDoor, '', false)).toBe(true);
        // The PIN is still a door: the person chooses.
        expect(
            shouldStartAutomatically({ oidc: { enabled: true, pin_disabled: false } }, '', false),
        ).toBe(false);
        // Not offered on this address: nowhere to go.
        expect(
            shouldStartAutomatically({ oidc: { enabled: false, pin_disabled: true } }, '', false),
        ).toBe(false);
        // Just refused: going back would loop.
        expect(shouldStartAutomatically(onlyDoor, '?oidc_error=not_allowed', false)).toBe(false);
        // Just logged out: the provider's session would sign them straight back in.
        expect(shouldStartAutomatically(onlyDoor, '', true)).toBe(false);
        expect(shouldStartAutomatically({}, '', false)).toBe(false);
    });

    it('remembers a logout in this tab until the person signs in again', () => {
        expect(wasSignedOut()).toBe(false);
        markSignedOut();
        expect(wasSignedOut()).toBe(true);
        expect(wasSignedOut()).toBe(true); // a reload still respects it
        const win = { location: { assign: vi.fn() }, sessionStorage };
        startOidcLogin(win);
        expect(win.location.assign).toHaveBeenCalledWith(OIDC_START_PATH);
        expect(OIDC_START_PATH).toBe('/auth/oidc/start');
        expect(wasSignedOut()).toBe(false);
    });

    it('survives a browser that refuses storage', () => {
        const get = vi.spyOn(Storage.prototype, 'getItem').mockImplementation(() => {
            throw new Error('denied');
        });
        const set = vi.spyOn(Storage.prototype, 'setItem').mockImplementation(() => {
            throw new Error('denied');
        });
        const remove = vi.spyOn(Storage.prototype, 'removeItem').mockImplementation(() => {
            throw new Error('denied');
        });
        expect(() => markSignedOut()).not.toThrow();
        expect(wasSignedOut()).toBe(false);
        const win = { location: { assign: vi.fn() }, sessionStorage };
        expect(() => startOidcLogin(win)).not.toThrow();
        expect(win.location.assign).toHaveBeenCalled();
        get.mockRestore();
        set.mockRestore();
        remove.mockRestore();
    });

    it('drops ?oidc_error= from the address bar and keeps the rest', () => {
        const win = {
            location: { href: 'https://mw.example.com/?oidc_error=x&keep=1#frag' },
            history: { state: { s: 1 }, replaceState: vi.fn() },
        };
        clearOidcError(win);
        expect(win.history.replaceState).toHaveBeenCalledWith({ s: 1 }, '', '/?keep=1#frag');
        win.history.replaceState.mockClear();
        win.location.href = 'https://mw.example.com/';
        clearOidcError(win);
        expect(win.history.replaceState).not.toHaveBeenCalled();
        expect(() => clearOidcError({ location: { href: 'not a url' } })).not.toThrow();
    });
});
