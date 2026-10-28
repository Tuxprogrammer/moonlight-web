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

// What the admin page shows once a clipboard agent token is issued: the
// server address the agent should dial, the key pins, and command lines ready
// to paste on the host. Pure, so the quoting is tested.

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace ClipboardAgentSetup {

/// Where the agent's token file lives by default (the agent reads it there
/// when --token-file is not given).
inline QString defaultTokenFile()
{
    return QStringLiteral("~/.config/moonlightweb/clipboard-agent.token");
}

/// The https:// origin an agent on the host should dial. The admin reached
/// us under @p hostHeader, which the host can most likely reach too — unless
/// it names this machine itself (loopback), in which case the first of
/// @p lanAddresses stands in. Always the HTTPS port: the token must not
/// travel in clear.
inline QString serverUrl(const QString& hostHeader, quint16 httpsPort,
                         const QList<QHostAddress>& lanAddresses)
{
    QString host = hostHeader.trimmed();
    // Drop the port: [v6]:port, v4:port or name:port.
    if (host.startsWith(QLatin1Char('['))) {
        const int end = host.indexOf(QLatin1Char(']'));
        host = end > 0 ? host.mid(1, end - 1) : QString();
    } else if (host.count(QLatin1Char(':')) == 1) {
        host = host.section(QLatin1Char(':'), 0, 0);
    }
    const QHostAddress literal(host);
    const bool loopback = host.isEmpty() ||
                          host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 ||
                          (!literal.isNull() && literal.isLoopback());
    if (loopback) {
        host.clear();
        for (const QHostAddress& a : lanAddresses) {
            if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback()) {
                host = a.toString();
                break;
            }
        }
        if (host.isEmpty()) host = QStringLiteral("localhost");
    }
    const QHostAddress parsed(host);
    if (!parsed.isNull() && parsed.protocol() == QAbstractSocket::IPv6Protocol)
        host = QLatin1Char('[') + host + QLatin1Char(']');
    return QStringLiteral("https://%1:%2").arg(host).arg(httpsPort);
}

/// @p value in POSIX single quotes (an embedded ' becomes '\'').
inline QString shellQuote(const QString& value)
{
    QString out = value;
    out.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + out + QLatin1Char('\'');
}

/// The issue/rotate answer: the token (shown once), the server URL, the pins,
/// the agent's arguments, and two POSIX lines — store the token owner-only,
/// then run the agent. The token never goes on the agent's command line,
/// where every user of the host could read it in the process list.
inline QJsonObject describe(const QString& token, const QString& server, const QStringList& pins)
{
    QStringList args{QStringLiteral("--server"), server};
    if (!pins.isEmpty()) args << QStringLiteral("--pin") << pins.join(QLatin1Char(','));

    QStringList quoted;
    for (int i = 0; i < args.size(); ++i)
        quoted << (i % 2 ? shellQuote(args[i]) : args[i]);

    const QString setup =
        QStringLiteral("mkdir -p ~/.config/moonlightweb && (umask 077 && printf '%s\\n' %1 > %2)")
            .arg(shellQuote(token), defaultTokenFile());

    QJsonObject o;
    o.insert(QStringLiteral("token"), token);
    o.insert(QStringLiteral("server"), server);
    o.insert(QStringLiteral("pins"), QJsonArray::fromStringList(pins));
    o.insert(QStringLiteral("token_file"), defaultTokenFile());
    o.insert(QStringLiteral("args"), QJsonArray::fromStringList(args));
    o.insert(QStringLiteral("setup"), setup);
    o.insert(QStringLiteral("command"),
             QStringLiteral("moonlightweb-clipboard-agent ") + quoted.join(QLatin1Char(' ')));
    return o;
}

} // namespace ClipboardAgentSetup
