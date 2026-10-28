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
 * ClipboardPanel — the stream's clipboard, Guacamole-style.
 *
 * A side panel with a text box, opened by Ctrl+Alt+Shift pressed and released
 * on its own (Cmd+Option+Ctrl on a Mac), by the same three + C, or by a button
 * for touch and TV remotes. While it is open the keyboard is the panel's:
 * StreamView sends the host nothing (the panel is a local keyboard target).
 *
 * Two modes, from the host's `clipboardcaps`:
 *  - Shared clipboard (the host is the server's own machine): the box follows
 *    the host clipboard, and Send sets it (`clipboardset`) without pressing
 *    anything.
 *  - Typing (every other host — GameStream has no clipboard channel): Send
 *    types the text on the host, one character per step at a steady pace,
 *    with a cap, a progress bar and Cancel. A character goes either through
 *    the `textinput` path or, when it has a key on a US keyboard and the
 *    setting says so, as that key pressed (`clipboard_type_method`).
 * "Copy to this device" writes the box to the local clipboard; the click is
 * the gesture `writeText` needs.
 *
 * Guests never see the host clipboard, and may type only when their invitation
 * gives them the keyboard. Nothing here is installed on the viewer's machine:
 * it is the page and the browser's own clipboard API.
 *
 * StreamView owns the stream; this module only draws the panel and decides.
 * Its hooks: open/close (release held keys, give the focus back), `send`.
 */

import { t } from '../i18n/i18n.js';
import { escapeHtml } from '../util/escapeHtml.js';

/** Where the two per-browser settings live (SettingsView writes them). */
export const SETTINGS_KEY = 'mw-streaming-settings';

/** Max text set on the host clipboard — mirrors ClipboardBridge::kMaxTextChars. */
export const MAX_SET_CHARS = 262144;

/** Max characters typed on the host: about five minutes at the pace below. */
export const MAX_TYPE_CHARS = 10000;

/**
 * Pause between two typed characters. Each one is a `textinput` message of its
 * own: Sunshine on Linux types a character as a Ctrl+Shift+U, hex, space
 * sequence, and several in one message came out mangled (`#$%&` arrived as
 * "5b", `é ü ß` were dropped), while one per message arrived byte for byte.
 * The pause keeps each sequence (about eight key events) apart from the next
 * one on a busy host. On a test VM without KVM, 30, 60 and 250 ms all left
 * the odd sequence broken by the guest itself, so a slower pace bought nothing
 * there; 30 ms types about 33 characters a second.
 */
export const TYPE_INTERVAL_MS = 30;

/** A Ctrl+V typing for longer than about a second shows a toast with Cancel. */
const PASTE_TOAST_STEPS = Math.ceil(1000 / TYPE_INTERVAL_MS);

/** Windows virtual key of Enter: a line break is typed as the key. */
const VK_RETURN = 0x0d;

/** Windows virtual key of Shift, as the touch keyboard's latched Shift sends it. */
const VK_SHIFT = 0x10;

/**
 * How Type on host types a character that has a key on a US keyboard
 * (the setting `clipboard_type_method`): 'auto' decides from the host
 * (typesWithKeys), 'keys' presses the key, 'text' sends it as Unicode text.
 */
export const TYPE_METHODS = ['auto', 'keys', 'text'];

/** The US layout's punctuation keys: [unshifted, shifted, virtual key]. */
const US_OEM_KEYS = [
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

/** The symbols over the digits 0-9 on a US keyboard. */
const US_SHIFTED_DIGITS = ')!@#$%^&*(';

/** Character → { vk, shift } for every character a US keyboard types (Enter aside). */
const US_KEYS = (() => {
    const map = new Map();
    for (let c = 0x41; c <= 0x5a; c++) {
        map.set(String.fromCharCode(c + 0x20), { vk: c, shift: false });
        map.set(String.fromCharCode(c), { vk: c, shift: true });
    }
    for (let d = 0; d <= 9; d++) {
        map.set(String(d), { vk: 0x30 + d, shift: false });
        map.set(US_SHIFTED_DIGITS[d], { vk: 0x30 + d, shift: true });
    }
    for (const [lo, hi, vk] of US_OEM_KEYS) {
        map.set(lo, { vk, shift: false });
        map.set(hi, { vk, shift: true });
    }
    map.set(' ', { vk: 0x20, shift: false });
    map.set('\t', { vk: 0x09, shift: false });
    return map;
})();

/**
 * The key that types `ch` on a US keyboard, and whether it needs Shift; null
 * for a character no US key types (é, 日, an emoji, a control character). A
 * line break is not here: typing presses Enter for it in either method.
 *
 * @param {string} ch one character
 * @returns {{ vk: number, shift: boolean } | null}
 */
export function usKeyFor(ch) {
    if (typeof ch !== 'string' || ch.length !== 1) return null;
    const key = US_KEYS.get(ch);
    return key ? { vk: key.vk, shift: key.shift } : null;
}

/**
 * The messages of one key press, as the touch keyboard's bar sends a latched
 * Shift and a character (StreamViewKeyboard._toggleMod, _sendToolbarKey,
 * _releaseLatchedMods): Shift down when needed, the key down and up with its
 * character as a chord (`char`, `chord: true`: a host that can presses its
 * own key for the character, InputMsg::resolveKey; any other one takes the US
 * position), Shift up last. A key without a character (Tab) is a plain press.
 * The press is whole: nothing is left down between two characters.
 *
 * @param {{ vk: number, shift: boolean, char: string|null, code?: string }} stroke
 * @returns {object[]}
 */
export function keyStrokeMessages(stroke) {
    const mods = (shift) => ({ ctrlKey: false, shiftKey: shift, altKey: false, metaKey: false });
    const shiftKey = (type, held) => ({
        type,
        keyCode: VK_SHIFT,
        code: '',
        key: '',
        ...mods(held),
    });
    /** @type {Record<string, unknown>} */
    const key = { keyCode: stroke.vk, code: '', key: '', ...mods(stroke.shift) };
    if (stroke.char) {
        key.char = stroke.char;
        key.chord = true;
    } else {
        key.code = stroke.code || '';
        key.key = stroke.code || '';
        key.char = null;
    }
    const msgs = [];
    if (stroke.shift) msgs.push(shiftKey('keydown', true));
    msgs.push({ type: 'keydown', ...key }, { type: 'keyup', ...key });
    if (stroke.shift) msgs.push(shiftKey('keyup', false));
    return msgs;
}

/**
 * The type method of this browser: 'auto' unless 'keys' or 'text' is stored.
 *
 * @param {Storage|null} [storage]
 * @returns {'auto'|'keys'|'text'}
 */
export function readTypeMethod(storage) {
    try {
        const s = storage === undefined ? globalThis.localStorage : storage;
        const raw = s ? s.getItem(SETTINGS_KEY) : null;
        const method = raw ? JSON.parse(raw).clipboard_type_method : undefined;
        if (method === 'keys' || method === 'text') return method;
    } catch {
        /* no storage, or not JSON: the default */
    }
    return 'auto';
}

/**
 * Whether typing presses the US key of a character, rather than sending it as
 * text. 'auto' presses keys where text may come out mangled: a Linux host,
 * whose Sunshine types text as Ctrl+Shift+U and hex, and an unidentified one
 * (a TTL of 64 is Linux or macOS, mostly Linux) — the hosts StreamViewKeyboard
 * warns about. Windows and macOS inject text directly, in any layout, and the
 * server's own machine types it in its real layout: text there.
 *
 * @param {string|undefined} method one of TYPE_METHODS
 * @param {{ hostOs?: string, nativeHost?: boolean }} host
 */
export function typesWithKeys(method, host = {}) {
    if (method === 'keys') return true;
    if (method === 'text') return false;
    if (host.nativeHost) return false;
    const os = (host.hostOs || '').toLowerCase();
    return os !== 'windows' && os !== 'macos';
}

/**
 * The two clipboard settings of this browser, off unless stored as true.
 *
 * @param {Storage|null} [storage]
 * @returns {{ syncOnFocus: boolean, ctrlVTypes: boolean }}
 */
export function readClipboardPrefs(storage) {
    const prefs = { syncOnFocus: false, ctrlVTypes: false };
    try {
        const s = storage === undefined ? globalThis.localStorage : storage;
        const raw = s ? s.getItem(SETTINGS_KEY) : null;
        if (!raw) return prefs;
        const data = JSON.parse(raw);
        prefs.syncOnFocus = data.clipboard_sync_on_focus === true;
        prefs.ctrlVTypes = data.clipboard_ctrlv_types === true;
    } catch {
        /* no storage, or not JSON: the defaults */
    }
    return prefs;
}

/**
 * The first `max` UTF-16 units of `text`, one fewer when the cut would leave
 * half of a surrogate pair at the end (ClipboardMsg::capText's twin).
 */
export function capText(text, max) {
    if (max <= 0) return '';
    if (text.length <= max) return text;
    let n = max;
    const last = text.charCodeAt(n - 1);
    if (last >= 0xd800 && last <= 0xdbff) n--;
    return text.slice(0, n);
}

/** The first `max` code points of `text`: an emoji counts as one character. */
export function capCodePoints(text, max) {
    const points = Array.from(text);
    return points.length <= max ? text : points.slice(0, Math.max(0, max)).join('');
}

/**
 * The text as typing sends it, in order: one `{ text }` step per code point
 * (Array.from, so a surrogate pair — an emoji — is never split) and
 * `{ enter: true }` for each line break (\r\n, \r or \n) — a key, since a
 * newline sent as text types nothing in most host apps. With `keys`, a
 * character that has a US key is a `{ key }` step instead (keyStrokeMessages);
 * the others stay text, each in its place. Each step is one character of the
 * progress bar.
 *
 * @param {string} text
 * @param {{ keys?: boolean }} [opts]
 * @returns {Array<{ text?: string, enter?: boolean,
 *                   key?: { vk: number, shift: boolean, char: string|null, code?: string },
 *                   chars: number }>}
 */
export function typingSteps(text, opts = {}) {
    return Array.from(text.replace(/\r\n?/g, '\n'), (ch) => {
        if (ch === '\n') return { enter: true, chars: 1 };
        const us = opts.keys ? usKeyFor(ch) : null;
        if (!us) return { text: ch, chars: 1 };
        const key =
            ch === '\t'
                ? { vk: us.vk, shift: false, char: null, code: 'Tab' }
                : { vk: us.vk, shift: us.shift, char: ch };
        return { key, chars: 1 };
    });
}

/** The three modifiers of the control combos, as KeyboardEvent codes. */
function chordGroups(isMac) {
    // Same convention as StreamView's combos: Ctrl+Alt+Shift, and on a Mac
    // Cmd+Option+Ctrl (a Shift chord collides with macOS shortcuts).
    return isMac
        ? [
              ['MetaLeft', 'MetaRight', 'metaKey'],
              ['AltLeft', 'AltRight', 'altKey'],
              ['ControlLeft', 'ControlRight', 'ctrlKey'],
          ]
        : [
              ['ControlLeft', 'ControlRight', 'ctrlKey'],
              ['AltLeft', 'AltRight', 'altKey'],
              ['ShiftLeft', 'ShiftRight', 'shiftKey'],
          ];
}

/**
 * Ctrl+Alt+Shift pressed and released on its own (Guacamole's gesture).
 *
 * Fires on the release that leaves none of the three down, if all three were
 * down together at some point and no other key was pressed in between: the
 * bare chord, never Ctrl+Alt+Shift+Q. Held modifiers are tracked by code and
 * checked against each event's flags, so a release the OS ate (Alt+Tab) does
 * not leave the gesture armed forever.
 */
export class ChordGesture {
    /** @param {boolean} isMac */
    constructor(isMac) {
        this._groups = chordGroups(isMac);
        /** @type {Set<string>} */
        this._down = new Set();
        this._armed = false;
        this._spoiled = false;
    }

    _group(code) {
        return this._groups.findIndex((g) => g[0] === code || g[1] === code);
    }

    /** Forget a held modifier whose flag the event says is up. */
    _reconcile(e) {
        for (const code of [...this._down]) {
            const g = this._groups[this._group(code)];
            if (code !== e.code && !e[g[2]]) this._down.delete(code);
        }
    }

    _groupsDown() {
        let n = 0;
        for (const g of this._groups) {
            if (this._down.has(g[0]) || this._down.has(g[1])) n++;
        }
        return n;
    }

    /** @param {KeyboardEvent} e */
    keyDown(e) {
        if (e.repeat) return;
        this._reconcile(e);
        // All three seen up again (their releases eaten): start over.
        if (this._groupsDown() === 0) {
            this._armed = false;
            this._spoiled = false;
        }
        if (this._group(e.code) < 0) {
            // Another key under one of the three: a combo, not the bare chord.
            // With none of them down it is plain typing, and none of ours.
            if (this._groupsDown() > 0) {
                this._spoiled = true;
                this._armed = false;
            }
            return;
        }
        this._down.add(e.code);
        if (this._groupsDown() === this._groups.length && !this._spoiled) this._armed = true;
    }

    /**
     * @param {KeyboardEvent} e
     * @returns {boolean} the bare chord just completed
     */
    keyUp(e) {
        if (this._group(e.code) < 0) return false;
        this._down.delete(e.code);
        this._reconcile(e);
        if (this._groupsDown() > 0) return false;
        const fired = this._armed && !this._spoiled;
        this.reset();
        return fired;
    }

    reset() {
        this._down.clear();
        this._armed = false;
        this._spoiled = false;
    }
}

/**
 * Types text on the host, one step at a time: the first at once, then one
 * every `intervalMs`. Ends by itself, or on cancel().
 */
export class TextTyper {
    /**
     * @param {{ send: (msg: object) => void,
     *           steps: ReturnType<typeof typingSteps>,
     *           intervalMs?: number,
     *           onProgress?: (done: number, total: number) => void,
     *           onDone?: (cancelled: boolean) => void,
     *           timers?: { setTimeout: Function, clearTimeout: Function } }} opts
     */
    constructor(opts) {
        this._opts = opts;
        this._timers = opts.timers || globalThis;
        this._steps = opts.steps;
        this._index = 0;
        this._done = 0;
        this.total = opts.steps.reduce((n, s) => n + s.chars, 0);
        this._timer = null;
        this.active = false;
    }

    start() {
        this.active = true;
        this._next();
    }

    _next() {
        this._timer = null;
        if (!this.active) return;
        const step = this._steps[this._index++];
        if (!step) {
            this._finish(false);
            return;
        }
        if (step.enter) {
            const key = { keyCode: VK_RETURN, code: 'Enter', key: 'Enter', char: null };
            const mods = { ctrlKey: false, shiftKey: false, altKey: false, metaKey: false };
            this._opts.send({ type: 'keydown', ...key, ...mods });
            this._opts.send({ type: 'keyup', ...key, ...mods });
        } else if (step.key) {
            // The whole press in one go: a cancel or a teardown between two
            // characters never leaves Shift, or the key, down on the host.
            for (const msg of keyStrokeMessages(step.key)) this._opts.send(msg);
        } else {
            this._opts.send({ type: 'textinput', text: step.text });
        }
        this._done += step.chars;
        if (this._opts.onProgress) this._opts.onProgress(this._done, this.total);
        if (this._index >= this._steps.length) {
            this._finish(false);
            return;
        }
        this._timer = this._timers.setTimeout(
            () => this._next(),
            this._opts.intervalMs ?? TYPE_INTERVAL_MS,
        );
    }

    cancel() {
        if (!this.active) return;
        this._finish(true);
    }

    _finish(cancelled) {
        if (this._timer !== null) this._timers.clearTimeout(this._timer);
        this._timer = null;
        this.active = false;
        if (this._opts.onDone) this._opts.onDone(cancelled);
    }
}

let panelSeq = 0;

export class ClipboardPanel {
    /**
     * @param {{ container: Element,
     *           send: (msg: object) => void,
     *           isMac?: boolean,
     *           canType?: () => boolean,
     *           isGuest?: () => boolean,
     *           onOpen?: () => void,
     *           onClose?: () => void,
     *           storage?: Storage|null,
     *           clipboard?: { readText?: Function, writeText?: Function }|null,
     *           toast?: { info: Function, warning: Function }|null,
     *           timers?: { setTimeout: Function, clearTimeout: Function },
     *           hostOs?: () => string,
     *           nativeHost?: () => boolean }} opts
     *   canType: whether this viewer may use the keyboard on the host (a guest
     *   only when the invitation says so). onOpen: release what the host holds
     *   and free the pointer. onClose: the stream takes the keyboard back.
     *   hostOs, nativeHost: what the type method 'auto' decides from, read when
     *   typing starts (HostOsProbe may settle the OS during the session).
     */
    constructor(opts) {
        this._opts = opts;
        this._id = `clipboard-panel-${++panelSeq}`;
        this._chord = new ChordGesture(!!opts.isMac);
        /** @type {HTMLElement|null} */
        this._el = null;
        this._open = false;
        this._syncAvailable = false;
        // What the box held when the host (or our own Send) last set it: an
        // edit away from it is the viewer's, and a host push keeps out of it.
        this._baseline = '';
        this._text = '';
        this._lastHostText = '';
        /** @type {TextTyper|null} */
        this._typer = null;
        this._typerToast = null;
        this._focusSyncBusy = false;
        this._focusSyncDenied = false;
        this._focusSyncWarned = false;
        this._lastFocusSent = null;
        /** @type {Element|null} */
        this._returnFocus = null;
        this._onKeyDown = (e) => this._handleKeyDown(e);
        this._onKeyUp = (e) => this._handleKeyUp(e);
    }

    get isOpen() {
        return this._open;
    }

    get syncAvailable() {
        return this._syncAvailable;
    }

    /** Typing on the host right now. */
    get typing() {
        return !!(this._typer && this._typer.active);
    }

    _isGuest() {
        return !!(this._opts.isGuest && this._opts.isGuest());
    }

    _canType() {
        return this._opts.canType ? !!this._opts.canType() : true;
    }

    _clipboard() {
        if (this._opts.clipboard !== undefined) return this._opts.clipboard;
        return typeof navigator !== 'undefined' ? navigator.clipboard || null : null;
    }

    /**
     * 'sync' — Send sets the host clipboard; 'type' — Send types the text;
     * 'none' — this viewer may not act on the host at all.
     */
    get mode() {
        if (this._syncAvailable && !this._isGuest()) return 'sync';
        return this._canType() ? 'type' : 'none';
    }

    /** Whether Ctrl+V should type the local clipboard (the setting, on a host without sync). */
    ctrlVTypes() {
        if (this._syncAvailable) return false;
        if (!this._canType()) return false;
        return readClipboardPrefs(this._storage()).ctrlVTypes;
    }

    _storage() {
        return this._opts.storage;
    }

    /** How typing goes right now: 'keys' (US keys, text for the rest) or 'text'. */
    get typeMethod() {
        const host = {
            hostOs: this._opts.hostOs ? this._opts.hostOs() : '',
            nativeHost: this._opts.nativeHost ? !!this._opts.nativeHost() : false,
        };
        return typesWithKeys(readTypeMethod(this._storage()), host) ? 'keys' : 'text';
    }

    // ── Host messages ───────────────────────────────────────────────────

    /** `clipboardcaps`: host clipboard sync is (un)available. */
    setSyncAvailable(available) {
        this._syncAvailable = !!available;
        this.refresh();
    }

    /** `clipboard`: the host clipboard changed. Never shown to a guest. */
    hostText(text) {
        if (typeof text !== 'string' || this._isGuest()) return;
        this._lastHostText = text;
        if (this._dirty()) {
            this._note(t('clipboard.hostChangedKept'));
            return;
        }
        this._setText(text);
        this._baseline = text;
    }

    // ── Open / close ────────────────────────────────────────────────────

    toggle() {
        if (this._open) this.close();
        else this.open();
    }

    /** Put the keyboard back in the panel (a click on the picture took it). */
    focus() {
        if (this._open && !this._el.contains(document.activeElement)) this._textarea().focus();
    }

    open() {
        if (this._open) return;
        this._build();
        this._returnFocus = typeof document !== 'undefined' ? document.activeElement : null;
        this._open = true;
        this._chord.reset();
        if (this._opts.onOpen) this._opts.onOpen();
        this._el.hidden = false;
        this._el.classList.add('open');
        this.refresh();
        this._textarea().focus();
    }

    close() {
        if (!this._open) return;
        this._open = false;
        this._chord.reset();
        // Read before hiding: focus inside a hidden subtree is dropped.
        const hadFocus = this._el.contains(document.activeElement);
        this._el.classList.remove('open');
        this._el.hidden = true;
        if (hadFocus && document.activeElement && this._el.contains(document.activeElement)) {
            /** @type {HTMLElement} */ (document.activeElement).blur();
        }
        const back = /** @type {HTMLElement|null} */ (this._returnFocus);
        this._returnFocus = null;
        if (back && back.isConnected && typeof back.focus === 'function') back.focus();
        if (this._opts.onClose) this._opts.onClose();
    }

    destroy() {
        if (this._typer) this._typer.cancel();
        this._typer = null;
        this._dismissTyperToast();
        if (this._el) {
            this._el.removeEventListener('keydown', this._onKeyDown);
            this._el.removeEventListener('keyup', this._onKeyUp);
            this._el.remove();
        }
        this._el = null;
        this._open = false;
    }

    // ── Actions ─────────────────────────────────────────────────────────

    /** The Send / Type on host button. */
    send() {
        const text = this._text;
        if (!text) {
            this._note(t('clipboard.nothingToSend'));
            return;
        }
        const mode = this.mode;
        if (mode === 'sync') {
            const capped = capText(text, MAX_SET_CHARS);
            this._opts.send({ type: 'clipboardset', text: capped });
            this._baseline = capped;
            this._lastHostText = capped;
            this._note(
                capped.length < text.length
                    ? t('clipboard.sentTruncated', { count: capped.length })
                    : t('clipboard.sent'),
            );
        } else if (mode === 'type') {
            this.typeText(text);
        } else {
            this._note(t('clipboard.noKeyboard'));
        }
    }

    /**
     * Type `text` on the host, replacing any typing in progress.
     *
     * @param {string} text
     * @param {{ fromPaste?: boolean }} [opts] fromPaste: Ctrl+V with the panel
     *   closed — long text then shows a toast with Cancel.
     * @returns {boolean} typing started
     */
    typeText(text, opts = {}) {
        if (!this._canType() || !text) return false;
        if (this._typer) this._typer.cancel();
        const capped = capCodePoints(text, MAX_TYPE_CHARS);
        const steps = typingSteps(capped, { keys: this.typeMethod === 'keys' });
        if (steps.length === 0) return false;
        const truncated = capped.length < text.length;
        const typer = new TextTyper({
            send: this._opts.send,
            steps,
            timers: this._opts.timers,
            onProgress: (done, total) => this._showProgress(done, total),
            onDone: (cancelled) => {
                if (this._typer !== typer) return;
                this._typer = null;
                this._dismissTyperToast();
                this._hideProgress();
                // Delivered: the box's text is no longer an edit to protect.
                if (!cancelled && text === this._text) this._baseline = text;
                this._note(
                    cancelled
                        ? t('clipboard.typingCancelled')
                        : truncated
                          ? t('clipboard.typedTruncated', { count: MAX_TYPE_CHARS })
                          : t('clipboard.typed'),
                );
            },
        });
        this._typer = typer;
        this._note('');
        if (opts.fromPaste && steps.length > PASTE_TOAST_STEPS && this._opts.toast) {
            this._typerToast = this._opts.toast.info(
                t('clipboard.typingToast', { count: typer.total }),
                {
                    sticky: true,
                    action: { label: t('clipboard.cancel'), onClick: () => this.cancelTyping() },
                },
            );
        }
        typer.start();
        return true;
    }

    cancelTyping() {
        if (this._typer) this._typer.cancel();
    }

    /** The Copy to this device button: the click is writeText's gesture. */
    async copyToDevice() {
        const cb = this._clipboard();
        if (!cb || typeof cb.writeText !== 'function') {
            this._note(t('clipboard.copyUnavailable'));
            return false;
        }
        try {
            await cb.writeText(this._text);
            this._note(t('clipboard.copied'));
            return true;
        } catch (err) {
            console.warn('[ClipboardPanel] writeText refused:', err);
            this._note(t('clipboard.copyFailed'));
            return false;
        }
    }

    /**
     * The stream got the focus back: with the setting on and sync available,
     * read this device's clipboard and set the host's, so the host's own
     * Ctrl+V pastes it. Quiet when the browser has no readText (or asks the
     * user each time and is refused): one notice, then never again this
     * session.
     */
    async onStreamFocus() {
        if (!readClipboardPrefs(this._storage()).syncOnFocus) return false;
        if (!this._syncAvailable || this._isGuest() || this._open) return false;
        if (this._focusSyncDenied || this._focusSyncBusy) return false;
        const cb = this._clipboard();
        if (!cb || typeof cb.readText !== 'function') {
            this._focusSyncDenied = true;
            this._warnFocusSync('clipboard.focusSyncUnavailable');
            return false;
        }
        this._focusSyncBusy = true;
        let text;
        try {
            text = await cb.readText();
        } catch (err) {
            // NotAllowedError: permission denied or the paste prompt dismissed.
            // Asked again on the next focus only if it was something else.
            if (err && err.name === 'NotAllowedError') {
                this._focusSyncDenied = true;
                this._warnFocusSync('clipboard.focusSyncDenied');
            }
            return false;
        } finally {
            this._focusSyncBusy = false;
        }
        if (typeof text !== 'string' || !text) return false;
        const capped = capText(text, MAX_SET_CHARS);
        if (capped === this._lastHostText || capped === this._lastFocusSent) return false;
        this._opts.send({ type: 'clipboardset', text: capped });
        this._lastFocusSent = capped;
        this._lastHostText = capped;
        if (!this._dirty()) {
            this._setText(capped);
            this._baseline = capped;
        }
        return true;
    }

    _warnFocusSync(key) {
        if (this._focusSyncWarned) return;
        this._focusSyncWarned = true;
        console.log('[ClipboardPanel] Clipboard sync on focus is off for this session:', key);
        if (this._opts.toast) this._opts.toast.warning(t(key), { durationMs: 8000 });
    }

    // ── DOM ─────────────────────────────────────────────────────────────

    /** The viewer's own edit is in the box (an empty box protects nothing). */
    _dirty() {
        return this._text !== '' && this._text !== this._baseline;
    }

    _setText(text) {
        this._text = text;
        if (this._el) this._textarea().value = text;
        this.refresh();
    }

    _textarea() {
        return /** @type {HTMLTextAreaElement} */ (this._el.querySelector('textarea'));
    }

    _q(sel) {
        return /** @type {HTMLElement} */ (this._el.querySelector(sel));
    }

    _build() {
        if (this._el) return;
        const id = this._id;
        const el = document.createElement('aside');
        el.className = 'clipboard-panel';
        el.id = id;
        el.hidden = true;
        el.setAttribute('role', 'dialog');
        el.setAttribute('aria-modal', 'true');
        el.setAttribute('aria-labelledby', `${id}-title`);
        el.setAttribute('aria-describedby', `${id}-mode`);
        el.innerHTML = `
            <div class="clipboard-panel-head">
                <h3 id="${id}-title">${escapeHtml(t('clipboard.title'))}</h3>
                <button type="button" class="clipboard-panel-close" data-act="close"
                        aria-label="${escapeHtml(t('clipboard.close'))}">×</button>
            </div>
            <p class="clipboard-panel-mode" id="${id}-mode"></p>
            <p class="clipboard-panel-method" hidden></p>
            <label class="clipboard-panel-label" for="${id}-text">${escapeHtml(t('clipboard.textLabel'))}</label>
            <textarea id="${id}-text" class="clipboard-panel-text" rows="8"
                      spellcheck="false" autocomplete="off" autocapitalize="off"
                      autocorrect="off"></textarea>
            <div class="clipboard-panel-progress" hidden>
                <progress max="1" value="0" aria-label="${escapeHtml(t('clipboard.progressLabel'))}"></progress>
                <span class="clipboard-panel-progress-text"></span>
                <button type="button" class="btn btn-secondary clipboard-panel-cancel" data-act="cancel">${escapeHtml(t('clipboard.cancel'))}</button>
            </div>
            <div class="clipboard-panel-actions">
                <button type="button" class="btn clipboard-panel-send" data-act="send"></button>
                <button type="button" class="btn btn-secondary clipboard-panel-copy" data-act="copy">${escapeHtml(t('clipboard.copy'))}</button>
            </div>
            <p class="clipboard-panel-note" role="status" aria-live="polite"></p>
            <p class="clipboard-panel-hint">${escapeHtml(t('clipboard.hint', { keys: this._opts.isMac ? 'Ctrl+Option+Cmd' : 'Ctrl+Alt+Shift' }))}</p>
        `;
        const area = /** @type {HTMLTextAreaElement} */ (el.querySelector('textarea'));
        area.value = this._text;
        area.addEventListener('input', () => {
            this._text = area.value;
            this.refresh();
        });
        el.addEventListener('click', (e) => {
            const btn = /** @type {Element} */ (e.target).closest('[data-act]');
            if (!btn || /** @type {HTMLButtonElement} */ (btn).disabled) return;
            const act = btn.getAttribute('data-act');
            if (act === 'close') this.close();
            else if (act === 'send') this.send();
            else if (act === 'copy') this.copyToDevice();
            else if (act === 'cancel') this.cancelTyping();
        });
        el.addEventListener('keydown', this._onKeyDown);
        el.addEventListener('keyup', this._onKeyUp);
        // The stream's trackpad and context menu listen on its whole overlay:
        // touches and right-clicks here are the panel's (focus the box, paste).
        for (const type of ['touchstart', 'touchmove', 'touchend', 'contextmenu', 'wheel']) {
            el.addEventListener(type, (e) => e.stopPropagation(), { passive: true });
        }
        this._opts.container.appendChild(el);
        this._el = el;
        this.refresh();
    }

    /** Bring the status line and buttons in line with the mode. */
    refresh() {
        if (!this._el) return;
        const mode = this.mode;
        const send = /** @type {HTMLButtonElement} */ (this._q('[data-act="send"]'));
        const copy = /** @type {HTMLButtonElement} */ (this._q('[data-act="copy"]'));
        const modeEl = this._q('.clipboard-panel-mode');
        modeEl.textContent =
            mode === 'sync'
                ? t('clipboard.modeSync')
                : mode === 'type'
                  ? t('clipboard.modeType')
                  : t('clipboard.modeNone');
        modeEl.dataset.mode = mode;
        const methodEl = this._q('.clipboard-panel-method');
        methodEl.hidden = mode !== 'type';
        if (mode === 'type') {
            const method = this.typeMethod;
            methodEl.dataset.method = method;
            methodEl.textContent = t(
                method === 'keys' ? 'clipboard.methodKeys' : 'clipboard.methodText',
            );
        }
        send.textContent = mode === 'sync' ? t('clipboard.send') : t('clipboard.typeOnHost');
        send.hidden = mode === 'none';
        send.disabled = mode === 'none' || !this._text;
        const cb = this._clipboard();
        copy.disabled = !this._text || !cb || typeof cb.writeText !== 'function';
    }

    _note(text) {
        if (!this._el) return;
        this._q('.clipboard-panel-note').textContent = text;
    }

    _showProgress(done, total) {
        if (!this._el) return;
        const box = this._q('.clipboard-panel-progress');
        box.hidden = false;
        const bar = /** @type {HTMLProgressElement} */ (box.querySelector('progress'));
        bar.max = Math.max(1, total);
        bar.value = done;
        box.querySelector('.clipboard-panel-progress-text').textContent = t(
            'clipboard.typingProgress',
            { done, total },
        );
    }

    _hideProgress() {
        if (!this._el) return;
        this._q('.clipboard-panel-progress').hidden = true;
    }

    _dismissTyperToast() {
        if (this._typerToast) this._typerToast.dismiss();
        this._typerToast = null;
    }

    /** Elements Tab cycles through inside the panel. */
    _focusables() {
        return /** @type {HTMLElement[]} */ (
            [...this._el.querySelectorAll('button, textarea')].filter(
                (n) => !(/** @type {HTMLButtonElement} */ (n).disabled) && !n.closest('[hidden]'),
            )
        );
    }

    _isComboC(e) {
        const mods = this._opts.isMac
            ? e.metaKey && e.altKey && e.ctrlKey
            : e.ctrlKey && e.altKey && e.shiftKey;
        return mods && (e.code === 'KeyC' || (e.key || '').toLowerCase() === 'c');
    }

    _handleKeyDown(e) {
        this._chord.keyDown(e);
        if (e.key === 'Escape' && !e.ctrlKey && !e.altKey && !e.metaKey && !e.shiftKey) {
            e.preventDefault();
            e.stopPropagation();
            this.close();
            return;
        }
        if (this._isComboC(e)) {
            e.preventDefault();
            e.stopPropagation();
            if (!e.repeat) this.close();
            return;
        }
        if (e.key === 'Tab') {
            const list = this._focusables();
            if (list.length === 0) return;
            const first = list[0];
            const last = list[list.length - 1];
            const active = document.activeElement;
            if (e.shiftKey && (active === first || !this._el.contains(active))) {
                e.preventDefault();
                last.focus();
            } else if (!e.shiftKey && (active === last || !this._el.contains(active))) {
                e.preventDefault();
                first.focus();
            }
        }
    }

    _handleKeyUp(e) {
        if (this._chord.keyUp(e)) this.close();
    }
}
