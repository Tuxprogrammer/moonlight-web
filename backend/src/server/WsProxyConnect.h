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

#include "common/Types.h"

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>

/// The WebSocket proxy's local leg (HttpServer::handleWebSocketUpgrade): the
/// browser's upgrade is forwarded to a slot's worker on this machine. A worker
/// that is still starting, or one a relaunch is replacing on the same port,
/// refuses the connection for a moment; closing the browser's socket then
/// reads as a 502 a proxy in front invents. So a refusal is retried while the
/// slot has a worker, and anything else is answered before the upgrade, with a
/// status the browser (or a proxy) can act on.
namespace wsproxy {

enum class SlotState
{
    Unknown, ///< no such slot: nothing will ever listen there
    Idle,    ///< the slot exists, no worker holds it
    Active,  ///< a worker holds it, live or still starting
};

enum class Verdict
{
    Retry,       ///< connect again shortly
    Unavailable, ///< 503 + Retry-After: 1
    NotFound,    ///< 404
};

constexpr int kRetryBudgetMs = 2000;
constexpr int kRetryIntervalMs = 100;

/// What to do after the local connect failed with @p error, @p elapsedMs after
/// the first attempt.
inline Verdict decide(SlotState state, QAbstractSocket::SocketError error, qint64 elapsedMs,
                      qint64 budgetMs = kRetryBudgetMs)
{
    if (state == SlotState::Unknown) return Verdict::NotFound;
    // Only a refusal means "not listening yet"; any other failure will not
    // mend itself in two seconds.
    if (state == SlotState::Active && error == QAbstractSocket::ConnectionRefusedError &&
        elapsedMs < budgetMs)
        return Verdict::Retry;
    return Verdict::Unavailable;
}

/// An error on the established tunnel that is only its normal end: the worker
/// closed its side because the page closed its WebSocket. Not worth a warning.
inline bool isNormalClose(QAbstractSocket::SocketError error)
{
    return error == QAbstractSocket::RemoteHostClosedError;
}

/// The answer the browser gets for a final verdict, sent before any upgrade.
inline HttpResponse response(Verdict v)
{
    if (v == Verdict::NotFound) return HttpResponse::error(404, "Not Found");
    HttpResponse r = HttpResponse::error(503, "Stream not ready");
    r.headers[QStringLiteral("Retry-After")] = QStringLiteral("1");
    return r;
}

/// Connect @p socket to the local @p port, retrying a refusal per decide().
/// @p slotState is asked again at each failure (a worker may exit meanwhile).
/// Exactly one of @p onConnected / @p onFailed is called, unless the socket is
/// destroyed first; errors after the connection are no longer this helper's.
inline void connectWithRetry(QTcpSocket* socket, quint16 port, std::function<SlotState()> slotState,
                             std::function<void()> onConnected,
                             std::function<void(Verdict)> onFailed, int budgetMs = kRetryBudgetMs,
                             int intervalMs = kRetryIntervalMs)
{
    struct State
    {
        QElapsedTimer since;
        bool done = false;
        QMetaObject::Connection connected, failed;
    };
    auto st = std::make_shared<State>();
    st->since.start();

    auto finish = [st]() {
        st->done = true;
        QObject::disconnect(st->connected);
        QObject::disconnect(st->failed);
    };
    st->connected =
        QObject::connect(socket, &QTcpSocket::connected, socket, [st, finish, onConnected]() {
            if (st->done) return;
            finish();
            onConnected();
        });
    QPointer<QTcpSocket> guarded(socket);
    st->failed =
        QObject::connect(socket, &QAbstractSocket::errorOccurred, socket,
                         [st, finish, guarded, port, slotState, onFailed, budgetMs,
                          intervalMs](QAbstractSocket::SocketError error) {
                             if (st->done) return;
                             const Verdict v = decide(slotState ? slotState() : SlotState::Idle,
                                                      error, st->since.elapsed(), budgetMs);
                             if (v != Verdict::Retry) {
                                 finish();
                                 onFailed(v);
                                 return;
                             }
                             // Context = the socket: destroyed meanwhile, the retry never runs.
                             QTimer::singleShot(intervalMs, guarded.data(), [st, guarded, port]() {
                                 if (st->done || !guarded) return;
                                 guarded->abort();
                                 guarded->connectToHost(QHostAddress::LocalHost, port);
                             });
                         });
    socket->connectToHost(QHostAddress::LocalHost, port);
}

} // namespace wsproxy
