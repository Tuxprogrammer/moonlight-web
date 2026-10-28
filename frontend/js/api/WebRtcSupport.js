/*
 * MoonlightWeb — can this browser carry a WebRTC rung at all?
 * Copyright (C) 2026 Bruno Martin. GPLv3 — see repository LICENSE.
 */

/**
 * The reason a transport gives when the browser has no WebRTC: Firefox with
 * media.peerconnection.enabled=false has no RTCPeerConnection at all, and some
 * locked-down browsers keep the name but refuse to construct one. Every
 * webrtc-* rung then fails the same way, so the chain skips them all.
 */
export const WEBRTC_UNAVAILABLE = 'webrtc_unavailable';

/**
 * What the backend answers (HTTP 409) when the chain is empty before any
 * attempt: nothing it may offer is something this browser can carry.
 */
export const NO_USABLE_TRANSPORT = 'no_usable_transport';

/** The reasons the backend gives with NO_USABLE_TRANSPORT, each with its message. */
const NO_USABLE_REASONS = ['webrtc_unavailable', 'policy', 'tunnel', 'udp_blocked'];

/**
 * Whether this constructor builds a peer connection. Not only its type: a
 * browser that keeps the name but forbids WebRTC throws from the constructor.
 * @param {unknown} PC the RTCPeerConnection to try
 * @returns {boolean}
 */
export function probeWebrtc(PC) {
    if (typeof PC !== 'function') return false;
    try {
        const pc = new /** @type {typeof RTCPeerConnection} */ (PC)();
        try {
            pc.close();
        } catch (_e) {
            /* built is what was asked */
        }
        return true;
    } catch (_e) {
        return false;
    }
}

/** @type {boolean|null} this page's verdict, once asked */
let verdict = null;

/**
 * Whether this browser can carry a WebRTC rung, asked once per page: the
 * answer does not change while it is open.
 * @returns {boolean}
 */
export function webrtcAvailable() {
    if (verdict === null) verdict = probeWebrtc(globalThis.RTCPeerConnection);
    return verdict;
}

/** Forget the verdict (tests, and a page that changed its globals). */
export function resetWebrtcProbe() {
    verdict = null;
}

/**
 * What this browser tells /start and a guest's join about itself. A backend
 * that knows the field drops every webrtc-* rung on `webrtc: false`; an older
 * one ignores it, and the transports then fail fast with WEBRTC_UNAVAILABLE.
 * @returns {{webrtc: boolean}}
 */
export function clientCapabilities() {
    return { webrtc: webrtcAvailable() };
}

/** @param {unknown} mode a transport chain entry @returns {boolean} */
export function isWebrtcRung(mode) {
    return typeof mode === 'string' && mode.startsWith('webrtc-');
}

/**
 * The first rung at or after @p from this browser can try: no webrtc-* rung
 * without WebRTC, and only a native-media one after the decoder failed.
 * @param {string[]} chain the backend's transport chain
 * @param {number} from
 * @param {{webrtc?: boolean, mediaOnly?: boolean}} [opts]
 * @returns {number} an index, chain.length when none is left
 */
export function nextUsableRung(chain, from, { webrtc = true, mediaOnly = false } = {}) {
    let i = Math.max(0, from);
    while (i < chain.length) {
        const mode = chain[i];
        const skip =
            (!webrtc && isWebrtcRung(mode)) ||
            (mediaOnly && !(typeof mode === 'string' && mode.startsWith('webrtc-media')));
        if (!skip) break;
        i++;
    }
    return i;
}

/**
 * The backend's reason when @p err is its 409 "no usable transport", else null.
 * @param {any} err a BackendClient error
 * @returns {string|null}
 */
export function noUsableTransportReason(err) {
    const body = err && err.responseBody;
    if (!err || err.statusCode !== 409 || !body || body.error !== NO_USABLE_TRANSPORT) return null;
    return typeof body.reason === 'string' ? body.reason : '';
}

/**
 * The locale key that explains a "no usable transport" reason to the user.
 * @param {string|null|undefined} reason
 * @returns {string}
 */
export function noUsableTransportKey(reason) {
    return NO_USABLE_REASONS.includes(/** @type {string} */ (reason))
        ? 'transport.noUsable.' + reason
        : 'transport.noUsable.generic';
}

/**
 * How long a rung may take to open after /start answered before the view
 * gives it up (StreamView's connect watchdog). Each sits above the longest
 * deadline the transport keeps on its own, so it only ever catches a hang:
 *  - WebRTC: the host's ICE deadline, clamped at 30 s (WebRtcDataChannel's
 *    ice-config), plus the 10 s it waits for the DataChannels once ICE is up
 *    (WS_GRACE_PERIOD_MS), plus 5 s of margin.
 *  - wss: one WebSocket upgrade, retried once a second later when it closes
 *    before opening (the relaunch race), well inside the 15 s the view also
 *    gives a connected stream to show its first frame.
 */
export const CONNECT_WATCHDOG_MS = Object.freeze({ webrtc: 45000, wss: 15000 });

/** @param {string} transport the rung a view runs @returns {number} */
export function connectWatchdogMs(transport) {
    return transport === 'wss' ? CONNECT_WATCHDOG_MS.wss : CONNECT_WATCHDOG_MS.webrtc;
}
