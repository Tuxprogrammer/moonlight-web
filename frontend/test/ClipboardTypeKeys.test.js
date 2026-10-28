/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
    ClipboardPanel,
    SETTINGS_KEY,
    TYPE_INTERVAL_MS,
    TYPE_METHODS,
    TextTyper,
    keyStrokeMessages,
    readClipboardPrefs,
    readTypeMethod,
    typesWithKeys,
    typingSteps,
    usKeyFor,
} from '../js/ui/ClipboardPanel.js';

/**
 * Type on host, as key presses.
 *
 * Sunshine on Linux types a `textinput` character as Ctrl+Shift+U, its hex
 * code and a space, which some apps and compositors mangle. A character that
 * has a key on a US keyboard can go as that key instead: down and up, with
 * Shift around it when the US layout needs Shift, the character riding along
 * as a chord (`char`, `chord: true`), exactly as the touch keyboard's bar
 * sends a latched Shift and a letter. Everything else (é, 日本, an emoji)
 * keeps the Unicode path, character by character, in order.
 *
 * The setting `clipboard_type_method` (per browser): `auto` (the default)
 * types keys on a host that may mangle text — Linux, or a host nobody could
 * identify — and text on Windows, macOS and the server's own machine; `keys`
 * and `text` force one or the other.
 */

const VK_SHIFT = 0x10;
const NO_MODS = { ctrlKey: false, shiftKey: false, altKey: false, metaKey: false };
const SHIFT = { ...NO_MODS, shiftKey: true };

function memoryStorage(settings) {
    const data = new Map();
    if (settings !== undefined) data.set(SETTINGS_KEY, JSON.stringify(settings));
    return {
        getItem: (k) => (data.has(k) ? data.get(k) : null),
        setItem: (k, v) => data.set(k, String(v)),
    };
}

/** The E2E test's string: 67 code points, 6 of them with no US key. */
const E2E = 'Hello, World! 123 #$%&*()[]{};:\'"<>/?\\|`~_+=-\nLine2: é ü ß 日本 \u{1F600} end';

/** What a message sequence types on a US host: key presses resolved by the US layout, text as is. */
function replayUs(messages) {
    const US_UNSHIFTED = {
        0xc0: '`',
        0xbd: '-',
        0xbb: '=',
        0xdb: '[',
        0xdd: ']',
        0xdc: '\\',
        0xba: ';',
        0xde: "'",
        0xbc: ',',
        0xbe: '.',
        0xbf: '/',
        0x20: ' ',
        0x09: '\t',
        0x0d: '\n',
    };
    const US_SHIFTED = {
        0xc0: '~',
        0xbd: '_',
        0xbb: '+',
        0xdb: '{',
        0xdd: '}',
        0xdc: '|',
        0xba: ':',
        0xde: '"',
        0xbc: '<',
        0xbe: '>',
        0xbf: '?',
        0x31: '!',
        0x32: '@',
        0x33: '#',
        0x34: '$',
        0x35: '%',
        0x36: '^',
        0x37: '&',
        0x38: '*',
        0x39: '(',
        0x30: ')',
    };
    let shift = false;
    let out = '';
    for (const m of messages) {
        if (m.type === 'textinput') {
            out += m.text;
            continue;
        }
        if (m.keyCode === VK_SHIFT) {
            shift = m.type === 'keydown';
            continue;
        }
        if (m.type !== 'keydown') continue;
        const vk = m.keyCode;
        if (vk >= 0x41 && vk <= 0x5a) {
            const ch = String.fromCharCode(vk);
            out += shift ? ch : ch.toLowerCase();
        } else if (vk >= 0x30 && vk <= 0x39 && !shift) {
            out += String.fromCharCode(vk);
        } else {
            out += (shift ? US_SHIFTED : US_UNSHIFTED)[vk] ?? '?';
        }
    }
    return { out, shiftHeld: shift };
}

describe('usKeyFor — the US key of a character', () => {
    it('a lowercase letter is its key, no Shift', () => {
        expect(usKeyFor('a')).toEqual({ vk: 0x41, shift: false });
        expect(usKeyFor('z')).toEqual({ vk: 0x5a, shift: false });
    });

    it('an uppercase letter is the same key under Shift', () => {
        expect(usKeyFor('A')).toEqual({ vk: 0x41, shift: true });
        expect(usKeyFor('Z')).toEqual({ vk: 0x5a, shift: true });
    });

    it('a digit is its key on the main row', () => {
        for (let d = 0; d <= 9; d++) {
            expect(usKeyFor(String(d))).toEqual({ vk: 0x30 + d, shift: false });
        }
    });

    it('the symbols over the digits are those keys under Shift', () => {
        const row = {
            ')': 0,
            '!': 1,
            '@': 2,
            '#': 3,
            $: 4,
            '%': 5,
            '^': 6,
            '&': 7,
            '*': 8,
            '(': 9,
        };
        for (const [ch, d] of Object.entries(row)) {
            expect(usKeyFor(ch)).toEqual({ vk: 0x30 + d, shift: true });
        }
    });

    it('punctuation, both levels of each OEM key', () => {
        const pairs = [
            ['`', '~', 0xc0],
            ['-', '_', 0xbd],
            ['=', '+', 0xbb],
            ['[', '{', 0xdb],
            [']', '}', 0xdd],
            ['\\', '|', 0xdc],
            [';', ':', 0xba],
            ["'", '"', 0xde],
            [',', '<', 0xbc],
            ['.', '>', 0xbe],
            ['/', '?', 0xbf],
        ];
        for (const [lo, hi, vk] of pairs) {
            expect(usKeyFor(lo)).toEqual({ vk, shift: false });
            expect(usKeyFor(hi)).toEqual({ vk, shift: true });
        }
    });

    it('space and Tab are keys of their own', () => {
        expect(usKeyFor(' ')).toEqual({ vk: 0x20, shift: false });
        expect(usKeyFor('\t')).toEqual({ vk: 0x09, shift: false });
    });

    it('every printable ASCII character has a key', () => {
        for (let c = 0x20; c < 0x7f; c++) {
            expect(usKeyFor(String.fromCharCode(c)), `0x${c.toString(16)}`).not.toBeNull();
        }
    });

    it('nothing outside a US keyboard: accents, CJK, emoji, other controls', () => {
        for (const ch of ['é', 'ü', 'ß', 'É', '日', '本', '€', '£', ' ', '\u{1F600}']) {
            expect(usKeyFor(ch), ch).toBeNull();
        }
        for (const ch of ['\u0000', '\u0007', '\b', '\v', '\f', '\u001b', '\u007f']) {
            expect(usKeyFor(ch)).toBeNull();
        }
    });

    it('a line break is no key here: typing presses Enter for it in either method', () => {
        expect(usKeyFor('\n')).toBeNull();
        expect(usKeyFor('\r')).toBeNull();
    });

    it('takes one character only', () => {
        expect(usKeyFor('')).toBeNull();
        expect(usKeyFor('ab')).toBeNull();
        expect(usKeyFor(undefined)).toBeNull();
        expect(usKeyFor(null)).toBeNull();
    });
});

describe('keyStrokeMessages — what one key press sends', () => {
    it('a lowercase letter: down and up, its character as a chord, no modifier', () => {
        expect(keyStrokeMessages({ vk: 0x41, shift: false, char: 'a' })).toEqual([
            {
                type: 'keydown',
                keyCode: 0x41,
                code: '',
                key: '',
                ...NO_MODS,
                char: 'a',
                chord: true,
            },
            { type: 'keyup', keyCode: 0x41, code: '', key: '', ...NO_MODS, char: 'a', chord: true },
        ]);
    });

    it('an uppercase letter: Shift down, the key under Shift, Shift up last', () => {
        expect(keyStrokeMessages({ vk: 0x41, shift: true, char: 'A' })).toEqual([
            { type: 'keydown', keyCode: VK_SHIFT, code: '', key: '', ...SHIFT },
            { type: 'keydown', keyCode: 0x41, code: '', key: '', ...SHIFT, char: 'A', chord: true },
            { type: 'keyup', keyCode: 0x41, code: '', key: '', ...SHIFT, char: 'A', chord: true },
            { type: 'keyup', keyCode: VK_SHIFT, code: '', key: '', ...NO_MODS },
        ]);
    });

    it('is what the touch keyboard sends for a latched Shift and a letter', () => {
        // StreamViewKeyboard: _toggleMod('shift') → _sendToolbarKey(vk, ch) →
        // _releaseLatchedMods(). Same four messages, same fields.
        const msgs = keyStrokeMessages({ vk: 0x51, shift: true, char: 'Q' });
        expect(msgs.map((m) => [m.type, m.keyCode, m.shiftKey])).toEqual([
            ['keydown', VK_SHIFT, true],
            ['keydown', 0x51, true],
            ['keyup', 0x51, true],
            ['keyup', VK_SHIFT, false],
        ]);
    });

    it('shifted punctuation: the US key under Shift, the character it makes as a chord', () => {
        const msgs = keyStrokeMessages({ ...usKeyFor('?'), char: '?' });
        expect(msgs).toHaveLength(4);
        expect(msgs[1]).toMatchObject({ keyCode: 0xbf, shiftKey: true, char: '?', chord: true });
        expect(msgs[3]).toMatchObject({ type: 'keyup', keyCode: VK_SHIFT, shiftKey: false });
    });

    it('a key without a character (Tab, Enter): plain down and up, no chord', () => {
        const msgs = keyStrokeMessages({ vk: 0x09, shift: false, char: null, code: 'Tab' });
        expect(msgs).toEqual([
            { type: 'keydown', keyCode: 0x09, code: 'Tab', key: 'Tab', ...NO_MODS, char: null },
            { type: 'keyup', keyCode: 0x09, code: 'Tab', key: 'Tab', ...NO_MODS, char: null },
        ]);
        expect(msgs.some((m) => 'chord' in m)).toBe(false);
    });

    it('never holds Ctrl, Alt or Meta, and lets go of every key it pressed', () => {
        for (let c = 0x20; c < 0x7f; c++) {
            const ch = String.fromCharCode(c);
            const msgs = keyStrokeMessages({ ...usKeyFor(ch), char: ch });
            const held = new Set();
            for (const m of msgs) {
                expect(m.ctrlKey || m.altKey || m.metaKey).toBe(false);
                if (m.type === 'keydown') held.add(m.keyCode);
                else held.delete(m.keyCode);
            }
            expect(held.size, ch).toBe(0);
            expect(msgs.at(-1).shiftKey).toBe(false);
        }
    });
});

describe('typingSteps with key presses', () => {
    it('without the option, every character is still text (unchanged)', () => {
        expect(typingSteps('aB')).toEqual([
            { text: 'a', chars: 1 },
            { text: 'B', chars: 1 },
        ]);
        expect(typingSteps('a\tb')[1]).toEqual({ text: '\t', chars: 1 });
    });

    it('a character with a US key becomes a key step, its character kept', () => {
        expect(typingSteps('aB!', { keys: true })).toEqual([
            { key: { vk: 0x41, shift: false, char: 'a' }, chars: 1 },
            { key: { vk: 0x42, shift: true, char: 'B' }, chars: 1 },
            { key: { vk: 0x31, shift: true, char: '!' }, chars: 1 },
        ]);
    });

    it('Tab is the Tab key, a line break still Enter', () => {
        expect(typingSteps('a\tb\r\nc', { keys: true })).toEqual([
            { key: { vk: 0x41, shift: false, char: 'a' }, chars: 1 },
            { key: { vk: 0x09, shift: false, char: null, code: 'Tab' }, chars: 1 },
            { key: { vk: 0x42, shift: false, char: 'b' }, chars: 1 },
            { enter: true, chars: 1 },
            { key: { vk: 0x43, shift: false, char: 'c' }, chars: 1 },
        ]);
    });

    it('falls back to text per character for what has no key, in order', () => {
        expect(typingSteps('aé日\u{1F600}b', { keys: true })).toEqual([
            { key: { vk: 0x41, shift: false, char: 'a' }, chars: 1 },
            { text: 'é', chars: 1 },
            { text: '日', chars: 1 },
            { text: '\u{1F600}', chars: 1 },
            { key: { vk: 0x42, shift: false, char: 'b' }, chars: 1 },
        ]);
    });

    it('the E2E string: 60 key presses, an Enter and 6 text steps, 67 in all', () => {
        const steps = typingSteps(E2E, { keys: true });
        expect(steps).toHaveLength(67);
        expect(steps).toHaveLength(Array.from(E2E).length);
        expect(steps.filter((s) => s.text).map((s) => s.text)).toEqual([
            'é',
            'ü',
            'ß',
            '日',
            '本',
            '\u{1F600}',
        ]);
        expect(steps.filter((s) => s.enter)).toHaveLength(1);
        expect(steps.filter((s) => s.key)).toHaveLength(60);
    });
});

describe('TextTyper with key steps', () => {
    beforeEach(() => vi.useFakeTimers());
    afterEach(() => vi.useRealTimers());

    const run = (text, keys = true) => {
        const sent = [];
        const typer = new TextTyper({
            send: (m) => sent.push(m),
            steps: typingSteps(text, { keys }),
        });
        typer.start();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * Array.from(text).length);
        return { sent, typer };
    };

    it('sends a whole key press at once, one character per interval', () => {
        const sent = [];
        const typer = new TextTyper({
            send: (m) => sent.push(m),
            steps: typingSteps('Ab', { keys: true }),
        });
        typer.start();
        // The first character, Shift included, in the same tick: nothing held in between.
        expect(sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keydown', VK_SHIFT],
            ['keydown', 0x41],
            ['keyup', 0x41],
            ['keyup', VK_SHIFT],
        ]);
        vi.advanceTimersByTime(TYPE_INTERVAL_MS - 1);
        expect(sent).toHaveLength(4);
        vi.advanceTimersByTime(1);
        expect(sent.slice(4)).toEqual([
            {
                type: 'keydown',
                keyCode: 0x42,
                code: '',
                key: '',
                ...NO_MODS,
                char: 'b',
                chord: true,
            },
            { type: 'keyup', keyCode: 0x42, code: '', key: '', ...NO_MODS, char: 'b', chord: true },
        ]);
        expect(typer.active).toBe(false);
    });

    it('upper, lower and shifted punctuation type exactly on a US host', () => {
        const { sent } = run('aZ9(;:\'"?/ ~`');
        expect(replayUs(sent)).toEqual({ out: 'aZ9(;:\'"?/ ~`', shiftHeld: false });
        expect(sent.some((m) => m.type === 'textinput')).toBe(false);
    });

    it('Enter and Tab are presses, never text', () => {
        const { sent } = run('a\tb\nc');
        expect(sent.some((m) => m.type === 'textinput')).toBe(false);
        expect(sent.filter((m) => m.keyCode === 0x09).map((m) => m.type)).toEqual([
            'keydown',
            'keyup',
        ]);
        expect(sent.filter((m) => m.keyCode === 0x0d).map((m) => m.type)).toEqual([
            'keydown',
            'keyup',
        ]);
        expect(replayUs(sent).out).toBe('a\tb\nc');
    });

    it('the E2E string arrives whole on a US host, Shift let go at the end', () => {
        const { sent } = run(E2E);
        expect(replayUs(sent)).toEqual({ out: E2E, shiftHeld: false });
        expect(sent.filter((m) => m.type === 'textinput').map((m) => m.text)).toEqual([
            'é',
            'ü',
            'ß',
            '日',
            '本',
            '\u{1F600}',
        ]);
    });

    it('a mixed string keeps its order across both paths', () => {
        const { sent } = run('Çа-b');
        // Ç and the Cyrillic а have no US key: text, between the key presses.
        expect(sent.map((m) => (m.type === 'textinput' ? m.text : m.type[3]))).toEqual([
            'Ç',
            'а',
            'd',
            'u',
            'd',
            'u',
        ]);
    });

    it('cancelled mid-way, no key is left down: each press is whole', () => {
        const sent = [];
        const typer = new TextTyper({
            send: (m) => sent.push(m),
            steps: typingSteps('ABCDEFGHIJ', { keys: true }),
        });
        typer.start();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * 3);
        typer.cancel();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * 20);
        expect(sent).toHaveLength(16);
        expect(replayUs(sent)).toEqual({ out: 'ABCD', shiftHeld: false });
    });
});

describe('the type method setting', () => {
    it('has three values, auto first', () => {
        expect(TYPE_METHODS).toEqual(['auto', 'keys', 'text']);
    });

    it('is auto when nothing is stored, or storage is broken', () => {
        expect(readTypeMethod(memoryStorage())).toBe('auto');
        expect(readTypeMethod(null)).toBe('auto');
        expect(readTypeMethod({ getItem: () => '{nope' })).toBe('auto');
        expect(
            readTypeMethod({
                getItem: () => {
                    throw new Error('SecurityError');
                },
            }),
        ).toBe('auto');
    });

    it('reads keys or text, and anything else as auto', () => {
        expect(readTypeMethod(memoryStorage({ clipboard_type_method: 'keys' }))).toBe('keys');
        expect(readTypeMethod(memoryStorage({ clipboard_type_method: 'text' }))).toBe('text');
        expect(readTypeMethod(memoryStorage({ clipboard_type_method: 'KEYS' }))).toBe('auto');
        expect(readTypeMethod(memoryStorage({ clipboard_type_method: true }))).toBe('auto');
    });

    it('reads localStorage when no storage is given', () => {
        localStorage.setItem(SETTINGS_KEY, JSON.stringify({ clipboard_type_method: 'text' }));
        expect(readTypeMethod()).toBe('text');
        localStorage.removeItem(SETTINGS_KEY);
        expect(readTypeMethod()).toBe('auto');
    });

    it('leaves the two other clipboard settings as they were', () => {
        const s = memoryStorage({ clipboard_type_method: 'keys' });
        expect(readClipboardPrefs(s)).toEqual({ syncOnFocus: false, ctrlVTypes: false });
    });
});

describe('typesWithKeys — the Automatic rule', () => {
    it('keys and text are what they say, whatever the host', () => {
        for (const hostOs of ['linux', 'windows', 'macos', 'unknown', '']) {
            for (const nativeHost of [false, true]) {
                expect(typesWithKeys('keys', { hostOs, nativeHost })).toBe(true);
                expect(typesWithKeys('text', { hostOs, nativeHost })).toBe(false);
            }
        }
    });

    it('auto: key presses on a Linux host', () => {
        expect(typesWithKeys('auto', { hostOs: 'linux', nativeHost: false })).toBe(true);
    });

    it('auto: key presses on a host nobody identified (mostly Linux: a TTL of 64)', () => {
        expect(typesWithKeys('auto', { hostOs: 'unknown', nativeHost: false })).toBe(true);
        expect(typesWithKeys('auto', { hostOs: '', nativeHost: false })).toBe(true);
        expect(typesWithKeys('auto', {})).toBe(true);
    });

    it('auto: text on Windows and macOS, which inject it directly, whatever the layout', () => {
        expect(typesWithKeys('auto', { hostOs: 'windows', nativeHost: false })).toBe(false);
        expect(typesWithKeys('auto', { hostOs: 'macos', nativeHost: false })).toBe(false);
        expect(typesWithKeys('auto', { hostOs: 'Windows', nativeHost: false })).toBe(false);
    });

    it("auto: text on the server's own machine, which types it in its real layout", () => {
        for (const hostOs of ['linux', 'unknown', 'windows']) {
            expect(typesWithKeys('auto', { hostOs, nativeHost: true })).toBe(false);
        }
    });

    it('an unknown method reads as auto', () => {
        expect(typesWithKeys('bogus', { hostOs: 'linux' })).toBe(true);
        expect(typesWithKeys(undefined, { hostOs: 'windows' })).toBe(false);
    });
});

describe('ClipboardPanel typing with key presses', () => {
    let container;
    let sent;

    beforeEach(() => {
        vi.useFakeTimers();
        document.body.innerHTML = '<div id="stream"></div>';
        container = document.getElementById('stream');
        sent = [];
    });
    afterEach(() => vi.useRealTimers());

    const make = (settings, host = {}) =>
        new ClipboardPanel({
            container,
            send: (m) => sent.push(m),
            canType: () => true,
            isGuest: () => false,
            storage: memoryStorage(settings),
            hostOs: () => host.os,
            nativeHost: () => !!host.native,
            toast: { info: vi.fn(() => ({ dismiss: vi.fn() })), warning: vi.fn() },
        });
    const typeAll = (p, text) => {
        p.typeText(text);
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * Array.from(text).length);
    };

    it('auto, on an unidentified host: ASCII as keys, the rest as text', () => {
        const p = make(undefined, { os: 'unknown' });
        expect(p.typeMethod).toBe('keys');
        typeAll(p, 'Hé!');
        expect(replayUs(sent)).toEqual({ out: 'Hé!', shiftHeld: false });
        expect(sent.filter((m) => m.type === 'textinput')).toEqual([
            { type: 'textinput', text: 'é' },
        ]);
    });

    it('auto, on a Windows host: all text, as before', () => {
        const p = make(undefined, { os: 'windows' });
        expect(p.typeMethod).toBe('text');
        typeAll(p, 'Hi!');
        expect(sent).toEqual([
            { type: 'textinput', text: 'H' },
            { type: 'textinput', text: 'i' },
            { type: 'textinput', text: '!' },
        ]);
    });

    it("auto, on the server's own machine: text", () => {
        const p = make(undefined, { os: 'linux', native: true });
        expect(p.typeMethod).toBe('text');
    });

    it('auto, with no host information at all: keys (as for an unidentified host)', () => {
        const p = new ClipboardPanel({
            container,
            send: (m) => sent.push(m),
            storage: memoryStorage(),
        });
        expect(p.typeMethod).toBe('keys');
    });

    it('the setting forces either method', () => {
        expect(make({ clipboard_type_method: 'keys' }, { os: 'windows' }).typeMethod).toBe('keys');
        expect(make({ clipboard_type_method: 'text' }, { os: 'linux' }).typeMethod).toBe('text');
    });

    it('is read at the moment typing starts: a change applies to the next text', () => {
        const storage = memoryStorage({ clipboard_type_method: 'text' });
        const p = new ClipboardPanel({
            container,
            send: (m) => sent.push(m),
            storage,
            hostOs: () => 'linux',
        });
        p.typeText('a');
        expect(sent).toEqual([{ type: 'textinput', text: 'a' }]);
        storage.setItem(SETTINGS_KEY, JSON.stringify({ clipboard_type_method: 'keys' }));
        sent.length = 0;
        p.typeText('a');
        expect(sent.map((m) => m.type)).toEqual(['keydown', 'keyup']);
    });

    it('the host OS is read when typing starts (the TTL may settle it mid-session)', () => {
        const host = { os: 'unknown' };
        const p = make(undefined, host);
        expect(p.typeMethod).toBe('keys');
        host.os = 'windows';
        expect(p.typeMethod).toBe('text');
    });

    it('progress counts characters, key presses and text alike', () => {
        const p = make({ clipboard_type_method: 'keys' });
        p.open();
        const area = container.querySelector('textarea');
        area.value = 'Aé\nb';
        area.dispatchEvent(new Event('input'));
        container.querySelector('[data-act="send"]').click();
        const bar = container.querySelector('.clipboard-panel-progress progress');
        expect([bar.value, bar.max]).toEqual([1, 4]);
        vi.advanceTimersByTime(TYPE_INTERVAL_MS);
        expect(bar.value).toBe(2);
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * 5);
        expect(p.typing).toBe(false);
        expect(container.querySelector('.clipboard-panel-note').textContent).toBe(
            'clipboard.typed',
        );
        expect(replayUs(sent)).toEqual({ out: 'Aé\nb', shiftHeld: false });
    });

    it('Cancel stops between two characters: nothing left held on the host', () => {
        const p = make({ clipboard_type_method: 'keys' });
        p.open();
        p.typeText('ABCDEFGH');
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * 2);
        container.querySelector('[data-act="cancel"]').click();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * 20);
        expect(replayUs(sent)).toEqual({ out: 'ABC', shiftHeld: false });
        expect(container.querySelector('.clipboard-panel-note').textContent).toBe(
            'clipboard.typingCancelled',
        );
    });

    it('closing the panel or tearing it down mid-way leaves nothing held either', () => {
        const p = make({ clipboard_type_method: 'keys' });
        p.open();
        p.typeText('ABCDEF');
        vi.advanceTimersByTime(TYPE_INTERVAL_MS);
        p.close();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS);
        p.destroy();
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * 20);
        expect(replayUs(sent).shiftHeld).toBe(false);
        const held = new Set();
        for (const m of sent) {
            if (m.type === 'keydown') held.add(m.keyCode);
            if (m.type === 'keyup') held.delete(m.keyCode);
        }
        expect(held.size).toBe(0);
    });

    it('says which method it types with, in typing mode only', () => {
        const p = make(undefined, { os: 'linux' });
        p.open();
        const line = container.querySelector('.clipboard-panel-method');
        expect(line.hidden).toBe(false);
        expect(line.textContent).toBe('clipboard.methodKeys');
        expect(line.dataset.method).toBe('keys');
        p.setSyncAvailable(true);
        expect(line.hidden).toBe(true);
    });

    it('says text on a host that gets text', () => {
        const p = make(undefined, { os: 'windows' });
        p.open();
        const line = container.querySelector('.clipboard-panel-method');
        expect(line.textContent).toBe('clipboard.methodText');
        expect(line.dataset.method).toBe('text');
    });

    it('a viewer who may not type sees no method line', () => {
        const p = new ClipboardPanel({
            container,
            send: (m) => sent.push(m),
            canType: () => false,
            storage: memoryStorage(),
        });
        p.open();
        expect(container.querySelector('.clipboard-panel-method').hidden).toBe(true);
    });

    it('the keyboard stays the panel’s: a key typed in it sends the host nothing', () => {
        const p = make({ clipboard_type_method: 'keys' });
        p.open();
        p.typeText('xyz');
        const before = sent.length;
        const area = container.querySelector('textarea');
        area.dispatchEvent(new KeyboardEvent('keydown', { code: 'KeyQ', key: 'q', bubbles: true }));
        area.dispatchEvent(new KeyboardEvent('keyup', { code: 'KeyQ', key: 'q', bubbles: true }));
        expect(sent).toHaveLength(before);
        vi.advanceTimersByTime(TYPE_INTERVAL_MS * 5);
        expect(replayUs(sent).out).toBe('xyz');
    });
});
