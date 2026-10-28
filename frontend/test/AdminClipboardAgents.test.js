/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

// The admin page holds the clipboard-agent panel: only when the server knows
// the routes and there is another host, mounted again after every render of
// the page, polled while the page is open, and stopped with it.
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getHosts: vi.fn(),
        getClipboardAgent: vi.fn(),
        setClipboardAgentEnabled: vi.fn(),
        issueClipboardAgentToken: vi.fn(),
        revokeClipboardAgentToken: vi.fn(),
    },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => (params ? key + ' ' + JSON.stringify(params) : key),
}));

import { AdminView } from '../js/ui/AdminView.js';
import { BackendClient } from '../js/api/BackendClient.js';

const VM = { uuid: 'VM-1', name: 'test box', pairState: 'paired', backendType: '' };
const NATIVE = {
    uuid: 'moonlightweb-native-host',
    name: 'this pc',
    pairState: 'paired',
    backendType: 'native',
};

describe('AdminView — clipboard on other hosts', () => {
    let view;

    beforeEach(() => {
        document.body.innerHTML = '<div></div>';
        view = new AdminView(document.body, () => {});
        for (const fn of Object.values(BackendClient)) fn.mockReset();
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    afterEach(() => {
        view.destroy();
        vi.useRealTimers();
        vi.restoreAllMocks();
    });

    it('keeps a panel when the server answers for another host', async () => {
        BackendClient.getHosts.mockResolvedValue({ hosts: [VM, NATIVE] });
        BackendClient.getClipboardAgent.mockResolvedValue({
            enabled: true,
            has_token: true,
            connected: true,
            backend: 'qt',
        });
        await view._loadClipboardAgents();
        expect(view._clipboardAgents).not.toBeNull();
        expect(view._clipboardAgents.hosts.map((h) => h.uuid)).toEqual(['VM-1']);

        // Mounted into the placeholder the page renders.
        document.body.innerHTML =
            '<div class="settings-section" id="admin-section-clipboard-agents"></div>';
        view.container = document.body;
        view._mountClipboardAgents();
        const sec = document.getElementById('admin-section-clipboard-agents');
        expect(sec.hidden).toBe(false);
        expect(sec.querySelectorAll('.ca-row').length).toBe(1);
        expect(sec.textContent).toContain('admin.clipboardAgents');
    });

    it('keeps none on an older server, or without another host', async () => {
        BackendClient.getHosts.mockResolvedValue({ hosts: [VM] });
        BackendClient.getClipboardAgent.mockRejectedValue(new Error('404'));
        await view._loadClipboardAgents();
        expect(view._clipboardAgents).toBeNull();

        BackendClient.getHosts.mockResolvedValue({ hosts: [NATIVE] });
        await view._loadClipboardAgents();
        expect(view._clipboardAgents).toBeNull();
        // Nothing to mount, and no placeholder: no throw.
        view._mountClipboardAgents();
    });

    it('stops polling the agents when the page closes', async () => {
        BackendClient.getHosts.mockResolvedValue({ hosts: [VM] });
        BackendClient.getClipboardAgent.mockResolvedValue({ enabled: false, has_token: false });
        await view._loadClipboardAgents();
        vi.useFakeTimers();
        view._clipboardAgents.startPolling(1000);
        await vi.advanceTimersByTimeAsync(1000);
        const calls = BackendClient.getClipboardAgent.mock.calls.length;
        expect(calls).toBeGreaterThan(1);
        view.destroy();
        await vi.advanceTimersByTimeAsync(5000);
        expect(BackendClient.getClipboardAgent.mock.calls.length).toBe(calls);
    });
});
