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
 * MoonlightWeb — Pairing dialog
 *
 * Flow:
 *   1. POST /api/hosts/:id/pair/start → returns the PIN; the backend sends the
 *      host the request it shows to its owner (stage 1)
 *   2. Dialog displays the PIN and polls POST /api/hosts/:id/pair, which says
 *      where it stands (`stage`): waiting for the host to show the request,
 *      the host has it (enter the PIN), the PIN is being checked (stages 2-5
 *      take a few LAN round trips, so a poll rarely lands on that one)
 *   3. User enters the PIN in Sunshine → a later poll answers paired
 */
import { BackendClient } from '../api/BackendClient.js';
import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

/** Poll this often while the server reports a stage: the polls are answered from
 *  memory, and "Paired" then shows within a second of the PIN being typed. */
const POLL_MS = 1000;
/** A server that reports no stage starts stage 1 on a poll: its old cadence. */
const POLL_LEGACY_MS = 5000;
/** Past this with no answer, the dialog says it is still waiting and offers Retry. */
const STILL_WAITING_MS = 60000;

export class PairDialog {
    constructor(host) {
        this.host = host;
        this.overlay = null;
        this.pin = null;
        this.pollTimer = null;
        this.stillWaitingTimer = null;
        this.onComplete = null;
        this.onCancel = null;
        // Where the pairing stands: 'contacting' | 'enter_pin' | 'verifying'.
        this._stage = null;
        this._legacy = false; // the server reports no stage
        this._stillWaiting = false;
        this._paired = false;
        // Bumped on every (re)start, so a poll from before a Retry is ignored.
        this._attempt = 0;
    }

    show() {
        this.render();
        document.body.appendChild(this.overlay);
        this.bindEvents();
        this.startPairing();
    }

    close() {
        this.stopPolling();
        if (this.overlay) {
            this.overlay.remove();
            this.overlay = null;
        }
    }

    async startPairing() {
        const attempt = ++this._attempt;
        this._stillWaiting = false;
        this._setRetryVisible(false);

        try {
            const result = await this._startPairingWithRetry();
            if (!this.overlay || attempt !== this._attempt) return;

            if (result.status === 'initiated' && result.pin) {
                this.pin = result.pin;
                const pinEl = this.overlay.querySelector('.pairing-pin-display');
                pinEl.textContent = this.pin;
                pinEl.classList.add('visible');
                this._applyStage(result);

                // Auto-poll for completion; a minute on, say it is still waiting.
                this.pollPairing();
                this.stillWaitingTimer = setTimeout(
                    () => this._showStillWaiting(),
                    STILL_WAITING_MS,
                );
            } else if (result.status === 'error') {
                this._showError(result.message);
            } else {
                this._setStatus(t('pairing.unexpectedResponse'), 'pairing-error');
            }
        } catch (err) {
            if (!this.overlay || attempt !== this._attempt) return;
            this._showError(t('pairing.contactFailed', { message: err.message }));
        }
    }

    // The initial PIN request occasionally fails at the network level ("failed
    // to fetch") on a transient hiccup; retry a couple of times before showing
    // an error so the user never has to reload the page. HTTP-level errors
    // (statusCode set) are real answers — no retry.
    async _startPairingWithRetry(attempts = 3) {
        for (let i = 1; ; i++) {
            try {
                return await BackendClient.startPairing(this.host.uuid);
            } catch (err) {
                if (err.statusCode || i >= attempts || !this.overlay) throw err;
                console.warn(`[PairDialog] pair request failed (try ${i}), retrying:`, err);
                await new Promise((r) => setTimeout(r, 1000 * i));
            }
        }
    }

    pollPairing() {
        // One poll at a time: the next is scheduled only after the previous answers.
        const attempt = this._attempt;
        const delay = () => (this._legacy ? POLL_LEGACY_MS : POLL_MS);
        const tick = async () => {
            if (!this.overlay || attempt !== this._attempt) return; // closed, or retried
            await this.tryComplete();
            if (!this.overlay || attempt !== this._attempt || !this.pollTimer) return;
            this.pollTimer = setTimeout(tick, delay());
        };
        this.pollTimer = setTimeout(tick, delay());
    }

    stopPolling() {
        if (this.pollTimer) {
            clearTimeout(this.pollTimer);
            this.pollTimer = null;
        }
        if (this.stillWaitingTimer) {
            clearTimeout(this.stillWaitingTimer);
            this.stillWaitingTimer = null;
        }
    }

    async tryComplete() {
        const attempt = this._attempt;
        try {
            const result = await BackendClient.confirmPairing(this.host.uuid);
            if (!this.overlay || attempt !== this._attempt) return;

            if (result.status === 'paired') {
                this.stopPolling();
                this._paired = true;
                this._setRetryVisible(false);
                this._setStatus(t('pairing.paired'), 'pairing-success');
                // Nothing left to cancel for the moment before it closes itself.
                this.overlay.querySelector('.btn-pair-cancel').textContent = t('common.close');
                if (this.onComplete) this.onComplete();
                setTimeout(() => this.close(), 1500);
            } else if (result.status === 'awaiting_pin') {
                // Still waiting — keep polling, keep the PIN displayed
                this._applyStage(result);
            } else if (result.status === 'error') {
                this._showError(result.message);
            }
        } catch (err) {
            // Network error — keep polling
        }
    }

    /**
     * Say where the pairing stands. A server that reports no `stage` (an older
     * backend) is read as "the host has it", the wording this dialog always had.
     */
    _applyStage(result) {
        const stage = result.stage || 'enter_pin';
        this._stage = stage;
        this._legacy = !result.stage;
        if (stage === 'verifying') {
            this._stillWaiting = false;
            this._setRetryVisible(false);
            this._setStatus(t('pairing.verifying'), 'pairing-info');
            return;
        }
        if (this._stillWaiting) return; // the soft message stays until something moves
        if (stage === 'contacting') {
            this._setStatus(
                t('pairing.waitingForHost', { name: this.host.displayName }),
                'pairing-info',
            );
        } else {
            this._setStatus(t('pairing.enterPin'), 'pairing-info');
        }
    }

    /** A minute and no pairing yet: not an error, the PIN stays, Retry appears. */
    _showStillWaiting() {
        this.stillWaitingTimer = null;
        if (!this.overlay || this._stage === 'verifying') return;
        this._stillWaiting = true;
        this._setStatus(t('pairing.stillWaiting', { name: this.host.displayName }), 'pairing-info');
        this._setRetryVisible(true);
    }

    _showError(message) {
        this.stopPolling();
        this._setStatus(message, 'pairing-error');
        this._setRetryVisible(true);
    }

    /** Start over: a fresh pair/start, and the dialog back in its waiting state. */
    _retry() {
        this.stopPolling();
        this._stage = null;
        this._setStatus(t('pairing.contacting'), 'pairing-info');
        this.startPairing();
    }

    _setStatus(text, kind) {
        if (!this.overlay) return;
        const statusEl = this.overlay.querySelector('.pairing-status-text');
        statusEl.textContent = text;
        statusEl.className = 'pairing-status-text ' + kind;
    }

    _setRetryVisible(visible) {
        if (!this.overlay) return;
        this.overlay.querySelector('.btn-pair-retry').hidden = !visible;
    }

    render() {
        this.overlay = document.createElement('div');
        this.overlay.className = 'pairing-overlay';
        this.overlay.innerHTML = `
            <div class="pairing-dialog">
                <h3>${this.esc(t('pairing.title', { name: this.host.displayName }))}</h3>
                <p class="pairing-instruction">
                    ${t('pairing.instruction')}
                </p>
                <div class="pairing-status">
                    <div class="pairing-pin-display">----</div>
                    <p class="pairing-status-text pairing-info">${t('pairing.contacting')}</p>
                </div>
                <div class="pairing-actions">
                    <button class="btn btn-primary btn-pair-retry" hidden>${t('common.retry')}</button>
                    <button class="btn btn-secondary btn-pair-cancel">${t('common.cancel')}</button>
                </div>
            </div>
        `;
    }

    bindEvents() {
        this.overlay
            .querySelector('.btn-pair-retry')
            .addEventListener('click', () => this._retry());

        // Once paired, closing is not cancelling: onComplete already ran.
        const dismiss = () => {
            this.close();
            if (!this._paired && this.onCancel) this.onCancel();
        };
        this.overlay.querySelector('.btn-pair-cancel').addEventListener('click', dismiss);

        this.overlay.addEventListener('click', (e) => {
            if (e.target === this.overlay) dismiss();
        });
    }

    esc(text) {
        return escapeHtml(text);
    }
}
