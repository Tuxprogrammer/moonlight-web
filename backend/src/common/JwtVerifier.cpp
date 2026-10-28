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

#include "JwtVerifier.h"

#include "PairingCrypto.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QList>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/x509.h>

#include <memory>

namespace JwtVerifier {

namespace {

struct PkeyDeleter
{
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct PkeyCtxDeleter
{
    void operator()(EVP_PKEY_CTX* c) const { EVP_PKEY_CTX_free(c); }
};
struct MdCtxDeleter
{
    void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); }
};
struct BnDeleter
{
    void operator()(BIGNUM* b) const { BN_free(b); }
};
struct ParamBldDeleter
{
    void operator()(OSSL_PARAM_BLD* b) const { OSSL_PARAM_BLD_free(b); }
};
struct ParamDeleter
{
    void operator()(OSSL_PARAM* p) const { OSSL_PARAM_free(p); }
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;
using BnPtr = std::unique_ptr<BIGNUM, BnDeleter>;
using ParamBldPtr = std::unique_ptr<OSSL_PARAM_BLD, ParamBldDeleter>;
using ParamPtr = std::unique_ptr<OSSL_PARAM, ParamDeleter>;

constexpr int P256_COORD_BYTES = 32;

/// Build a public key from @p params, then confirm OpenSSL considers it a sound
/// public key (on the curve, sane modulus).
PkeyPtr keyFromParams(const char* type, OSSL_PARAM* params)
{
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr));
    if (!ctx || EVP_PKEY_fromdata_init(ctx.get()) != 1) return nullptr;
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_fromdata(ctx.get(), &raw, EVP_PKEY_PUBLIC_KEY, params) != 1) return nullptr;
    PkeyPtr key(raw);
    PkeyCtxPtr check(EVP_PKEY_CTX_new_from_pkey(nullptr, key.get(), nullptr));
    if (!check || EVP_PKEY_public_check(check.get()) != 1) return nullptr;
    return key;
}

QByteArray spkiOf(EVP_PKEY* key)
{
    const int len = i2d_PUBKEY(key, nullptr);
    if (len <= 0) return {};
    QByteArray der(len, Qt::Uninitialized);
    auto* p = reinterpret_cast<unsigned char*>(der.data());
    if (i2d_PUBKEY(key, &p) != len) return {};
    return der;
}

BnPtr bignum(const QByteArray& bytes)
{
    return BnPtr(BN_bin2bn(reinterpret_cast<const unsigned char*>(bytes.constData()),
                           static_cast<int>(bytes.size()), nullptr));
}

QByteArray rsaSpki(const QJsonObject& jwk)
{
    const auto n = base64UrlDecode(jwk.value(QStringLiteral("n")).toString().toLatin1());
    const auto e = base64UrlDecode(jwk.value(QStringLiteral("e")).toString().toLatin1());
    if (!n || !e || n->isEmpty() || e->isEmpty() || e->size() > 8) return {};

    BnPtr bn(bignum(*n)), be(bignum(*e));
    ParamBldPtr bld(OSSL_PARAM_BLD_new());
    if (!bn || !be || !bld) return {};
    if (OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_N, bn.get()) != 1 ||
        OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_E, be.get()) != 1)
        return {};
    ParamPtr params(OSSL_PARAM_BLD_to_param(bld.get()));
    if (!params) return {};
    PkeyPtr key = keyFromParams("RSA", params.get());
    if (!key || EVP_PKEY_get_bits(key.get()) < MIN_RSA_BITS) return {};
    return spkiOf(key.get());
}

QByteArray ecSpki(const QJsonObject& jwk)
{
    if (jwk.value(QStringLiteral("crv")).toString() != QLatin1String("P-256")) return {};
    const auto x = base64UrlDecode(jwk.value(QStringLiteral("x")).toString().toLatin1());
    const auto y = base64UrlDecode(jwk.value(QStringLiteral("y")).toString().toLatin1());
    // RFC 7518 §6.2.1.2: each coordinate is exactly the field size, left-padded.
    if (!x || !y || x->size() != P256_COORD_BYTES || y->size() != P256_COORD_BYTES) return {};

    const QByteArray point = QByteArray(1, '\x04') + *x + *y; // uncompressed
    ParamBldPtr bld(OSSL_PARAM_BLD_new());
    if (!bld) return {};
    if (OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0) !=
            1 ||
        OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, point.constData(),
                                         static_cast<size_t>(point.size())) != 1)
        return {};
    ParamPtr params(OSSL_PARAM_BLD_to_param(bld.get()));
    if (!params) return {};
    PkeyPtr key = keyFromParams("EC", params.get());
    return key ? spkiOf(key.get()) : QByteArray();
}

bool verifyRs256(const QByteArray& spki, const QByteArray& input, const QByteArray& sig)
{
    const auto* p = reinterpret_cast<const unsigned char*>(spki.constData());
    PkeyPtr key(d2i_PUBKEY(nullptr, &p, static_cast<long>(spki.size())));
    if (!key || EVP_PKEY_get_base_id(key.get()) != EVP_PKEY_RSA) return false;
    if (EVP_PKEY_get_bits(key.get()) < MIN_RSA_BITS) return false;
    // A PKCS#1 v1.5 signature is exactly the modulus length.
    if (sig.size() != EVP_PKEY_get_size(key.get())) return false;

    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) return false;
    EVP_PKEY_CTX* pctx = nullptr;
    if (EVP_DigestVerifyInit(ctx.get(), &pctx, EVP_sha256(), nullptr, key.get()) != 1) return false;
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PADDING) != 1) return false;
    return EVP_DigestVerify(ctx.get(), reinterpret_cast<const unsigned char*>(sig.constData()),
                            static_cast<size_t>(sig.size()),
                            reinterpret_cast<const unsigned char*>(input.constData()),
                            static_cast<size_t>(input.size())) == 1;
}

std::optional<QJsonObject> decodeJsonObject(const QByteArray& part)
{
    const auto bytes = base64UrlDecode(part);
    if (!bytes) return std::nullopt;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(*bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    return doc.object();
}

/// The keys of @p jwks this token may have been signed with.
QList<QByteArray> candidateKeys(const QJsonObject& jwks, const Header& header)
{
    const bool rsa = header.alg == QLatin1String("RS256");
    const QString kty = rsa ? QStringLiteral("RSA") : QStringLiteral("EC");

    QList<QByteArray> out;
    for (const QJsonValue& v : jwks.value(QStringLiteral("keys")).toArray()) {
        const QJsonObject jwk = v.toObject();
        if (jwk.value(QStringLiteral("kty")).toString() != kty) continue;
        if (!header.kid.isEmpty() && jwk.value(QStringLiteral("kid")).toString() != header.kid)
            continue;
        // Optional fields, but binding when present (RFC 7517 §4.2, §4.4).
        if (jwk.contains(QStringLiteral("use")) &&
            jwk.value(QStringLiteral("use")).toString() != QLatin1String("sig"))
            continue;
        if (jwk.contains(QStringLiteral("alg")) &&
            jwk.value(QStringLiteral("alg")).toString() != header.alg)
            continue;
        const QByteArray spki = jwkToSpki(jwk);
        if (!spki.isEmpty()) out.append(spki);
    }
    return out;
}

bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

bool isNumber(const QJsonValue& v)
{
    return v.isDouble();
}

qint64 seconds(const QJsonValue& v)
{
    return static_cast<qint64>(v.toDouble());
}

} // namespace

QString errorName(Error error)
{
    switch (error) {
    case Error::None: return QStringLiteral("none");
    case Error::Malformed: return QStringLiteral("malformed");
    case Error::UnsupportedAlg: return QStringLiteral("unsupported_alg");
    case Error::UnknownKey: return QStringLiteral("unknown_key");
    case Error::BadSignature: return QStringLiteral("bad_signature");
    case Error::WrongIssuer: return QStringLiteral("wrong_issuer");
    case Error::WrongAudience: return QStringLiteral("wrong_audience");
    case Error::WrongAuthorized: return QStringLiteral("wrong_azp");
    case Error::Expired: return QStringLiteral("expired");
    case Error::NotYetValid: return QStringLiteral("not_yet_valid");
    case Error::IssuedInFuture: return QStringLiteral("issued_in_future");
    case Error::MissingClaim: return QStringLiteral("missing_claim");
    case Error::NonceMismatch: return QStringLiteral("nonce_mismatch");
    }
    return QStringLiteral("unknown");
}

std::optional<QByteArray> base64UrlDecode(const QByteArray& in)
{
    // A length of 1 mod 4 cannot come out of any byte string.
    if (in.size() % 4 == 1) return std::nullopt;
    for (const char c : in) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return std::nullopt;
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        in, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals |
                QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) return std::nullopt;
    return decoded.decoded;
}

std::optional<Header> peekHeader(const QByteArray& token)
{
    if (token.size() > MAX_TOKEN_BYTES) return std::nullopt;
    const QList<QByteArray> parts = token.split('.');
    if (parts.size() != 3) return std::nullopt;
    const auto header = decodeJsonObject(parts[0]);
    if (!header) return std::nullopt;
    const QJsonValue alg = header->value(QStringLiteral("alg"));
    const QJsonValue kid = header->value(QStringLiteral("kid"));
    if (!alg.isString() || (!kid.isUndefined() && !kid.isString())) return std::nullopt;
    return Header{alg.toString(), kid.toString()};
}

QByteArray jwkToSpki(const QJsonObject& jwk)
{
    // A private key published in a JWKS is a misconfiguration we refuse to use.
    if (jwk.contains(QStringLiteral("d"))) return {};
    const QString kty = jwk.value(QStringLiteral("kty")).toString();
    if (kty == QLatin1String("RSA")) return rsaSpki(jwk);
    if (kty == QLatin1String("EC")) return ecSpki(jwk);
    return {};
}

Result verify(const QByteArray& token, const QJsonObject& jwks, const Expectations& expect)
{
    Result r;
    const auto header = peekHeader(token);
    if (!header) return r; // Malformed
    r.kid = header->kid;

    // The allow-list comes first: no key is looked up, nothing is decoded
    // further, for a token whose algorithm we do not run.
    if (header->alg != QLatin1String("RS256") && header->alg != QLatin1String("ES256")) {
        r.error = Error::UnsupportedAlg;
        return r;
    }

    const QList<QByteArray> parts = token.split('.');
    const auto headerObj = decodeJsonObject(parts[0]);
    if (!headerObj || headerObj->contains(QStringLiteral("crit"))) return r; // Malformed
    const auto claims = decodeJsonObject(parts[1]);
    const auto sig = base64UrlDecode(parts[2]);
    if (!claims || !sig || sig->isEmpty()) return r;

    const QList<QByteArray> keys = candidateKeys(jwks, *header);
    if (keys.isEmpty()) {
        r.error = Error::UnknownKey;
        return r;
    }

    const QByteArray input = parts[0] + '.' + parts[1];
    const bool rsa = header->alg == QLatin1String("RS256");
    bool verified = false;
    for (const QByteArray& spki : keys) {
        verified = rsa ? verifyRs256(spki, input, *sig)
                       : sig->size() == PairingCrypto::SIGNATURE_BYTES &&
                             PairingCrypto::verify(spki, input, *sig);
        if (verified) break;
    }
    if (!verified) {
        r.error = Error::BadSignature;
        return r;
    }

    // ── Claims (OpenID Connect Core §3.1.3.7) ────────────────────────────────
    const QJsonValue sub = claims->value(QStringLiteral("sub"));
    const QJsonValue exp = claims->value(QStringLiteral("exp"));
    const QJsonValue iat = claims->value(QStringLiteral("iat"));
    if (!sub.isString() || sub.toString().isEmpty() || !isNumber(exp) || !isNumber(iat)) {
        r.error = Error::MissingClaim;
        return r;
    }

    if (claims->value(QStringLiteral("iss")).toString() != expect.issuer ||
        expect.issuer.isEmpty()) {
        r.error = Error::WrongIssuer;
        return r;
    }

    QStringList audiences;
    const QJsonValue aud = claims->value(QStringLiteral("aud"));
    if (aud.isString()) {
        audiences << aud.toString();
    } else if (aud.isArray()) {
        for (const QJsonValue& a : aud.toArray())
            if (a.isString()) audiences << a.toString();
    }
    if (expect.audience.isEmpty() || !audiences.contains(expect.audience)) {
        r.error = Error::WrongAudience;
        return r;
    }
    const QJsonValue azp = claims->value(QStringLiteral("azp"));
    if ((!azp.isUndefined() && azp.toString() != expect.audience) ||
        (azp.isUndefined() && audiences.size() > 1)) {
        r.error = Error::WrongAuthorized;
        return r;
    }

    if (expect.now > seconds(exp) + expect.skew) {
        r.error = Error::Expired;
        return r;
    }
    const QJsonValue nbf = claims->value(QStringLiteral("nbf"));
    if (!nbf.isUndefined() && (!isNumber(nbf) || seconds(nbf) > expect.now + expect.skew)) {
        r.error = Error::NotYetValid;
        return r;
    }
    if (seconds(iat) > expect.now + expect.skew) {
        r.error = Error::IssuedInFuture;
        return r;
    }

    // The nonce ties this token to the authorization request this browser made;
    // without it a token issued for another login could be replayed here.
    const QJsonValue nonce = claims->value(QStringLiteral("nonce"));
    if (expect.nonce.isEmpty() || !nonce.isString() ||
        !constantTimeEquals(nonce.toString().toUtf8(), expect.nonce.toUtf8())) {
        r.error = Error::NonceMismatch;
        return r;
    }

    r.error = Error::None;
    r.claims = *claims;
    return r;
}

} // namespace JwtVerifier
