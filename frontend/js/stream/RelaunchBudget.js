/*
 * MoonlightWeb — how many times one launch may start itself again.
 * Copyright (C) 2026 Bruno Martin. GPLv3 — see repository LICENSE.
 */

/**
 * Codec relaunches one launch may make: the codec chain has three hops at
 * most (HEVC HDR → AV1 HDR → HEVC SDR → H.264 SDR), and one more as a safety
 * net, as before.
 */
export const CODEC_RELAUNCH_MAX = 4;

/**
 * Automatic relaunches of every kind one launch may make: codec fallbacks,
 * transport rungs, launch retries and restarts after a worker crash. The
 * longest honest sequence is three codec hops, a full walk of the five-rung
 * chain (four rungs down, the silent retry of the first, one launch retry)
 * and the two crash restarts CrashRelaunch allows: 3 + 6 + 2 = 11. One more
 * than that, and a launch that keeps failing in a way nothing else counts
 * stops instead of relaunching for good — which is what a browser whose codec
 * fallback and transport failures took turns did: each kind kept its own
 * count, a codec relaunch started the chain over, and nothing bounded the two
 * together.
 */
export const AUTO_RELAUNCH_MAX = 12;

/**
 * A stream that showed pictures this long proved the launch: what fails after
 * is a new trouble, with a fresh budget. A loop whose every turn plays a
 * minute is no longer a hot loop.
 */
export const STABLE_STREAM_MS = 60000;

/** @typedef {'codec'|'transport'|'launch'|'crash'} RelaunchKind */
/** @typedef {{codec: number, total: number}} RelaunchBudget */

/** @returns {RelaunchBudget} the budget of a launch the user just started */
export function freshRelaunchBudget() {
    return { codec: 0, total: 0 };
}

/**
 * Whether one more relaunch of @p kind fits in @p budget.
 * @param {RelaunchBudget|null|undefined} budget
 * @param {RelaunchKind} kind
 * @returns {boolean}
 */
export function mayRelaunch(budget, kind) {
    const b = budget || freshRelaunchBudget();
    if (b.total >= AUTO_RELAUNCH_MAX) return false;
    return kind !== 'codec' || b.codec < CODEC_RELAUNCH_MAX;
}

/**
 * @p budget with one relaunch of @p kind spent.
 * @param {RelaunchBudget|null|undefined} budget
 * @param {RelaunchKind} kind
 * @returns {RelaunchBudget}
 */
export function chargeRelaunch(budget, kind) {
    const b = budget || freshRelaunchBudget();
    return { codec: b.codec + (kind === 'codec' ? 1 : 0), total: b.total + 1 };
}

/**
 * The budget to charge, given the view the relaunch replaces: a fresh one when
 * that view showed its first picture STABLE_STREAM_MS ago or more.
 * @param {RelaunchBudget|null|undefined} budget
 * @param {number|undefined} firstFrameAt when the view showed its first picture (ms, performance clock), 0 or absent when it never did
 * @param {number} now
 * @returns {RelaunchBudget}
 */
export function settleRelaunchBudget(budget, firstFrameAt, now) {
    if (
        typeof firstFrameAt === 'number' &&
        firstFrameAt > 0 &&
        now - firstFrameAt >= STABLE_STREAM_MS
    )
        return freshRelaunchBudget();
    return budget || freshRelaunchBudget();
}
