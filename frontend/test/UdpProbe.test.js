import { afterEach, describe, expect, it, vi } from 'vitest';
import { probeUdp } from '../js/api/UdpProbe.js';
import { resetWebrtcProbe } from '../js/api/WebRtcSupport.js';

/** A fake peer connection that emits the given candidates after the offer. */
function fakePc(candidates, { delayMs = 10, endGathering = false } = {}) {
    return class {
        constructor() {
            this.closed = false;
            this.onicecandidate = null;
        }
        createDataChannel() {}
        async createOffer() {
            return { type: 'offer', sdp: '' };
        }
        async setLocalDescription() {
            let t = 0;
            for (const c of candidates) {
                t += delayMs;
                setTimeout(() => this.onicecandidate?.({ candidate: c }), t);
            }
            if (endGathering)
                setTimeout(() => this.onicecandidate?.({ candidate: null }), t + delayMs);
        }
        close() {
            this.closed = true;
        }
    };
}

const host = {
    type: 'host',
    protocol: 'udp',
    candidate: 'candidate:1 1 udp 1 10.0.0.2 5000 typ host',
};
const srflx = {
    type: 'srflx',
    protocol: 'udp',
    candidate: 'candidate:2 1 udp 1 82.1.2.3 5000 typ srflx raddr 10.0.0.2 rport 5000',
};

describe('probeUdp', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.useRealTimers();
        resetWebrtcProbe();
    });

    it('answers true on the first server-reflexive UDP candidate', async () => {
        vi.stubGlobal('RTCPeerConnection', fakePc([host, srflx]));
        await expect(probeUdp(1500)).resolves.toBe(true);
    });

    it('answers false when only host candidates come and the window closes', async () => {
        vi.useFakeTimers();
        vi.stubGlobal('RTCPeerConnection', fakePc([host]));
        const verdict = probeUdp(1500);
        await vi.advanceTimersByTimeAsync(1600);
        await expect(verdict).resolves.toBe(false);
    });

    it('answers false as soon as gathering ends without an answer', async () => {
        vi.stubGlobal('RTCPeerConnection', fakePc([host], { endGathering: true }));
        await expect(probeUdp(60000)).resolves.toBe(false);
    });

    it('reads the type from the SDP line when the fields are missing', async () => {
        vi.stubGlobal('RTCPeerConnection', fakePc([{ candidate: srflx.candidate }]));
        await expect(probeUdp(1500)).resolves.toBe(true);
    });

    it('answers null where the browser has no WebRTC', async () => {
        vi.stubGlobal('RTCPeerConnection', undefined);
        await expect(probeUdp(1500)).resolves.toBe(null);
    });

    // The one capability probe (WebRtcSupport.webrtcAvailable) decides: a
    // browser that keeps the name but refuses to build one cannot ask either.
    it('answers null where the constructor refuses', async () => {
        vi.stubGlobal(
            'RTCPeerConnection',
            class {
                constructor() {
                    throw new DOMException('blocked', 'NotAllowedError');
                }
            },
        );
        await expect(probeUdp(1500)).resolves.toBe(null);
    });

    it('answers null when making the offer throws', async () => {
        const Pc = fakePc([]);
        Pc.prototype.createDataChannel = () => {
            throw new Error('no channels here');
        };
        vi.stubGlobal('RTCPeerConnection', Pc);
        await expect(probeUdp(1500)).resolves.toBe(null);
    });
});
