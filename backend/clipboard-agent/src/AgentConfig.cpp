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

#include "AgentConfig.h"

#include "BackendSelect.h"
#include "clipboard/ClipboardAgentProtocol.h"

#include <QHostAddress>
#include <QRegularExpression>
#include <QStringList>

namespace AgentConfigParse {

namespace {
bool isLoopbackHost(const QString& host)
{
    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0) return true;
    const QHostAddress a(host);
    return !a.isNull() && a.isLoopback();
}
} // namespace

QUrl agentUrl(const QString& server, QString* error)
{
    QString s = server.trimmed();
    if (s.isEmpty()) {
        *error = QStringLiteral("no server given (--server https://<address>:<port>)");
        return QUrl();
    }
    if (!s.contains(QLatin1String("://"))) s.prepend(QLatin1String("https://"));
    QUrl in(s, QUrl::StrictMode);
    if (!in.isValid() || in.host().isEmpty()) {
        *error = QStringLiteral("not a server address: %1").arg(server);
        return QUrl();
    }
    const QString scheme = in.scheme().toLower();
    QUrl out;
    if (scheme == QLatin1String("https") || scheme == QLatin1String("wss")) {
        out.setScheme(QStringLiteral("wss"));
    } else if (scheme == QLatin1String("http") || scheme == QLatin1String("ws")) {
        if (!isLoopbackHost(in.host())) {
            *error = QStringLiteral("refusing to send the token in clear to %1: use https://")
                         .arg(in.host());
            return QUrl();
        }
        out.setScheme(QStringLiteral("ws"));
    } else {
        *error = QStringLiteral("unsupported scheme: %1").arg(in.scheme());
        return QUrl();
    }
    if (!in.userInfo().isEmpty()) {
        *error = QStringLiteral("credentials do not belong in the server address");
        return QUrl();
    }
    out.setHost(in.host());
    if (in.port() > 0) out.setPort(in.port());
    out.setPath(ClipboardAgent::path());
    return out;
}

bool parsePins(const QString& spec, QList<QByteArray>* out, QString* error)
{
    out->clear();
    const QStringList parts = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (QString p : parts) {
        p = p.trimmed();
        if (p.isEmpty()) continue;
        if (p.startsWith(QLatin1String("sha256/"), Qt::CaseInsensitive)) p = p.mid(7);
        // curl writes sha256//<base64>; accept its spelling too.
        while (p.startsWith(QLatin1Char('/')))
            p = p.mid(1);
        const auto decoded =
            QByteArray::fromBase64Encoding(p.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.size() != 32) {
            *error = QStringLiteral("not a SHA-256 key pin: %1").arg(p);
            out->clear();
            return false;
        }
        out->append(decoded.decoded);
    }
    return true;
}

QString tokenFromFile(const QByteArray& contents)
{
    const QList<QByteArray> lines = contents.split('\n');
    for (const QByteArray& line : lines) {
        const QString t = QString::fromUtf8(line).trimmed();
        if (!t.isEmpty()) return t;
    }
    return QString();
}

QMap<QString, QString> parseEnvFile(const QByteArray& contents)
{
    QMap<QString, QString> out;
    for (const QByteArray& raw : contents.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) continue;
        const QString key = line.left(eq).trimmed();
        QString value = line.mid(eq + 1).trimmed();
        if (value.size() >= 2 &&
            ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) ||
             (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))))
            value = value.mid(1, value.size() - 2);
        out.insert(key, value);
    }
    return out;
}

bool looksLikeToken(const QString& token)
{
    static const QRegularExpression re(QStringLiteral("^mwca_[A-Za-z0-9_-]{43}$"));
    return re.match(token).hasMatch();
}

bool resolve(const Inputs& in, AgentConfig* out, QString* error)
{
    AgentConfig c;
    c.url = agentUrl(in.server, error);
    if (!c.url.isValid()) return false;

    if (!parsePins(in.pins, &c.pins, error)) return false;

    c.token = in.envToken.trimmed();
    if (c.token.isEmpty() && in.tokenFileRead) c.token = tokenFromFile(in.tokenFile);
    if (c.token.isEmpty()) {
        *error = QStringLiteral("no token: put the one the admin page gave in the token file, or "
                                "in MW_CLIPBOARD_AGENT_TOKEN");
        return false;
    }
    if (!looksLikeToken(c.token)) {
        *error = QStringLiteral("the token is not one MoonlightWeb issues (mwca_…)");
        return false;
    }

    c.backend = in.backend.trimmed().toLower();
    if (c.backend.isEmpty()) c.backend = QStringLiteral("auto");
    if (c.backend != QLatin1String("auto") && !BackendSelect::fromName(c.backend)) {
        *error = QStringLiteral("unknown backend %1 (auto, %2)")
                     .arg(c.backend, BackendSelect::allNames().join(QLatin1String(", ")));
        return false;
    }
    *out = c;
    return true;
}

} // namespace AgentConfigParse
