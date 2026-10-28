/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * OpenID Connect sign-in, browser side (docs/design/oidc-login.md).
 *
 * The server does the whole exchange: the page only navigates to
 * /auth/oidc/start, and reads back the `?oidc_error=` a refused login lands on.
 * What is decided here is when to go there without being asked — an expired
 * session on an instance where the provider is the only door — and when not to:
 * right after a refusal (or it would loop) and right after "Log out" (or the
 * provider's still-open session would sign the person straight back in).
 */
import { t } from '../i18n/i18n.js';

export const OIDC_START_PATH = '/auth/oidc/start';

/** Set by Log out, read once by the next page: do not sign back in silently. */
const SIGNED_OUT_KEY = 'mw-oidc-signed-out';

/** The codes the server may put in ?oidc_error=, and what to tell the person. */
const ERROR_KEYS = {
    invalid_state: 'login.oidcErrorState',
    wrong_origin: 'login.oidcErrorState',
    idp_error: 'login.oidcErrorIdp',
    idp_unreachable: 'login.oidcErrorUnreachable',
    token_exchange: 'login.oidcErrorFailed',
    invalid_token: 'login.oidcErrorFailed',
    not_allowed: 'login.oidcErrorNotAllowed',
    rate_limited: 'login.oidcErrorRateLimited',
};

/** The `oidc` block of /api/auth/status, with every field defaulted. */
export function oidcStatus(status) {
    const o = (status && status.oidc) || {};
    return {
        enabled: o.enabled === true,
        label: typeof o.label === 'string' ? o.label : '',
        pinDisabled: o.pin_disabled === true,
    };
}

/** The ?oidc_error= code of @p search ("" when none). */
export function oidcErrorFrom(search) {
    try {
        return new URLSearchParams(search || '').get('oidc_error') || '';
    } catch {
        return '';
    }
}

/** What to say for an error code: a known message, else the generic one. */
export function oidcErrorMessage(code) {
    if (!code) return '';
    return t(ERROR_KEYS[code] || 'login.oidcErrorFailed');
}

/** "Sign in with Authentik", or a neutral label when none was configured. */
export function oidcButtonText(label) {
    return t('login.oidcSignIn', { name: label || t('login.oidcDefaultLabel') });
}

/** Remember, for this tab only, that the person just logged out. */
export function markSignedOut() {
    try {
        sessionStorage.setItem(SIGNED_OUT_KEY, '1');
    } catch {
        // No storage: the next page may sign in silently, which is the worst case.
    }
}

/** Whether the person logged out in this tab and has not signed in since. */
export function wasSignedOut() {
    try {
        return sessionStorage.getItem(SIGNED_OUT_KEY) === '1';
    } catch {
        return false;
    }
}

/**
 * Whether a page with no session should leave for the provider at once: only
 * where it is the only door (the PIN is disabled), the door is open here, and
 * the person neither was just refused nor just logged out.
 */
export function shouldStartAutomatically(status, search, signedOut) {
    const o = oidcStatus(status);
    return o.enabled && o.pinDisabled && !oidcErrorFrom(search) && !signedOut;
}

/** Leave for the provider: asked for, so the logout is no longer in the way. */
export function startOidcLogin(win = window) {
    try {
        win.sessionStorage.removeItem(SIGNED_OUT_KEY);
    } catch {
        // Nothing to forget.
    }
    win.location.assign(OIDC_START_PATH);
}

/** Drop ?oidc_error= from the address bar once shown, so a reload is clean. */
export function clearOidcError(win = window) {
    try {
        const url = new URL(win.location.href);
        if (!url.searchParams.has('oidc_error')) return;
        url.searchParams.delete('oidc_error');
        win.history.replaceState(win.history.state, '', url.pathname + url.search + url.hash);
    } catch {
        // Cosmetic only.
    }
}
