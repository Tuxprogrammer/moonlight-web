/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * AudioPipeline's lifecycle against a fast relaunch: a teardown (close()) can
 * land while init() or resume() is still awaiting the browser. What must hold:
 * neither call throws or touches a context that close() already released, a
 * closed pipeline reports false and stays closed, and the normal paths still
 * resume and initialise.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { AudioPipeline } from '../js/audio/AudioPipeline.js';

/** A promise with its resolve/reject exposed, to hold the browser mid-await. */
function deferred() {
    let resolve, reject;
    const promise = new Promise((res, rej) => {
        resolve = res;
        reject = rej;
    });
    return { promise, resolve, reject };
}

const contexts = [];

class FakeAudioContext {
    constructor(opts = {}) {
        this.sampleRate = opts.sampleRate || 48000;
        this.state = FakeAudioContext.initialState;
        this.baseLatency = 0.01;
        this.destination = {};
        this.onstatechange = null;
        this.resumeGate = null; // a deferred: resume() waits on it when set
        this.audioWorklet = {
            addModule: vi.fn(async () => {
                const gate = FakeAudioContext.addModuleGate;
                if (gate) await gate.promise;
            }),
        };
        this.resume = vi.fn(async () => {
            if (this.resumeGate) await this.resumeGate.promise;
            if (this.state !== 'closed') this.state = 'running';
        });
        this.close = vi.fn(() => {
            this.state = 'closed';
            return Promise.resolve();
        });
        contexts.push(this);
    }
    createGain() {
        return { gain: { value: 1 }, connect: vi.fn(), disconnect: vi.fn() };
    }
}
FakeAudioContext.initialState = 'suspended';
FakeAudioContext.addModuleGate = null; // a deferred: addModule() waits on it when set

class FakeWorkletNode {
    constructor() {
        this.port = { onmessage: null, postMessage: vi.fn() };
        this.connect = vi.fn();
        this.disconnect = vi.fn();
    }
}

class FakeAudioDecoder {
    constructor(init) {
        this.init = init;
        this.state = 'unconfigured';
    }
    configure() {
        this.state = 'configured';
    }
    close() {
        this.state = 'closed';
    }
}

beforeEach(() => {
    contexts.length = 0;
    FakeAudioContext.initialState = 'suspended';
    FakeAudioContext.addModuleGate = null;
    vi.stubGlobal('AudioContext', FakeAudioContext);
    vi.stubGlobal('AudioWorkletNode', FakeWorkletNode);
    vi.stubGlobal('AudioDecoder', FakeAudioDecoder);
    // No decode worker in these tests: main-thread WebCodecs decode.
    vi.stubGlobal('Worker', undefined);
    vi.spyOn(console, 'log').mockImplementation(() => {});
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    vi.spyOn(console, 'error').mockImplementation(() => {});
});

afterEach(() => {
    vi.unstubAllGlobals();
    vi.restoreAllMocks();
});

describe('AudioPipeline.init', () => {
    it('initialises a suspended context with the WebCodecs decoder', async () => {
        const audio = new AudioPipeline();
        expect(await audio.init()).toBe(true);
        expect(audio.ready).toBe(true);
        expect(audio.context).toBe(contexts[0]);
        expect(audio.decoder.state).toBe('configured');
        expect(console.error).not.toHaveBeenCalled();
        audio.close();
    });

    it('returns false without creating a context once closed', async () => {
        const audio = new AudioPipeline();
        audio.close();
        expect(await audio.init()).toBe(false);
        expect(contexts).toHaveLength(0);
    });

    it('aborts quietly when close() lands while the worklet module loads', async () => {
        FakeAudioContext.addModuleGate = deferred();
        const audio = new AudioPipeline();
        const pending = audio.init();
        audio.close();
        FakeAudioContext.addModuleGate.resolve();
        expect(await pending).toBe(false);
        expect(audio.ready).toBe(false);
        expect(audio.context).toBe(null);
        expect(console.error).not.toHaveBeenCalled();
    });

    it('aborts quietly when close() lands while the decoder is set up', async () => {
        const audio = new AudioPipeline();
        const real = audio._setupDecoder.bind(audio);
        const gate = deferred();
        audio._setupDecoder = async () => {
            const ok = await real();
            await gate.promise;
            return ok;
        };
        const pending = audio.init();
        // Let init() reach the decoder await, then tear down underneath it.
        await vi.waitFor(() => expect(audio.decoder).not.toBe(null));
        audio.close();
        gate.resolve();
        expect(await pending).toBe(false);
        expect(audio.ready).toBe(false);
        expect(audio.context).toBe(null);
        expect(audio.decoder).toBe(null);
        // A teardown is not an initialisation failure.
        expect(console.error).not.toHaveBeenCalled();
    });

    it('releases the context when no decoder is available', async () => {
        const audio = new AudioPipeline();
        audio._setupDecoder = async () => false;
        expect(await audio.init()).toBe(false);
        expect(audio.context).toBe(null);
        expect(contexts[0].close).toHaveBeenCalled();
    });
});

describe('AudioPipeline.resume', () => {
    it('resumes a suspended context and reports it running', async () => {
        const audio = new AudioPipeline();
        await audio.init();
        expect(await audio.resume()).toBe(true);
        expect(contexts[0].resume).toHaveBeenCalledTimes(1);
        audio.close();
    });

    it('does not call resume() on a context that already runs', async () => {
        FakeAudioContext.initialState = 'running';
        const audio = new AudioPipeline();
        await audio.init();
        expect(await audio.resume()).toBe(true);
        expect(contexts[0].resume).not.toHaveBeenCalled();
        audio.close();
    });

    it('returns false before init() and after close()', async () => {
        const audio = new AudioPipeline();
        expect(await audio.resume()).toBe(false);
        await audio.init();
        audio.close();
        expect(await audio.resume()).toBe(false);
    });

    it('returns false when the browser refuses to resume', async () => {
        const audio = new AudioPipeline();
        await audio.init();
        contexts[0].resume.mockRejectedValueOnce(new Error('not allowed'));
        expect(await audio.resume()).toBe(false);
        audio.close();
    });

    it('survives close() landing while the browser resumes (fast relaunch)', async () => {
        const audio = new AudioPipeline();
        await audio.init();
        const ctx = contexts[0];
        ctx.resumeGate = deferred();
        const pending = audio.resume();
        // The relaunch tears the pipeline down before resume() settles.
        audio.close();
        expect(audio.context).toBe(null);
        ctx.resumeGate.resolve();
        await expect(pending).resolves.toBe(false);
        expect(audio._closed).toBe(true);
    });

    it('reports false when close() lands while a refused resume settles', async () => {
        const audio = new AudioPipeline();
        await audio.init();
        const ctx = contexts[0];
        ctx.resumeGate = deferred();
        const pending = audio.resume();
        audio.close();
        ctx.resumeGate.reject(new Error('closed'));
        await expect(pending).resolves.toBe(false);
    });
});

describe('AudioPipeline gesture resume', () => {
    it('resumes a suspended context on the first user gesture', async () => {
        const audio = new AudioPipeline();
        await audio.init();
        window.dispatchEvent(new Event('pointerdown'));
        await vi.waitFor(() => expect(contexts[0].state).toBe('running'));
        audio.close();
    });

    it('a gesture after close() touches no context', async () => {
        const audio = new AudioPipeline();
        await audio.init();
        const ctx = contexts[0];
        audio.close();
        window.dispatchEvent(new Event('keydown'));
        await Promise.resolve();
        expect(ctx.resume).not.toHaveBeenCalled();
    });

    it('survives close() landing while a gesture resumes the context', async () => {
        const audio = new AudioPipeline();
        await audio.init();
        const ctx = contexts[0];
        ctx.resumeGate = deferred();
        window.dispatchEvent(new Event('click'));
        expect(ctx.resume).toHaveBeenCalledTimes(1);
        audio.close();
        ctx.resumeGate.resolve();
        await ctx.resumeGate.promise;
        await Promise.resolve();
        expect(audio.context).toBe(null);
    });
});
