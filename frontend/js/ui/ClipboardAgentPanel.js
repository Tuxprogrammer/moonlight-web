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
 * Admin page — "Clipboard on other hosts": one row per paired host that is
 * another machine, for its clipboard agent (docs/design/clipboard-agent.md).
 *
 * Per host: the switch (off until the admin turns it on), the agent's state
 * (connected, with its backend; or when it was last seen), and its token —
 * generate, replace, revoke. A new token is shown once, with the lines to paste
 * on the host; it lives in this panel's memory until Done, never in storage.
 *
 * AdminView re-renders its whole page often, so the panel keeps its own state
 * and is mounted again into its placeholder after each render.
 */
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
import { confirmAction } from './ConfirmDialog.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

/** How often the agents' state is refreshed while the page is open. */
export const POLL_MS = 5000;

/**
 * The hosts an agent makes sense for: paired, and not this machine's own
 * native host (its clipboard is this machine's already).
 * @param {Array<any>} hosts
 */
export function eligibleHosts(hosts) {
    return (hosts || []).filter(
        (h) => h && h.uuid && h.pairState === 'paired' && h.backendType !== 'native',
    );
}

/**
 * The status line for one host.
 * @param {any} s the server's status object
 * @param {(ms: number) => string} [formatTime]
 */
export function statusText(s, formatTime = (ms) => new Date(ms).toLocaleString()) {
    if (!s) return t('admin.clipboardAgentUnknown');
    if (s.connected) {
        const what = [s.backend, s.platform].filter(Boolean).join(' · ');
        const base = s.enabled
            ? t('admin.clipboardAgentConnected')
            : t('admin.clipboardAgentConnectedOff');
        return what ? `${base} (${what})` : base;
    }
    if (!s.has_token) return t('admin.clipboardAgentNoToken');
    if (s.last_seen > 0)
        return t('admin.clipboardAgentLastSeen', { when: formatTime(s.last_seen) });
    return t('admin.clipboardAgentNeverSeen');
}

export class ClipboardAgentPanel {
    /**
     * @param {{ client?: any, confirm?: (msg: string, opts?: any) => Promise<boolean> }} [deps]
     */
    constructor(deps = {}) {
        this._client = deps.client || BackendClient;
        this._confirm = deps.confirm || confirmAction;
        /** @type {Array<{uuid: string, name: string, status: any}>} */
        this.hosts = [];
        /** False until the server answered one status: an older server has no such route. */
        this.supported = false;
        /** uuid → the issue answer (token, setup…), until Done. */
        this.issued = new Map();
        /** uuids with a request in flight. */
        this.busy = new Set();
        this._el = null;
        this._timer = null;
    }

    /** Read the hosts and each one's agent state. */
    async load() {
        let hosts = [];
        try {
            const r = await this._client.getHosts();
            hosts = eligibleHosts(r && r.hosts);
        } catch (err) {
            console.warn('[ClipboardAgent] Cannot read the hosts:', err);
            return;
        }
        const rows = await Promise.all(
            hosts.map(async (h) => {
                try {
                    const status = await this._client.getClipboardAgent(h.uuid);
                    this.supported = true;
                    return { uuid: h.uuid, name: h.name || h.uuid, status };
                } catch (err) {
                    return { uuid: h.uuid, name: h.name || h.uuid, status: null };
                }
            }),
        );
        this.hosts = rows;
    }

    /** Refresh only the states (the polling tick), and redraw the rows. */
    async refresh() {
        await Promise.all(
            this.hosts.map(async (row) => {
                try {
                    row.status = await this._client.getClipboardAgent(row.uuid);
                    this.supported = true;
                } catch (_) {
                    /* keep the last known state */
                }
            }),
        );
        this._redraw();
    }

    startPolling(ms = POLL_MS) {
        this.stopPolling();
        this._timer = setInterval(() => void this.refresh(), ms);
    }

    stopPolling() {
        if (this._timer) clearInterval(this._timer);
        this._timer = null;
    }

    destroy() {
        this.stopPolling();
        this.issued.clear();
        this._el = null;
    }

    /** Draw into @p el (AdminView's placeholder) and listen there. */
    mount(el) {
        if (!el) return;
        this._el = el;
        el.hidden = !this.supported || !this.hosts.length;
        if (!el.dataset.caBound) {
            el.dataset.caBound = '1';
            el.addEventListener('click', (e) => void this._onClick(e));
            el.addEventListener('change', (e) => void this._onChange(e));
        }
        this._redraw();
    }

    _redraw() {
        if (!this._el) return;
        this._el.hidden = !this.supported || !this.hosts.length;
        this._el.innerHTML = this.render();
    }

    render() {
        const rows = this.hosts.map((row) => this._renderRow(row)).join('');
        return `
                    <h3 class="settings-section-title">${t('admin.clipboardAgents')}</h3>
                    <p class="setting-desc">${t('admin.clipboardAgentsDesc')}</p>
                    <div class="ca-rows">${rows}</div>
        `;
    }

    _renderRow(row) {
        const s = row.status || {};
        const id = escapeHtml(row.uuid);
        const busy = this.busy.has(row.uuid) ? 'disabled' : '';
        const state = s.connected ? (s.enabled ? 'on' : 'idle') : 'off';
        const tokenButtons = s.has_token
            ? `<button type="button" class="btn btn-secondary" data-ca-action="rotate" data-uuid="${id}" ${busy}>${t('admin.clipboardAgentRotate')}</button>
               <button type="button" class="btn btn-danger" data-ca-action="revoke" data-uuid="${id}" ${busy}>${t('admin.clipboardAgentRevoke')}</button>`
            : `<button type="button" class="btn btn-secondary" data-ca-action="issue" data-uuid="${id}" ${busy}>${t('admin.clipboardAgentIssue')}</button>`;
        const issued = this.issued.get(row.uuid);
        return `
                        <div class="settings-field ca-row" data-uuid="${id}">
                            <div class="ca-head">
                                <span class="settings-label">${escapeHtml(row.name)}</span>
                                <span class="ca-state ca-state-${state}">${escapeHtml(statusText(row.status))}</span>
                            </div>
                            <label class="settings-checkbox-label">
                                <input type="checkbox" class="ca-enabled" data-uuid="${id}" ${s.enabled ? 'checked' : ''} ${busy} />
                                <span class="settings-checkbox-text">${t('admin.clipboardAgentEnable')}</span>
                            </label>
                            <div class="ca-actions">${tokenButtons}</div>
                            ${issued ? this._renderIssued(row, issued) : ''}
                        </div>
        `;
    }

    _renderIssued(row, a) {
        const id = escapeHtml(row.uuid);
        const block = (label, text, key) => `
                                <div class="ca-code-block">
                                    <span class="settings-hint">${label}</span>
                                    <div class="ca-code-row">
                                        <pre class="ca-code" data-ca-code="${key}">${escapeHtml(text || '')}</pre>
                                        <button type="button" class="btn btn-secondary btn-small" data-ca-action="copy" data-uuid="${id}" data-key="${key}">${t('common.copy')}</button>
                                    </div>
                                </div>`;
        return `
                            <div class="ca-issued" role="status">
                                <p class="ca-once"><strong>${t('admin.clipboardAgentOnce')}</strong> ${t('admin.clipboardAgentPasteOn', { host: escapeHtml(row.name) })}</p>
                                ${block(t('admin.clipboardAgentSetupLabel'), a.setup, 'setup')}
                                ${block(t('admin.clipboardAgentStartLabel'), a.command, 'command')}
                                <p class="settings-hint">${t('admin.clipboardAgentStartHint')}</p>
                                ${block(t('admin.clipboardAgentTokenLabel'), a.token, 'token')}
                                <p class="settings-hint">${t('admin.clipboardAgentServerLine', {
                                    server: escapeHtml(a.server || ''),
                                    pins: escapeHtml((a.pins || []).join(', ')),
                                })}</p>
                                <button type="button" class="btn" data-ca-action="done" data-uuid="${id}">${t('admin.clipboardAgentDone')}</button>
                            </div>
        `;
    }

    _row(uuid) {
        return this.hosts.find((r) => r.uuid === uuid);
    }

    async _run(uuid, fn) {
        if (this.busy.has(uuid)) return;
        this.busy.add(uuid);
        this._redraw();
        try {
            await fn();
        } finally {
            this.busy.delete(uuid);
            this._redraw();
        }
    }

    async _onChange(e) {
        const box = /** @type {HTMLInputElement} */ (e.target);
        if (!box || !box.classList || !box.classList.contains('ca-enabled')) return;
        const uuid = box.dataset.uuid;
        const row = this._row(uuid);
        if (!row) return;
        const enabled = box.checked;
        await this._run(uuid, async () => {
            try {
                row.status = await this._client.setClipboardAgentEnabled(uuid, enabled);
                Toast.success(
                    enabled
                        ? t('admin.clipboardAgentEnabledOn')
                        : t('admin.clipboardAgentEnabledOff'),
                );
            } catch (err) {
                console.warn('[ClipboardAgent] Switch failed:', err);
                Toast.error(t('admin.clipboardAgentFailed'));
            }
        });
    }

    async _onClick(e) {
        const btn = /** @type {HTMLElement|null} */ (
            e.target && e.target.closest ? e.target.closest('[data-ca-action]') : null
        );
        if (!btn) return;
        const uuid = btn.dataset.uuid;
        const row = this._row(uuid);
        if (!row) return;
        const action = btn.dataset.caAction;
        if (action === 'copy') return this._copy(btn, uuid, btn.dataset.key);
        if (action === 'done') {
            this.issued.delete(uuid);
            this._redraw();
            return;
        }
        if (action === 'issue' || action === 'rotate') {
            if (
                action === 'rotate' &&
                !(await this._confirm(t('admin.clipboardAgentConfirmRotate', { host: row.name })))
            )
                return;
            await this._run(uuid, async () => {
                try {
                    const answer = await this._client.issueClipboardAgentToken(uuid);
                    this.issued.set(uuid, answer);
                    if (answer && answer.status) row.status = answer.status;
                } catch (err) {
                    console.warn('[ClipboardAgent] Token failed:', err);
                    Toast.error(t('admin.clipboardAgentFailed'));
                }
            });
            return;
        }
        if (action === 'revoke') {
            if (
                !(await this._confirm(t('admin.clipboardAgentConfirmRevoke', { host: row.name }), {
                    danger: true,
                    confirmLabel: t('admin.clipboardAgentRevoke'),
                }))
            )
                return;
            await this._run(uuid, async () => {
                try {
                    row.status = await this._client.revokeClipboardAgentToken(uuid);
                    this.issued.delete(uuid);
                    Toast.success(t('admin.clipboardAgentRevoked'));
                } catch (err) {
                    console.warn('[ClipboardAgent] Revoke failed:', err);
                    Toast.error(t('admin.clipboardAgentFailed'));
                }
            });
        }
    }

    async _copy(btn, uuid, key) {
        const a = this.issued.get(uuid);
        const text = a ? a[key] : '';
        if (!text) return;
        try {
            await navigator.clipboard.writeText(text);
            btn.textContent = t('common.copied');
            setTimeout(() => {
                btn.textContent = t('common.copy');
            }, 2000);
        } catch (err) {
            // Leave it selected so it can be copied by hand.
            const pre = btn.parentElement && btn.parentElement.querySelector('.ca-code');
            if (pre && typeof window.getSelection === 'function') {
                const range = document.createRange();
                range.selectNodeContents(pre);
                const sel = window.getSelection();
                sel.removeAllRanges();
                sel.addRange(range);
            }
        }
    }
}
