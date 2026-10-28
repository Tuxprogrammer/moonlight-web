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

#include "PairingSteps.h"

#include <QByteArray>
#include <QString>
#include <functional>

/**
 * @brief Drives one GameStream pairing handshake from stage 1 to stage 5.
 *
 * This is the *sequencing* only — deliberately no policy. Which messages the
 * user sees, what gets written to an NvComputer, and when a session is torn
 * down all stay with the caller, because those differ per backend and the
 * Sunshine path must keep behaving exactly as it does today.
 *
 * It exists because two callers need the same five stages driven the same way:
 *  - ComputerManager, where a human reads the PIN off the MoonlightWeb UI and
 *    types it into Sunshine, and
 *  - WolfBackend, where nobody types anything: Wolf parks phase 1 on an
 *    unresolved `user_pin` promise, and MoonlightWeb — being the Moonlight
 *    client — posts the PIN it would have displayed straight to
 *    `/api/v1/pair/client`.
 *
 * That second case is why `announcer` exists and why it fires when it does.
 */
namespace PairingChain {

enum class Outcome
{
    Paired,   ///< Stages 1-5 completed; Result::serverCertPem is set.
    Retry,    ///< Non-terminal: stage 1 never landed, or the PIN wasn't accepted
              ///< yet. The caller may run the chain again with the same session.
    HostBusy, ///< Stage 1 says the host is already pairing with someone.
    Failed,   ///< Terminal.
};

struct Result
{
    Outcome outcome = Outcome::Failed;
    QByteArray serverCertPem; ///< Valid only when outcome == Paired.
};

using ResultCallback = std::function<void(const Result&)>;

/// Where a run stands while it waits on the host, for a page to say so.
enum class Stage
{
    Contacting,     ///< Stage 1 is on its way; the host shows nothing yet.
    HostHasRequest, ///< Stage 1 reached the host, which shows it and waits for the PIN.
    Verifying,      ///< The host answered stage 1 (the PIN was typed): stages 2-5 run.
};

/// Fires on each stage change, in order, each stage at most once per run.
using StageCallback = std::function<void(Stage)>;

/// The stage as the /pair reply names it: "contacting", "enter_pin", "verifying".
QString stageName(Stage stage);

/// Whether the host has the request (it shows it, or already answered it).
bool hostContacted(Stage stage);

/// A fresh 4-digit PIN, zero-padded. The *client* picks it in GameStream
/// pairing — a human then relays it to Sunshine, while for Wolf we post it
/// ourselves and nobody ever sees it.
QString generatePin();

/// Invoked once, right after stage 1 has been *dispatched* — not after it
/// resolves. That ordering is the whole point: a Wolf host holds the stage-1
/// response open until someone supplies the PIN, so an announcer that waited
/// for stage 1 to return would deadlock against the very request it is meant to
/// unblock. Sunshine leaves this empty; a human is already reading the PIN.
using PinAnnouncer = std::function<void(const QString& pin)>;

/**
 * Run the handshake. `cb` fires exactly once, on a terminal or retryable
 * outcome; `onStage` (optional) on each stage change before it.
 *
 * `pm` is borrowed, never freed here, and must outlive the run — its own async
 * callbacks reference it. The caller keeps it alive for the whole chain, the
 * way ComputerManager holds the host uuid in m_SubmitInFlight.
 */
void run(PairingSteps* pm, const QString& pin, PinAnnouncer announcer, ResultCallback cb,
         StageCallback onStage = {});

} // namespace PairingChain
