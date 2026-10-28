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
 * The browser's Connection choice (Settings → Connection): `transport_pref`
 * in `mw-streaming-settings`, sent in the launch body. The backend filters the
 * chain by it inside the admin's `allowed_transports`
 * (backend/src/streaming/TransportChain.h); this module decides what may be
 * offered, so a choice the server would answer with 409 is never on the menu.
 */
import { pageCameThroughTunnel } from '../net/tunnelBridge.js';

export const TRANSPORT_PREFS = ['auto', 'webrtc', 'wss'];
export const ALLOWED_TRANSPORTS = ['all', 'webrtc', 'wss'];

/** True when this browser can build a WebRTC peer connection at all. */
export function browserHasWebrtc() {
    return typeof globalThis.RTCPeerConnection === 'function';
}

/** The admin's policy as the server reported it; anything else is "all". */
export function normalizeAllowed(value) {
    return ALLOWED_TRANSPORTS.includes(value) ? value : 'all';
}

/** A stored preference; anything else is "auto". */
export function normalizePref(value) {
    return TRANSPORT_PREFS.includes(value) ? value : 'auto';
}

/**
 * The preferences worth offering: Automatic, plus each family the policy
 * allows and this page can run — no WebRTC without RTCPeerConnection, no
 * WebSocket through the rendezvous (it rides the TLS connection that served
 * the page). With one family or none left there is nothing to choose, and
 * only Automatic comes back.
 * @param {{allowed?: string, webrtc?: boolean, tunnelled?: boolean}} [opts]
 * @returns {string[]}
 */
export function transportChoices({
    allowed = 'all',
    webrtc = browserHasWebrtc(),
    tunnelled = pageCameThroughTunnel(),
} = {}) {
    const policy = normalizeAllowed(allowed);
    const families = [];
    if (policy !== 'wss' && webrtc) families.push('webrtc');
    if (policy !== 'webrtc' && !tunnelled) families.push('wss');
    return families.length > 1 ? ['auto', ...families] : ['auto'];
}

/** @p pref when it is on offer, else Automatic. */
export function clampTransportPref(pref, choices) {
    const p = normalizePref(pref);
    return choices.includes(p) ? p : 'auto';
}

/**
 * The preference a launch sends, from what this page knows without asking the
 * server: a stored WebRTC preference on a browser that lost WebRTC, or a
 * WebSocket one on a page that came through the rendezvous, would only leave
 * the backend an empty chain.
 * @param {*} pref the stored transport_pref
 * @param {{webrtc?: boolean, tunnelled?: boolean}} [opts]
 * @returns {string}
 */
export function launchTransportPref(
    pref,
    { webrtc = browserHasWebrtc(), tunnelled = pageCameThroughTunnel() } = {},
) {
    const p = normalizePref(pref);
    if (p === 'webrtc' && !webrtc) return 'auto';
    if (p === 'wss' && tunnelled) return 'auto';
    return p;
}
