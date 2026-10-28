/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The web manifest is fetched with credentials. A browser asks for the file a
 * <link rel="manifest"> names in CORS mode without cookies unless the link says
 * `crossorigin="use-credentials"`, so behind a proxy that signs users in (a
 * cookie on every request) the manifest is the one request that arrives
 * anonymous, is refused, and the page cannot be installed. On a page served
 * straight by the host nothing changes: the manifest needs no cookie there.
 */

import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const FRONTEND = join(dirname(fileURLToPath(import.meta.url)), '..');
const HOST = 'b'.repeat(26);

vi.mock('../js/net/tunnelBridge.js', async (importOriginal) => ({
    ...(await importOriginal()),
    tunnelHostId: vi.fn(() => HOST),
}));

const { MoonlightApp } = await import('../js/app.js');
const bridge = await import('../js/net/tunnelBridge.js');

describe('the manifest link in the shell', () => {
    const html = readFileSync(join(FRONTEND, 'index.html'), 'utf8');
    const doc = new DOMParser().parseFromString(html, 'text/html');

    it('is there once, and asks for the manifest with credentials', () => {
        const links = doc.querySelectorAll('link[rel="manifest"]');
        expect(links).toHaveLength(1);
        expect(links[0].getAttribute('href')).toBe('/manifest.webmanifest');
        expect(links[0].getAttribute('crossorigin')).toBe('use-credentials');
    });
});

describe('the manifest link after the home-screen handoff', () => {
    let cache;

    beforeEach(() => {
        cache = { put: vi.fn(async () => {}) };
        vi.stubGlobal('caches', { open: vi.fn(async () => cache) });
        vi.stubGlobal(
            'fetch',
            vi.fn(async () => ({ ok: true, json: async () => ({ name: 'MoonlightWeb' }) })),
        );
        vi.spyOn(MoonlightApp, '_runsAsShortcut').mockReturnValue(false);
        bridge.tunnelHostId.mockReturnValue(HOST);
    });

    afterEach(() => {
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
        document.head.innerHTML = '';
    });

    it('keeps asking with credentials when it is pointed at the fresh copy', async () => {
        document.head.innerHTML =
            '<link rel="manifest" href="/manifest.webmanifest" crossorigin="use-credentials" />';
        await MoonlightApp._offerHomeScreenHandoff();
        const link = document.querySelector('link[rel="manifest"]');
        expect(link.getAttribute('href')).toMatch(/^\/manifest\.webmanifest\?t=\d+$/);
        expect(link.getAttribute('crossorigin')).toBe('use-credentials');
        expect(cache.put).toHaveBeenCalledWith('/manifest.webmanifest', expect.anything());
    });

    it('adds the credentials to a link that came without them', async () => {
        // A shell cached by an older build.
        document.head.innerHTML = '<link rel="manifest" href="/manifest.webmanifest" />';
        await MoonlightApp._offerHomeScreenHandoff();
        const link = document.querySelector('link[rel="manifest"]');
        expect(link.getAttribute('crossorigin')).toBe('use-credentials');
        expect(link.getAttribute('href')).toMatch(/\?t=\d+$/);
    });

    it('leaves the link alone when the host gives no fresh manifest', async () => {
        document.head.innerHTML = '<link rel="manifest" href="/manifest.webmanifest" />';
        fetch.mockResolvedValue({ ok: false });
        await MoonlightApp._offerHomeScreenHandoff();
        const link = document.querySelector('link[rel="manifest"]');
        expect(link.getAttribute('href')).toBe('/manifest.webmanifest');
        expect(link.hasAttribute('crossorigin')).toBe(false);
    });

    it('leaves the link alone on a direct page', async () => {
        document.head.innerHTML = '<link rel="manifest" href="/manifest.webmanifest" />';
        bridge.tunnelHostId.mockReturnValue(null);
        await MoonlightApp._offerHomeScreenHandoff();
        expect(fetch).not.toHaveBeenCalled();
        const link = document.querySelector('link[rel="manifest"]');
        expect(link.getAttribute('href')).toBe('/manifest.webmanifest');
    });
});
