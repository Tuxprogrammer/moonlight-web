/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The clipboard agents' credentials (ClipboardAgentRegistry): one token per
 * host, random, shown once and kept only as a hash; rotating or revoking it
 * drops the agent that used it. Part of both runners — the host clipboard is
 * an exfiltration channel, and this is what keeps strangers off it.
 */

#include "test_framework.h"
#include "server/ClipboardAgentRegistry.h"

#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

void run_clipboard_agent_token_tests()
{
    SECTION("ClipboardAgentRegistry — tokens are random, prefixed and stored hashed");
    {
        const QString a = ClipboardAgentRegistry::generateToken();
        const QString b = ClipboardAgentRegistry::generateToken();
        CHECK(a != b);
        CHECK(a.startsWith("mwca_"));
        CHECK_EQ(a.size(), 5 + 43);
        CHECK(QRegularExpression("^mwca_[A-Za-z0-9_-]{43}$").match(a).hasMatch());
        CHECK_EQ(ClipboardAgentRegistry::hashToken(a), ClipboardAgentRegistry::hashToken(a));
        CHECK(ClipboardAgentRegistry::hashToken(a) != ClipboardAgentRegistry::hashToken(b));
        CHECK(!ClipboardAgentRegistry::hashToken(a).contains(a.mid(5)));

        QTemporaryDir dir;
        const QString path = dir.filePath("clipboard-agents.json");
        ClipboardAgentRegistry reg(path);
        CHECK(!reg.hasToken("H1"));
        const QString token = reg.issueToken("H1", 1000);
        CHECK(reg.hasToken("H1"));
        CHECK_EQ(reg.hostForToken(token), QString("H1"));
        CHECK(reg.hostForToken(token + "x").isEmpty());
        CHECK(reg.hostForToken("").isEmpty());
        CHECK(reg.hostForToken(ClipboardAgentRegistry::hashToken(token)).isEmpty());
        CHECK(reg.issueToken("", 1).isEmpty());

        QFile f(path);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray onDisk = f.readAll();
        CHECK(!onDisk.contains(token.toLatin1()));
        CHECK(onDisk.contains(ClipboardAgentRegistry::hashToken(token).toLatin1()));
#ifdef Q_OS_UNIX
        CHECK_EQ(f.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
                 QFileDevice::Permissions());
#endif
    }

    SECTION("ClipboardAgentRegistry — rotation and revocation drop the old credential");
    {
        ClipboardAgentRegistry reg{QString()};
        int revokedCount = 0;
        QObject::connect(&reg, &ClipboardAgentRegistry::credentialsRevoked,
                         [&](const QString&) { ++revokedCount; });
        const QString t1 = reg.issueToken("H1", 1);
        CHECK_EQ(revokedCount, 0); // nothing to drop the first time
        const QString t2 = reg.issueToken("H1", 2);
        CHECK(t1 != t2);
        CHECK_EQ(revokedCount, 1);
        CHECK(reg.hostForToken(t1).isEmpty());
        CHECK_EQ(reg.hostForToken(t2), QString("H1"));
        CHECK_EQ(reg.statusJson("H1").value("token_created").toDouble(), 2.0);

        // Each host has its own.
        const QString other = reg.issueToken("H2", 3);
        CHECK_EQ(reg.hostForToken(other), QString("H2"));
        CHECK_EQ(reg.hostForToken(t2), QString("H1"));

        CHECK(reg.revokeToken("H1"));
        CHECK_EQ(revokedCount, 2);
        CHECK(!reg.hasToken("H1"));
        CHECK(reg.hostForToken(t2).isEmpty());
        CHECK(!reg.revokeToken("H1")); // already gone
        CHECK(!reg.revokeToken("unknown"));
        CHECK_EQ(revokedCount, 2);
        CHECK_EQ(reg.hostForToken(other), QString("H2"));

        reg.forgetHost("H2");
        CHECK(reg.hostForToken(other).isEmpty());
        CHECK_EQ(revokedCount, 3);
        reg.forgetHost("H2"); // twice is harmless
        CHECK_EQ(revokedCount, 3);
    }

    SECTION("ClipboardAgentRegistry — a token opens its own host and nothing else");
    {
        ClipboardAgentRegistry reg{QString()};
        QStringList tokens;
        for (int i = 0; i < 20; ++i)
            tokens << reg.issueToken(QStringLiteral("host-%1").arg(i), i);
        for (int i = 0; i < 20; ++i)
            CHECK_EQ(reg.hostForToken(tokens[i]), QStringLiteral("host-%1").arg(i));
        // Near misses: a character changed, a prefix, the bare secret.
        QString changed = tokens[3];
        changed[10] = changed[10] == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
        CHECK(reg.hostForToken(changed).isEmpty());
        CHECK(reg.hostForToken(tokens[3].left(20)).isEmpty());
        CHECK(reg.hostForToken(tokens[3].mid(5)).isEmpty());
        CHECK(reg.hostForToken(tokens[3] + "\n").isEmpty());
        // A host whose token was revoked matches nothing, not even empty.
        reg.revokeToken("host-3");
        CHECK(reg.hostForToken(tokens[3]).isEmpty());
        CHECK(reg.hostForToken(QString()).isEmpty());
        // Enabling a host does not need, or make, a token.
        reg.setEnabled("fresh", true);
        CHECK(!reg.hasToken("fresh"));
        CHECK(reg.hostForToken("").isEmpty());
    }
}
