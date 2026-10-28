/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The pairing dialog while it waits. The PIN is on screen at once, but typing
 * it is only useful once the host shows the request, which can take a while:
 * the dialog says "waiting for the host" until the server reports the host has
 * it (`stage`, docs/wiki/08-REST-API.md), then "enter this PIN", then that the
 * PIN is being checked. A host that never shows it gets a soft "still
 * waiting" after a minute, with a Retry that starts over; nothing fails on
 * its own. A server that does not report `stage` keeps the dialog as it was.
 */

import { describe, it, expect, vi, beforeAll, beforeEach, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { PairDialog } from '../js/ui/PairDialog.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { init as i18nInit, t } from '../js/i18n/i18n.js';

const LOCALES = join(dirname(fileURLToPath(import.meta.url)), '..', 'locales');

beforeAll(async () => {
    localStorage.setItem('mw-lang', 'en');
    const en = JSON.parse(readFileSync(join(LOCALES, 'en.json'), 'utf8'));
    vi.stubGlobal(
        'fetch',
        vi.fn(async () => ({ ok: true, status: 200, json: async () => en })),
    );
    await i18nInit();
    vi.unstubAllGlobals();
});

const HOST = { uuid: 'host-1', displayName: 'Den PC' };
const waiting = (stage) => ({
    status: 'awaiting_pin',
    stage,
    host_contacted: stage !== 'contacting',
});

let dialog;
let polls; // what the next /pair polls answer, in order; the last one repeats

beforeEach(() => {
    vi.useFakeTimers();
    polls = [waiting('contacting')];
    vi.spyOn(BackendClient, 'startPairing').mockResolvedValue({
        status: 'initiated',
        pin: '0420',
        stage: 'contacting',
        host_contacted: false,
    });
    vi.spyOn(BackendClient, 'confirmPairing').mockImplementation(async () =>
        polls.length > 1 ? polls.shift() : polls[0],
    );
    vi.spyOn(console, 'warn').mockImplementation(() => {});
});

afterEach(() => {
    if (dialog) dialog.close();
    dialog = null;
    vi.useRealTimers();
    vi.restoreAllMocks();
    document.body.innerHTML = '';
});

const status = () => document.querySelector('.pairing-status-text');
const pin = () => document.querySelector('.pairing-pin-display');
const retry = () => document.querySelector('.btn-pair-retry');

async function open() {
    dialog = new PairDialog(HOST);
    dialog.show();
    await vi.advanceTimersByTimeAsync(0);
}

describe('PairDialog while the host is contacted', () => {
    it('shows the PIN and says it waits for the host to show the request', async () => {
        await open();
        expect(pin().textContent).toBe('0420');
        expect(status().textContent).toBe(t('pairing.waitingForHost', { name: 'Den PC' }));
        expect(status().textContent).toContain('Den PC');
        expect(status().classList.contains('pairing-info')).toBe(true);
        expect(retry().hidden).toBe(true);
    });

    it('asks for the PIN as soon as the host has the request', async () => {
        polls = [waiting('contacting'), waiting('enter_pin')];
        await open();
        await vi.advanceTimersByTimeAsync(1000);
        expect(status().textContent).toBe(t('pairing.waitingForHost', { name: 'Den PC' }));
        await vi.advanceTimersByTimeAsync(1000);
        expect(status().textContent).toBe(t('pairing.enterPin'));
    });

    it("polls every second, on the host's turn and on the user's", async () => {
        // "Paired" then shows within a second of the PIN being typed.
        polls = [waiting('contacting'), waiting('contacting'), waiting('enter_pin')];
        await open();
        await vi.advanceTimersByTimeAsync(3000);
        expect(BackendClient.confirmPairing).toHaveBeenCalledTimes(3);
        await vi.advanceTimersByTimeAsync(900);
        expect(BackendClient.confirmPairing).toHaveBeenCalledTimes(3);
        await vi.advanceTimersByTimeAsync(100);
        expect(BackendClient.confirmPairing).toHaveBeenCalledTimes(4);
        await vi.advanceTimersByTimeAsync(1000);
        expect(BackendClient.confirmPairing).toHaveBeenCalledTimes(5);
    });

    it('says the PIN is being checked once it was typed', async () => {
        polls = [waiting('enter_pin'), waiting('verifying')];
        await open();
        await vi.advanceTimersByTimeAsync(1000);
        expect(status().textContent).toBe(t('pairing.enterPin'));
        await vi.advanceTimersByTimeAsync(1000);
        expect(status().textContent).toBe(t('pairing.verifying'));
    });

    it('starts on "enter the PIN" when the host already has the request', async () => {
        BackendClient.startPairing.mockResolvedValue({
            status: 'initiated',
            pin: '1111',
            stage: 'enter_pin',
            host_contacted: true,
        });
        polls = [waiting('enter_pin')];
        await open();
        expect(status().textContent).toBe(t('pairing.enterPin'));
        await vi.advanceTimersByTimeAsync(900);
        expect(BackendClient.confirmPairing).not.toHaveBeenCalled();
        await vi.advanceTimersByTimeAsync(100);
        expect(BackendClient.confirmPairing).toHaveBeenCalledTimes(1);
    });

    it('keeps the old wording and cadence with a server that reports no stage', async () => {
        BackendClient.startPairing.mockResolvedValue({ status: 'initiated', pin: '2222' });
        polls = [{ status: 'awaiting_pin' }];
        await open();
        expect(status().textContent).toBe(t('pairing.enterPin'));
        await vi.advanceTimersByTimeAsync(4900);
        expect(BackendClient.confirmPairing).not.toHaveBeenCalled();
        await vi.advanceTimersByTimeAsync(100);
        expect(BackendClient.confirmPairing).toHaveBeenCalledTimes(1);
        expect(status().textContent).toBe(t('pairing.enterPin'));
    });
});

describe('PairDialog after a minute', () => {
    it('says it is still waiting, offers Retry, and keeps polling', async () => {
        await open();
        await vi.advanceTimersByTimeAsync(59000);
        expect(retry().hidden).toBe(true);
        await vi.advanceTimersByTimeAsync(1000);
        expect(status().textContent).toBe(t('pairing.stillWaiting', { name: 'Den PC' }));
        expect(status().classList.contains('pairing-error')).toBe(false);
        expect(retry().hidden).toBe(false);
        const calls = BackendClient.confirmPairing.mock.calls.length;
        await vi.advanceTimersByTimeAsync(1000);
        expect(BackendClient.confirmPairing.mock.calls.length).toBeGreaterThan(calls);
        // Not an error: the PIN stays on screen.
        expect(pin().textContent).toBe('0420');
    });

    it('still asks for the PIN behind it when the host has the request', async () => {
        polls = [waiting('enter_pin')];
        await open();
        await vi.advanceTimersByTimeAsync(60000);
        expect(status().textContent).toBe(t('pairing.stillWaiting', { name: 'Den PC' }));
        expect(retry().hidden).toBe(false);
    });

    it('lets a PIN being checked replace the message', async () => {
        polls = [waiting('contacting')];
        await open();
        await vi.advanceTimersByTimeAsync(60000);
        polls = [waiting('verifying')];
        await vi.advanceTimersByTimeAsync(1000);
        expect(status().textContent).toBe(t('pairing.verifying'));
        expect(retry().hidden).toBe(true);
    });

    it('Retry starts over: a fresh start, the waiting state, another minute', async () => {
        await open();
        await vi.advanceTimersByTimeAsync(60000);
        BackendClient.startPairing.mockResolvedValue({
            status: 'initiated',
            pin: '5678',
            stage: 'contacting',
            host_contacted: false,
        });
        retry().click();
        await vi.advanceTimersByTimeAsync(0);
        expect(BackendClient.startPairing).toHaveBeenCalledTimes(2);
        expect(pin().textContent).toBe('5678');
        expect(status().textContent).toBe(t('pairing.waitingForHost', { name: 'Den PC' }));
        expect(retry().hidden).toBe(true);
        await vi.advanceTimersByTimeAsync(59000);
        expect(retry().hidden).toBe(true);
        await vi.advanceTimersByTimeAsync(1000);
        expect(retry().hidden).toBe(false);
    });

    it('Retry does not leave the old polling running beside the new one', async () => {
        await open();
        await vi.advanceTimersByTimeAsync(60000);
        retry().click();
        await vi.advanceTimersByTimeAsync(0);
        const before = BackendClient.confirmPairing.mock.calls.length;
        await vi.advanceTimersByTimeAsync(1000);
        expect(BackendClient.confirmPairing.mock.calls.length).toBe(before + 1);
    });
});

describe('PairDialog endings', () => {
    it('reports success, calls back and closes', async () => {
        polls = [waiting('verifying'), { status: 'paired' }];
        await open();
        dialog.onComplete = vi.fn();
        await vi.advanceTimersByTimeAsync(1000);
        await vi.advanceTimersByTimeAsync(1000);
        expect(status().textContent).toBe(t('pairing.paired'));
        expect(status().classList.contains('pairing-success')).toBe(true);
        expect(dialog.onComplete).toHaveBeenCalledTimes(1);
        // Nothing left to cancel: the button says Close until the dialog goes.
        const button = document.querySelector('.btn-pair-cancel');
        expect(button.textContent).toBe(t('common.close'));
        expect(retry().hidden).toBe(true);
        await vi.advanceTimersByTimeAsync(1500);
        expect(document.querySelector('.pairing-overlay')).toBe(null);
    });

    it('Close after success closes without cancelling', async () => {
        polls = [{ status: 'paired' }];
        await open();
        dialog.onComplete = vi.fn();
        dialog.onCancel = vi.fn();
        await vi.advanceTimersByTimeAsync(1000);
        document.querySelector('.btn-pair-cancel').click();
        expect(document.querySelector('.pairing-overlay')).toBe(null);
        // onComplete already restarted the host list; a cancel would do it twice.
        expect(dialog.onCancel).not.toHaveBeenCalled();
        expect(dialog.onComplete).toHaveBeenCalledTimes(1);
        await vi.advanceTimersByTimeAsync(1500); // the auto-close finds nothing to do
    });

    it('the button reads Cancel while pairing is still under way', async () => {
        await open();
        expect(document.querySelector('.btn-pair-cancel').textContent).toBe(t('common.cancel'));
    });

    it('shows a terminal error, stops polling, and offers Retry', async () => {
        polls = [{ status: 'error', message: 'Pairing already in progress on host.' }];
        await open();
        await vi.advanceTimersByTimeAsync(1000);
        expect(status().textContent).toBe('Pairing already in progress on host.');
        expect(status().classList.contains('pairing-error')).toBe(true);
        expect(retry().hidden).toBe(false);
        await vi.advanceTimersByTimeAsync(120000);
        expect(BackendClient.confirmPairing).toHaveBeenCalledTimes(1);
        // No "still waiting" over an error.
        expect(status().textContent).toBe('Pairing already in progress on host.');
    });

    it('shows a refused start with Retry', async () => {
        BackendClient.startPairing.mockResolvedValue({
            status: 'error',
            message: 'Host is offline',
        });
        await open();
        expect(status().textContent).toBe('Host is offline');
        expect(retry().hidden).toBe(false);
        expect(BackendClient.confirmPairing).not.toHaveBeenCalled();
    });

    it('keeps polling through a network error', async () => {
        BackendClient.confirmPairing
            .mockRejectedValueOnce(new Error('Failed to fetch'))
            .mockResolvedValue(waiting('enter_pin'));
        await open();
        await vi.advanceTimersByTimeAsync(1000);
        expect(status().textContent).toBe(t('pairing.waitingForHost', { name: 'Den PC' }));
        await vi.advanceTimersByTimeAsync(1000);
        expect(status().textContent).toBe(t('pairing.enterPin'));
    });

    it('Cancel closes it and nothing runs after', async () => {
        await open();
        dialog.onCancel = vi.fn();
        document.querySelector('.btn-pair-cancel').click();
        expect(dialog.onCancel).toHaveBeenCalledTimes(1);
        expect(document.querySelector('.pairing-overlay')).toBe(null);
        await vi.advanceTimersByTimeAsync(120000);
        expect(BackendClient.confirmPairing).not.toHaveBeenCalled();
    });

    it('a hidden Retry stays hidden under the stylesheet', () => {
        // jsdom applies no CSS: .btn's inline-flex would outrank [hidden] in a browser.
        const css = readFileSync(join(LOCALES, '..', 'css', 'components.css'), 'utf8');
        expect(css).toMatch(/\.pairing-actions \.btn\[hidden\]\s*\{\s*display:\s*none;/);
    });

    it('a host name with markup is shown as text', async () => {
        dialog = new PairDialog({ uuid: 'h', displayName: '<b>x</b>' });
        dialog.show();
        await vi.advanceTimersByTimeAsync(0);
        expect(document.querySelector('.pairing-overlay b')).toBe(null);
        expect(status().textContent).toContain('<b>x</b>');
    });
});
