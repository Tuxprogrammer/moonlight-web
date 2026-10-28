/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
    ChordGesture,
    ClipboardPanel,
    MAX_SET_CHARS,
    MAX_TYPE_CHARS,
    SETTINGS_KEY,
    TYPE_INTERVAL_MS,
    TextTyper,
    capCodePoints,
    capText,
    readClipboardPrefs,
    typingSteps,
} from '../js/ui/ClipboardPanel.js';

/**
 * The stream's clipboard panel (Guacamole-style).
 *
 * It opens on Ctrl+Alt+Shift pressed and released alone, never on one of the
 * control combos; while open the keyboard is its own. With a host that shares
 * its clipboard, Send sets it (`clipboardset`); with any other host Send types
 * the text through `textinput`, paced, capped and cancellable. Guests never see
 * the host clipboard, and type only when their invitation lets them.
 *
 * No dictionary is loaded, so `t()` returns the key: the keys are what these
 * tests pin, and `npm run i18n` proves each has a translation.
 */

/** A keyboard event as the gesture and the panel read it. */
function key(code, mods = {}, extra = {}) {
    return {
        code,
        key: extra.key ?? code,
        repeat: false,
        ctrlKey: false,
        altKey: false,
        shiftKey: false,
        metaKey: false,
        ...mods,
        ...extra,
    };
}

/** Press then release the three modifiers, in that order, reporting flags as a browser does. */
function bareChord(g, order = ['ControlLeft', 'AltLeft', 'ShiftLeft']) {
    const flag = { ControlLeft: 'ctrlKey', AltLeft: 'altKey', ShiftLeft: 'shiftKey' };
    const held = {};
    for (const c of order) {
        held[flag[c]] = true;
        g.keyDown(key(c, { ...held }));
    }
    let fired = false;
    for (const c of order) {
        held[flag[c]] = false;
        fired = g.keyUp(key(c, { ...held })) || fired;
    }
    return fired;
}

const ALL = { ctrlKey: true, altKey: true, shiftKey: true };

function memoryStorage(settings) {
    const data = new Map();
    if (settings !== undefined) data.set(SETTINGS_KEY, JSON.stringify(settings));
    return {
        getItem: (k) => (data.has(k) ? data.get(k) : null),
        setItem: (k, v) => data.set(k, String(v)),
    };
}

describe('readClipboardPrefs', () => {
    it('is off for both settings when nothing is stored', () => {
        expect(readClipboardPrefs(memoryStorage())).toEqual({
            syncOnFocus: false,
            ctrlVTypes: false,
        });
        expect(readClipboardPrefs(null)).toEqual({ syncOnFocus: false, ctrlVTypes: false });
    });

    it('reads each setting only when stored as true', () => {
        expect(
            readClipboardPrefs(
                memoryStorage({ clipboard_sync_on_focus: true, clipboard_ctrlv_types: false }),
            ),
        ).toEqual({ syncOnFocus: true, ctrlVTypes: false });
        expect(
            readClipboardPrefs(
                memoryStorage({ clipboard_sync_on_focus: 'yes', clipboard_ctrlv_types: true }),
            ),
        ).toEqual({ syncOnFocus: false, ctrlVTypes: true });
    });

    it('falls back to off on broken or unreadable storage', () => {
        const broken = { getItem: () => '{not json' };
        expect(readClipboardPrefs(broken).syncOnFocus).toBe(false);
        const throwing = {
            getItem: () => {
                throw new Error('SecurityError');
            },
        };
        expect(readClipboardPrefs(throwing).ctrlVTypes).toBe(false);
    });

    it('reads localStorage when no storage is given', () => {
        localStorage.setItem(SETTINGS_KEY, JSON.stringify({ clipboard_ctrlv_types: true }));
        expect(readClipboardPrefs().ctrlVTypes).toBe(true);
        localStorage.removeItem(SETTINGS_KEY);
        expect(readClipboardPrefs().ctrlVTypes).toBe(false);
    });
});

describe('capText', () => {
    it('keeps a text under the cap whole', () => {
        expect(capText('hello', 10)).toBe('hello');
        expect(capText('hello', 5)).toBe('hello');
    });

    it('cuts at the cap, never inside a surrogate pair', () => {
        expect(capText('abcdef', 3)).toBe('abc');
        const s = 'a\u{1F600}b'; // the emoji is two units
        expect(capText(s, 2)).toBe('a');
        expect(capText(s, 3)).toBe('a\u{1F600}');
        expect(capText(s, 0)).toBe('');
        expect(capText(s, -4)).toBe('');
    });
});

describe('typingSteps', () => {
    it('sends one code point per message: Sunshine on Linux mangles more', () => {
        const steps = typingSteps('a #$%&*()[');
        expect(steps.map((x) => x.text)).toEqual([
            'a',
            ' ',
            '#',
            '$',
            '%',
            '&',
            '*',
            '(',
            ')',
            '[',
        ]);
        expect(steps.every((x) => x.chars === 1)).toBe(true);
    });

    it('keeps accented and CJK characters whole', () => {
        expect(typingSteps('é ü ß 日本').map((x) => x.text)).toEqual([
            'é',
            ' ',
            'ü',
            ' ',
            'ß',
            ' ',
            '日',
            '本',
        ]);
    });

    it('never splits a surrogate pair: an emoji is one message', () => {
        const steps = typingSteps('a\u{1F600}b\u{1F44D}');
        expect(steps.map((x) => x.text)).toEqual(['a', '\u{1F600}', 'b', '\u{1F44D}']);
        expect(steps[1].text).toHaveLength(2);
        for (const x of steps) {
            const last = x.text.charCodeAt(x.text.length - 1);
            expect(last >= 0xd800 && last <= 0xdbff).toBe(false);
        }
    });

    it('types each line break as Enter, whatever its spelling', () => {
        const steps = typingSteps('a\r\nb\rc\nd');
        expect(steps).toEqual([
            { text: 'a', chars: 1 },
            { enter: true, chars: 1 },
            { text: 'b', chars: 1 },
            { enter: true, chars: 1 },
            { text: 'c', chars: 1 },
            { enter: true, chars: 1 },
            { text: 'd', chars: 1 },
        ]);
        expect(typingSteps('\n\n')).toEqual([
            { enter: true, chars: 1 },
            { enter: true, chars: 1 },
        ]);
    });

    it('has nothing to type for an empty text', () => {
        expect(typingSteps('')).toEqual([]);
    });

    it('the E2E test string: one step per character, 66 of them', () => {
        const test =
            'Hello, World! 123 #$%&*()[]{};:\'"<>/?\\|`~_+=-\nLine2: é ü ß 日本 \u{1F600} end';
        const steps = typingSteps(test);
        expect(steps).toHaveLength(Array.from(test).length);
        expect(steps.filter((x) => x.enter)).toHaveLength(1);
        expect(steps.map((x) => (x.enter ? '\n' : x.text)).join('')).toBe(test);
    });
});

describe('capCodePoints', () => {
    it('counts an emoji as one character', () => {
        expect(capCodePoints('ab\u{1F600}cd', 3)).toBe('ab\u{1F600}');
        expect(capCodePoints('ab\u{1F600}cd', 2)).toBe('ab');
        expect(capCodePoints('abc', 5)).toBe('abc');
        expect(capCodePoints('abc', 0)).toBe('');
        expect(capCodePoints('abc', -1)).toBe('');
    });
});

describe('ChordGesture — the bare Ctrl+Alt+Shift', () => {
    it('fires when the three go down together and all come back up', () => {
        const g = new ChordGesture(false);
        expect(bareChord(g)).toBe(true);
    });

    it('fires whatever the order, and with the right-hand keys', () => {
        expect(bareChord(new ChordGesture(false), ['ShiftLeft', 'ControlLeft', 'AltLeft'])).toBe(
            true,
        );
        const g = new ChordGesture(false);
        g.keyDown(key('ControlRight', { ctrlKey: true }));
        g.keyDown(key('AltRight', { ctrlKey: true, altKey: true }));
        g.keyDown(key('ShiftRight', ALL));
        expect(g.keyUp(key('AltRight', { ctrlKey: true, shiftKey: true }))).toBe(false);
        expect(g.keyUp(key('ShiftRight', { ctrlKey: true }))).toBe(false);
        expect(g.keyUp(key('ControlRight'))).toBe(true);
    });

    it('waits for the last of the three to come up', () => {
        const g = new ChordGesture(false);
        g.keyDown(key('ControlLeft', { ctrlKey: true }));
        g.keyDown(key('AltLeft', { ctrlKey: true, altKey: true }));
        g.keyDown(key('ShiftLeft', ALL));
        expect(g.keyUp(key('ShiftLeft', { ctrlKey: true, altKey: true }))).toBe(false);
        expect(g.keyUp(key('ControlLeft', { altKey: true }))).toBe(false);
        expect(g.keyUp(key('AltLeft'))).toBe(true);
    });

    it('never fires for Ctrl+Alt+Shift+Q, nor for any other combo', () => {
        for (const other of ['KeyQ', 'KeyC', 'KeyX', 'ArrowLeft', 'MetaLeft']) {
            const g = new ChordGesture(false);
            g.keyDown(key('ControlLeft', { ctrlKey: true }));
            g.keyDown(key('AltLeft', { ctrlKey: true, altKey: true }));
            g.keyDown(key('ShiftLeft', ALL));
            g.keyDown(key(other, ALL));
            expect(g.keyUp(key(other, ALL))).toBe(false);
            expect(g.keyUp(key('ShiftLeft', { ctrlKey: true, altKey: true }))).toBe(false);
            expect(g.keyUp(key('AltLeft', { ctrlKey: true }))).toBe(false);
            expect(g.keyUp(key('ControlLeft'))).toBe(false);
        }
    });

    it('never fires when the other key came before the third modifier', () => {
        const g = new ChordGesture(false);
        g.keyDown(key('ControlLeft', { ctrlKey: true }));
        g.keyDown(key('KeyA', { ctrlKey: true }));
        g.keyUp(key('KeyA', { ctrlKey: true }));
        g.keyDown(key('AltLeft', { ctrlKey: true, altKey: true }));
        g.keyDown(key('ShiftLeft', ALL));
        g.keyUp(key('ShiftLeft', { ctrlKey: true, altKey: true }));
        g.keyUp(key('AltLeft', { ctrlKey: true }));
        expect(g.keyUp(key('ControlLeft'))).toBe(false);
        // …and the next bare chord works again.
        expect(bareChord(g)).toBe(true);
    });

    it('never fires for two of the three', () => {
        const g = new ChordGesture(false);
        g.keyDown(key('ControlLeft', { ctrlKey: true }));
        g.keyDown(key('AltLeft', { ctrlKey: true, altKey: true }));
        g.keyUp(key('AltLeft', { ctrlKey: true }));
        expect(g.keyUp(key('ControlLeft'))).toBe(false);
    });

    it('is not spoiled by typing done before the chord', () => {
        const g = new ChordGesture(false);
        g.keyDown(key('KeyH'));
        g.keyUp(key('KeyH'));
        expect(bareChord(g)).toBe(true);
    });

    it('ignores auto-repeat of a held modifier', () => {
        const g = new ChordGesture(false);
        g.keyDown(key('ControlLeft', { ctrlKey: true }));
        g.keyDown(key('ControlLeft', { ctrlKey: true }, { repeat: true }));
        g.keyDown(key('AltLeft', { ctrlKey: true, altKey: true }));
        g.keyDown(key('ShiftLeft', ALL));
        g.keyDown(key('ShiftLeft', ALL, { repeat: true }));
        g.keyUp(key('ShiftLeft', { ctrlKey: true, altKey: true }));
        g.keyUp(key('AltLeft', { ctrlKey: true }));
        expect(g.keyUp(key('ControlLeft'))).toBe(true);
    });

    it('a release that never came (eaten by the OS) does not leave it armed', () => {
        const g = new ChordGesture(false);
        g.keyDown(key('ControlLeft', { ctrlKey: true }));
        g.keyDown(key('AltLeft', { ctrlKey: true, altKey: true }));
        g.keyDown(key('ShiftLeft', ALL));
        // Alt+Tab: no key-ups arrive. The next event's flags say all are up.
        g.keyDown(key('KeyW'));
        g.keyUp(key('KeyW'));
        g.keyDown(key('ControlLeft', { ctrlKey: true }));
        expect(g.keyUp(key('ControlLeft'))).toBe(false);
    });

    it('releases with no presses seen do not fire', () => {
        const g = new ChordGesture(false);
        expect(g.keyUp(key('KeyC', ALL))).toBe(false);
        expect(g.keyUp(key('ShiftLeft', { ctrlKey: true, altKey: true }))).toBe(false);
        expect(g.keyUp(key('AltLeft', { ctrlKey: true }))).toBe(false);
        expect(g.keyUp(key('ControlLeft'))).toBe(false);
    });

    it('reset() forgets a chord in progress', () => {
        const g = new ChordGesture(false);
        g.keyDown(key('ControlLeft', { ctrlKey: true }));
        g.keyDown(key('AltLeft', { ctrlKey: true, altKey: true }));
        g.keyDown(key('ShiftLeft', ALL));
        g.reset();
        g.keyUp(key('ShiftLeft', { ctrlKey: true, altKey: true }));
        g.keyUp(key('AltLeft', { ctrlKey: true }));
        expect(g.keyUp(key('ControlLeft'))).toBe(false);
    });

    it('on a Mac it is Cmd+Option+Ctrl, and Shift is not one of them', () => {
        const mac = new ChordGesture(true);
        mac.keyDown(key('MetaLeft', { metaKey: true }));
        mac.keyDown(key('AltLeft', { metaKey: true, altKey: true }));
        mac.keyDown(key('ControlLeft', { metaKey: true, altKey: true, ctrlKey: true }));
        mac.keyUp(key('ControlLeft', { metaKey: true, altKey: true }));
        mac.keyUp(key('AltLeft', { metaKey: true }));
        expect(mac.keyUp(key('MetaLeft'))).toBe(true);

        expect(bareChord(new ChordGesture(true))).toBe(false);
    });
});

describe('TextTyper', () => {
    beforeEach(() => vi.useFakeTimers());
    afterEach(() => vi.useRealTimers());

    it('paces one character every TYPE_INTERVAL_MS, the first at once', () => {
        expect(TYPE_INTERVAL_MS).toBe(30);
        const sent = [];
        const progress = [];
        const onDone = vi.fn();
        const typer = new TextTyper({
            send: (m) => sent.push(m),
            steps: typingSteps('ab\nc'),
            onProgress: (d, total) => progress.push([d, total]),
            onDone,
        });
        typer.start();
        expect(typer.active).toBe(true);
        expect(sent).toEqual([{ type: 'textinput', text: 'a' }]);
        // Nothing early: one tick short of the interval sends nothing more.
        vi.advanceTimersByTime(TYPE_INTERVAL_MS - 1);
        expect(sent).toHaveLength(1);
        vi.advanceTimersByTime(1);
        expect(sent[1]).toEqual({ type: 'textinput', text: 'b' });
        vi.advanceTimersByTime(TYPE_INTERVAL_MS);
        // The line break: Enter pressed and let go, no modifier.
        expect(sent[2]).toMatchObject({ type: 'keydown', keyCode: 0x0d, code: 'Enter' });
        expect(sent[3]).toMatchObject({ type: 'keyup', keyCode: 0x0d, code: 'Enter' });
        expect(sent[2].ctrlKey || sent[2].shiftKey || sent[2].altKey || sent[2].metaKey).toBe(
            false,
        );
        expect(onDone).not.toHaveBeenCalled();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS);
        expect(sent[4]).toEqual({ type: 'textinput', text: 'c' });
        expect(onDone).toHaveBeenCalledWith(false);
        expect(typer.active).toBe(false);
        expect(progress).toEqual([
            [1, 4],
            [2, 4],
            [3, 4],
            [4, 4],
        ]);
        expect(typer.total).toBe(4);
    });

    it('takes N - 1 intervals for N characters', () => {
        const sent = [];
        const onDone = vi.fn();
        const typer = new TextTyper({
            send: (m) => sent.push(m),
            steps: typingSteps('x'.repeat(100)),
            onDone,
        });
        typer.start();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * 98);
        expect(sent).toHaveLength(99);
        expect(onDone).not.toHaveBeenCalled();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS);
        expect(sent).toHaveLength(100);
        expect(onDone).toHaveBeenCalledWith(false);
    });

    it('cancelled mid-way, stops on that character exactly and says so once', () => {
        const sent = [];
        const progress = [];
        const onDone = vi.fn();
        const typer = new TextTyper({
            send: (m) => sent.push(m),
            steps: typingSteps('x'.repeat(200)),
            onProgress: (d) => progress.push(d),
            onDone,
        });
        typer.start();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * 9);
        expect(sent).toHaveLength(10);
        typer.cancel();
        typer.cancel();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * 50);
        expect(sent).toHaveLength(10);
        expect(progress.at(-1)).toBe(10);
        expect(onDone).toHaveBeenCalledTimes(1);
        expect(onDone).toHaveBeenCalledWith(true);
    });

    it('takes injected timers and its own pace', () => {
        const timers = { setTimeout: vi.fn(() => 7), clearTimeout: vi.fn() };
        const typer = new TextTyper({
            send: () => {},
            steps: typingSteps('yy'),
            intervalMs: 5,
            timers,
        });
        typer.start();
        expect(timers.setTimeout).toHaveBeenCalledWith(expect.any(Function), 5);
        typer.cancel();
        expect(timers.clearTimeout).toHaveBeenCalledWith(7);
    });

    it('with no steps, is done at once', () => {
        const onDone = vi.fn();
        const typer = new TextTyper({ send: vi.fn(), steps: [], onDone });
        typer.start();
        expect(onDone).toHaveBeenCalledWith(false);
    });
});

describe('ClipboardPanel', () => {
    let container;
    let sent;
    let state;

    const make = (overrides = {}) =>
        new ClipboardPanel({
            container,
            send: (m) => sent.push(m),
            isMac: false,
            canType: () => state.canType,
            isGuest: () => state.guest,
            onOpen: vi.fn(),
            onClose: vi.fn(),
            storage: memoryStorage(state.settings),
            clipboard: state.clipboard,
            toast: state.toast,
            ...overrides,
        });
    const $ = (sel) => container.querySelector(sel);
    const area = () => /** @type {HTMLTextAreaElement} */ ($('.clipboard-panel textarea'));
    const type = (text) => {
        area().value = text;
        area().dispatchEvent(new Event('input'));
    };
    const keydown = (target, code, mods = {}, extra = {}) => {
        const e = new KeyboardEvent('keydown', {
            code,
            key: extra.key ?? code,
            bubbles: true,
            cancelable: true,
            ...mods,
        });
        target.dispatchEvent(e);
        return e;
    };
    const keyup = (target, code, mods = {}) => {
        const e = new KeyboardEvent('keyup', { code, key: code, bubbles: true, ...mods });
        target.dispatchEvent(e);
        return e;
    };

    beforeEach(() => {
        document.body.innerHTML = '<button id="outside">x</button><div id="stream"></div>';
        container = document.getElementById('stream');
        sent = [];
        state = {
            canType: true,
            guest: false,
            settings: undefined,
            clipboard: {
                writeText: vi.fn(async () => {}),
                readText: vi.fn(async () => 'local text'),
            },
            toast: {
                info: vi.fn(() => ({ dismiss: vi.fn() })),
                warning: vi.fn(() => ({ dismiss: vi.fn() })),
            },
        };
    });

    afterEach(() => vi.useRealTimers());

    describe('opening and closing', () => {
        it('opens as a labelled modal dialog with the keyboard in its text box', () => {
            const p = make();
            expect($('.clipboard-panel')).toBeNull();
            p.open();
            const el = $('.clipboard-panel');
            expect(p.isOpen).toBe(true);
            expect(el.hidden).toBe(false);
            expect(el.getAttribute('role')).toBe('dialog');
            expect(el.getAttribute('aria-modal')).toBe('true');
            const title = document.getElementById(el.getAttribute('aria-labelledby'));
            expect(title.textContent).toBe('clipboard.title');
            expect(document.getElementById(el.getAttribute('aria-describedby'))).not.toBeNull();
            expect($('[data-act="close"]').getAttribute('aria-label')).toBe('clipboard.close');
            expect(area().labels[0].textContent).toBe('clipboard.textLabel');
            expect(document.activeElement).toBe(area());
            expect(p._opts.onOpen).toHaveBeenCalledTimes(1);
        });

        it('closes, says so, and gives the focus back where it was', () => {
            const outside = document.getElementById('outside');
            outside.focus();
            const p = make();
            p.open();
            p.close();
            expect(p.isOpen).toBe(false);
            expect($('.clipboard-panel').hidden).toBe(true);
            expect(document.activeElement).toBe(outside);
            expect(p._opts.onClose).toHaveBeenCalledTimes(1);
            // Twice is once.
            p.close();
            expect(p._opts.onClose).toHaveBeenCalledTimes(1);
        });

        it('toggles, and keeps its text between two openings', () => {
            const p = make();
            p.toggle();
            type('kept');
            p.toggle();
            expect(p.isOpen).toBe(false);
            p.toggle();
            expect(p.isOpen).toBe(true);
            expect(area().value).toBe('kept');
            expect(container.querySelectorAll('.clipboard-panel')).toHaveLength(1);
        });

        it('the × button closes it', () => {
            const p = make();
            p.open();
            $('[data-act="close"]').click();
            expect(p.isOpen).toBe(false);
        });

        it('Escape closes it, and the stream never sees that Escape', () => {
            const p = make();
            p.open();
            const seen = vi.fn();
            document.addEventListener('keydown', seen);
            const e = keydown(area(), 'Escape', {}, { key: 'Escape' });
            document.removeEventListener('keydown', seen);
            expect(p.isOpen).toBe(false);
            expect(e.defaultPrevented).toBe(true);
            expect(seen).not.toHaveBeenCalled();
        });

        it('Ctrl+Alt+Shift+C closes it again', () => {
            const p = make();
            p.open();
            const e = keydown(area(), 'KeyC', ALL, { key: 'C' });
            expect(p.isOpen).toBe(false);
            expect(e.defaultPrevented).toBe(true);
        });

        it('a repeat of C does not close and reopen', () => {
            const p = make();
            p.open();
            keydown(area(), 'KeyC', { ...ALL, repeat: true }, { key: 'C' });
            expect(p.isOpen).toBe(true);
        });

        it('the bare chord closes it, Ctrl+Alt+Shift+Q does not', () => {
            const p = make();
            p.open();
            keydown(area(), 'ControlLeft', { ctrlKey: true });
            keydown(area(), 'AltLeft', { ctrlKey: true, altKey: true });
            keydown(area(), 'ShiftLeft', ALL);
            keydown(area(), 'KeyQ', ALL);
            keyup(area(), 'KeyQ', ALL);
            keyup(area(), 'ShiftLeft', { ctrlKey: true, altKey: true });
            keyup(area(), 'AltLeft', { ctrlKey: true });
            keyup(area(), 'ControlLeft');
            expect(p.isOpen).toBe(true);

            keydown(area(), 'ControlLeft', { ctrlKey: true });
            keydown(area(), 'AltLeft', { ctrlKey: true, altKey: true });
            keydown(area(), 'ShiftLeft', ALL);
            keyup(area(), 'ShiftLeft', { ctrlKey: true, altKey: true });
            keyup(area(), 'AltLeft', { ctrlKey: true });
            keyup(area(), 'ControlLeft');
            expect(p.isOpen).toBe(false);
        });

        it('the releases of the combo that opened it do not close it', () => {
            // Opened by Ctrl+Alt+Shift+C in the stream: the key-ups land here.
            const p = make();
            p.open();
            keyup(area(), 'KeyC', ALL);
            keyup(area(), 'ShiftLeft', { ctrlKey: true, altKey: true });
            keyup(area(), 'AltLeft', { ctrlKey: true });
            keyup(area(), 'ControlLeft');
            expect(p.isOpen).toBe(true);
        });

        it('on a Mac, Cmd+Option+Ctrl+C is the combo', () => {
            const p = make({ isMac: true });
            p.open();
            keydown(area(), 'KeyC', ALL, { key: 'C' });
            expect(p.isOpen).toBe(true);
            keydown(area(), 'KeyC', { metaKey: true, altKey: true, ctrlKey: true }, { key: 'ç' });
            expect(p.isOpen).toBe(false);
            expect(p._el.querySelector('.clipboard-panel-hint').textContent).toBe('clipboard.hint');
        });

        it('keeps Tab inside the panel, both ways', () => {
            const p = make();
            p.open();
            type('abc');
            const close = $('[data-act="close"]');
            const copy = $('[data-act="copy"]');
            copy.focus();
            const fwd = keydown(copy, 'Tab', {}, { key: 'Tab' });
            expect(fwd.defaultPrevented).toBe(true);
            expect(document.activeElement).toBe(close);
            const back = keydown(close, 'Tab', { shiftKey: true }, { key: 'Tab' });
            expect(back.defaultPrevented).toBe(true);
            expect(document.activeElement).toBe(copy);
            // In the middle, Tab is left to the browser.
            area().focus();
            expect(keydown(area(), 'Tab', {}, { key: 'Tab' }).defaultPrevented).toBe(false);
        });

        it('focus() takes the keyboard back from the stream', () => {
            const p = make();
            p.focus(); // closed: nothing to do
            p.open();
            document.getElementById('outside').focus();
            p.focus();
            expect(document.activeElement).toBe(area());
        });

        it('keeps touches and right-clicks from the stream underneath', () => {
            const p = make();
            p.open();
            const seen = vi.fn();
            container.addEventListener('touchstart', seen);
            container.addEventListener('contextmenu', seen);
            area().dispatchEvent(new Event('touchstart', { bubbles: true }));
            area().dispatchEvent(new Event('contextmenu', { bubbles: true }));
            expect(seen).not.toHaveBeenCalled();
        });

        it('destroy() takes the panel away and stops typing', () => {
            vi.useFakeTimers();
            const p = make();
            p.open();
            p.typeText('z'.repeat(100));
            p.destroy();
            vi.advanceTimersByTime(1000);
            expect($('.clipboard-panel')).toBeNull();
            expect(sent).toHaveLength(1);
            expect(p.isOpen).toBe(false);
            p.destroy();
        });
    });

    describe('modes', () => {
        it('types on a host without clipboard sync', () => {
            const p = make();
            p.open();
            expect(p.mode).toBe('type');
            expect($('.clipboard-panel-mode').textContent).toBe('clipboard.modeType');
            expect($('[data-act="send"]').textContent).toBe('clipboard.typeOnHost');
        });

        it('switches to the shared clipboard on clipboardcaps, and back', () => {
            const p = make();
            p.open();
            p.setSyncAvailable(true);
            expect(p.syncAvailable).toBe(true);
            expect(p.mode).toBe('sync');
            expect($('.clipboard-panel-mode').textContent).toBe('clipboard.modeSync');
            expect($('.clipboard-panel-mode').dataset.mode).toBe('sync');
            expect($('[data-act="send"]').textContent).toBe('clipboard.send');
            p.setSyncAvailable(false);
            expect(p.mode).toBe('type');
            expect($('[data-act="send"]').textContent).toBe('clipboard.typeOnHost');
        });

        it('a guest never gets the shared clipboard, even if told it exists', () => {
            state.guest = true;
            const p = make();
            p.setSyncAvailable(true);
            expect(p.mode).toBe('type');
        });

        it('a guest without the keyboard can neither send nor type', () => {
            state.guest = true;
            state.canType = false;
            const p = make();
            p.open();
            expect(p.mode).toBe('none');
            expect($('.clipboard-panel-mode').textContent).toBe('clipboard.modeNone');
            expect($('[data-act="send"]').hidden).toBe(true);
            type('hello');
            p.send();
            expect(sent).toEqual([]);
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.noKeyboard');
            expect(p.typeText('hello')).toBe(false);
            expect(sent).toEqual([]);
        });

        it('Send is disabled while the box is empty', () => {
            const p = make();
            p.open();
            expect($('[data-act="send"]').disabled).toBe(true);
            type('x');
            expect($('[data-act="send"]').disabled).toBe(false);
        });
    });

    describe('the host clipboard', () => {
        it('the box follows it, opened or not', () => {
            const p = make();
            p.setSyncAvailable(true);
            p.hostText('from host');
            p.open();
            expect(area().value).toBe('from host');
            p.hostText('again');
            expect(area().value).toBe('again');
        });

        it('is never shown to a guest', () => {
            state.guest = true;
            const p = make();
            p.open();
            p.hostText('secret');
            expect(area().value).toBe('');
        });

        it('keeps an edit in progress, and says the host changed', () => {
            const p = make();
            p.setSyncAvailable(true);
            p.open();
            p.hostText('first');
            type('my edit');
            p.hostText('second');
            expect(area().value).toBe('my edit');
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.hostChangedKept');
        });

        it('an emptied box is no edit: the next push fills it', () => {
            const p = make();
            p.setSyncAvailable(true);
            p.open();
            p.hostText('first');
            type('my edit');
            type('');
            p.hostText('second');
            expect(area().value).toBe('second');
        });

        it('ignores something that is not text', () => {
            const p = make();
            p.open();
            p.hostText(undefined);
            p.hostText(42);
            expect(area().value).toBe('');
        });
    });

    describe('Send with a shared clipboard', () => {
        it('sets the host clipboard and presses nothing', () => {
            const p = make();
            p.setSyncAvailable(true);
            p.open();
            type('line 1\nline 2');
            $('[data-act="send"]').click();
            expect(sent).toEqual([{ type: 'clipboardset', text: 'line 1\nline 2' }]);
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.sent');
            // What was sent is the host's now: its echo does not count as an edit.
            p.hostText('line 1\nline 2');
            p.hostText('newer');
            expect(area().value).toBe('newer');
        });

        it('caps it at the bridge limit', () => {
            const p = make();
            p.setSyncAvailable(true);
            p.open();
            type('a'.repeat(MAX_SET_CHARS + 5));
            p.send();
            expect(sent[0].text).toHaveLength(MAX_SET_CHARS);
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.sentTruncated');
        });

        it('says there is nothing to send for an empty box', () => {
            const p = make();
            p.setSyncAvailable(true);
            p.open();
            p.send();
            expect(sent).toEqual([]);
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.nothingToSend');
        });
    });

    describe('Type on host', () => {
        beforeEach(() => vi.useFakeTimers());

        it('types the box one character per message, with exact progress', () => {
            const p = make();
            p.open();
            type('b\u{1F600}\n!');
            $('[data-act="send"]').click();
            expect(p.typing).toBe(true);
            expect(sent).toEqual([{ type: 'textinput', text: 'b' }]);
            const progress = $('.clipboard-panel-progress');
            const bar = progress.querySelector('progress');
            expect(progress.hidden).toBe(false);
            expect([bar.value, bar.max]).toEqual([1, 4]);
            vi.advanceTimersByTime(TYPE_INTERVAL_MS);
            expect(sent[1]).toEqual({ type: 'textinput', text: '\u{1F600}' });
            expect(bar.value).toBe(2);
            expect(progress.querySelector('.clipboard-panel-progress-text').textContent).toBe(
                'clipboard.typingProgress',
            );
            vi.advanceTimersByTime(TYPE_INTERVAL_MS * 10);
            expect(sent.map((m) => m.type)).toEqual([
                'textinput',
                'textinput',
                'keydown',
                'keyup',
                'textinput',
            ]);
            expect(sent.some((m) => m.type === 'clipboardset')).toBe(false);
            expect(p.typing).toBe(false);
            expect(progress.hidden).toBe(true);
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.typed');
        });

        it('a new typing clears the last outcome', () => {
            const p = make();
            p.open();
            type('one');
            p.send();
            vi.advanceTimersByTime(TYPE_INTERVAL_MS * 5);
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.typed');
            type('two '.repeat(20));
            p.send();
            expect($('.clipboard-panel-note').textContent).toBe('');
        });

        it('Cancel mid-way stops on the character it reached', () => {
            const p = make();
            p.open();
            type('c'.repeat(500));
            p.send();
            vi.advanceTimersByTime(TYPE_INTERVAL_MS * 19);
            expect(sent).toHaveLength(20);
            $('[data-act="cancel"]').click();
            vi.advanceTimersByTime(TYPE_INTERVAL_MS * 50);
            expect(sent).toHaveLength(20);
            expect($('.clipboard-panel-progress').hidden).toBe(true);
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.typingCancelled');
        });

        it('types at most MAX_TYPE_CHARS characters, and says so', () => {
            const p = make();
            p.open();
            type('d'.repeat(MAX_TYPE_CHARS + 100));
            p.send();
            vi.advanceTimersByTime(TYPE_INTERVAL_MS * (MAX_TYPE_CHARS + 5));
            expect(sent).toHaveLength(MAX_TYPE_CHARS);
            expect(sent.map((m) => m.text).join('')).toHaveLength(MAX_TYPE_CHARS);
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.typedTruncated');
        });

        it('the cap counts an emoji as one character', () => {
            const p = make();
            p.typeText('\u{1F600}'.repeat(MAX_TYPE_CHARS + 1));
            vi.advanceTimersByTime(TYPE_INTERVAL_MS * (MAX_TYPE_CHARS + 5));
            expect(sent).toHaveLength(MAX_TYPE_CHARS);
            expect(sent.every((m) => m.text === '\u{1F600}')).toBe(true);
        });

        it('a second text replaces the first', () => {
            const p = make();
            p.typeText('e'.repeat(100));
            p.typeText('f');
            vi.advanceTimersByTime(TYPE_INTERVAL_MS * 10);
            expect(sent.map((m) => m.text)).toEqual(['e', 'f']);
        });

        it('has nothing to type for an empty text', () => {
            const p = make();
            expect(p.typeText('')).toBe(false);
            expect(sent).toEqual([]);
        });

        it('from Ctrl+V, more than a second of typing shows a toast whose button cancels', () => {
            const p = make();
            const dismiss = vi.fn();
            state.toast.info.mockReturnValue({ dismiss });
            p.typeText('g'.repeat(Math.ceil(1000 / TYPE_INTERVAL_MS) + 1), { fromPaste: true });
            expect(state.toast.info).toHaveBeenCalledTimes(1);
            const [, opts] = state.toast.info.mock.calls[0];
            expect(opts.sticky).toBe(true);
            opts.action.onClick();
            expect(p.typing).toBe(false);
            expect(dismiss).toHaveBeenCalled();
        });

        it('from Ctrl+V, short text needs no toast; the panel stays shut', () => {
            const p = make();
            p.typeText('g'.repeat(Math.ceil(1000 / TYPE_INTERVAL_MS)), { fromPaste: true });
            expect(state.toast.info).not.toHaveBeenCalled();
            expect(p.isOpen).toBe(false);
            expect(sent).toEqual([{ type: 'textinput', text: 'g' }]);
        });

        it('once typed, the text is no edit to protect: the host clipboard replaces it', () => {
            const p = make();
            p.open();
            type('typed out');
            p.send();
            vi.advanceTimersByTime(TYPE_INTERVAL_MS * 10);
            p.setSyncAvailable(true);
            p.hostText('host copy');
            expect(area().value).toBe('host copy');
        });

        it('cancelled, it is still the viewer’s edit', () => {
            const p = make();
            p.open();
            type('x'.repeat(200));
            p.send();
            p.cancelTyping();
            p.hostText('host copy');
            expect(area().value).toBe('x'.repeat(200));
        });

        it('the toast goes when typing ends by itself', () => {
            const p = make();
            const dismiss = vi.fn();
            state.toast.info.mockReturnValue({ dismiss });
            const n = Math.ceil(1000 / TYPE_INTERVAL_MS) + 5;
            p.typeText('h'.repeat(n), { fromPaste: true });
            vi.advanceTimersByTime(TYPE_INTERVAL_MS * n);
            expect(dismiss).toHaveBeenCalledTimes(1);
        });
    });

    describe('Copy to this device', () => {
        it('writes the box to the local clipboard', async () => {
            const p = make();
            p.open();
            type('to copy');
            $('[data-act="copy"]').click();
            await vi.waitFor(() =>
                expect($('.clipboard-panel-note').textContent).toBe('clipboard.copied'),
            );
            expect(state.clipboard.writeText).toHaveBeenCalledWith('to copy');
        });

        it('says so when the browser refuses', async () => {
            state.clipboard.writeText.mockRejectedValue(new DOMException('no', 'NotAllowedError'));
            const p = make();
            p.open();
            type('x');
            expect(await p.copyToDevice()).toBe(false);
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.copyFailed');
        });

        it('says so when the browser has no clipboard API, and greys the button', async () => {
            state.clipboard = null;
            const p = make();
            p.open();
            type('x');
            expect($('[data-act="copy"]').disabled).toBe(true);
            expect(await p.copyToDevice()).toBe(false);
            expect($('.clipboard-panel-note').textContent).toBe('clipboard.copyUnavailable');
        });

        it('is greyed while the box is empty', () => {
            const p = make();
            p.open();
            expect($('[data-act="copy"]').disabled).toBe(true);
        });

        it("defaults to the browser's own navigator.clipboard", () => {
            const p = make({ clipboard: undefined });
            expect(p._clipboard()).toBe(navigator.clipboard || null);
        });
    });

    describe('sync on focus (the setting)', () => {
        const on = { clipboard_sync_on_focus: true };

        it('is off by default: nothing read', async () => {
            const p = make();
            p.setSyncAvailable(true);
            expect(await p.onStreamFocus()).toBe(false);
            expect(state.clipboard.readText).not.toHaveBeenCalled();
        });

        it('on, sets the host clipboard from this device', async () => {
            state.settings = on;
            const p = make();
            p.setSyncAvailable(true);
            expect(await p.onStreamFocus()).toBe(true);
            expect(sent).toEqual([{ type: 'clipboardset', text: 'local text' }]);
            p.open();
            expect(area().value).toBe('local text');
        });

        it('sends the same text once, and never what the host already holds', async () => {
            state.settings = on;
            const p = make();
            p.setSyncAvailable(true);
            await p.onStreamFocus();
            await p.onStreamFocus();
            expect(sent).toHaveLength(1);
            state.clipboard.readText.mockResolvedValue('host has it');
            p.hostText('host has it');
            expect(await p.onStreamFocus()).toBe(false);
            expect(sent).toHaveLength(1);
        });

        it('caps the text at the bridge limit', async () => {
            state.settings = on;
            state.clipboard.readText.mockResolvedValue('q'.repeat(MAX_SET_CHARS + 1));
            const p = make();
            p.setSyncAvailable(true);
            await p.onStreamFocus();
            expect(sent[0].text).toHaveLength(MAX_SET_CHARS);
        });

        it('does nothing without sync, for a guest, or while the panel is open', async () => {
            state.settings = on;
            const p = make();
            expect(await p.onStreamFocus()).toBe(false);
            p.setSyncAvailable(true);
            p.open();
            expect(await p.onStreamFocus()).toBe(false);
            state.guest = true;
            const g = make();
            g.setSyncAvailable(true);
            expect(await g.onStreamFocus()).toBe(false);
            expect(state.clipboard.readText).not.toHaveBeenCalled();
        });

        it('permission denied: one notice, then never asked again this session', async () => {
            state.settings = on;
            state.clipboard.readText.mockRejectedValue(new DOMException('no', 'NotAllowedError'));
            const p = make();
            p.setSyncAvailable(true);
            expect(await p.onStreamFocus()).toBe(false);
            expect(await p.onStreamFocus()).toBe(false);
            expect(state.clipboard.readText).toHaveBeenCalledTimes(1);
            expect(state.toast.warning).toHaveBeenCalledTimes(1);
            expect(state.toast.warning.mock.calls[0][0]).toBe('clipboard.focusSyncDenied');
            expect(sent).toEqual([]);
        });

        it('another failure is retried on the next focus, silently', async () => {
            state.settings = on;
            state.clipboard.readText
                .mockRejectedValueOnce(new DOMException('busy', 'NotFoundError'))
                .mockResolvedValueOnce('second try');
            const p = make();
            p.setSyncAvailable(true);
            expect(await p.onStreamFocus()).toBe(false);
            expect(await p.onStreamFocus()).toBe(true);
            expect(state.toast.warning).not.toHaveBeenCalled();
        });

        it('a browser without readText (Firefox, older Safari): one notice, no error', async () => {
            state.settings = on;
            state.clipboard = { writeText: vi.fn() };
            const p = make();
            p.setSyncAvailable(true);
            expect(await p.onStreamFocus()).toBe(false);
            expect(await p.onStreamFocus()).toBe(false);
            expect(state.toast.warning).toHaveBeenCalledTimes(1);
            expect(state.toast.warning.mock.calls[0][0]).toBe('clipboard.focusSyncUnavailable');
        });

        it('no clipboard API at all is the same', async () => {
            state.settings = on;
            state.clipboard = null;
            const p = make({ toast: null });
            p.setSyncAvailable(true);
            expect(await p.onStreamFocus()).toBe(false);
        });

        it('an empty clipboard sends nothing', async () => {
            state.settings = on;
            state.clipboard.readText.mockResolvedValue('');
            const p = make();
            p.setSyncAvailable(true);
            expect(await p.onStreamFocus()).toBe(false);
            expect(sent).toEqual([]);
        });

        it('focus and visibility arriving together read once', async () => {
            state.settings = on;
            let resolve;
            state.clipboard.readText.mockImplementation(() => new Promise((r) => (resolve = r)));
            const p = make();
            p.setSyncAvailable(true);
            const first = p.onStreamFocus();
            expect(await p.onStreamFocus()).toBe(false);
            resolve('once');
            expect(await first).toBe(true);
            expect(state.clipboard.readText).toHaveBeenCalledTimes(1);
        });

        it('a prompt still open (Firefox’s Paste button) is not asked again', async () => {
            state.settings = on;
            state.clipboard.readText.mockImplementation(() => new Promise(() => {}));
            const p = make();
            p.setSyncAvailable(true);
            p.onStreamFocus();
            expect(await p.onStreamFocus()).toBe(false);
            expect(await p.onStreamFocus()).toBe(false);
            expect(state.clipboard.readText).toHaveBeenCalledTimes(1);
            expect(sent).toEqual([]);
        });

        it('keeps an edit in the box', async () => {
            state.settings = on;
            const p = make();
            p.setSyncAvailable(true);
            p.open();
            type('editing');
            p.close();
            await p.onStreamFocus();
            p.open();
            expect(area().value).toBe('editing');
        });
    });

    describe('Ctrl+V types my clipboard (the setting)', () => {
        it('is off by default', () => {
            expect(make().ctrlVTypes()).toBe(false);
        });

        it('on, applies to a host without sync, for a viewer who may type', () => {
            state.settings = { clipboard_ctrlv_types: true };
            const p = make();
            expect(p.ctrlVTypes()).toBe(true);
            p.setSyncAvailable(true);
            expect(p.ctrlVTypes()).toBe(false);
            state.canType = false;
            expect(make().ctrlVTypes()).toBe(false);
        });
    });
});
