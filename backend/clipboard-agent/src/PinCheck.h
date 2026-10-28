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
#include <QList>
#include <QSslCertificate>

/// The server's key, pinned. MoonlightWeb serves a self-signed certificate
/// that no CA vouches for, so the agent checks the key it presents against
/// the pin the admin page gave (SHA-256 of its SubjectPublicKeyInfo, as
/// CertManager::spkiPin computes it). The key, not the certificate: the
/// server re-issues its local certificate at every start but keeps the key.
namespace PinCheck {

/// SHA-256 of @p cert's SubjectPublicKeyInfo; empty for a null certificate.
QByteArray spkiSha256(const QSslCertificate& cert);

/// "sha256/<base64>" of the same, as the admin page writes it.
QString pinText(const QSslCertificate& cert);

/// @p cert's key is one of @p pins. Never true for an empty list.
bool matches(const QSslCertificate& cert, const QList<QByteArray>& pins);

} // namespace PinCheck

/// The delay before the agent's next connection attempt: doubling from
/// @p initialMs per failed attempt, capped at @p maxMs, then spread by
/// @p jitter (0..1, a random draw) over ±20 % so a fleet of agents restarted
/// together does not knock in step.
int backoffDelayMs(int attempt, int initialMs, int maxMs, double jitter);
