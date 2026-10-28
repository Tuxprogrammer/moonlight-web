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

// moonlightweb-clipboard-agent: shares the clipboard of the desktop session
// it runs in with MoonlightWeb, for a host streamed by Sunshine on another
// machine. Run it inside the host user's graphical session (a systemd user
// unit, a login item…). docs/design/clipboard-agent.md, wiki §5.

#include "AgentClient.h"
#include "AgentConfig.h"
#include "BackendSelect.h"
#include "ClipboardBackend.h"
#include "QtClipboardBackend.h"
#include "WlClipboardBackend.h"
#ifdef MW_AGENT_HAVE_DBUS
#include "KlipperBackend.h"
#endif

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QMap>
#include <QStandardPaths>
#include <QtDebug>

#include <cstdio>
#include <functional>
#include <memory>

namespace {

QString configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/moonlightweb");
}

QString defaultTokenFile()
{
    return configDir() + QStringLiteral("/clipboard-agent.token");
}

/// The settings file: MW_CLIPBOARD_AGENT_SERVER / _PIN / _BACKEND lines, so a
/// launcher (systemd, an autostart entry, a login item) needs no arguments.
QString defaultSettingsFile()
{
    return configDir() + QStringLiteral("/clipboard-agent.env");
}

ClipboardBackend* makeBackend(BackendSelect::Backend b, QObject* parent)
{
    using BackendSelect::Backend;
    switch (b) {
    case Backend::WlClipboard:
        return new WlClipboardBackend(QStandardPaths::findExecutable(QStringLiteral("wl-paste")),
                                      QStandardPaths::findExecutable(QStringLiteral("wl-copy")),
                                      parent);
    case Backend::Klipper:
#ifdef MW_AGENT_HAVE_DBUS
        return new KlipperBackend(parent);
#else
        return nullptr;
#endif
    case Backend::Qt:
    case Backend::QtXWayland: return new QtClipboardBackend(BackendSelect::name(b), parent);
    }
    return nullptr;
}

/// Starts the candidates one after the other until one is ready.
class BackendChain : public QObject
{
public:
    BackendChain(QList<BackendSelect::Backend> candidates,
                 std::function<void(ClipboardBackend*)> onReady)
        : m_Candidates(std::move(candidates))
        , m_OnReady(std::move(onReady))
    {}

    void next()
    {
        if (m_Candidates.isEmpty()) {
            qCritical() << "[agent] No clipboard backend works in this session";
            QCoreApplication::exit(1);
            return;
        }
        const BackendSelect::Backend b = m_Candidates.takeFirst();
        ClipboardBackend* backend = makeBackend(b, QCoreApplication::instance());
        if (!backend) {
            next();
            return;
        }
        connect(backend, &ClipboardBackend::ready, this, [this, backend, b]() {
            qInfo().noquote() << "[agent] Clipboard backend:" << BackendSelect::describe(b);
            m_OnReady(backend);
        });
        connect(backend, &ClipboardBackend::failed, this, [this, backend, b](const QString& why) {
            qWarning().noquote() << "[agent] Clipboard backend" << BackendSelect::name(b)
                                 << "unavailable:" << why;
            backend->deleteLater();
            next();
        });
        backend->start();
    }

private:
    QList<BackendSelect::Backend> m_Candidates;
    std::function<void(ClipboardBackend*)> m_OnReady;
};

} // namespace

int main(int argc, char** argv)
{
    QStringList args;
    for (int i = 0; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Shares this desktop session's clipboard with a MoonlightWeb server."));
    parser.addHelpOption();
    const QCommandLineOption versionOpt(QStringLiteral("version"),
                                        QStringLiteral("Print the version and exit."));
    const QCommandLineOption serverOpt(
        QStringLiteral("server"),
        QStringLiteral("The MoonlightWeb server, https://<address>:<port> "
                       "(or MW_CLIPBOARD_AGENT_SERVER, in the environment or in %1).")
            .arg(defaultSettingsFile()),
        QStringLiteral("url"));
    const QCommandLineOption pinOpt(
        QStringLiteral("pin"),
        QStringLiteral("The server key pin(s) from the admin page, sha256/<base64>, "
                       "comma-separated (or MW_CLIPBOARD_AGENT_PIN). Without one the "
                       "certificate must verify against the system's CAs."),
        QStringLiteral("pins"));
    const QCommandLineOption tokenFileOpt(
        QStringLiteral("token-file"),
        QStringLiteral("File holding the host's token (default: %1). "
                       "MW_CLIPBOARD_AGENT_TOKEN overrides it.")
            .arg(defaultTokenFile()),
        QStringLiteral("path"));
    const QCommandLineOption backendOpt(
        QStringLiteral("backend"),
        QStringLiteral("auto (default), %1 (or MW_CLIPBOARD_AGENT_BACKEND).")
            .arg(BackendSelect::allNames().join(", ")),
        QStringLiteral("name"));
    const QCommandLineOption verboseOpt(QStringLiteral("verbose"),
                                        QStringLiteral("Debug logging (never clipboard text)."));
    parser.addOptions({versionOpt, serverOpt, pinOpt, tokenFileOpt, backendOpt, verboseOpt});
    if (!parser.parse(args)) {
        std::fprintf(stderr, "%s\n", qPrintable(parser.errorText()));
        return 64;
    }
    if (parser.isSet(QStringLiteral("help"))) {
        std::fprintf(stdout, "%s", qPrintable(parser.helpText()));
        return 0;
    }
    if (parser.isSet(versionOpt)) {
        std::fprintf(stdout, "moonlightweb-clipboard-agent %s\n",
                     qPrintable(AgentClient::version()));
        return 0;
    }

    // Read everything before any app exists: which kind of app to make
    // depends on the backend.
    // Arguments first, then the environment, then the settings file.
    QMap<QString, QString> settings;
    {
        QFile f(defaultSettingsFile());
        if (f.open(QIODevice::ReadOnly)) settings = AgentConfigParse::parseEnvFile(f.read(65536));
    }
    auto pick = [&](const QCommandLineOption& opt, const char* var) {
        if (parser.isSet(opt)) return parser.value(opt);
        if (!qEnvironmentVariableIsEmpty(var)) return qEnvironmentVariable(var);
        return settings.value(QString::fromLatin1(var));
    };
    AgentConfigParse::Inputs in;
    in.server = pick(serverOpt, "MW_CLIPBOARD_AGENT_SERVER");
    in.pins = pick(pinOpt, "MW_CLIPBOARD_AGENT_PIN");
    in.envToken = qEnvironmentVariable("MW_CLIPBOARD_AGENT_TOKEN");
    in.backend = pick(backendOpt, "MW_CLIPBOARD_AGENT_BACKEND");
    const QString tokenPath =
        parser.isSet(tokenFileOpt) ? parser.value(tokenFileOpt) : defaultTokenFile();
    QFile tokenFile(tokenPath);
    if (in.envToken.isEmpty() && tokenFile.open(QIODevice::ReadOnly)) {
        in.tokenFile = tokenFile.read(4096);
        in.tokenFileRead = true;
#ifdef Q_OS_UNIX
        const QFileDevice::Permissions loose = QFileDevice::ReadGroup | QFileDevice::ReadOther;
        if (tokenFile.permissions() & loose)
            std::fprintf(stderr, "warning: %s is readable by other users (chmod 600 it)\n",
                         qPrintable(tokenPath));
#endif
    }
    AgentConfig config;
    QString error;
    if (!AgentConfigParse::resolve(in, &config, &error)) {
        std::fprintf(stderr, "moonlightweb-clipboard-agent: %s\n", qPrintable(error));
        return 64;
    }

    const BackendSelect::Environment env = BackendSelect::detect(config.backend);
    QList<BackendSelect::Backend> candidates = BackendSelect::candidates(env);
    bool gui = false;
    bool xwayland = false;
    for (BackendSelect::Backend b : candidates) {
        gui |= BackendSelect::needsGui(b);
        xwayland |= b == BackendSelect::Backend::QtXWayland;
    }
    // Log lines on stderr whatever stderr is: the systemd unit hands them to
    // the journal itself, and a redirect to a file then gets them too.
    if (qEnvironmentVariableIsEmpty("QT_FORCE_STDERR_LOGGING"))
        qputenv("QT_FORCE_STDERR_LOGGING", "1");
    // QClipboard in a Wayland session reads XWayland's clipboard.
    if (xwayland) qputenv("QT_QPA_PLATFORM", "xcb");

    std::unique_ptr<QCoreApplication> app;
    if (gui) {
        // A program with no window: keep it out of the session's task list.
        QGuiApplication::setQuitOnLastWindowClosed(false);
        app = std::make_unique<QGuiApplication>(argc, argv);
    } else {
        app = std::make_unique<QCoreApplication>(argc, argv);
    }
    QCoreApplication::setApplicationName(QStringLiteral("moonlightweb-clipboard-agent"));
    QCoreApplication::setApplicationVersion(AgentClient::version());
    if (!parser.isSet(verboseOpt))
        QLoggingCategory::setFilterRules(QStringLiteral("*.debug=false"));

    qInfo().noquote() << "[agent] moonlightweb-clipboard-agent" << AgentClient::version() << "on"
                      << AgentClient::platform();
    if (candidates.isEmpty()) {
        qCritical().noquote()
            << "[agent] No clipboard backend for this session"
            << (config.backend != QLatin1String("auto")
                    ? QStringLiteral("(--backend %1 is not available here)").arg(config.backend)
                    : QStringLiteral("(no WAYLAND_DISPLAY with wl-clipboard or Klipper, no "
                                     "DISPLAY): run it inside the desktop session"));
        return 1;
    }

    AgentClient* client = nullptr;
    BackendChain chain(candidates, [&client, &config](ClipboardBackend* backend) {
        client = new AgentClient(config, backend, QCoreApplication::instance());
        QObject::connect(client, &AgentClient::fatal, QCoreApplication::instance(),
                         [](int code, const QString&) { QCoreApplication::exit(code); });
        client->start();
    });
    chain.next();
    return app->exec();
}
