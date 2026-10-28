/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { ChordGesture } from '../js/ui/ClipboardPanel.js';
import { StreamRemoteMenu } from '../js/ui/StreamRemoteMenu.js';

/**
 * How the stream view hands its keyboard to the clipboard panel.
 *
 * Ctrl+Alt+Shift+C and the bare Ctrl+Alt+Shift open it, never a key for the
 * host; while it is open nothing typed reaches the host, and opening it lets
 * go of whatever the host still held. The host's clipboard messages feed it,
 * and the setting "Ctrl+V types my clipboard" turns the native paste into
 * typing on a host without clipboard sync.
 */

const P = StreamView.prototype;

function panelStub(overrides = {}) {
    return {
        isOpen: false,
        close: vi.fn(function () {
            this.isOpen = false;
        }),
        focus: vi.fn(),
        toggle: vi.fn(),
        refresh: vi.fn(),
        setSyncAvailable: vi.fn(),
        hostText: vi.fn(),
        typeText: vi.fn(() => true),
        ctrlVTypes: vi.fn(() => false),
        ...overrides,
    };
}

/** The keyboard path of a StreamView. */
function view(overrides = {}) {
    const sent = [];
    const v = {
        sent,
        webrtc: { send: (m) => sent.push(m) },
        _heldPhysKeys: new Map(),
        _heldMouseButtons: new Set(),
        _metaTapCodes: new Set(),
        _swallowKeyUpCodes: new Set(),
        _appleKeyboard: false,
        _quitting: false,
        _manualQuitting: false,
        _kbdCapture: null,
        _keyField: null,
        _locksSynced: true,
        _cssFullscreen: false,
        _clipboardEnabled: false,
        _layoutMap: null,
        _gamingMode: false,
        _pendingPasteKey: null,
        _suppressPasteKeyUpCode: null,
        _pendingClipboardWrite: null,
        _remoteMenu: null,
        _okHold: null,
        _remoteMenuClosedAt: -1e9,
        _clipboardPanelClosedAt: 0,
        _clipboardChord: new ChordGesture(false),
        _clipboardPanel: panelStub(),
        _handleManualQuit: vi.fn(),
        _toggleClipboardPanel: vi.fn(),
        _syncLockState: () => {},
        handleKeyDown: P.handleKeyDown,
        handleKeyUp: P.handleKeyUp,
        handlePaste: P.handlePaste,
        _sendKeyEvent: P._sendKeyEvent,
        _reconcileModifiers: P._reconcileModifiers,
        _forgetHeldKey: P._forgetHeldKey,
        _holdsThroughStall: P._holdsThroughStall,
        _releaseKeysHeldUnderMeta: P._releaseKeysHeldUnderMeta,
        _releaseHeldModifiers: P._releaseHeldModifiers,
        _releaseHeldMeta: P._releaseHeldMeta,
        _releaseAllPhysKeys: P._releaseAllPhysKeys,
        _sendPendingPasteKey: P._sendPendingPasteKey,
        _handleStatsMessage: P._handleStatsMessage,
        _applyHostClipboard: vi.fn(),
        _onClipboardPanelOpen: P._onClipboardPanelOpen,
        _onClipboardPanelClose: P._onClipboardPanelClose,
        ...overrides,
    };
    return v;
}

function ev(type, code, key, mods = {}) {
    return {
        type,
        target: document.body,
        code,
        key,
        keyCode: 0,
        repeat: false,
        ctrlKey: false,
        shiftKey: false,
        altKey: false,
        metaKey: false,
        defaultPrevented: false,
        preventDefault() {
            this.defaultPrevented = true;
        },
        ...mods,
    };
}
const down = (code, key, mods) => ev('keydown', code, key, mods);
const up = (code, key, mods) => ev('keyup', code, key, mods);
const ALL = { ctrlKey: true, altKey: true, shiftKey: true };

/** Press Ctrl, Alt, Shift; then whatever `middle` does; then let them go. */
function chord(v, middle = () => {}) {
    v.handleKeyDown(down('ControlLeft', 'Control', { ctrlKey: true }));
    v.handleKeyDown(down('AltLeft', 'Alt', { ctrlKey: true, altKey: true }));
    v.handleKeyDown(down('ShiftLeft', 'Shift', ALL));
    middle();
    v.handleKeyUp(up('ShiftLeft', 'Shift', { ctrlKey: true, altKey: true }));
    v.handleKeyUp(up('ControlLeft', 'Control', { altKey: true }));
    v.handleKeyUp(up('AltLeft', 'Alt'));
}

const flush = () => new Promise((r) => queueMicrotask(r));

describe('opening the clipboard panel from the stream', () => {
    it('Ctrl+Alt+Shift+C toggles it, and the C never reaches the host', () => {
        const v = view();
        const e = down('KeyC', 'C', ALL);
        v.handleKeyDown(e);
        expect(v._toggleClipboardPanel).toHaveBeenCalledTimes(1);
        expect(e.defaultPrevented).toBe(true);
        expect(v.sent.some((m) => m.keyCode === 0x43)).toBe(false);
    });

    it('a held C toggles once, not once per auto-repeat', () => {
        const v = view();
        v.handleKeyDown(down('KeyC', 'C', ALL));
        v.handleKeyDown(down('KeyC', 'C', { ...ALL, repeat: true }));
        expect(v._toggleClipboardPanel).toHaveBeenCalledTimes(1);
    });

    it('Cmd+Option+Ctrl+C on a Mac, through the layout label like the other combos', () => {
        const platform = vi.spyOn(navigator, 'platform', 'get').mockReturnValue('MacIntel');
        const v = view({ _layoutMap: new Map([['KeyC', 'c']]) });
        // Option mangles e.key (Option+C → ç): the layout map still says C.
        v.handleKeyDown(down('KeyC', 'ç', { metaKey: true, altKey: true, ctrlKey: true }));
        platform.mockRestore();
        expect(v._toggleClipboardPanel).toHaveBeenCalledTimes(1);
    });

    it('the bare Ctrl+Alt+Shift opens it once the last key is up', async () => {
        const v = view();
        chord(v);
        expect(v._toggleClipboardPanel).not.toHaveBeenCalled();
        await flush();
        expect(v._toggleClipboardPanel).toHaveBeenCalledTimes(1);
    });

    it('Ctrl+Alt+Shift+Q quits and does not open it', async () => {
        const v = view();
        chord(v, () => {
            v.handleKeyDown(down('KeyQ', 'Q', ALL));
            v.handleKeyUp(up('KeyQ', 'Q', ALL));
        });
        await flush();
        expect(v._handleManualQuit).toHaveBeenCalledTimes(1);
        expect(v._toggleClipboardPanel).not.toHaveBeenCalled();
    });

    it('Ctrl+Alt+Shift+C does not open it a second time on its release', async () => {
        const v = view();
        chord(v, () => {
            v.handleKeyDown(down('KeyC', 'C', ALL));
            v.handleKeyUp(up('KeyC', 'C', ALL));
        });
        await flush();
        expect(v._toggleClipboardPanel).toHaveBeenCalledTimes(1);
    });

    it('a dialog typing Ctrl+Alt+Shift is none of the stream’s business', async () => {
        const v = view();
        const input = document.createElement('input');
        const at = (e) => ({ ...e, target: input });
        v.handleKeyDown(at(down('ControlLeft', 'Control', { ctrlKey: true })));
        v.handleKeyDown(at(down('AltLeft', 'Alt', { ctrlKey: true, altKey: true })));
        v.handleKeyDown(at(down('ShiftLeft', 'Shift', ALL)));
        v.handleKeyUp(at(up('ShiftLeft', 'Shift', { ctrlKey: true, altKey: true })));
        v.handleKeyUp(at(up('ControlLeft', 'Control', { altKey: true })));
        v.handleKeyUp(at(up('AltLeft', 'Alt')));
        await flush();
        expect(v._toggleClipboardPanel).not.toHaveBeenCalled();
    });

    it('a focus loss mid-chord forgets it', async () => {
        const v = view();
        v.handleKeyDown(down('ControlLeft', 'Control', { ctrlKey: true }));
        v.handleKeyDown(down('AltLeft', 'Alt', { ctrlKey: true, altKey: true }));
        v.handleKeyDown(down('ShiftLeft', 'Shift', ALL));
        v._releaseAllPhysKeys(); // window blur
        v.handleKeyUp(up('AltLeft', 'Alt'));
        await flush();
        expect(v._toggleClipboardPanel).not.toHaveBeenCalled();
    });
});

describe('while the panel is open', () => {
    it('its own keys are local keyboard targets: the stream leaves them alone', () => {
        const panel = document.createElement('aside');
        panel.className = 'clipboard-panel';
        const btn = document.createElement('button');
        panel.appendChild(btn);
        document.body.appendChild(panel);
        expect(StreamView.isLocalKeyboardTarget(btn)).toBe(true);
        expect(StreamView.isLocalKeyboardTarget(panel)).toBe(true);
        const v = view({ _clipboardPanel: panelStub({ isOpen: true }) });
        const e = { ...down('KeyA', 'a'), target: btn };
        v.handleKeyDown(e);
        expect(v.sent).toEqual([]);
        panel.remove();
    });

    it('a key typed elsewhere (focus on the picture) is not sent; the panel takes the focus back', () => {
        const v = view({ _clipboardPanel: panelStub({ isOpen: true }) });
        v.handleKeyDown(down('KeyA', 'a'));
        v.handleKeyUp(up('KeyA', 'a'));
        expect(v.sent).toEqual([]);
        expect(v._clipboardPanel.focus).toHaveBeenCalled();
    });

    it('Escape typed elsewhere closes it, and is not sent either', () => {
        const v = view({ _clipboardPanel: panelStub({ isOpen: true }) });
        const e = down('Escape', 'Escape');
        v.handleKeyDown(e);
        expect(v._clipboardPanel.close).toHaveBeenCalled();
        expect(e.defaultPrevented).toBe(true);
        expect(v.sent).toEqual([]);
    });

    it('opening it lets go of what the host held, and frees the pointer', () => {
        const v = view({
            _remotePointer: { releaseAll: vi.fn() },
            _kbdVisible: false,
            _clipboardBtn: document.createElement('button'),
        });
        // Ctrl and Alt went down on the host before the C opened the panel.
        v.handleKeyDown(down('ControlLeft', 'Control', { ctrlKey: true }));
        v.handleKeyDown(down('AltLeft', 'Alt', { ctrlKey: true, altKey: true }));
        v.sent.length = 0;
        const exit = vi.fn();
        Object.defineProperty(document, 'pointerLockElement', {
            configurable: true,
            get: () => document.body,
        });
        document.exitPointerLock = exit;
        v._onClipboardPanelOpen();
        delete document.pointerLockElement;
        expect(v.sent.map((m) => [m.type, m.code])).toEqual([
            ['keyup', 'ControlLeft'],
            ['keyup', 'AltLeft'],
        ]);
        expect(v.sent.every((m) => !m.ctrlKey && !m.altKey)).toBe(true);
        expect(v._heldPhysKeys.size).toBe(0);
        expect(v._remotePointer.releaseAll).toHaveBeenCalled();
        expect(exit).toHaveBeenCalled();
        expect(v._clipboardBtn.classList.contains('active')).toBe(true);
    });

    it('the keys that closed it do not land on the host as unmatched releases', () => {
        const v = view({ _clipboardBtn: document.createElement('button') });
        v._onClipboardPanelClose();
        expect(v._clipboardPanelClosedAt).toBeGreaterThan(0);
        v.handleKeyUp(up('Escape', 'Escape'));
        v.handleKeyUp(up('KeyC', 'C'));
        expect(v.sent).toEqual([]);
        // A key the host does hold still comes up.
        v._heldPhysKeys.set('KeyW', { type: 'keyup', keyCode: 0x57, code: 'KeyW' });
        v.handleKeyUp(up('KeyW', 'w'));
        expect(v.sent.map((m) => m.code)).toEqual(['KeyW']);
    });
});

describe('the host clipboard messages', () => {
    it('clipboardcaps switches the panel’s mode', () => {
        const v = view();
        v._handleStatsMessage({ type: 'clipboardcaps', available: true });
        expect(v._clipboardEnabled).toBe(true);
        expect(v._clipboardPanel.setSyncAvailable).toHaveBeenCalledWith(true);
        v._handleStatsMessage({ type: 'clipboardcaps', available: false });
        expect(v._clipboardPanel.setSyncAvailable).toHaveBeenLastCalledWith(false);
    });

    it('clipboard reaches the panel as well as the local clipboard', () => {
        const v = view();
        v._handleStatsMessage({ type: 'clipboard', text: 'hi' });
        expect(v._applyHostClipboard).toHaveBeenCalledWith('hi');
        expect(v._clipboardPanel.hostText).toHaveBeenCalledWith('hi');
    });

    it('works without a panel (a view torn down)', () => {
        const v = view({ _clipboardPanel: null });
        v._handleStatsMessage({ type: 'clipboardcaps', available: true });
        v._handleStatsMessage({ type: 'clipboard', text: 'hi' });
        expect(v._applyHostClipboard).toHaveBeenCalledWith('hi');
    });
});

describe('Ctrl+V types my clipboard', () => {
    const paste = (text) => ({
        target: document.body,
        preventDefault: vi.fn(),
        clipboardData: { getData: () => text },
    });

    it('off (the default), Ctrl+V reaches a host without sync as keys, as before', () => {
        const v = view();
        v.handleKeyDown(down('ControlLeft', 'Control', { ctrlKey: true }));
        v.handleKeyDown(down('KeyV', 'v', { ctrlKey: true }));
        expect(v._pendingPasteKey).toBeNull();
        expect(v.sent.map((m) => [m.type, m.code])).toEqual([
            ['keydown', 'ControlLeft'],
            ['keydown', 'KeyV'],
        ]);
    });

    it('on, the native paste is typed on the host, with Ctrl let go first', () => {
        vi.useFakeTimers();
        const v = view({ _clipboardPanel: panelStub({ ctrlVTypes: vi.fn(() => true) }) });
        v.handleKeyDown(down('ControlLeft', 'Control', { ctrlKey: true }));
        v.handleKeyDown(down('KeyV', 'v', { ctrlKey: true }));
        expect(v._pendingPasteKey).not.toBeNull();
        expect(v._pendingPasteKey.typing).toBe(true);
        v.sent.length = 0;
        v.handlePaste(paste('typed text'));
        expect(v._clipboardPanel.typeText).toHaveBeenCalledWith('typed text', { fromPaste: true });
        expect(v.sent).toEqual([expect.objectContaining({ type: 'keyup', code: 'ControlLeft' })]);
        expect(v.sent.some((m) => m.type === 'clipboardpaste')).toBe(false);
        // The V and Ctrl releases that follow are swallowed.
        v.handleKeyUp(up('KeyV', 'v', { ctrlKey: true }));
        v.handleKeyUp(up('ControlLeft', 'Control'));
        expect(v.sent).toHaveLength(1);
        vi.advanceTimersByTime(500);
        expect(v.sent).toHaveLength(1);
        vi.useRealTimers();
    });

    it('on, an empty local clipboard falls back to the host’s own Ctrl+V', () => {
        vi.useFakeTimers();
        const v = view({ _clipboardPanel: panelStub({ ctrlVTypes: vi.fn(() => true) }) });
        v.handleKeyDown(down('ControlLeft', 'Control', { ctrlKey: true }));
        v.handleKeyDown(down('KeyV', 'v', { ctrlKey: true }));
        vi.advanceTimersByTime(200); // no paste event came
        expect(v.sent.map((m) => [m.type, m.code])).toEqual([
            ['keydown', 'ControlLeft'],
            ['keydown', 'KeyV'],
        ]);
        expect(v._clipboardPanel.typeText).not.toHaveBeenCalled();
        vi.useRealTimers();
    });

    it('on a Mac, an empty clipboard sends Cmd+V as the tap it always was', () => {
        vi.useFakeTimers();
        const v = view({
            _appleKeyboard: true,
            _clipboardPanel: panelStub({ ctrlVTypes: vi.fn(() => true) }),
        });
        v.handleKeyDown(down('MetaLeft', 'Meta', { metaKey: true }));
        v.handleKeyDown(down('KeyV', 'v', { metaKey: true }));
        vi.advanceTimersByTime(200);
        expect(v.sent.map((m) => [m.type, m.code])).toEqual([
            ['keydown', 'MetaLeft'],
            ['keydown', 'KeyV'],
            ['keyup', 'KeyV'],
        ]);
        expect(v.sent.some((m) => m.type === 'clipboardpaste')).toBe(false);
        vi.useRealTimers();
    });

    it('with sync available, Ctrl+V keeps the shared-clipboard paste', () => {
        const v = view({
            _clipboardEnabled: true,
            _clipboardPanel: panelStub({ ctrlVTypes: vi.fn(() => false) }),
        });
        v.handleKeyDown(down('KeyV', 'v', { ctrlKey: true }));
        v.handlePaste(paste('shared'));
        expect(v.sent).toEqual([{ type: 'clipboardpaste', text: 'shared', injectCtrl: false }]);
        expect(v._clipboardPanel.typeText).not.toHaveBeenCalled();
        clearTimeout(v._pendingPasteKey?.timer);
    });
});

describe("the TV remote's menu", () => {
    it('has a Clipboard entry when the stream gives it one', () => {
        const onClipboard = vi.fn();
        const menu = new StreamRemoteMenu({
            container: document.body,
            statsOn: () => false,
            onResume: vi.fn(),
            onStats: vi.fn(),
            onStop: vi.fn(),
            onClipboard,
        });
        menu.open();
        const btn = document.querySelector('[data-act="clipboard"]');
        expect(btn).not.toBeNull();
        expect(btn.textContent).toBe('clipboard.title');
        btn.click();
        expect(onClipboard).toHaveBeenCalledTimes(1);
        menu.close();
    });

    it('has none without it', () => {
        const menu = new StreamRemoteMenu({
            container: document.body,
            statsOn: () => false,
            onResume: vi.fn(),
            onStats: vi.fn(),
            onStop: vi.fn(),
        });
        menu.open();
        expect(document.querySelector('[data-act="clipboard"]')).toBeNull();
        menu.close();
    });

    it('the stream wires it: the menu closes and the panel opens', () => {
        const v = view({
            _rootEl: document.body,
            _padHolds: new Set(),
            _gamepadManager: null,
            _remotePointer: null,
            _showPerfStats: false,
            _statsClosed: false,
            _remoteMenuAcceptsClick: () => true,
            _openRemoteMenu: P._openRemoteMenu,
            _closeRemoteMenu: P._closeRemoteMenu,
            _holdPads: P._holdPads,
            _releasePads: P._releasePads,
        });
        v._openRemoteMenu();
        document.querySelector('[data-act="clipboard"]').click();
        expect(v._remoteMenu.isOpen).toBe(false);
        expect(v._toggleClipboardPanel).toHaveBeenCalledTimes(1);
    });
});
