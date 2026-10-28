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

#pragma once

#include "ClipboardMessage.h"

#include <QObject>
#include <QString>

class IMediaEngine;

/// Where a relay's host clipboard lives, so the relays don't care which:
///  - ClipboardBridge: this machine's QClipboard (the host is this machine);
///  - RemoteClipboardProvider: a clipboard agent on the host, reached through
///    the server (docs/design/clipboard-agent.md).
///
/// A provider lives on the Qt main thread. Every method below is thread-safe:
/// relays call in from their own threads and the work hops to the main thread.
/// Relays connect to the signals with themselves as context, so the slots run
/// on the relay's thread.
class ClipboardProvider : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    /// Whether host clipboard sync works right now. Always true for the local
    /// bridge; an agent comes and goes (availabilityChanged).
    virtual bool available() const = 0;

    /// Put @p text on the host clipboard, then inject the paste chord — the
    /// chord never overtakes the write. An empty text only injects the chord.
    virtual void pasteFromClient(IMediaEngine* engine, const QString& text, bool wrapCtrl) = 0;

    /// Put @p text on the host clipboard and press nothing.
    virtual void setFromClient(const QString& text) = 0;

    /// Emit hostTextChanged with the current host clipboard once, even if
    /// unchanged (a browser just connected).
    virtual void requestAnnounce() = 0;

    /// Relay-thread entry for a decoded clipboard message (ClipboardMsg):
    /// paste, set, or a debug line when it is ignored. @p provider is the
    /// relay's, and is not touched unless there is something to do.
    static void dispatch(ClipboardProvider* provider, IMediaEngine* engine,
                         const ClipboardMsg::Request& req, const char* logTag);

    /// The paste chord (V down/up, wrapped in Ctrl when @p wrapCtrl), through
    /// the stream's input path. Li input calls are thread-safe.
    static void injectPasteChord(IMediaEngine* engine, bool wrapCtrl);

signals:
    /// The host clipboard changed (or an announce was asked for).
    void hostTextChanged(const QString& text);
    /// available() changed.
    void availabilityChanged(bool available);
};
