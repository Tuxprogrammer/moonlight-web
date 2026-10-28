/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The WebSocket proxy's local leg (server/WsProxyConnect.h). A worker that is
 * still starting, or one a relaunch is replacing on the same port, refuses the
 * proxy for a moment: that refusal is retried while the slot has a worker, and
 * anything else is answered with a real status before the upgrade, instead of
 * a closed socket a proxy in front turns into a 502 of its own.
 */
#include "test_framework.h"
#include "server/WsProxyConnect.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

using wsproxy::decide;
using wsproxy::SlotState;
using wsproxy::Verdict;

namespace {

bool waitFor(const std::function<bool()>& cond, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!cond() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return cond();
}

// A port nothing listens on: bind one, note it, close it.
quint16 freePort()
{
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost, 0);
    const quint16 p = probe.serverPort();
    probe.close();
    return p;
}

void testDecision()
{
    SECTION("WsProxyConnect: decision table");
    const auto refused = QAbstractSocket::ConnectionRefusedError;
    const auto timeout = QAbstractSocket::SocketTimeoutError;
    const auto other = QAbstractSocket::NetworkError;

    // A worker holds the slot: a refusal is "not listening yet", inside the budget.
    CHECK(decide(SlotState::Active, refused, 0) == Verdict::Retry);
    CHECK(decide(SlotState::Active, refused, 1999) == Verdict::Retry);
    CHECK(decide(SlotState::Active, refused, 2000) == Verdict::Unavailable);
    CHECK(decide(SlotState::Active, refused, 60000) == Verdict::Unavailable);
    CHECK(decide(SlotState::Active, refused, 499, 500) == Verdict::Retry);
    CHECK(decide(SlotState::Active, refused, 500, 500) == Verdict::Unavailable);
    // Any other failure does not mend itself: answer at once.
    CHECK(decide(SlotState::Active, timeout, 0) == Verdict::Unavailable);
    CHECK(decide(SlotState::Active, other, 0) == Verdict::Unavailable);
    // No worker: nothing is coming, but the slot exists — come back later.
    CHECK(decide(SlotState::Idle, refused, 0) == Verdict::Unavailable);
    CHECK(decide(SlotState::Idle, timeout, 0) == Verdict::Unavailable);
    // No such slot: nothing will ever listen there.
    CHECK(decide(SlotState::Unknown, refused, 0) == Verdict::NotFound);
    CHECK(decide(SlotState::Unknown, other, 5000) == Verdict::NotFound);
}

void testNormalClose()
{
    SECTION("WsProxyConnect: a normal close is not an error");
    // The page closing its WebSocket: the worker closes its side.
    CHECK(wsproxy::isNormalClose(QAbstractSocket::RemoteHostClosedError));
    // Real failures stay warnings.
    CHECK(!wsproxy::isNormalClose(QAbstractSocket::ConnectionRefusedError));
    CHECK(!wsproxy::isNormalClose(QAbstractSocket::NetworkError));
    CHECK(!wsproxy::isNormalClose(QAbstractSocket::SocketTimeoutError));
    CHECK(!wsproxy::isNormalClose(QAbstractSocket::UnknownSocketError));
}

void testWorkerCloseIsReportedAsRemoteClose()
{
    SECTION("WsProxyConnect: a worker closing the tunnel reads as a normal close");
    // What the proxy sees when a stream ends: the far end closes an established
    // connection. Qt reports it as RemoteHostClosedError, which must classify
    // as normal.
    QTcpServer worker;
    CHECK(worker.listen(QHostAddress::LocalHost, 0));
    QTcpSocket target;
    QAbstractSocket::SocketError seen = QAbstractSocket::UnknownSocketError;
    bool errored = false;
    QObject::connect(&target, &QAbstractSocket::errorOccurred, [&](QAbstractSocket::SocketError e) {
        seen = e;
        errored = true;
    });
    target.connectToHost(QHostAddress::LocalHost, worker.serverPort());
    CHECK(waitFor([&] { return worker.hasPendingConnections(); }, 2000));
    QTcpSocket* accepted = worker.nextPendingConnection();
    CHECK(waitFor([&] { return target.state() == QAbstractSocket::ConnectedState; }, 2000));
    accepted->close();
    CHECK(waitFor([&] { return errored; }, 2000));
    CHECK(seen == QAbstractSocket::RemoteHostClosedError);
    CHECK(wsproxy::isNormalClose(seen));
    delete accepted;
}

void testResponses()
{
    SECTION("WsProxyConnect: answers before the upgrade");
    const HttpResponse busy = wsproxy::response(Verdict::Unavailable);
    CHECK_EQ(busy.statusCode, 503);
    CHECK_EQ(busy.headers.value(QStringLiteral("Retry-After")), QStringLiteral("1"));
    CHECK(!busy.body.isEmpty());
    const HttpResponse gone = wsproxy::response(Verdict::NotFound);
    CHECK_EQ(gone.statusCode, 404);
    CHECK(!gone.headers.contains(QStringLiteral("Retry-After")));
}

struct Outcome
{
    bool connected = false;
    bool failed = false;
    Verdict verdict = Verdict::Retry;
    int calls = 0;
};

void testLateListener()
{
    SECTION("WsProxyConnect: a worker that starts listening late is reached");
    const quint16 port = freePort();
    QTcpServer late;
    QTimer::singleShot(400, &late, [&]() { late.listen(QHostAddress::LocalHost, port); });

    QTcpSocket socket;
    Outcome o;
    QElapsedTimer t;
    t.start();
    wsproxy::connectWithRetry(
        &socket, port, [] { return SlotState::Active; },
        [&] {
            o.connected = true;
            ++o.calls;
        },
        [&](Verdict v) {
            o.failed = true;
            o.verdict = v;
            ++o.calls;
        },
        2000, 50);
    CHECK(waitFor([&] { return o.calls > 0; }, 4000));
    CHECK(o.connected);
    CHECK(!o.failed);
    CHECK(t.elapsed() >= 300); // it did wait for the listener
    CHECK(waitFor([&] { return late.hasPendingConnections(); }, 1000));
    // Settled once: nothing more comes, even when the connection drops.
    late.close();
    delete late.nextPendingConnection();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    CHECK_EQ(o.calls, 1);
}

void testNeverListens()
{
    SECTION("WsProxyConnect: a worker that never listens costs the budget, then 503");
    const quint16 port = freePort();
    QTcpSocket socket;
    Outcome o;
    QElapsedTimer t;
    t.start();
    wsproxy::connectWithRetry(
        &socket, port, [] { return SlotState::Active; }, [&] { ++o.calls; },
        [&](Verdict v) {
            o.failed = true;
            o.verdict = v;
            ++o.calls;
        },
        300, 30);
    CHECK(waitFor([&] { return o.calls > 0; }, 3000));
    CHECK(o.failed);
    CHECK(o.verdict == Verdict::Unavailable);
    CHECK(t.elapsed() >= 300);
    CHECK(t.elapsed() < 2000);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    CHECK_EQ(o.calls, 1);
}

void testIdleAndUnknownAnswerAtOnce()
{
    SECTION("WsProxyConnect: no worker, or no slot, is answered at once");
    for (const auto& [state, expected] : {std::pair{SlotState::Idle, Verdict::Unavailable},
                                          std::pair{SlotState::Unknown, Verdict::NotFound}}) {
        const quint16 port = freePort();
        QTcpSocket socket;
        Outcome o;
        QElapsedTimer t;
        t.start();
        wsproxy::connectWithRetry(
            &socket, port, [s = state] { return s; }, [&] { ++o.calls; },
            [&](Verdict v) {
                o.failed = true;
                o.verdict = v;
                ++o.calls;
            });
        CHECK(waitFor([&] { return o.calls > 0; }, 2000));
        CHECK(o.failed);
        CHECK(o.verdict == expected);
        CHECK(t.elapsed() < 1000); // no retry budget spent
    }
}

void testWorkerExitsDuringRetries()
{
    SECTION("WsProxyConnect: the worker exiting mid-retry stops the retries");
    const quint16 port = freePort();
    SlotState state = SlotState::Active;
    QTimer::singleShot(200, [&]() { state = SlotState::Idle; });
    QTcpSocket socket;
    Outcome o;
    QElapsedTimer t;
    t.start();
    wsproxy::connectWithRetry(
        &socket, port, [&] { return state; }, [&] { ++o.calls; },
        [&](Verdict v) {
            o.verdict = v;
            ++o.calls;
        },
        5000, 30);
    CHECK(waitFor([&] { return o.calls > 0; }, 3000));
    CHECK(o.verdict == Verdict::Unavailable);
    CHECK(t.elapsed() < 2000); // not the 5 s budget
}

void testSocketDestroyedMidRetry()
{
    SECTION("WsProxyConnect: a client gone mid-retry leaves nothing running");
    const quint16 port = freePort();
    Outcome o;
    auto* socket = new QTcpSocket;
    wsproxy::connectWithRetry(
        socket, port, [] { return SlotState::Active; }, [&] { ++o.calls; },
        [&](Verdict) { ++o.calls; }, 2000, 30);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    delete socket; // the proxy's cleanup, when the browser hangs up
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    CHECK_EQ(o.calls, 0);
}

} // namespace

void run_ws_proxy_connect_tests()
{
    testDecision();
    testResponses();
    testNormalClose();
    testWorkerCloseIsReportedAsRemoteClose();
    testLateListener();
    testNeverListens();
    testIdleAndUnknownAnswerAtOnce();
    testWorkerExitsDuringRetries();
    testSocketDestroyedMidRetry();
}
