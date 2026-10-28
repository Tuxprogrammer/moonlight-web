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

// The browser's clipboard input messages, read once for the three relays.
//
//  - 'clipboardpaste' { text, injectCtrl }: Ctrl/Cmd+V in the stream. The text
//    goes to the host clipboard and the paste chord is injected after it.
//  - 'clipboardset' { text }: the clipboard panel's Send, or the stream getting
//    the focus back. The text goes to the host clipboard and nothing is
//    pressed: the person pastes on the host when they choose to.
//
// Both act only while the relay's clipboard switch is on — the streamed host
// is this machine and the session is unrestricted (Session.cpp). Otherwise
// they are dropped. Whether the sender may touch the host at all is decided
// before this, by InputMsg::allowed(), which files both under keyboard/mouse.
//
// Pure and dependency-free (no QClipboard), so the decision is unit-tested.

#include <QJsonObject>
#include <QString>

namespace ClipboardMsg {

/// Max text size relayed in either direction (UTF-16 code units). Larger
/// payloads are truncated: clipboard sync is for text, not file transfer,
/// and input-channel messages must stay well under SCTP message limits.
constexpr int kMaxTextChars = 256 * 1024;

enum class Action
{
    None,   ///< Not a clipboard message.
    Ignore, ///< A clipboard message, dropped: the clipboard switch is off.
    Paste,  ///< Commit the text, then inject the paste chord.
    Set,    ///< Commit the text, inject nothing.
};

struct Request
{
    Action action = Action::None;
    /// Capped to kMaxTextChars (never splitting a surrogate pair).
    QString text;
    /// Paste only: wrap the injected V in Ctrl (a Mac's Cmd reaches the host
    /// as the Windows key).
    bool wrapCtrl = false;
    /// The text arrived longer than kMaxTextChars and was cut.
    bool truncated = false;
};

/// Whether @p type is one of the messages this header reads.
inline bool isClipboardType(const QString& type)
{
    return type == QLatin1String("clipboardpaste") || type == QLatin1String("clipboardset");
}

/// The first @p maxChars UTF-16 units of @p text, one fewer when the cut would
/// leave half of a surrogate pair at the end (an emoji, say): the host
/// clipboard would otherwise hold an invalid string.
inline QString capText(const QString& text, int maxChars = kMaxTextChars)
{
    if (maxChars <= 0) return QString();
    if (text.size() <= maxChars) return text;
    int n = maxChars;
    if (text.at(n - 1).isHighSurrogate()) --n;
    return text.left(n);
}

/// What a relay does with a message of @p type. @p bridgeActive is the relay's
/// clipboard switch.
inline Request decode(const QString& type, const QJsonObject& msg, bool bridgeActive)
{
    Request r;
    if (!isClipboardType(type)) return r;
    if (!bridgeActive) {
        r.action = Action::Ignore;
        return r;
    }
    const QString text = msg.value(QLatin1String("text")).toString();
    r.text = capText(text);
    r.truncated = r.text.size() < text.size();
    if (type == QLatin1String("clipboardpaste")) {
        // An empty paste still replays the chord: the host pastes its own
        // clipboard (StreamView._sendPendingPasteKey relies on it).
        r.action = Action::Paste;
        r.wrapCtrl = msg.value(QLatin1String("injectCtrl")).toBool(false);
    } else {
        // Nothing to set: an empty clipboard is not something to push.
        r.action = r.text.isEmpty() ? Action::Ignore : Action::Set;
    }
    return r;
}

} // namespace ClipboardMsg
