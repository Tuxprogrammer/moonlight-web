/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// "Clipboard on other hosts" on the admin page: a row per paired host that is
// another machine, with the switch, the agent's state and its token. A new
// token is shown once, with the lines to paste on the host, and forgotten on
// Done; rotating and revoking ask first; nothing is shown by an older server.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getHosts: vi.fn(),
        getClipboardAgent: vi.fn(),
        setClipboardAgentEnabled: vi.fn(),
        issueClipboardAgentToken: vi.fn(),
        revokeClipboardAgentToken: vi.fn(),
    },
}));
// The real English catalog, so every key this panel uses must exist there.
vi.mock('../js/i18n/i18n.js', async () => {
    const fs = await import('node:fs');
    const path = await import('node:path');
    const en = JSON.parse(fs.readFileSync(path.resolve(process.cwd(), 'locales/en.json'), 'utf8'));
    const get = (key) => key.split('.').reduce((o, p) => (o ? o[p] : undefined), en);
    return {
        t: (key, vars) => {
            const s = get(key);
            if (typeof s !== 'string') return key;
            return vars
                ? s.replace(/\{\{\s*(\w+)\s*\}\}/g, (m, n) => (n in vars ? String(vars[n]) : m))
                : s;
        },
    };
});
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn() },
}));

import {
    ClipboardAgentPanel,
    eligibleHosts,
    statusText,
    POLL_MS,
} from '../js/ui/ClipboardAgentPanel.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { Toast } from '../js/ui/Toast.js';

const VM = { uuid: 'VM-1', name: 'test box', pairState: 'paired', backendType: '' };
const OTHER = { uuid: 'PC-2', name: 'other pc', pairState: 'paired', backendType: 'wolf' };
const NATIVE = {
    uuid: 'moonlightweb-native-host',
    name: 'this pc',
    pairState: 'paired',
    backendType: 'native',
};
const UNPAIRED = { uuid: 'NEW-3', name: 'new', pairState: 'not_paired', backendType: '' };

const status = (over = {}) => ({
    enabled: false,
    has_token: false,
    token_created: 0,
    connected: false,
    available: false,
    last_seen: 0,
    platform: '',
    backend: '',
    agent_version: '',
    ...over,
});
const ISSUED = {
    token: 'mwca_' + 'A'.repeat(43),
    server: 'https://192.0.2.10:443',
    pins: ['sha256/AAAA='],
    setup: "mkdir -p ~/.config/moonlightweb && (umask 077 && printf '%s\\n' 'mwca_x' > f)",
    command: "moonlightweb-clipboard-agent --server 'https://192.0.2.10:443' --pin 'sha256/AAAA='",
    status: status({ has_token: true, token_created: 5, enabled: true }),
};

const flush = async () => {
    for (let i = 0; i < 6; i++) await Promise.resolve();
};

describe('ClipboardAgentPanel — pure parts', () => {
    it('lists the paired hosts that are other machines', () => {
        expect(eligibleHosts([VM, NATIVE, UNPAIRED, OTHER, null, {}]).map((h) => h.uuid)).toEqual([
            'VM-1',
            'PC-2',
        ]);
        expect(eligibleHosts(undefined)).toEqual([]);
    });

    it('says what the agent is doing', () => {
        const fmt = () => 'WHEN';
        expect(statusText(null)).toBe('Agent state unknown');
        expect(statusText(status())).toBe('No agent yet: generate a token');
        expect(statusText(status({ has_token: true }))).toBe('Agent not connected yet');
        expect(statusText(status({ has_token: true, last_seen: 7 }), fmt)).toBe(
            'Agent not connected (last seen WHEN)',
        );
        expect(
            statusText(
                status({ connected: true, enabled: true, backend: 'qt', platform: 'Debian' }),
            ),
        ).toBe('Agent connected (qt · Debian)');
        expect(statusText(status({ connected: true, enabled: false }))).toBe(
            'Agent connected, sharing off',
        );
        expect(statusText(status({ connected: true, enabled: true }))).toBe('Agent connected');
        // The default formatter is a locale date string.
        expect(statusText(status({ has_token: true, last_seen: 1000 }))).toContain('last seen');
    });
});

describe('ClipboardAgentPanel — the section', () => {
    let panel;
    let el;
    let confirm;

    const $ = (sel) => el.querySelector(sel);
    const click = async (sel) => {
        $(sel).click();
        await flush();
    };
    const load = async (statuses) => {
        BackendClient.getHosts.mockResolvedValue({ hosts: [VM, NATIVE, UNPAIRED, OTHER] });
        BackendClient.getClipboardAgent.mockImplementation(async (uuid) => {
            const s = statuses[uuid];
            if (s instanceof Error) throw s;
            return s;
        });
        await panel.load();
        panel.mount(el);
    };

    beforeEach(() => {
        document.body.innerHTML = '<div id="sec"></div>';
        el = document.getElementById('sec');
        for (const fn of Object.values(BackendClient)) fn.mockReset();
        Toast.success.mockReset();
        Toast.error.mockReset();
        confirm = vi.fn().mockResolvedValue(true);
        panel = new ClipboardAgentPanel({ confirm });
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        panel.destroy();
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('shows a row per eligible host, with its state and switch', async () => {
        await load({
            'VM-1': status({ connected: true, enabled: true, has_token: true, backend: 'qt' }),
            'PC-2': status(),
        });
        expect(panel.supported).toBe(true);
        expect(el.hidden).toBe(false);
        const rows = el.querySelectorAll('.ca-row');
        expect(rows.length).toBe(2);
        expect(rows[0].textContent).toContain('test box');
        expect(rows[0].querySelector('.ca-state').textContent).toContain('Agent connected');
        expect(rows[0].querySelector('.ca-state').className).toContain('ca-state-on');
        expect(rows[0].querySelector('.ca-enabled').checked).toBe(true);
        expect(rows[0].querySelector('[data-ca-action="rotate"]')).not.toBeNull();
        expect(rows[0].querySelector('[data-ca-action="revoke"]')).not.toBeNull();
        expect(rows[0].querySelector('[data-ca-action="issue"]')).toBeNull();
        expect(rows[1].querySelector('.ca-enabled').checked).toBe(false);
        expect(rows[1].querySelector('[data-ca-action="issue"]')).not.toBeNull();
        expect(rows[1].querySelector('[data-ca-action="revoke"]')).toBeNull();
        // Never asked about the native host or an unpaired one.
        expect(BackendClient.getClipboardAgent).toHaveBeenCalledTimes(2);
        expect(el.textContent).not.toContain('this pc');
        // Names are escaped.
        expect(panel.render()).not.toContain('<script');
    });

    it('escapes what the server says', async () => {
        BackendClient.getHosts.mockResolvedValue({
            hosts: [{ ...VM, name: '<img src=x onerror=alert(1)>' }],
        });
        BackendClient.getClipboardAgent.mockResolvedValue(
            status({ connected: true, enabled: true, backend: '<b>x</b>' }),
        );
        await panel.load();
        panel.mount(el);
        expect(el.querySelector('img')).toBeNull();
        expect(el.querySelector('b')).toBeNull();
        expect(el.textContent).toContain('<img src=x onerror=alert(1)>');
    });

    it('stays hidden on an older server, or with no other host', async () => {
        await load({ 'VM-1': new Error('404'), 'PC-2': new Error('404') });
        expect(panel.supported).toBe(false);
        expect(el.hidden).toBe(true);

        const p2 = new ClipboardAgentPanel({ confirm });
        BackendClient.getHosts.mockResolvedValue({ hosts: [NATIVE] });
        await p2.load();
        p2.mount(el);
        expect(el.hidden).toBe(true);

        const p3 = new ClipboardAgentPanel({ confirm });
        BackendClient.getHosts.mockRejectedValue(new Error('down'));
        await p3.load();
        expect(p3.hosts).toEqual([]);
        p3.mount(null); // nothing to mount into: no throw
    });

    it('turns the switch on and off', async () => {
        await load({ 'VM-1': status(), 'PC-2': status() });
        BackendClient.setClipboardAgentEnabled.mockResolvedValue(status({ enabled: true }));
        const box = $('.ca-row[data-uuid="VM-1"] .ca-enabled');
        box.checked = true;
        box.dispatchEvent(new Event('change', { bubbles: true }));
        await flush();
        expect(BackendClient.setClipboardAgentEnabled).toHaveBeenCalledWith('VM-1', true);
        expect(Toast.success).toHaveBeenCalledWith('Clipboard sharing on for this host');
        expect($('.ca-row[data-uuid="VM-1"] .ca-enabled').checked).toBe(true);

        BackendClient.setClipboardAgentEnabled.mockResolvedValue(status({ enabled: false }));
        const box2 = $('.ca-row[data-uuid="VM-1"] .ca-enabled');
        box2.checked = false;
        box2.dispatchEvent(new Event('change', { bubbles: true }));
        await flush();
        expect(BackendClient.setClipboardAgentEnabled).toHaveBeenLastCalledWith('VM-1', false);
        expect(Toast.success).toHaveBeenLastCalledWith('Clipboard sharing off for this host');
    });

    it('puts the switch back when the server refuses', async () => {
        await load({ 'VM-1': status(), 'PC-2': status() });
        BackendClient.setClipboardAgentEnabled.mockRejectedValue(new Error('403'));
        const box = $('.ca-row[data-uuid="VM-1"] .ca-enabled');
        box.checked = true;
        box.dispatchEvent(new Event('change', { bubbles: true }));
        await flush();
        expect(Toast.error).toHaveBeenCalledWith('The server refused the change');
        expect($('.ca-row[data-uuid="VM-1"] .ca-enabled').checked).toBe(false);
    });

    it('ignores changes that are not the switch, and unknown hosts', async () => {
        await load({ 'VM-1': status(), 'PC-2': status() });
        el.insertAdjacentHTML(
            'beforeend',
            '<input id="x" type="checkbox"><button data-ca-action="issue" data-uuid="nope" id="y"></button>',
        );
        $('#x').dispatchEvent(new Event('change', { bubbles: true }));
        await click('#y');
        const box = document.createElement('input');
        box.className = 'ca-enabled';
        box.dataset.uuid = 'nope';
        el.appendChild(box);
        box.dispatchEvent(new Event('change', { bubbles: true }));
        await flush();
        expect(BackendClient.setClipboardAgentEnabled).not.toHaveBeenCalled();
        expect(BackendClient.issueClipboardAgentToken).not.toHaveBeenCalled();
    });

    it('shows a new token once, with the lines to paste, until Done', async () => {
        await load({ 'VM-1': status({ enabled: true }), 'PC-2': status() });
        BackendClient.issueClipboardAgentToken.mockResolvedValue(ISSUED);
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="issue"]');
        expect(BackendClient.issueClipboardAgentToken).toHaveBeenCalledWith('VM-1');
        expect(confirm).not.toHaveBeenCalled(); // nothing to lose the first time
        const box = $('.ca-row[data-uuid="VM-1"] .ca-issued');
        expect(box).not.toBeNull();
        expect(box.textContent).toContain('This token is shown only once.');
        expect(box.textContent).toContain('On test box');
        expect($('[data-ca-code="setup"]').textContent).toBe(ISSUED.setup);
        expect($('[data-ca-code="command"]').textContent).toBe(ISSUED.command);
        expect($('[data-ca-code="token"]').textContent).toBe(ISSUED.token);
        expect(box.textContent).toContain('https://192.0.2.10:443');
        expect(box.textContent).toContain('sha256/AAAA=');
        // The row now offers rotate/revoke (the answer carried the status).
        expect($('.ca-row[data-uuid="VM-1"] [data-ca-action="rotate"]')).not.toBeNull();
        // Polling redraws keep it…
        BackendClient.getClipboardAgent.mockResolvedValue(ISSUED.status);
        await panel.refresh();
        expect($('.ca-row[data-uuid="VM-1"] .ca-issued')).not.toBeNull();
        // …Done forgets it.
        await click('[data-ca-action="done"]');
        expect($('.ca-issued')).toBeNull();
        expect(panel.issued.size).toBe(0);
        expect(el.textContent).not.toContain(ISSUED.token);
        // Never written anywhere the page keeps.
        expect(JSON.stringify(localStorage)).not.toContain('mwca_');
    });

    it('copies each line, and selects it when the clipboard refuses', async () => {
        await load({ 'VM-1': status(), 'PC-2': status() });
        BackendClient.issueClipboardAgentToken.mockResolvedValue(ISSUED);
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="issue"]');
        const writeText = vi.fn().mockResolvedValue(undefined);
        Object.defineProperty(navigator, 'clipboard', { value: { writeText }, configurable: true });
        vi.useFakeTimers();
        const btn = $('[data-ca-action="copy"][data-key="setup"]');
        btn.click();
        await flush();
        expect(writeText).toHaveBeenCalledWith(ISSUED.setup);
        expect(btn.textContent).toBe('Copied!');
        vi.advanceTimersByTime(2000);
        expect(btn.textContent).toBe('Copy');
        vi.useRealTimers();

        writeText.mockRejectedValue(new Error('denied'));
        const tokenBtn = $('[data-ca-action="copy"][data-key="token"]');
        tokenBtn.click();
        await flush();
        expect(window.getSelection().toString()).toBe(ISSUED.token);

        // A key with nothing behind it copies nothing.
        writeText.mockClear();
        tokenBtn.dataset.key = 'nothing';
        tokenBtn.click();
        await flush();
        expect(writeText).not.toHaveBeenCalled();
    });

    it('asks before replacing a token, and does nothing when declined', async () => {
        await load({ 'VM-1': status({ has_token: true }), 'PC-2': status() });
        confirm.mockResolvedValueOnce(false);
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="rotate"]');
        expect(confirm).toHaveBeenCalledWith(expect.stringContaining('test box'));
        expect(BackendClient.issueClipboardAgentToken).not.toHaveBeenCalled();

        BackendClient.issueClipboardAgentToken.mockResolvedValue(ISSUED);
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="rotate"]');
        expect(BackendClient.issueClipboardAgentToken).toHaveBeenCalledWith('VM-1');
        expect($('.ca-issued')).not.toBeNull();
    });

    it('asks before revoking, then forgets any token on show', async () => {
        await load({ 'VM-1': status({ enabled: true }), 'PC-2': status() });
        BackendClient.issueClipboardAgentToken.mockResolvedValue(ISSUED);
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="issue"]');
        confirm.mockResolvedValueOnce(false);
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="revoke"]');
        expect(BackendClient.revokeClipboardAgentToken).not.toHaveBeenCalled();
        expect(confirm).toHaveBeenLastCalledWith(
            expect.stringContaining('test box'),
            expect.objectContaining({ danger: true }),
        );

        BackendClient.revokeClipboardAgentToken.mockResolvedValue(status({ enabled: true }));
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="revoke"]');
        expect(BackendClient.revokeClipboardAgentToken).toHaveBeenCalledWith('VM-1');
        expect(Toast.success).toHaveBeenCalledWith('Token revoked');
        expect($('.ca-issued')).toBeNull();
        expect($('.ca-row[data-uuid="VM-1"] [data-ca-action="issue"]')).not.toBeNull();
    });

    it('says so when a token request fails', async () => {
        await load({ 'VM-1': status({ has_token: true }), 'PC-2': status() });
        BackendClient.issueClipboardAgentToken.mockRejectedValue(new Error('500'));
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="rotate"]');
        expect(Toast.error).toHaveBeenCalledWith('The server refused the change');
        expect($('.ca-issued')).toBeNull();
        BackendClient.revokeClipboardAgentToken.mockRejectedValue(new Error('500'));
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="revoke"]');
        expect(Toast.error).toHaveBeenCalledTimes(2);
    });

    it('disables a row while its request runs, and ignores a second click', async () => {
        await load({ 'VM-1': status(), 'PC-2': status() });
        let resolve;
        BackendClient.issueClipboardAgentToken.mockReturnValue(new Promise((r) => (resolve = r)));
        $('.ca-row[data-uuid="VM-1"] [data-ca-action="issue"]').click();
        await flush();
        expect($('.ca-row[data-uuid="VM-1"] [data-ca-action="issue"]').disabled).toBe(true);
        expect($('.ca-row[data-uuid="VM-1"] .ca-enabled').disabled).toBe(true);
        expect($('.ca-row[data-uuid="PC-2"] [data-ca-action="issue"]').disabled).toBe(false);
        await panel._run('VM-1', async () => {
            throw new Error('must not run');
        });
        resolve(ISSUED);
        await flush();
        expect(BackendClient.issueClipboardAgentToken).toHaveBeenCalledTimes(1);
        expect($('.ca-row[data-uuid="VM-1"] [data-ca-action="rotate"]').disabled).toBe(false);
    });

    it('polls the agents while open, and stops when destroyed', async () => {
        await load({ 'VM-1': status({ has_token: true }), 'PC-2': status() });
        vi.useFakeTimers();
        BackendClient.getClipboardAgent.mockReset();
        BackendClient.getClipboardAgent.mockResolvedValue(
            status({ has_token: true, connected: true, enabled: true, backend: 'wl-clipboard' }),
        );
        panel.startPolling();
        await vi.advanceTimersByTimeAsync(POLL_MS);
        expect(BackendClient.getClipboardAgent).toHaveBeenCalledTimes(2);
        expect($('.ca-row[data-uuid="VM-1"] .ca-state').textContent).toContain('wl-clipboard');
        // A failing poll keeps the last state.
        BackendClient.getClipboardAgent.mockRejectedValue(new Error('down'));
        await vi.advanceTimersByTimeAsync(POLL_MS);
        expect($('.ca-row[data-uuid="VM-1"] .ca-state').textContent).toContain('wl-clipboard');
        panel.destroy();
        const calls = BackendClient.getClipboardAgent.mock.calls.length;
        await vi.advanceTimersByTimeAsync(POLL_MS * 3);
        expect(BackendClient.getClipboardAgent.mock.calls.length).toBe(calls);
        panel.refresh(); // detached: no throw
    });

    it('mounts again into a fresh placeholder without losing its state', async () => {
        await load({ 'VM-1': status(), 'PC-2': status() });
        BackendClient.issueClipboardAgentToken.mockResolvedValue(ISSUED);
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="issue"]');
        // AdminView re-rendered its page: a new placeholder.
        document.body.innerHTML = '<div id="sec2"></div>';
        el = document.getElementById('sec2');
        panel.mount(el);
        expect($('.ca-issued')).not.toBeNull();
        // Its listeners work on the new element (and are not doubled).
        await click('[data-ca-action="done"]');
        expect($('.ca-issued')).toBeNull();
        panel.mount(el);
        BackendClient.issueClipboardAgentToken.mockClear();
        await click('.ca-row[data-uuid="VM-1"] [data-ca-action="rotate"]');
        expect(BackendClient.issueClipboardAgentToken).toHaveBeenCalledTimes(1);
    });
});
