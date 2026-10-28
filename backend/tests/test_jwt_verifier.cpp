/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * ID-token verification (JwtVerifier): every way a token can be refused, with
 * keys generated here. The verifier is pure, so each case is one call: the
 * token, the key set, what we expect, and the first reason to say no.
 */
#include "test_framework.h"
#include "jwt_test_keys.h"

#include "common/JwtVerifier.h"

#include <QSet>

using namespace jwttest;
using JwtVerifier::Error;

namespace {

constexpr qint64 kNow = 1'800'000'000;
const QString kIssuer = QStringLiteral("https://idp.example.com/application/o/mw/");
const QString kClient = QStringLiteral("moonlightweb");
const QString kNonce = QStringLiteral("n-0S6_WzA2Mj");

QJsonObject claims()
{
    return QJsonObject{{"iss", kIssuer},   {"sub", "user-123"},
                       {"aud", kClient},   {"exp", kNow + 300},
                       {"iat", kNow - 10}, {"nonce", kNonce},
                       {"name", "Alice"},  {"groups", QJsonArray{"streamers"}}};
}

JwtVerifier::Expectations expect()
{
    JwtVerifier::Expectations e;
    e.issuer = kIssuer;
    e.audience = kClient;
    e.nonce = kNonce;
    e.now = kNow;
    return e;
}

QJsonObject hdr(const QString& alg, const QString& kid = QStringLiteral("k1"))
{
    QJsonObject h{{"alg", alg}, {"typ", "JWT"}};
    if (!kid.isEmpty()) h["kid"] = kid;
    return h;
}

Error check(const QByteArray& token, const QJsonObject& keys,
            const JwtVerifier::Expectations& e = expect())
{
    return JwtVerifier::verify(token, keys, e).error;
}

QJsonObject with(QJsonObject o, const QString& key, const QJsonValue& v)
{
    if (v.isUndefined())
        o.remove(key);
    else
        o[key] = v;
    return o;
}

} // namespace

void run_jwt_verifier_tests()
{
    SECTION("JwtVerifier — ID token verification");

    const Key rsa = rsaKey();
    const Key rsaOther = rsaKey();
    const Key ec = ecKey();
    const QJsonObject rsaJwks = jwks({jwk(rsa, "k1")});
    const QJsonObject ecJwks = jwks({jwk(ec, "k1")});

    // ── The good paths ──────────────────────────────────────────────────────
    {
        const auto r = JwtVerifier::verify(token(rsa, hdr("RS256"), claims()), rsaJwks, expect());
        CHECK(r.ok());
        CHECK_EQ(r.claims.value("sub").toString(), QString("user-123"));
        CHECK_EQ(r.kid, QString("k1"));
        CHECK(JwtVerifier::verify(token(ec, hdr("ES256"), claims()), ecJwks, expect()).ok());
        // No kid in the header: any key of the right type may have signed it.
        CHECK_EQ(check(token(rsa, hdr("RS256", ""), claims()),
                       jwks({jwk(rsaOther, "x"), jwk(rsa, "y")})),
                 Error::None);
        // A JWKS mixing key types still finds the right one by kid.
        CHECK_EQ(check(token(ec, hdr("ES256", "e"), claims()), jwks({jwk(rsa, "r"), jwk(ec, "e")})),
                 Error::None);
        // use=sig and a matching alg on the JWK are accepted.
        QJsonObject k = jwk(rsa, "k1");
        k["use"] = "sig";
        k["alg"] = "RS256";
        CHECK_EQ(check(token(rsa, hdr("RS256"), claims()), jwks({k})), Error::None);
    }

    // ── Algorithm allow-list ────────────────────────────────────────────────
    {
        // alg=none with no signature at all: the oldest JWT attack there is.
        const QByteArray none = b64u(compact(hdr("none"))) + '.' + b64u(compact(claims())) + '.';
        CHECK_EQ(check(none, rsaJwks), Error::UnsupportedAlg);
        for (const char* alg : {"None", "NONE", "nOnE"}) {
            const QByteArray t =
                b64u(compact(hdr(alg))) + '.' + b64u(compact(claims())) + '.' + b64u("x");
            CHECK_EQ(check(t, rsaJwks), Error::UnsupportedAlg);
        }
        // Alg confusion: HS256 keyed with the provider's PUBLIC key, in each
        // form an implementation might hand an HMAC — refused before any key.
        const QByteArray spki = JwtVerifier::jwkToSpki(jwk(rsa, "k1"));
        CHECK(!spki.isEmpty());
        CHECK_EQ(check(hs256(spki, hdr("HS256"), claims()), rsaJwks), Error::UnsupportedAlg);
        CHECK_EQ(check(hs256(compact(jwk(rsa, "k1")), hdr("HS256"), claims()), rsaJwks),
                 Error::UnsupportedAlg);
        // And with a JWKS that publishes an "oct" key under that kid.
        const QJsonObject oct{{"kty", "oct"}, {"kid", "k1"}, {"k", QString(b64u("secret"))}};
        CHECK_EQ(check(hs256("secret", hdr("HS256"), claims()), jwks({oct})),
                 Error::UnsupportedAlg);
        for (const char* alg : {"HS384", "HS512", "RS384", "RS512", "PS256", "ES384", "EdDSA", ""})
            CHECK_EQ(check(token(rsa, hdr(alg), claims()), rsaJwks), Error::UnsupportedAlg);
        // alg must be a string.
        QJsonObject numeric = hdr("RS256");
        numeric["alg"] = 256;
        CHECK_EQ(check(token(rsa, numeric, claims()), rsaJwks), Error::Malformed);
    }

    // ── The key type follows the algorithm, never the key ───────────────────
    {
        // RS256 claimed, only an EC key under that kid: nothing to verify with.
        CHECK_EQ(check(token(ec, hdr("RS256"), claims()), ecJwks), Error::UnknownKey);
        // ES256 claimed, only an RSA key under that kid.
        CHECK_EQ(check(token(rsa, hdr("ES256"), claims()), rsaJwks), Error::UnknownKey);
        // A key for encryption, or announced for another algorithm, is not used.
        QJsonObject enc = jwk(rsa, "k1");
        enc["use"] = "enc";
        CHECK_EQ(check(token(rsa, hdr("RS256"), claims()), jwks({enc})), Error::UnknownKey);
        QJsonObject other = jwk(rsa, "k1");
        other["alg"] = "PS256";
        CHECK_EQ(check(token(rsa, hdr("RS256"), claims()), jwks({other})), Error::UnknownKey);
    }

    // ── Key lookup and signatures ───────────────────────────────────────────
    {
        const auto unknown =
            JwtVerifier::verify(token(rsa, hdr("RS256", "rotated"), claims()), rsaJwks, expect());
        CHECK_EQ(unknown.error, Error::UnknownKey);
        CHECK_EQ(unknown.kid, QString("rotated")); // what OidcClient refetches on
        CHECK_EQ(check(token(rsa, hdr("RS256"), claims()), QJsonObject{}), Error::UnknownKey);

        // Right kid, wrong key.
        CHECK_EQ(check(token(rsaOther, hdr("RS256"), claims()), rsaJwks), Error::BadSignature);
        CHECK_EQ(check(token(ecKey(), hdr("ES256"), claims()), ecJwks), Error::BadSignature);

        // A payload altered after signing.
        QByteArray t = token(rsa, hdr("RS256"), claims());
        QList<QByteArray> parts = t.split('.');
        parts[1] = b64u(compact(with(claims(), "sub", "admin")));
        CHECK_EQ(check(parts.join('.'), rsaJwks), Error::BadSignature);

        // A truncated RSA signature, and an ES256 signature left in DER.
        parts = t.split('.');
        parts[2] = b64u(JwtVerifier::base64UrlDecode(parts[2])->left(255));
        CHECK_EQ(check(parts.join('.'), rsaJwks), Error::BadSignature);
        const QByteArray input =
            b64u(compact(hdr("ES256"))) + '.' + b64u(compact(claims())); // signed in DER below
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        size_t len = 0;
        EVP_DigestSignInit(ctx, nullptr, EVP_sha256(), nullptr, ec.get());
        EVP_DigestSign(ctx, nullptr, &len, reinterpret_cast<const unsigned char*>(input.data()),
                       input.size());
        QByteArray der(static_cast<int>(len), '\0');
        EVP_DigestSign(ctx, reinterpret_cast<unsigned char*>(der.data()), &len,
                       reinterpret_cast<const unsigned char*>(input.data()), input.size());
        EVP_MD_CTX_free(ctx);
        der.resize(static_cast<int>(len));
        CHECK_EQ(check(input + '.' + b64u(der), ecJwks), Error::BadSignature);
    }

    // ── JWK → key ───────────────────────────────────────────────────────────
    {
        CHECK(!JwtVerifier::jwkToSpki(jwk(rsa, "a")).isEmpty());
        CHECK(!JwtVerifier::jwkToSpki(jwk(ec, "a")).isEmpty());
        // A private parameter in a public key set is refused outright.
        CHECK(JwtVerifier::jwkToSpki(with(jwk(rsa, "a"), "d", "AQAB")).isEmpty());
        CHECK(JwtVerifier::jwkToSpki(with(jwk(ec, "a"), "d", "AQAB")).isEmpty());
        // Too short a modulus.
        const Key weak = rsaKey(1024);
        CHECK(JwtVerifier::jwkToSpki(jwk(weak, "w")).isEmpty());
        CHECK_EQ(check(token(weak, hdr("RS256", "w"), claims()), jwks({jwk(weak, "w")})),
                 Error::UnknownKey);
        // Another curve, a point off the curve, a coordinate of the wrong length.
        CHECK(JwtVerifier::jwkToSpki(with(jwk(ec, "a"), "crv", "P-384")).isEmpty());
        QJsonObject off = jwk(ec, "a");
        QByteArray y = *JwtVerifier::base64UrlDecode(off["y"].toString().toLatin1());
        y[31] = static_cast<char>(y[31] ^ 0x01);
        off["y"] = QString::fromLatin1(b64u(y));
        CHECK(JwtVerifier::jwkToSpki(off).isEmpty());
        QJsonObject shortX = jwk(ec, "a");
        shortX["x"] = QString::fromLatin1(
            b64u(JwtVerifier::base64UrlDecode(shortX["x"].toString().toLatin1())->mid(1)));
        CHECK(JwtVerifier::jwkToSpki(shortX).isEmpty());
        // Unknown or missing kty, missing members, a silly exponent.
        CHECK(JwtVerifier::jwkToSpki(with(jwk(rsa, "a"), "kty", "OKP")).isEmpty());
        CHECK(JwtVerifier::jwkToSpki(with(jwk(rsa, "a"), "kty", QJsonValue())).isEmpty());
        CHECK(JwtVerifier::jwkToSpki(with(jwk(rsa, "a"), "n", QJsonValue())).isEmpty());
        CHECK(JwtVerifier::jwkToSpki(with(jwk(rsa, "a"), "e", "")).isEmpty());
        CHECK(JwtVerifier::jwkToSpki(with(jwk(rsa, "a"), "e", "AQAB+")).isEmpty());
        CHECK(JwtVerifier::jwkToSpki(with(jwk(rsa, "a"), "e", "AQABAQABAQABAQAB")).isEmpty());
    }

    // ── Malformed tokens ────────────────────────────────────────────────────
    {
        const QByteArray good = token(rsa, hdr("RS256"), claims());
        const QList<QByteArray> p = good.split('.');
        CHECK_EQ(check(p[0] + '.' + p[1], rsaJwks), Error::Malformed);
        CHECK_EQ(check(good + ".x", rsaJwks), Error::Malformed);
        CHECK_EQ(check(good + ".x.y", rsaJwks), Error::Malformed); // a JWE has five parts
        CHECK_EQ(check(QByteArray(), rsaJwks), Error::Malformed);
        CHECK_EQ(check("..", rsaJwks), Error::Malformed);
        CHECK_EQ(check(p[0] + "=." + p[1] + '.' + p[2], rsaJwks), Error::Malformed);
        CHECK_EQ(check(p[0] + '.' + p[1] + '.', rsaJwks), Error::Malformed); // empty signature
        CHECK_EQ(check(p[0] + '.' + p[1] + ".ab+/", rsaJwks), Error::Malformed);
        CHECK_EQ(check(b64u("not json") + '.' + p[1] + '.' + p[2], rsaJwks), Error::Malformed);
        CHECK_EQ(check(b64u("[1,2]") + '.' + p[1] + '.' + p[2], rsaJwks), Error::Malformed);
        CHECK_EQ(check(p[0] + '.' + b64u("\"str\"") + '.' + p[2], rsaJwks), Error::Malformed);
        QJsonObject kidNum = hdr("RS256");
        kidNum["kid"] = 7;
        CHECK_EQ(check(token(rsa, kidNum, claims()), rsaJwks), Error::Malformed);
        // crit names an extension we would have to understand: we understand none.
        QJsonObject crit = hdr("RS256");
        crit["crit"] = QJsonArray{"b64"};
        crit["b64"] = false;
        CHECK_EQ(check(token(rsa, crit, claims()), rsaJwks), Error::Malformed);
        // Oversized.
        const QByteArray huge =
            token(rsa, hdr("RS256"),
                  with(claims(), "pad", QString(JwtVerifier::MAX_TOKEN_BYTES, QLatin1Char('a'))));
        CHECK_EQ(check(huge, rsaJwks), Error::Malformed);
        CHECK(!JwtVerifier::peekHeader(huge).has_value());
    }

    // ── Issuer and audience ─────────────────────────────────────────────────
    {
        const auto t = [&](const QJsonObject& c) {
            return check(token(rsa, hdr("RS256"), c), rsaJwks);
        };
        CHECK_EQ(t(with(claims(), "iss", "https://evil.example/")), Error::WrongIssuer);
        // Exact string comparison: a missing trailing slash is another issuer.
        CHECK_EQ(t(with(claims(), "iss", "https://idp.example.com/application/o/mw")),
                 Error::WrongIssuer);
        CHECK_EQ(t(with(claims(), "iss", QJsonValue())), Error::WrongIssuer);
        JwtVerifier::Expectations noIss = expect();
        noIss.issuer.clear();
        CHECK_EQ(check(token(rsa, hdr("RS256"), with(claims(), "iss", "")), rsaJwks, noIss),
                 Error::WrongIssuer);

        CHECK_EQ(t(with(claims(), "aud", "another-client")), Error::WrongAudience);
        CHECK_EQ(t(with(claims(), "aud", QJsonValue())), Error::WrongAudience);
        CHECK_EQ(t(with(claims(), "aud", QJsonArray{"a", "b"})), Error::WrongAudience);
        CHECK_EQ(t(with(claims(), "aud", QJsonArray{kClient})), Error::None);
        // Several audiences: azp must be present and name us.
        CHECK_EQ(t(with(claims(), "aud", QJsonArray{kClient, "api"})), Error::WrongAuthorized);
        CHECK_EQ(t(with(with(claims(), "aud", QJsonArray{kClient, "api"}), "azp", kClient)),
                 Error::None);
        CHECK_EQ(t(with(claims(), "azp", "another-client")), Error::WrongAuthorized);
        CHECK_EQ(t(with(claims(), "azp", kClient)), Error::None);
        JwtVerifier::Expectations noAud = expect();
        noAud.audience.clear();
        CHECK_EQ(check(token(rsa, hdr("RS256"), with(claims(), "aud", "")), rsaJwks, noAud),
                 Error::WrongAudience);
    }

    // ── Time, with 60 s of skew either way ──────────────────────────────────
    {
        const auto t = [&](const QJsonObject& c) {
            return check(token(rsa, hdr("RS256"), c), rsaJwks);
        };
        CHECK_EQ(t(with(claims(), "exp", kNow - 60)), Error::None);
        CHECK_EQ(t(with(claims(), "exp", kNow - 61)), Error::Expired);
        CHECK_EQ(t(with(claims(), "exp", kNow - 86400)), Error::Expired);
        CHECK_EQ(t(with(claims(), "iat", kNow + 60)), Error::None);
        CHECK_EQ(t(with(claims(), "iat", kNow + 61)), Error::IssuedInFuture);
        CHECK_EQ(t(with(claims(), "nbf", kNow + 60)), Error::None);
        CHECK_EQ(t(with(claims(), "nbf", kNow + 61)), Error::NotYetValid);
        CHECK_EQ(t(with(claims(), "nbf", "soon")), Error::NotYetValid);
        JwtVerifier::Expectations strict = expect();
        strict.skew = 0;
        CHECK_EQ(check(token(rsa, hdr("RS256"), with(claims(), "exp", kNow - 1)), rsaJwks, strict),
                 Error::Expired);
    }

    // ── Required claims ─────────────────────────────────────────────────────
    {
        const auto t = [&](const QJsonObject& c) {
            return check(token(rsa, hdr("RS256"), c), rsaJwks);
        };
        CHECK_EQ(t(with(claims(), "sub", QJsonValue())), Error::MissingClaim);
        CHECK_EQ(t(with(claims(), "sub", "")), Error::MissingClaim);
        CHECK_EQ(t(with(claims(), "sub", 42)), Error::MissingClaim);
        CHECK_EQ(t(with(claims(), "exp", QJsonValue())), Error::MissingClaim);
        CHECK_EQ(t(with(claims(), "exp", "tomorrow")), Error::MissingClaim);
        CHECK_EQ(t(with(claims(), "iat", QJsonValue())), Error::MissingClaim);
    }

    // ── Nonce ───────────────────────────────────────────────────────────────
    {
        const auto t = [&](const QJsonObject& c) {
            return check(token(rsa, hdr("RS256"), c), rsaJwks);
        };
        CHECK_EQ(t(with(claims(), "nonce", "another-login")), Error::NonceMismatch);
        CHECK_EQ(t(with(claims(), "nonce", QJsonValue())), Error::NonceMismatch);
        CHECK_EQ(t(with(claims(), "nonce", kNonce + "x")), Error::NonceMismatch);
        CHECK_EQ(t(with(claims(), "nonce", 1)), Error::NonceMismatch);
        // We never verify without a nonce of our own, even if the token has none.
        JwtVerifier::Expectations noNonce = expect();
        noNonce.nonce.clear();
        CHECK_EQ(check(token(rsa, hdr("RS256"), with(claims(), "nonce", "")), rsaJwks, noNonce),
                 Error::NonceMismatch);
    }

    // ── Helpers ─────────────────────────────────────────────────────────────
    {
        CHECK_EQ(*JwtVerifier::base64UrlDecode("aGk"), QByteArray("hi"));
        CHECK_EQ(*JwtVerifier::base64UrlDecode(""), QByteArray());
        CHECK(!JwtVerifier::base64UrlDecode("aGk=").has_value());
        CHECK(!JwtVerifier::base64UrlDecode("a").has_value());
        CHECK(!JwtVerifier::base64UrlDecode("a+b/").has_value());
        CHECK(!JwtVerifier::base64UrlDecode("a b").has_value());
        CHECK(JwtVerifier::base64UrlDecode("-_-_").has_value());

        const auto h = JwtVerifier::peekHeader(token(rsa, hdr("ES256", "kid-9"), claims()));
        CHECK(h.has_value());
        CHECK_EQ(h->alg, QString("ES256"));
        CHECK_EQ(h->kid, QString("kid-9"));
        CHECK(JwtVerifier::peekHeader(token(rsa, hdr("RS256", ""), claims()))->kid.isEmpty());
        CHECK(!JwtVerifier::peekHeader("garbage").has_value());

        QSet<QString> names;
        for (Error e : {Error::None, Error::Malformed, Error::UnsupportedAlg, Error::UnknownKey,
                        Error::BadSignature, Error::WrongIssuer, Error::WrongAudience,
                        Error::WrongAuthorized, Error::Expired, Error::NotYetValid,
                        Error::IssuedInFuture, Error::MissingClaim, Error::NonceMismatch})
            names.insert(JwtVerifier::errorName(e));
        CHECK_EQ(names.size(), 13);
        CHECK(!names.contains(QStringLiteral("unknown")));
    }
}
