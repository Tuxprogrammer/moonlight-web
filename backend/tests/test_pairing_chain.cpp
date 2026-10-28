/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The pairing handshake against a fake host. Stage 1 is the request a Sunshine
 * host shows to its owner and then holds until a PIN is typed there, so it
 * answers late by design: the chain must say where it stands meanwhile
 * (contacting the host, the host has the request, verifying the PIN) so the
 * page can tell "waiting for the host to show it" from "enter the PIN now". A
 * wrong PIN is not the end (Retry), a host already pairing with someone is.
 */
#include "test_framework.h"
#include "backend/PairingChain.h"
#include "backend/PairingSteps.h"

#include <QEventLoop>
#include <QTimer>
#include <QVector>

namespace {

using PairingChain::Outcome;
using PairingChain::Result;
using PairingChain::Stage;

/// A host whose answers the test hands out, whenever it likes.
struct FakeHost : PairingSteps
{
    std::function<void(InitResult)> stage1;
    std::function<void()> sent;
    std::function<void(PairState, const QByteArray&)> stages25;
    QString pinTried;
    bool stage1Done = false; // as NvPairingManager: a landed stage 1 is not sent again
    int stage1Calls = 0;
    int stages25Calls = 0;

    void initiatePairing(std::function<void(InitResult)> cb, std::function<void()> onSent) override
    {
        ++stage1Calls;
        if (stage1Done) {
            cb(INIT_OK);
            return;
        }
        stage1 = std::move(cb);
        sent = std::move(onSent);
    }

    void completePairing(const QString& pin,
                         std::function<void(PairState, const QByteArray&)> cb) override
    {
        ++stages25Calls;
        pinTried = pin;
        stages25 = std::move(cb);
    }

    void deliver()
    {
        if (sent) sent();
    }

    void answerStage1(InitResult r)
    {
        if (r == INIT_OK) stage1Done = true;
        auto cb = std::move(stage1);
        stage1 = {};
        cb(r);
    }

    void answerStages25(PairState s, const QByteArray& cert = {})
    {
        auto cb = std::move(stages25);
        stages25 = {};
        cb(s, cert);
    }
};

/// What one run reported: its stages in order, and its outcome(s).
struct Watch
{
    QVector<Stage> stages;
    QVector<Result> results;

    PairingChain::StageCallback onStage()
    {
        return [this](Stage s) { stages.append(s); };
    }
    PairingChain::ResultCallback onResult()
    {
        return [this](const Result& r) { results.append(r); };
    }
};

void waitMs(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

} // namespace

void run_pairing_chain_tests()
{
    std::fprintf(stderr, "[pairing_chain]\n");

    // A host that shows the request and answers once the PIN is typed, late.
    {
        FakeHost host;
        Watch w;
        PairingChain::run(&host, QStringLiteral("0420"), {}, w.onResult(), w.onStage());
        CHECK_EQ(host.stage1Calls, 1);
        CHECK(w.stages == QVector<Stage>({Stage::Contacting}));
        CHECK(w.results.isEmpty());

        host.deliver(); // the request reached the host, which shows it
        CHECK(w.stages == QVector<Stage>({Stage::Contacting, Stage::HostHasRequest}));
        CHECK(w.results.isEmpty());
        CHECK_EQ(host.stages25Calls, 0);

        host.answerStage1(PairingSteps::INIT_OK); // the PIN was typed there
        CHECK(w.stages ==
              QVector<Stage>({Stage::Contacting, Stage::HostHasRequest, Stage::Verifying}));
        CHECK_EQ(host.stages25Calls, 1);
        CHECK_EQ(host.pinTried, QStringLiteral("0420"));
        CHECK(w.results.isEmpty());

        host.answerStages25(PairingSteps::PAIRED, QByteArrayLiteral("CERT"));
        CHECK_EQ(w.results.size(), 1);
        CHECK(w.results.value(0).outcome == Outcome::Paired);
        CHECK_EQ(w.results.value(0).serverCertPem, QByteArrayLiteral("CERT"));
        CHECK_EQ(w.stages.size(), 3);
    }

    // The host's answer to stage 1 is enough to move on, even without a word
    // that the request was sent (an older Qt, or a very fast host).
    {
        FakeHost host;
        Watch w;
        PairingChain::run(&host, QStringLiteral("1111"), {}, w.onResult(), w.onStage());
        host.answerStage1(PairingSteps::INIT_OK);
        CHECK(w.stages == QVector<Stage>({Stage::Contacting, Stage::Verifying}));
        host.deliver(); // a late "sent" changes nothing once past it
        CHECK(w.stages == QVector<Stage>({Stage::Contacting, Stage::Verifying}));
    }

    // "Sent" may be said more than once (a redirect): one stage change.
    {
        FakeHost host;
        Watch w;
        PairingChain::run(&host, QStringLiteral("2222"), {}, w.onResult(), w.onStage());
        host.deliver();
        host.deliver();
        CHECK(w.stages == QVector<Stage>({Stage::Contacting, Stage::HostHasRequest}));
    }

    // A wrong PIN is Retry, and the next run goes straight back to verifying:
    // stage 1 already landed, so nothing new is shown on the host.
    {
        FakeHost host;
        Watch w;
        PairingChain::run(&host, QStringLiteral("3333"), {}, w.onResult(), w.onStage());
        host.deliver();
        host.answerStage1(PairingSteps::INIT_OK);
        host.answerStages25(PairingSteps::PIN_WRONG);
        CHECK_EQ(w.results.size(), 1);
        CHECK(w.results.value(0).outcome == Outcome::Retry);
        CHECK(w.results.value(0).serverCertPem.isEmpty());

        Watch again;
        PairingChain::run(&host, QStringLiteral("3333"), {}, again.onResult(), again.onStage());
        CHECK_EQ(host.stage1Calls, 2);
        CHECK(again.stages == QVector<Stage>({Stage::Verifying}));
        CHECK_EQ(host.stages25Calls, 2);
        host.answerStages25(PairingSteps::PAIRED, QByteArrayLiteral("C2"));
        CHECK_EQ(again.results.size(), 1);
        CHECK(again.results.value(0).outcome == Outcome::Paired);
    }

    // A host already pairing with someone else: terminal, no PIN is tried.
    {
        FakeHost host;
        Watch w;
        PairingChain::run(&host, QStringLiteral("4444"), {}, w.onResult(), w.onStage());
        host.deliver();
        host.answerStage1(PairingSteps::INIT_ALREADY_IN_PROGRESS);
        CHECK_EQ(w.results.size(), 1);
        CHECK(w.results.value(0).outcome == Outcome::HostBusy);
        CHECK_EQ(host.stages25Calls, 0);
        CHECK(!w.stages.contains(Stage::Verifying));
    }

    // Stage 1 timed out (nobody typed the PIN in time) or the host never
    // answered: Retry, never past "the host has the request".
    {
        FakeHost host;
        Watch w;
        PairingChain::run(&host, QStringLiteral("5555"), {}, w.onResult(), w.onStage());
        host.deliver();
        host.answerStage1(PairingSteps::INIT_FAILED);
        CHECK_EQ(w.results.size(), 1);
        CHECK(w.results.value(0).outcome == Outcome::Retry);
        CHECK(w.stages == QVector<Stage>({Stage::Contacting, Stage::HostHasRequest}));
        CHECK_EQ(host.stages25Calls, 0);
    }
    {
        FakeHost host; // unreachable: the request never left
        Watch w;
        PairingChain::run(&host, QStringLiteral("5556"), {}, w.onResult(), w.onStage());
        host.answerStage1(PairingSteps::INIT_FAILED);
        CHECK(w.results.value(0).outcome == Outcome::Retry);
        CHECK(w.stages == QVector<Stage>({Stage::Contacting}));
    }

    // Stages 2-5 failing outright is terminal.
    for (const auto state : {PairingSteps::FAILED, PairingSteps::ALREADY_IN_PROGRESS}) {
        FakeHost host;
        Watch w;
        PairingChain::run(&host, QStringLiteral("6666"), {}, w.onResult(), w.onStage());
        host.answerStage1(PairingSteps::INIT_OK);
        host.answerStages25(state);
        CHECK_EQ(w.results.size(), 1);
        CHECK(w.results.value(0).outcome == Outcome::Failed);
    }

    // No host: Failed at once, no stage reported.
    {
        Watch w;
        PairingChain::run(nullptr, QStringLiteral("7777"), {}, w.onResult(), w.onStage());
        CHECK_EQ(w.results.size(), 1);
        CHECK(w.results.value(0).outcome == Outcome::Failed);
        CHECK(w.stages.isEmpty());
    }

    // Without a stage callback (the Wolf and multi-seat backends) nothing changes.
    {
        FakeHost host;
        Watch w;
        PairingChain::run(&host, QStringLiteral("8888"), {}, w.onResult());
        host.deliver();
        host.answerStage1(PairingSteps::INIT_OK);
        host.answerStages25(PairingSteps::PAIRED, QByteArrayLiteral("C"));
        CHECK_EQ(w.results.size(), 1);
        CHECK(w.results.value(0).outcome == Outcome::Paired);
    }

    // The announcer fires once stage 1 has had time to land, and only when a
    // fresh stage 1 went out: a run that found it already done announces
    // nothing, since nothing new waits on the host.
    {
        FakeHost fresh;
        FakeHost landed;
        landed.stage1Done = true;
        int freshAnnounced = 0;
        int landedAnnounced = 0;
        QString announcedPin;
        Watch w1, w2;
        PairingChain::run(
            &fresh, QStringLiteral("9090"),
            [&](const QString& pin) {
                ++freshAnnounced;
                announcedPin = pin;
            },
            w1.onResult());
        PairingChain::run(
            &landed, QStringLiteral("9191"), [&](const QString&) { ++landedAnnounced; },
            w2.onResult());
        CHECK_EQ(freshAnnounced, 0); // not before the dispatch had time to land
        waitMs(3300);
        CHECK_EQ(freshAnnounced, 1);
        CHECK_EQ(announcedPin, QStringLiteral("9090"));
        CHECK_EQ(landedAnnounced, 0);
    }

    // What the /pair reply says for each stage.
    {
        CHECK_EQ(PairingChain::stageName(Stage::Contacting), QStringLiteral("contacting"));
        CHECK_EQ(PairingChain::stageName(Stage::HostHasRequest), QStringLiteral("enter_pin"));
        CHECK_EQ(PairingChain::stageName(Stage::Verifying), QStringLiteral("verifying"));
        CHECK(!PairingChain::hostContacted(Stage::Contacting));
        CHECK(PairingChain::hostContacted(Stage::HostHasRequest));
        CHECK(PairingChain::hostContacted(Stage::Verifying));
    }

    // A fresh PIN: four digits, zero-padded.
    for (int i = 0; i < 50; ++i) {
        const QString pin = PairingChain::generatePin();
        CHECK_EQ(pin.size(), 4);
        bool digits = true;
        for (const QChar c : pin)
            digits = digits && c.isDigit();
        CHECK(digits);
    }
}
