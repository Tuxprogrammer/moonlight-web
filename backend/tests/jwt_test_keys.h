/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * Keys and tokens for the OIDC tests, generated in the test itself so no key
 * material is ever committed: an RSA or P-256 key, its public JWK, and a JWT
 * signed with it (or deliberately mis-signed).
 */
#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageAuthenticationCode>
#include <QString>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>

#include <memory>

namespace jwttest {

struct KeyFree
{
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
using Key = std::shared_ptr<EVP_PKEY>;

inline QByteArray b64u(const QByteArray& in)
{
    return in.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

inline Key rsaKey(unsigned bits = 2048)
{
    return Key(EVP_RSA_gen(bits), KeyFree());
}

inline Key ecKey(const char* curve = "P-256")
{
    return Key(EVP_EC_gen(curve), KeyFree());
}

inline QByteArray bnParam(EVP_PKEY* key, const char* name, int pad = 0)
{
    BIGNUM* bn = nullptr;
    if (EVP_PKEY_get_bn_param(key, name, &bn) != 1) return {};
    const int len = pad > 0 ? pad : BN_num_bytes(bn);
    QByteArray out(len, '\0');
    BN_bn2binpad(bn, reinterpret_cast<unsigned char*>(out.data()), len);
    BN_free(bn);
    return out;
}

/// The public JWK of @p key, with @p kid (omitted when empty).
inline QJsonObject jwk(const Key& key, const QString& kid)
{
    QJsonObject o;
    if (EVP_PKEY_get_base_id(key.get()) == EVP_PKEY_RSA) {
        o["kty"] = "RSA";
        o["n"] = QString::fromLatin1(b64u(bnParam(key.get(), OSSL_PKEY_PARAM_RSA_N)));
        o["e"] = QString::fromLatin1(b64u(bnParam(key.get(), OSSL_PKEY_PARAM_RSA_E)));
    } else {
        o["kty"] = "EC";
        o["crv"] = "P-256";
        o["x"] = QString::fromLatin1(b64u(bnParam(key.get(), OSSL_PKEY_PARAM_EC_PUB_X, 32)));
        o["y"] = QString::fromLatin1(b64u(bnParam(key.get(), OSSL_PKEY_PARAM_EC_PUB_Y, 32)));
    }
    if (!kid.isEmpty()) o["kid"] = kid;
    return o;
}

inline QJsonObject jwks(std::initializer_list<QJsonObject> keys)
{
    QJsonArray arr;
    for (const QJsonObject& k : keys)
        arr.append(k);
    return QJsonObject{{"keys", arr}};
}

/// Raw signature of @p input: PKCS#1 v1.5 for RSA, r‖s (64 bytes) for EC.
inline QByteArray sign(const Key& key, const QByteArray& input)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    size_t len = 0;
    EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, key.get());
    EVP_DigestSign(ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(input.constData()),
                   input.size());
    QByteArray sig(static_cast<int>(len), '\0');
    EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(sig.data()), &len,
                   reinterpret_cast<const unsigned char*>(input.constData()), input.size());
    EVP_MD_CTX_free(ctx);
    sig.resize(static_cast<int>(len));
    if (EVP_PKEY_get_base_id(key.get()) != EVP_PKEY_EC) return sig;

    const unsigned char* p = reinterpret_cast<const unsigned char*>(sig.constData());
    ECDSA_SIG* es = d2i_ECDSA_SIG(nullptr, &p, sig.size());
    const BIGNUM* r = nullptr;
    const BIGNUM* s = nullptr;
    ECDSA_SIG_get0(es, &r, &s);
    QByteArray raw(64, '\0');
    BN_bn2binpad(r, reinterpret_cast<unsigned char*>(raw.data()), 32);
    BN_bn2binpad(s, reinterpret_cast<unsigned char*>(raw.data()) + 32, 32);
    ECDSA_SIG_free(es);
    return raw;
}

inline QByteArray compact(const QJsonObject& o)
{
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

/// A JWT with @p header and @p claims, signed by @p key.
inline QByteArray token(const Key& key, const QJsonObject& header, const QJsonObject& claims)
{
    const QByteArray input = b64u(compact(header)) + '.' + b64u(compact(claims));
    return input + '.' + b64u(sign(key, input));
}

/// An HS256 token whose HMAC secret is @p secret — the alg-confusion attack
/// uses the provider's public key bytes for it.
inline QByteArray hs256(const QByteArray& secret, const QJsonObject& header,
                        const QJsonObject& claims)
{
    const QByteArray input = b64u(compact(header)) + '.' + b64u(compact(claims));
    return input + '.' +
           b64u(QMessageAuthenticationCode::hash(input, secret, QCryptographicHash::Sha256));
}

} // namespace jwttest
