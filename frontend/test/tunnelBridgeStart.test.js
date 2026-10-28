/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * startTunnel() and the bootstrap's transport. A page reached at the host's own
 * address (a LAN IP, localhost, a mesh VPN name) has no /v1/tunnel.js to load:
 * asking for it anyway costs a 404 and a MIME-type error in every console. So a
 * direct page never imports it, and a page served by the bootstrap's worker
 * still does, and still comes up on the connection it makes.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

const HOST = 'a'.repeat(26);

/** Served by the bootstrap's worker (tunnelled), or not (direct). */
function servedBy({ worker, controller }) {
    if (!worker) {
        delete navigator.serviceWorker;
        return;
    }
    Object.defineProperty(navigator, 'serviceWorker', {
        configurable: true,
        value: { controller: controller ? {} : null, addEventListener: vi.fn() },
    });
}

/** A stand-in for the bootstrap's /v1/tunnel.js module. */
function fakeTransport({ id = HOST, connect = () => Promise.resolve() } = {}) {
    const tunnels = [];
    class Tunnel {
        constructor(hostId) {
            this.hostId = hostId;
            this.connected = true;
            this.connect = vi.fn(connect);
            tunnels.push(this);
        }
    }
    return {
        tunnels,
        module: {
            Tunnel,
            hostIdFromLocation: () => id,
            hostKeyFromLocation: () => 'key-1',
            shareTokenFromLocation: () => null,
        },
    };
}

let bridge;

beforeEach(async () => {
    vi.resetModules();
    bridge = await import('../js/net/tunnelBridge.js');
});

afterEach(() => {
    delete navigator.serviceWorker;
    vi.restoreAllMocks();
});

describe('startTunnel on a direct page', () => {
    it('never imports the transport without service worker support', async () => {
        servedBy({ worker: false });
        const load = vi.spyOn(bridge.transportLoader, 'load');
        expect(await bridge.startTunnel()).toBe(false);
        expect(load).not.toHaveBeenCalled();
        expect(bridge.isTunnelMode()).toBe(false);
    });

    it('never imports the transport when no worker controls the page', async () => {
        servedBy({ worker: true, controller: false });
        const load = vi.spyOn(bridge.transportLoader, 'load');
        expect(await bridge.startTunnel()).toBe(false);
        expect(load).not.toHaveBeenCalled();
        expect(bridge.isTunnelMode()).toBe(false);
        expect(bridge.tunnelHostId()).toBe(null);
    });

    it('leaves the signalling socket a plain WebSocket', async () => {
        servedBy({ worker: true, controller: false });
        await bridge.startTunnel();
        const ws = vi.fn();
        vi.stubGlobal('WebSocket', ws);
        bridge.openSignalingSocket('wss://192.0.2.10/ws/stream');
        expect(ws).toHaveBeenCalledWith('wss://192.0.2.10/ws/stream');
        vi.unstubAllGlobals();
    });
});

describe('startTunnel on a page served by the bootstrap', () => {
    it('imports /v1/tunnel.js from its own origin and comes up on it', async () => {
        servedBy({ worker: true, controller: true });
        const { module, tunnels } = fakeTransport();
        const load = vi.spyOn(bridge.transportLoader, 'load').mockResolvedValue(module);
        const stages = vi.fn();

        expect(await bridge.startTunnel(stages)).toBe(true);
        expect(load).toHaveBeenCalledTimes(1);
        expect(load).toHaveBeenCalledWith(new URL('/v1/tunnel.js', location.origin).href);
        expect(tunnels).toHaveLength(1);
        expect(tunnels[0].hostId).toBe(HOST);
        expect(tunnels[0].onstatus).toBe(stages);
        expect(tunnels[0].connect).toHaveBeenCalled();
        expect(bridge.isTunnelMode()).toBe(true);
        expect(bridge.tunnelHostId()).toBe(HOST);
        expect(bridge.takeTunnelHostKey()).toBe('key-1');
        // The worker's requests are answered from here on.
        expect(navigator.serviceWorker.addEventListener).toHaveBeenCalledWith(
            'message',
            expect.any(Function),
        );
    });

    it('hands back to the bootstrap when nothing names a machine', async () => {
        servedBy({ worker: true, controller: true });
        const { module, tunnels } = fakeTransport({ id: null });
        vi.spyOn(bridge.transportLoader, 'load').mockResolvedValue(module);
        const replace = vi.fn();
        const original = window.location;
        Object.defineProperty(window, 'location', {
            value: { origin: original.origin, replace },
            configurable: true,
        });
        try {
            let settled = false;
            bridge.startTunnel().then(() => (settled = true));
            await vi.waitFor(() => expect(replace).toHaveBeenCalledWith('/?mw=pick'));
            await Promise.resolve();
            // The navigation is under way: the application must not start under it.
            expect(settled).toBe(false);
            expect(tunnels).toHaveLength(0);
        } finally {
            Object.defineProperty(window, 'location', { value: original, configurable: true });
        }
    });

    it('resolves false when the transport cannot be loaded', async () => {
        servedBy({ worker: true, controller: true });
        vi.spyOn(bridge.transportLoader, 'load').mockRejectedValue(new TypeError('404'));
        expect(await bridge.startTunnel()).toBe(false);
        expect(bridge.isTunnelMode()).toBe(false);
    });

    it('rejects when the host does not answer', async () => {
        servedBy({ worker: true, controller: true });
        const { module } = fakeTransport({
            connect: () => Promise.reject(new Error('no answer')),
        });
        vi.spyOn(bridge.transportLoader, 'load').mockResolvedValue(module);
        await expect(bridge.startTunnel()).rejects.toThrow('no answer');
        expect(bridge.isTunnelMode()).toBe(false);
    });
});
