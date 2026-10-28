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

#include "ClipboardProvider.h"
#include "IMediaEngine.h"

#include <QtDebug>

namespace {
// Windows virtual-key codes for the injected paste chord (the Moonlight
// protocol speaks VK regardless of the host OS — Sunshine translates).
constexpr short kVkControl = 0x11;
constexpr short kVkV = 0x56;
// Modifier bitmask matching the relays' keydown encoding (Ctrl = 0x02).
constexpr char kModCtrl = 0x02;
} // namespace

void ClipboardProvider::dispatch(ClipboardProvider* provider, IMediaEngine* engine,
                                 const ClipboardMsg::Request& req, const char* logTag)
{
    switch (req.action) {
    case ClipboardMsg::Action::None: return;
    case ClipboardMsg::Action::Ignore:
        qDebug() << logTag << "Clipboard message ignored (no host clipboard sync here)";
        return;
    case ClipboardMsg::Action::Paste:
        if (!provider) return;
        if (req.truncated) qInfo() << logTag << "Clipboard paste truncated to" << req.text.size();
        provider->pasteFromClient(engine, req.text, req.wrapCtrl);
        return;
    case ClipboardMsg::Action::Set:
        if (!provider) return;
        if (req.truncated) qInfo() << logTag << "Clipboard set truncated to" << req.text.size();
        provider->setFromClient(req.text);
        return;
    }
}

void ClipboardProvider::injectPasteChord(IMediaEngine* engine, bool wrapCtrl)
{
    if (!engine) return;
    if (wrapCtrl) engine->sendKeyEvent(kVkControl, true, kModCtrl, 0);
    engine->sendKeyEvent(kVkV, true, kModCtrl, 0);
    engine->sendKeyEvent(kVkV, false, kModCtrl, 0);
    if (wrapCtrl) engine->sendKeyEvent(kVkControl, false, 0, 0);
}
