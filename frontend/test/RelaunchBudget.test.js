/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, expect, it } from 'vitest';
import {
    AUTO_RELAUNCH_MAX,
    CODEC_RELAUNCH_MAX,
    STABLE_STREAM_MS,
    chargeRelaunch,
    freshRelaunchBudget,
    mayRelaunch,
    settleRelaunchBudget,
} from '../js/stream/RelaunchBudget.js';

/**
 * One launch may start itself again only so many times, whatever mix of
 * failures asks it to: codec fallbacks on their own count, and every kind of
 * automatic relaunch together.
 */

const spend = (kinds, b = freshRelaunchBudget()) => kinds.reduce((x, k) => chargeRelaunch(x, k), b);

describe('RelaunchBudget', () => {
    it('starts empty', () => {
        expect(freshRelaunchBudget()).toEqual({ codec: 0, total: 0 });
        expect(freshRelaunchBudget()).not.toBe(freshRelaunchBudget());
    });

    it('counts a codec relaunch in both counts, any other in the total only', () => {
        expect(spend(['codec'])).toEqual({ codec: 1, total: 1 });
        for (const k of ['transport', 'launch', 'crash'])
            expect(spend([k]), k).toEqual({ codec: 0, total: 1 });
    });

    it('never changes the budget it is given', () => {
        const b = freshRelaunchBudget();
        chargeRelaunch(b, 'codec');
        expect(b).toEqual({ codec: 0, total: 0 });
    });

    it('allows CODEC_RELAUNCH_MAX codec relaunches, then none', () => {
        let b = freshRelaunchBudget();
        for (let i = 0; i < CODEC_RELAUNCH_MAX; i++) {
            expect(mayRelaunch(b, 'codec')).toBe(true);
            b = chargeRelaunch(b, 'codec');
        }
        expect(mayRelaunch(b, 'codec')).toBe(false);
        // Other kinds still fit: the codec cap is its own.
        expect(mayRelaunch(b, 'transport')).toBe(true);
    });

    it('allows AUTO_RELAUNCH_MAX relaunches of any mix, then none of any kind', () => {
        const kinds = ['transport', 'codec', 'launch', 'crash'];
        let b = freshRelaunchBudget();
        for (let i = 0; i < AUTO_RELAUNCH_MAX; i++) {
            const k =
                kinds[i % kinds.length] === 'codec' && b.codec >= CODEC_RELAUNCH_MAX
                    ? 'transport'
                    : kinds[i % kinds.length];
            expect(mayRelaunch(b, k), `#${i} ${k}`).toBe(true);
            b = chargeRelaunch(b, k);
        }
        for (const k of [...kinds]) expect(mayRelaunch(b, k), k).toBe(false);
    });

    it('lets every honest sequence through: 3 codec hops, a full chain walk, 2 crash restarts', () => {
        const honest = [
            ...Array(3).fill('codec'),
            'transport', // silent retry of the first rung
            ...Array(4).fill('transport'), // down the five rungs
            'launch', // one launch retry
            'launch', // a walk-on after a launch failure
            'crash',
            'crash',
        ];
        let b = freshRelaunchBudget();
        for (const k of honest) {
            expect(mayRelaunch(b, k), k).toBe(true);
            b = chargeRelaunch(b, k);
        }
        expect(b.total).toBeLessThanOrEqual(AUTO_RELAUNCH_MAX);
    });

    it('reads a missing budget as a fresh one', () => {
        expect(mayRelaunch(null, 'codec')).toBe(true);
        expect(mayRelaunch(undefined, 'transport')).toBe(true);
        expect(chargeRelaunch(null, 'crash')).toEqual({ codec: 0, total: 1 });
    });
});

describe('settleRelaunchBudget', () => {
    const spent = { codec: 4, total: 12 };

    it('starts over once the view it replaces showed pictures for a minute', () => {
        expect(settleRelaunchBudget(spent, 1000, 1000 + STABLE_STREAM_MS)).toEqual({
            codec: 0,
            total: 0,
        });
    });

    it('keeps counting for a view that never showed one, or not for long', () => {
        expect(settleRelaunchBudget(spent, undefined, 1e9)).toBe(spent);
        expect(settleRelaunchBudget(spent, 0, 1e9)).toBe(spent);
        expect(settleRelaunchBudget(spent, 1000, 1000 + STABLE_STREAM_MS - 1)).toBe(spent);
        expect(settleRelaunchBudget(spent, NaN, 1e9)).toBe(spent);
        expect(settleRelaunchBudget(spent, '5', 1e9)).toBe(spent);
    });

    it('gives a fresh budget for a missing one', () => {
        expect(settleRelaunchBudget(null, undefined, 0)).toEqual({ codec: 0, total: 0 });
    });

    it('sizes its constants as documented', () => {
        expect(CODEC_RELAUNCH_MAX).toBe(4);
        expect(AUTO_RELAUNCH_MAX).toBe(12);
        expect(STABLE_STREAM_MS).toBe(60000);
    });
});
