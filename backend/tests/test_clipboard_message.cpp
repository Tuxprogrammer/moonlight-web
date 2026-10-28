/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The browser's clipboard messages (ClipboardMessage.h), as all three relays
 * read them.
 *
 * 'clipboardpaste' commits text and injects the paste chord; 'clipboardset'
 * commits text and presses nothing. Neither does anything unless the relay's
 * clipboard switch is on (the host is this machine and the session is
 * unrestricted), and neither gets past InputMsg::allowed() for a guest who may
 * not use the keyboard.
 */

#include "test_framework.h"
#include "streaming/ClipboardMessage.h"
#include "streaming/InputPolicy.h"

#include <QJsonObject>

using ClipboardMsg::Action;

namespace {

QJsonObject message(const char* type, const QString& text, bool injectCtrl = false)
{
    QJsonObject m;
    m["type"] = type;
    m["text"] = text;
    if (injectCtrl) m["injectCtrl"] = true;
    return m;
}

} // namespace

void run_clipboard_message_tests()
{
    SECTION("ClipboardMsg — which types it reads");
    {
        CHECK(ClipboardMsg::isClipboardType("clipboardpaste"));
        CHECK(ClipboardMsg::isClipboardType("clipboardset"));
        CHECK(!ClipboardMsg::isClipboardType("clipboard"));
        CHECK(!ClipboardMsg::isClipboardType("clipboardcaps"));
        CHECK(!ClipboardMsg::isClipboardType("textinput"));
        CHECK(!ClipboardMsg::isClipboardType(""));
        // Another type is none of its business, whatever the switch says.
        CHECK(ClipboardMsg::decode("textinput", message("textinput", "a"), true).action ==
              Action::None);
        CHECK(ClipboardMsg::decode("keydown", QJsonObject(), false).action == Action::None);
    }

    SECTION("ClipboardMsg — clipboardset commits the text and presses nothing");
    {
        const auto r = ClipboardMsg::decode("clipboardset", message("clipboardset", "hello"), true);
        CHECK(r.action == Action::Set);
        CHECK_EQ(r.text, QString("hello"));
        CHECK(!r.wrapCtrl);
        CHECK(!r.truncated);
        // injectCtrl means nothing to a set: there is no chord to wrap.
        const auto w =
            ClipboardMsg::decode("clipboardset", message("clipboardset", "x", true), true);
        CHECK(w.action == Action::Set);
        CHECK(!w.wrapCtrl);
    }

    SECTION("ClipboardMsg — clipboardpaste keeps its chord, even with no text");
    {
        const auto r =
            ClipboardMsg::decode("clipboardpaste", message("clipboardpaste", "abc", true), true);
        CHECK(r.action == Action::Paste);
        CHECK_EQ(r.text, QString("abc"));
        CHECK(r.wrapCtrl);
        // Empty: the browser asks for the chord alone (the host pastes its own).
        const auto e = ClipboardMsg::decode("clipboardpaste", message("clipboardpaste", ""), true);
        CHECK(e.action == Action::Paste);
        CHECK(e.text.isEmpty());
        CHECK(!e.wrapCtrl);
    }

    SECTION("ClipboardMsg — an empty or missing clipboardset is dropped");
    {
        CHECK(ClipboardMsg::decode("clipboardset", message("clipboardset", ""), true).action ==
              Action::Ignore);
        QJsonObject noText;
        noText["type"] = "clipboardset";
        CHECK(ClipboardMsg::decode("clipboardset", noText, true).action == Action::Ignore);
        // A text that is not a string reads as none.
        QJsonObject number;
        number["type"] = "clipboardset";
        number["text"] = 42;
        CHECK(ClipboardMsg::decode("clipboardset", number, true).action == Action::Ignore);
    }

    SECTION("ClipboardMsg — ignored while the clipboard switch is off");
    {
        // A remote host, or a restricted session: both types are dropped, and
        // the text is not even kept.
        const auto s =
            ClipboardMsg::decode("clipboardset", message("clipboardset", "secret"), false);
        CHECK(s.action == Action::Ignore);
        CHECK(s.text.isEmpty());
        const auto p =
            ClipboardMsg::decode("clipboardpaste", message("clipboardpaste", "secret"), false);
        CHECK(p.action == Action::Ignore);
        CHECK(p.text.isEmpty());
    }

    SECTION("ClipboardMsg — the size cap");
    {
        const int cap = ClipboardMsg::kMaxTextChars;
        CHECK_EQ(cap, 256 * 1024);
        const QString exact(cap, QChar('a'));
        const auto atCap =
            ClipboardMsg::decode("clipboardset", message("clipboardset", exact), true);
        CHECK_EQ(atCap.text.size(), cap);
        CHECK(!atCap.truncated);

        const QString over(cap + 10, QChar('b'));
        const auto cut = ClipboardMsg::decode("clipboardset", message("clipboardset", over), true);
        CHECK(cut.action == Action::Set);
        CHECK_EQ(cut.text.size(), cap);
        CHECK(cut.truncated);
        const auto pasted =
            ClipboardMsg::decode("clipboardpaste", message("clipboardpaste", over), true);
        CHECK_EQ(pasted.text.size(), cap);
        CHECK(pasted.truncated);
    }

    SECTION("ClipboardMsg — the cap never splits a surrogate pair");
    {
        // "a😀b": the emoji is two UTF-16 units.
        const QString text = QString("a") + QString::fromUtf8("\xF0\x9F\x98\x80") + "b";
        CHECK_EQ(text.size(), 4);
        // Cut after its high half: that half goes too.
        CHECK_EQ(ClipboardMsg::capText(text, 2), QString("a"));
        // Cut after the whole pair: kept.
        CHECK_EQ(ClipboardMsg::capText(text, 3), text.left(3));
        CHECK_EQ(ClipboardMsg::capText(text, 4), text);
        CHECK_EQ(ClipboardMsg::capText(text, 100), text);
        CHECK(ClipboardMsg::capText(text, 0).isEmpty());
        CHECK(ClipboardMsg::capText(text, -1).isEmpty());
        CHECK(ClipboardMsg::capText(QString(), 5).isEmpty());

        // At the real cap too.
        QString big(ClipboardMsg::kMaxTextChars - 1, QChar('x'));
        big += QString::fromUtf8("\xF0\x9F\x98\x80");
        const auto r = ClipboardMsg::decode("clipboardset", message("clipboardset", big), true);
        CHECK_EQ(r.text.size(), ClipboardMsg::kMaxTextChars - 1);
        CHECK(!r.text.back().isHighSurrogate());
        CHECK(r.truncated);
    }

    SECTION("ClipboardMsg — a guest's permissions gate both types like the keyboard");
    {
        InputMsg::Policy owner;
        CHECK(InputMsg::allowed("clipboardset", owner));
        CHECK(InputMsg::allowed("clipboardpaste", owner));

        InputMsg::Policy viewOnly;
        viewOnly.gamepad = false;
        viewOnly.keyboardMouse = false;
        CHECK(!InputMsg::allowed("clipboardset", viewOnly));
        CHECK(!InputMsg::allowed("clipboardpaste", viewOnly));

        InputMsg::Policy padOnly;
        padOnly.keyboardMouse = false;
        CHECK(!InputMsg::allowed("clipboardset", padOnly));
        CHECK(!InputMsg::allowed("clipboardpaste", padOnly));

        // Keyboard without gamepad: may type, so may set — but the session is
        // restricted, so the relay's switch is off (Session.cpp) and the
        // message is dropped there.
        InputMsg::Policy keysOnly;
        keysOnly.gamepad = false;
        CHECK(InputMsg::allowed("clipboardset", keysOnly));
        CHECK(!keysOnly.unrestricted());
        const bool bridgeActive = /* hostIsSelf */ true && keysOnly.unrestricted();
        CHECK(ClipboardMsg::decode("clipboardset", message("clipboardset", "x"), bridgeActive)
                  .action == Action::Ignore);
    }
}
