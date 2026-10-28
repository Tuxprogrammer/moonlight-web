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

#include "PinCheck.h"

#include <QCryptographicHash>
#include <QSslKey>

#include <algorithm>
#include <cmath>

namespace PinCheck {

QByteArray spkiSha256(const QSslCertificate& cert)
{
    if (cert.isNull()) return QByteArray();
    // A public QSslKey's DER is the SubjectPublicKeyInfo.
    const QByteArray spki = cert.publicKey().toDer();
    if (spki.isEmpty()) return QByteArray();
    return QCryptographicHash::hash(spki, QCryptographicHash::Sha256);
}

QString pinText(const QSslCertificate& cert)
{
    const QByteArray d = spkiSha256(cert);
    return d.isEmpty() ? QString() : QStringLiteral("sha256/") + QString::fromLatin1(d.toBase64());
}

bool matches(const QSslCertificate& cert, const QList<QByteArray>& pins)
{
    const QByteArray d = spkiSha256(cert);
    if (d.isEmpty()) return false;
    return std::any_of(pins.begin(), pins.end(), [&d](const QByteArray& p) { return p == d; });
}

} // namespace PinCheck

int backoffDelayMs(int attempt, int initialMs, int maxMs, double jitter)
{
    double d = initialMs;
    for (int i = 0; i < attempt && d < maxMs; ++i)
        d *= 2;
    d = std::min<double>(d, maxMs);
    const double j = std::clamp(jitter, 0.0, 1.0);
    d *= 0.8 + 0.4 * j;
    return static_cast<int>(std::lround(std::max(d, 0.0)));
}
