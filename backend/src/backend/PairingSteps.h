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

#include <QByteArray>
#include <QString>
#include <functional>

/**
 * @brief The two calls a pairing handshake makes on the host.
 *
 * NvPairingManager speaks them to a real GameStream host; PairingChain only
 * sequences them, so a test can drive the chain against a fake host that
 * answers whenever it likes.
 */
class PairingSteps
{
public:
    enum PairState
    {
        PAIRED,
        PIN_WRONG,
        FAILED,
        ALREADY_IN_PROGRESS
    };

    enum InitResult
    {
        INIT_OK,
        INIT_FAILED,
        INIT_ALREADY_IN_PROGRESS
    };

    virtual ~PairingSteps() = default;

    /// Stage 1: salt + client certificate. A Sunshine host shows the request
    /// and holds the answer until its owner types the PIN. `onSent` (may be
    /// empty) fires once the request has left for the host; `cb` when it
    /// resolves. A stage 1 that already landed answers `cb` synchronously and
    /// sends nothing.
    virtual void initiatePairing(std::function<void(InitResult)> cb,
                                 std::function<void()> onSent) = 0;

    /// Stages 2-5: challenge/response with the PIN. `cb` gets the final state
    /// and, on PAIRED, the server certificate PEM.
    virtual void completePairing(const QString& pin,
                                 std::function<void(PairState, const QByteArray&)> cb) = 0;
};
