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

#include "WlClipboardBackend.h"

#include <QProcess>
#include <QTimer>
#include <QtDebug>

#include <algorithm>
#include <memory>

WlClipboardBackend::WlClipboardBackend(const QString& wlPaste, const QString& wlCopy,
                                       QObject* parent)
    : ClipboardBackend(parent)
    , m_WlPaste(wlPaste)
    , m_WlCopy(wlCopy)
{
    m_Grace = new QTimer(this);
    m_Grace->setSingleShot(true);
    connect(m_Grace, &QTimer::timeout, this, [this]() {
        if (m_Ready || !m_Watcher) return;
        m_Ready = true;
        emit ready();
    });
}

WlClipboardBackend::~WlClipboardBackend()
{
    if (m_Watcher) {
        m_Watcher->disconnect(this);
        m_Watcher->kill();
        m_Watcher->waitForFinished(1000);
    }
}

void WlClipboardBackend::start()
{
    spawnWatcher();
}

void WlClipboardBackend::spawnWatcher()
{
    auto* p = new QProcess(this);
    m_Watcher = p;
    // Its own stdin is nothing; `echo` inherits its stdout, which is ours.
    p->setStandardInputFile(QProcess::nullDevice());
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p]() {
        p->readAllStandardOutput(); // one line per change; the content is not in it
        if (m_Ready && m_Watching) read();
    });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
        const QString why = QString::fromUtf8(p->readAllStandardError()).trimmed();
        p->deleteLater();
        if (m_Watcher == p) m_Watcher = nullptr;
        if (!m_Ready) {
            m_Grace->stop();
            emit failed(why.isEmpty() ? QStringLiteral("wl-paste --watch exited (%1)").arg(code)
                                      : why.section(QLatin1Char('\n'), 0, 0));
            return;
        }
        // It worked once: a compositor restart, say. Back after a pause.
        const int delay = std::min(2000 << std::min(m_Restarts, 4), 30000);
        ++m_Restarts;
        qWarning().noquote() << "[agent] wl-paste --watch stopped" << (why.isEmpty() ? "" : why)
                             << "— restarting in" << delay / 1000 << "s";
        QTimer::singleShot(delay, this, [this]() {
            if (!m_Watcher) spawnWatcher();
        });
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        p->deleteLater();
        if (m_Watcher == p) m_Watcher = nullptr;
        if (!m_Ready) emit failed(QStringLiteral("cannot run %1").arg(m_WlPaste));
    });
    p->start(m_WlPaste, {QStringLiteral("--watch"), QStringLiteral("echo")});
    if (!m_Ready) m_Grace->start(m_StartGraceMs);
}

void WlClipboardBackend::read()
{
    if (m_Reading) {
        m_ReadAgain = true;
        return;
    }
    m_Reading = true;
    auto* p = new QProcess(this);
    p->setStandardInputFile(QProcess::nullDevice());
    auto finish = [this, p](const QString& text) {
        p->deleteLater();
        m_Reading = false;
        emit observed(text);
        if (m_ReadAgain) {
            m_ReadAgain = false;
            read();
        }
    };
    connect(p, &QProcess::finished, this, [p, finish](int code, QProcess::ExitStatus status) {
        // Non-zero: nothing copied, or nothing that is text (an image).
        const bool ok = status == QProcess::NormalExit && code == 0;
        finish(ok ? QString::fromUtf8(p->readAllStandardOutput()) : QString());
    });
    connect(p, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finish(QString());
    });
    p->start(m_WlPaste,
             {QStringLiteral("--no-newline"), QStringLiteral("--type"), QStringLiteral("text")});
}

void WlClipboardBackend::write(const QString& text, std::function<void(bool ok)> done)
{
    auto* p = new QProcess(this);
    // wl-copy stays behind to serve the clipboard: it must not hold our pipes.
    p->setStandardOutputFile(QProcess::nullDevice());
    p->setStandardErrorFile(QProcess::nullDevice());
    auto settled = std::make_shared<bool>(false);
    auto finish = [p, done, settled](bool ok) {
        if (*settled) return;
        *settled = true;
        p->deleteLater();
        if (done) done(ok);
    };
    connect(p, &QProcess::started, this, [p, text]() {
        p->write(text.toUtf8());
        p->closeWriteChannel();
    });
    connect(p, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit && code == 0);
    });
    connect(p, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) finish(false);
    });
    QTimer::singleShot(kWriteTimeoutMs, p, [p, finish]() {
        p->kill();
        finish(false);
    });
    p->start(m_WlCopy, {QStringLiteral("--type"), QStringLiteral("text/plain;charset=utf-8")});
}
