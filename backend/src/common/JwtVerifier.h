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
#include <QJsonObject>
#include <QString>

#include <optional>

/**
 * Verification of an OpenID Connect ID token (a signed JWT), and nothing else.
 *
 * Pure: no network, no clock of its own, no state. The caller hands it the
 * token, the identity provider's key set (JWKS) and what it expects, and gets
 * back either the verified claims or the first reason to refuse them. That is
 * what lets every refusal be tested with keys generated in the test itself
 * (tests/test_jwt_verifier.cpp). See docs/design/oidc-login.md.
 *
 * Deliberately narrow:
 *   - two algorithms, RS256 and ES256 (P-256). `none`, every HS* (which would
 *     turn a public key into an HMAC secret) and everything else is refused
 *     before a key is even looked at;
 *   - the key type is decided by the algorithm, never by the key, so a token
 *     cannot pick which verifier runs on it;
 *   - a header carrying `crit` is refused: we implement no extension.
 */
namespace JwtVerifier {

enum class Error
{
    None,
    Malformed,      ///< not three base64url parts of JSON objects, or too large
    UnsupportedAlg, ///< alg is not RS256 or ES256 (none, HS256, PS256, ...)
    UnknownKey,     ///< no key in the JWKS fits (kid, kty, use, alg)
    BadSignature,
    WrongIssuer,
    WrongAudience,   ///< aud does not contain the client id
    WrongAuthorized, ///< azp names someone else, or is missing with several audiences
    Expired,
    NotYetValid,    ///< nbf in the future
    IssuedInFuture, ///< iat in the future
    MissingClaim,   ///< sub, exp or iat absent or of the wrong type
    NonceMismatch,
};

/// Stable machine-readable name of @p error ("bad_signature"), for logs and
/// error answers.
QString errorName(Error error);

/// What the caller expects of the token.
struct Expectations
{
    QString issuer;   ///< exact `iss` (the discovery document's issuer)
    QString audience; ///< the client id
    QString nonce;    ///< the nonce sent in the authorization request
    qint64 now = 0;   ///< unix seconds
    qint64 skew = 60; ///< clock tolerance on exp / nbf / iat, in seconds
};

struct Header
{
    QString alg;
    QString kid; ///< empty when the header carries none
};

struct Result
{
    Error error = Error::Malformed; ///< fail closed
    QJsonObject claims;             ///< set only when error == None
    QString kid;                    ///< the token's kid, also on failure
    bool ok() const { return error == Error::None; }
};

/// Largest token accepted, in bytes. An ID token is a few hundred bytes to a
/// few kilobytes; anything near this is not one.
inline constexpr int MAX_TOKEN_BYTES = 16 * 1024;

/// Smallest RSA modulus accepted, in bits.
inline constexpr int MIN_RSA_BITS = 2048;

/// The token's header, or nullopt when it does not parse. Says nothing about
/// whether the token is valid: used to decide whether a JWKS refetch is worth it.
std::optional<Header> peekHeader(const QByteArray& token);

/// Verify @p token against the key set @p jwks ({"keys":[...]}) and @p expect.
Result verify(const QByteArray& token, const QJsonObject& jwks, const Expectations& expect);

/// A JWK as a DER SubjectPublicKeyInfo: RSA (n, e; at least MIN_RSA_BITS) or EC
/// on P-256 (x, y). Empty for anything else, including private parameters,
/// which have no business in a JWKS.
QByteArray jwkToSpki(const QJsonObject& jwk);

/// Strict unpadded base64url decode. nullopt on any character outside the
/// alphabet, on padding, or on an impossible length.
std::optional<QByteArray> base64UrlDecode(const QByteArray& in);

} // namespace JwtVerifier
