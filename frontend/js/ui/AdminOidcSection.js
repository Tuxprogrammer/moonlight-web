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
 * Admin page → "Sign-in with an identity provider" (OpenID Connect).
 *
 * Its own module and its own Save button: the settings belong together (an
 * issuer without its client id is no setting at all), the server takes them
 * all or nothing (POST /api/admin/oidc), and AdminView only has to place it.
 * The client secret is write-only: the server says whether one is set and
 * where it comes from, never what it is, and an empty field leaves it alone.
 * See docs/design/oidc-login.md.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

const SECTION_ID = 'admin-section-oidc';

/** A list typed one per line or separated by commas, as the server reads it. */
export function splitList(text) {
    return String(text || '')
        .split(/[\n,]/)
        .map((s) => s.trim())
        .filter((s, i, all) => s && all.indexOf(s) === i);
}

/** The localized reason for a refused save (`error`, `problem` of the 400). */
export function saveErrorMessage(body) {
    const error = body && body.error;
    if (error === 'incomplete') {
        const missing = {
            issuer: t('admin.oidcIssuer'),
            client_id: t('admin.oidcClientId'),
            public_url: t('admin.oidcPublicUrls'),
            scopes: t('admin.oidcScopes'),
        }[body.problem];
        return t('admin.oidcIncomplete', { field: missing || body.problem || '' });
    }
    const field = {
        invalid_issuer: t('admin.oidcIssuer'),
        invalid_public_url: t('admin.oidcPublicUrls'),
        invalid_session_ttl_hours: t('admin.oidcTtl'),
        invalid_scopes: t('admin.oidcScopes'),
    }[error];
    return field ? t('admin.oidcInvalid', { field }) : t('admin.oidcSaveFailed');
}

export class AdminOidcSection {
    constructor() {
        /** GET /api/admin/oidc, or null when this server has no such route. */
        this.state = null;
        /** The last "Test discovery" answer, shown under the buttons. */
        this._test = null;
        this._busy = false;
    }

    async load() {
        try {
            this.state = await BackendClient.getOidcSettings();
        } catch (err) {
            // An older server: the section is simply not there.
            console.warn('[Admin] OIDC settings unavailable:', err);
            this.state = null;
        }
    }

    render() {
        const s = this.state;
        if (!s) return '';
        const esc = escapeHtml;
        const list = (a) => esc((Array.isArray(a) ? a : []).join(', '));
        const secretNote = !s.client_secret_set
            ? t('admin.oidcSecretNone')
            : s.client_secret_source === 'env' || s.client_secret_source === 'env_file'
              ? t('admin.oidcSecretEnv')
              : t('admin.oidcSecretSet');
        const redirects = Array.isArray(s.redirect_uris) ? s.redirect_uris : [];
        return `
                <div class="settings-section" id="${SECTION_ID}">
                    <h3 class="settings-section-title">${t('admin.oidc')}</h3>
                    <p class="setting-desc">${t('admin.oidcDesc')}</p>
                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="oidc-enabled" ${s.enabled ? 'checked' : ''} />
                            <span class="settings-checkbox-text">${t('admin.oidcEnable')}</span>
                        </label>
                        ${s.enabled && !s.usable && s.problem ? `<div class="settings-status settings-status-pending">${esc(saveErrorMessage({ error: 'incomplete', problem: s.problem }))}</div>` : ''}
                    </div>
                    ${this._field('oidc-issuer', t('admin.oidcIssuer'), s.issuer, 'https://idp.example.com/application/o/moonlightweb/')}
                    ${this._field('oidc-client-id', t('admin.oidcClientId'), s.client_id, 'moonlightweb')}
                    <div class="settings-field">
                        <label class="settings-label" for="oidc-client-secret">${t('admin.oidcClientSecret')}</label>
                        <input type="password" id="oidc-client-secret" class="settings-input"
                               autocomplete="new-password"
                               placeholder="${esc(s.client_secret_set ? t('admin.oidcSecretKeep') : t('admin.oidcSecretPlaceholder'))}" />
                        <p class="settings-hint">${secretNote}</p>
                        ${
                            s.client_secret_set && s.client_secret_source === 'settings'
                                ? `<label class="settings-checkbox-label">
                            <input type="checkbox" id="oidc-clear-secret" />
                            <span class="settings-checkbox-text">${t('admin.oidcSecretClear')}</span>
                        </label>`
                                : ''
                        }
                    </div>
                    <div class="settings-field">
                        <label class="settings-label" for="oidc-public-urls">${t('admin.oidcPublicUrls')}</label>
                        <textarea id="oidc-public-urls" class="settings-input" rows="2"
                                  placeholder="https://mw.example.com">${esc((s.public_url || []).join('\n'))}</textarea>
                        <p class="settings-hint">${t('admin.oidcPublicUrlsHint')}</p>
                        ${
                            redirects.length
                                ? `<p class="settings-hint">${t('admin.oidcRedirectUris')}</p>
                        <ul class="settings-hint" id="oidc-redirect-uris">${redirects.map((u) => `<li><code>${esc(u)}</code></li>`).join('')}</ul>`
                                : ''
                        }
                    </div>
                    ${this._field('oidc-allowed-groups', t('admin.oidcAllowedGroups'), list(s.allowed_groups), '', t('admin.oidcAllowedGroupsHint'), true)}
                    ${this._field('oidc-admin-groups', t('admin.oidcAdminGroups'), list(s.admin_groups), '', t('admin.oidcAdminGroupsHint'), true)}
                    ${this._field('oidc-groups-claim', t('admin.oidcGroupsClaim'), s.groups_claim, 'groups')}
                    ${this._field('oidc-scopes', t('admin.oidcScopes'), s.scopes, 'openid profile email')}
                    ${this._field('oidc-label', t('admin.oidcLabel'), s.button_label, 'Authentik')}
                    <div class="settings-field">
                        <label class="settings-label" for="oidc-ttl">${t('admin.oidcTtl')}</label>
                        <input type="number" id="oidc-ttl" class="settings-input" min="1" max="720"
                               value="${esc(String(s.session_ttl_hours || 24))}" />
                        <p class="settings-hint">${t('admin.oidcTtlHint')}</p>
                    </div>
                    <div class="settings-field">
                        <label class="settings-checkbox-label">
                            <input type="checkbox" id="oidc-disable-pin" ${s.disable_pin ? 'checked' : ''} />
                            <span class="settings-checkbox-text">${t('admin.oidcDisablePin')}</span>
                        </label>
                        <p class="setting-desc">${t('admin.oidcDisablePinDesc')}</p>
                    </div>
                    <div class="u-mt-2">
                        <button class="btn btn-neutral" id="btn-oidc-save" ${this._busy ? 'disabled' : ''}>${t('admin.oidcSave')}</button>
                        <button class="btn btn-secondary" id="btn-oidc-test" ${this._busy ? 'disabled' : ''}>${t('admin.oidcTest')}</button>
                    </div>
                    <p class="settings-hint u-mt-2" id="oidc-test-result">${escapeHtml(this._renderTestText())}</p>
                </div>`;
    }

    _field(id, label, value, placeholder, hint = '', escaped = false) {
        const v = escaped ? value : escapeHtml(value || '');
        return `
                    <div class="settings-field">
                        <label class="settings-label" for="${id}">${label}</label>
                        <input type="text" id="${id}" class="settings-input" autocomplete="off"
                               placeholder="${escapeHtml(placeholder)}" value="${v}" />
                        ${hint ? `<p class="settings-hint">${hint}</p>` : ''}
                    </div>`;
    }

    /** What the form says, as POST /api/admin/oidc reads it. */
    collect(root) {
        const val = (id) => {
            const el = /** @type {HTMLInputElement|null} */ (root.querySelector('#' + id));
            return el ? el.value : '';
        };
        const checked = (id) => {
            const el = /** @type {HTMLInputElement|null} */ (root.querySelector('#' + id));
            return !!(el && el.checked);
        };
        const body = {
            enabled: checked('oidc-enabled'),
            issuer: val('oidc-issuer').trim(),
            client_id: val('oidc-client-id').trim(),
            public_url: splitList(val('oidc-public-urls')),
            allowed_groups: splitList(val('oidc-allowed-groups')),
            admin_groups: splitList(val('oidc-admin-groups')),
            groups_claim: val('oidc-groups-claim').trim(),
            scopes: val('oidc-scopes').trim(),
            button_label: val('oidc-label').trim(),
            session_ttl_hours: Number(val('oidc-ttl')),
            disable_pin: checked('oidc-disable-pin'),
        };
        // Write-only: sent only when typed, so an empty field keeps the secret.
        const secret = val('oidc-client-secret');
        if (secret) body.client_secret = secret;
        if (checked('oidc-clear-secret')) body.clear_client_secret = true;
        return body;
    }

    /** Wire the section found under @p root; re-renders itself in place. */
    bind(root) {
        const section = root.querySelector('#' + SECTION_ID);
        if (!section) return;
        section.querySelector('#btn-oidc-save')?.addEventListener('click', () => this._save(root));
        section
            .querySelector('#btn-oidc-test')
            ?.addEventListener('click', () => this._runTest(root));
    }

    _rerender(root) {
        const section = root.querySelector('#' + SECTION_ID);
        if (!section) return;
        section.outerHTML = this.render();
        this.bind(root);
    }

    async _save(root) {
        const body = this.collect(root);
        this._busy = true;
        try {
            const saved = await BackendClient.saveOidcSettings(body);
            this.state = saved;
            Toast.success(
                saved.sessions_ended > 0
                    ? t('admin.oidcSavedEnded', { count: saved.sessions_ended })
                    : t('admin.oidcSaved'),
            );
        } catch (err) {
            Toast.error(saveErrorMessage(err && err.responseBody));
        } finally {
            this._busy = false;
            this._rerender(root);
        }
    }

    async _runTest(root) {
        const issuer = /** @type {HTMLInputElement|null} */ (root.querySelector('#oidc-issuer'));
        this._busy = true;
        const result = root.querySelector('#oidc-test-result');
        if (result) result.textContent = t('admin.oidcTesting');
        try {
            this._test = await BackendClient.testOidcDiscovery(issuer ? issuer.value.trim() : '');
        } catch (err) {
            this._test = { ok: false, error: (err && err.message) || '' };
        } finally {
            this._busy = false;
        }
        // Keep what was typed: only the result line changes.
        const line = root.querySelector('#oidc-test-result');
        if (line) line.textContent = this._renderTestText();
        root.querySelectorAll('#btn-oidc-save, #btn-oidc-test').forEach((b) => {
            /** @type {HTMLButtonElement} */ (b).disabled = false;
        });
    }

    _renderTestText() {
        const r = this._test;
        if (!r) return '';
        return r.ok
            ? t('admin.oidcTestOk', { issuer: r.issuer || '', keys: r.keys || 0 })
            : t('admin.oidcTestFailed', { error: r.error || '' });
    }
}
