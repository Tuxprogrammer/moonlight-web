/*
 * MoonlightWeb — frontend TNR. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The hosts page's "turn Internet Access on" banner, and a build or run that
 * stays on the LAN (MW_LAN_ONLY). There the switch refuses to turn on, so a
 * banner pointing at it sends the reader to a setting that cannot help. The
 * host reports it in /api/internet/status as `lan_only`; with it the banner is
 * not shown, on the host machine or anywhere else, and the "other computers
 * can connect at" box (the LAN address) stays as it was.
 */

import { describe, it, expect, vi, beforeEach, afterEach, beforeAll } from 'vitest';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { MoonlightApp } from '../js/app.js';
import { BackendClient } from '../js/api/BackendClient.js';
import { init as i18nInit } from '../js/i18n/i18n.js';

const LOCALES = join(dirname(fileURLToPath(import.meta.url)), '..', 'locales');

// The real English strings: the banner's link is spliced into its sentence.
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

/** What /api/internet/status answers on a machine with Internet Access off. */
function status(extra = {}) {
    return {
        active: false,
        internet_access_enabled: false,
        local_ip: '192.0.2.10',
        domain: '',
        upnp_available: false,
        ...extra,
    };
}

let saved;

beforeEach(() => {
    saved = { adminLocked: BackendClient.adminLocked };
    BackendClient.adminLocked = false;
    document.body.innerHTML = '<div id="view-hosts"><div class="host-grid"></div></div>';
    vi.spyOn(BackendClient, 'getAdminSettings').mockResolvedValue({ https_port: 443 });
    vi.spyOn(MoonlightApp, '_isHostMachine').mockReturnValue(false);
    vi.spyOn(MoonlightApp, '_canReachAdmin').mockReturnValue(true);
    vi.spyOn(MoonlightApp, '_openOverlay').mockImplementation(() => {});
});

afterEach(() => {
    vi.restoreAllMocks();
    BackendClient.adminLocked = saved.adminLocked;
    document.body.innerHTML = '';
});

const banners = () => [...document.querySelectorAll('#view-hosts .remote-access-banner')];
const enableLink = () => document.getElementById('banner-enable-internet');

async function mountWith(answer) {
    vi.spyOn(BackendClient, 'getInternetStatus').mockResolvedValue(answer);
    await MoonlightApp._mountHostsBanners();
}

describe('the "turn Internet Access on" banner', () => {
    it('shows, with its link to the admin page, when the machine may go online', async () => {
        await mountWith(status({ lan_only: false }));
        expect(banners()).toHaveLength(1);
        expect(enableLink()).not.toBe(null);
        enableLink().click();
        expect(MoonlightApp._openOverlay).toHaveBeenCalledWith('admin', { scrollTo: 'internet' });
    });

    it('still shows when an older host does not report lan_only', async () => {
        await mountWith(status());
        expect(banners()).toHaveLength(1);
        expect(enableLink()).not.toBe(null);
    });

    it('is not shown when the host is LAN-only', async () => {
        await mountWith(status({ lan_only: true }));
        expect(banners()).toHaveLength(0);
        expect(enableLink()).toBe(null);
    });

    it('is not shown on a LAN-only host machine, which keeps its LAN address box', async () => {
        MoonlightApp._isHostMachine.mockReturnValue(true);
        await mountWith(status({ lan_only: true }));
        expect(banners()).toHaveLength(1);
        expect(banners()[0].textContent).toContain('https://192.0.2.10');
        expect(enableLink()).toBe(null);
    });

    it('shows below the address box on a host machine that may go online', async () => {
        MoonlightApp._isHostMachine.mockReturnValue(true);
        await mountWith(status({ lan_only: false }));
        expect(banners()).toHaveLength(2);
        expect(banners()[0].textContent).toContain('https://192.0.2.10');
        expect(banners()[1].querySelector('#banner-enable-internet')).not.toBe(null);
    });

    it('stays plain text for a browser that cannot open the admin page', async () => {
        MoonlightApp._canReachAdmin.mockReturnValue(false);
        await mountWith(status({ lan_only: false }));
        expect(banners()).toHaveLength(1);
        expect(enableLink()).toBe(null);
    });

    it('is not shown in booth mode, LAN-only or not', async () => {
        BackendClient.adminLocked = true;
        await mountWith(status({ lan_only: false }));
        expect(banners()).toHaveLength(0);
        document.body.innerHTML = '<div id="view-hosts"></div>';
        await mountWith(status({ lan_only: true }));
        expect(banners()).toHaveLength(0);
    });
});
