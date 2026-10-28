# OpenID Connect sign-in — a fourth door, proven by the identity provider

**Status**: proposed with its implementation (backend), for review.
**Audience**: reviewers. It assumes the reader knows [Security §6.2–6.4](../wiki/06-Security.md#62-authentication)
and [MW-BIND-v1](pairing-signature.md), and nothing else about OIDC beyond the name.
**Date**: 2026-10-28.

---

## 1. Why this document exists

MoonlightWeb has three ways to get a session: the PIN, the certificate file and
the host key. All three are secrets this machine hands out and checks itself.
That is the right model for one owner and a few devices. It stops fitting when
the instance sits in a home or a small office that already runs an identity
provider (Authentik, Keycloak, Authelia, Zitadel, Pocket ID, Entra, Google...):
every person there already has an account, with MFA, and a place where an
administrator removes them when they leave. Today the only way to put that in
front of MoonlightWeb is a reverse proxy doing *forward auth*, which grants
nothing to MoonlightWeb itself: whoever reaches the port directly, or through a
misconfigured proxy, is not stopped by it, and the person still needs a PIN.

This document adds a fourth door, **OpenID Connect** (authorization code flow
with PKCE), and states what it trusts, what it checks, and what it deliberately
does not do.

The one-sentence version: **a successful OIDC login mints an ordinary
`mw_session`**, so everything downstream — sessions table, revocation, logout,
MW-BIND-v1, rate limits — keeps working unchanged, and the only new trust is in
a signature by a key the operator's provider publishes.

---

## 2. Options considered

| Option | Verdict |
|---|---|
| **OIDC, code flow + PKCE** | **Chosen.** Identity is proven by a signature, not by where the packet came from. Works with or without a proxy, on the LAN and behind one. Generic across providers. |
| Trusted header (forward auth: `X-…-User` from a proxy) | Rejected as the primary door. Trust comes from the network position of the sender: any LAN client that reaches the port directly forges an identity. It contradicts §6.2's own reasoning ("a proxy on this machine forwards the whole internet"). Possible later as an opt-in. |
| LDAP bind | Rejected: no SSO, no MFA from the provider, a new native dependency on three platforms. |
| SAML | Rejected: XML signature verification is heavy and error-prone, and buys nothing over OIDC here. |
| Plain OAuth2 | Folded into OIDC: OAuth2 authorizes, it does not identify. The ID token is the signed statement we need. |

---

## 3. The flow

```
browser                         MoonlightWeb (public_url origin)              provider
   │ GET /auth/oidc/start ─────────►│                                              │
   │                                │ state, nonce, PKCE verifier (memory, 10 min) │
   │◄── 302 authorize?…  + Set-Cookie mw_oidc (Lax, HttpOnly, Secure, /auth/oidc)  │
   │ ──────────────────────────────────────────────────────────────── authorize ──►│
   │                                                     user signs in (MFA…)      │
   │◄──────────────────────────────── 302 /auth/oidc/callback?code&state ─────────│
   │ GET /auth/oidc/callback ──────►│ take(state): single use, cookie must match   │
   │      (cross-site, top-level)   │ POST token (code, redirect_uri, verifier) ──►│
   │                                │◄─────────────────────────────── id_token ────│
   │                                │ verify id_token (JWKS, iss, aud, nonce, exp) │
   │                                │ groups → allowed? admin?                     │
   │◄── 200 tiny page, Set-Cookie mw_session (Strict), meta refresh → /            │
   │ GET / (same-site: mw_session is sent)                                         │
   │ POST /api/auth/pairing-key  (MW-BIND-v1, §8)                                  │
```

Two details decide whether this works at all:

- **The routes are outside `/api/`.** The provider sends the browser back with a
  cross-site top-level `GET`, which `RequestGuard::evaluate` rightly refuses on
  `/api/*` (403 `cross_site_request_blocked`). `/auth/oidc/*` is dispatched by
  `HttpServer::serveRequest` ahead of the static files and never behind the
  session gate — it is how a browser without a session gets one. The CSRF gate
  on the API is untouched; a test pins both halves.
- **The callback answers 200, not 302.** `mw_session` is `SameSite=Strict`. A
  redirect at the end of a chain that started on the provider's site is still a
  cross-site navigation, so `/` would arrive without the cookie and show the PIN
  page. The callback therefore answers a small page that navigates to `/` by
  `<meta http-equiv="refresh">`: a navigation *this* page starts, same-site, so
  the cookie is sent. No inline script — the CSP (`script-src 'self'`) forbids
  it, and a meta refresh needs no exception. The page also carries
  `Referrer-Policy: no-referrer` and `Cache-Control: no-store`, since its URL
  holds the code.

`mw_oidc` is `SameSite=Lax` precisely so the provider's top-level redirect back
carries it, and scoped to `Path=/auth/oidc` so it travels with nothing else.

---

## 4. What the callback checks, in order

| Step | Check | On failure |
|---|---|---|
| 1 | Not through the rendezvous tunnel, not `--noadmin`, login usable | 404 |
| 2 | `oidc|<ip>` bucket not locked out (same 3/5/10 tiers as the PIN) | `rate_limited` |
| 3 | `state` is known — and is **removed now**, whatever follows | `invalid_state` ★ |
| 4 | the attempt is under 10 minutes old | `invalid_state` ★ |
| 5 | `SHA-256(mw_oidc)` equals the hash recorded at start (constant time) | `invalid_state` ★ |
| 6 | the request is on the origin the attempt's `redirect_uri` was built on | `wrong_origin` ★ |
| 7 | the provider sent a `code`, not an `error` | `idp_error` |
| 8 | code exchange: `redirect_uri` and `code_verifier` of *this* attempt | `token_exchange` ★ (4xx) / `idp_unreachable` |
| 9 | ID token verified (§5) | `invalid_token` ★ |
| 10 | groups admit the user (§6) | `not_allowed` ★ |

★ counts against the caller's rate-limit bucket and feeds `ConnectionGuard`,
like a wrong PIN. The provider being down, or the user declining at the
provider, counts against nobody. Every refusal is a `302 /?oidc_error=<code>`
the login page can explain; nothing in it is secret.

Step 3 before anything else is what makes a callback URL **single use** even
when it is refused: a replayed or leaked `?code=…&state=…` finds nothing.
Step 5 is what makes it **this browser's**: an attacker who starts a flow and
lures a victim onto its callback (login CSRF) does not hold the victim's
`mw_oidc`, and a victim lured onto the attacker's callback does not hold the
attacker's.

The attempt table is in memory, bounded at 256 entries (the oldest gives way —
`/auth/oidc/start` is reachable without a session), and a restart abandons every
login in flight, which costs one click.

---

## 5. ID-token verification (`JwtVerifier`)

Pure: no network, no clock of its own, no state — so every refusal below is a
test with keys generated in the test.

- **Algorithms: `RS256` and `ES256` only.** `none`, every `HS*`, `PS*`, `ES384`,
  `EdDSA` are refused *before a key is looked at*. The **key type follows the
  algorithm, never the key**: an `RS256` token is only ever checked against
  `kty: RSA` keys, an `ES256` one against P-256 keys. That is what closes alg
  confusion — an `HS256` token whose HMAC secret is the provider's public key
  never reaches a verifier.
- **Keys** from the JWKS by `kid` (any compatible key when the token has none);
  `use`, when present, must be `sig`; `alg`, when present, must match. RSA
  moduli under 2048 bits, curves other than P-256, points off the curve and
  JWKs carrying private parameters are refused. JWK → `EVP_PKEY` with OpenSSL 3
  `EVP_PKEY_fromdata` plus `EVP_PKEY_public_check`. An ES256 signature is the
  raw `r‖s` pair (as in MW-BIND-v1, and verified by the same
  `PairingCrypto::verify`); a DER signature is refused.
- **Structure**: exactly three strict base64url parts (no padding, no `+`/`/`),
  JSON objects, 16 KiB at most; a header with `crit` is refused (we implement no
  extension).
- **Claims** (OpenID Connect Core §3.1.3.7): `iss` equals the discovery
  document's issuer exactly; `aud` contains the client id; `azp`, when present,
  is the client id, and is required when there are several audiences; `exp`,
  `iat` required, `nbf` optional, each with 60 s of skew; `sub` non-empty;
  `nonce` equals the attempt's, compared in constant time — and we never verify
  without a nonce of our own.

`OidcClient` caches discovery and the key set for an hour, and **fetches the
key set once more when a token names a `kid` it does not hold** — that is how a
provider's key rotation reaches us. Any other refusal stands without a refetch,
and an unknown `kid` costs exactly one extra fetch, never a loop.

---

## 6. Roles: `oidcAdmin` is not `isAdmin`

`oidc_allowed_groups` empty admits anyone the provider authenticates;
otherwise a group must be on it. A member of `oidc_admin_groups` is admitted
whether or not they are on the allow-list, and their session carries
**`oidcAdmin`**. Matching is exact and case-sensitive, on the claim named by
`oidc_groups_claim` (default `groups`; an array, or a single string).

`oidcAdmin` deliberately does **not** ride on `SessionInfo::isAdmin`. That flag
belongs to the remote admin password door (§6.2.1): changing the password,
disabling remote administration or closing its internet side calls
`demoteAdminSessions()`, and a persisted `is_admin` is discarded at load while
that door is shut. None of that has anything to say about what the provider
granted, and an operator with no admin password set at all must still be able
to have OIDC administrators.

What `oidcAdmin` grants, in `RequestGuard::Context`:

- `adminPrivilege` (the 🏠 routes), exactly like a host session or a password
  unlock — **and still only with the per-run `X-MW-Admin-Key` on writes**,
  which `/api/admin/token` now hands to an `oidcAdmin` session too;
- **never `hostMachine`**: the provider vouched for a person, which proves
  nothing about where they sit. No host key, no setup wizard.
- nothing under `--noadmin`, and nothing through the tunnel (§9).

Groups are read **once, at login**. A user removed from a group at the provider
keeps what they had until the session ends. That is why OIDC sessions have an
**absolute** lifetime (`oidc_session_ttl_hours`, default 24, 1–720) that
activity does not slide, and why it is stated here rather than hidden. The
admin's other levers are immediate: revoking the session on the admin page,
and any change to who may sign in (§7).

---

## 7. Sessions

An OIDC session is a normal `SessionInfo` with `authMethod = "oidc"`,
`subject` (`sub`), `displayName` (name → preferred_username → email → sub,
control characters dropped, 64 characters), `oidcAdmin`, and `expiresAt`. Its
cookie is the PIN's (`HttpOnly; Secure; Path=/; SameSite=Strict`) with
`Max-Age` equal to the remaining lifetime; the refresh in `/api/auth/status`
keeps that end rather than sliding to 90 days. It is persisted in
`sessions.json` like any remembered session (token hash only).

Revocation:

- the admin page's sessions table revokes it like any other;
- **a change to who may sign in ends every OIDC session** — switching the login
  off, another issuer or client id, another groups claim, other allowed or
  admin groups — the caller's own included: the old rules admitted them;
- at load, an OIDC session is dropped when its end has passed, when it has no
  end at all (a hand-edited file), or when the login is no longer usable;
- a hand-edited `oidc_admin` on a non-OIDC session is ignored.

Logout is local (`POST /api/auth/logout`, unchanged). See §11 for what that
does not do.

---

## 8. MW-BIND-v1 after an OIDC login

The callback is a navigation from the provider: it cannot carry the browser's
public key the way a PIN login's body does. So after an OIDC login the page
binds its key with the existing `POST /api/auth/pairing-key`, which it already
calls on every authenticated load.

That route only accepts a silent binding from `NetClassify::isTrustedPeer()`
(loopback, LAN, tunnel) — the rule of [pairing-signature §7](pairing-signature.md#7-migrating-existing-installations).
For an OIDC session **the provider's proof replaces the peer check, for the
first 10 minutes of the session**. The reasoning is the PIN's: binding from
anywhere is allowed at the moment the visitor has just proven who they are, and
an OIDC login is exactly that moment. The window is what keeps the "no key swap"
rule meaningful: the page binds within seconds of the callback, and a cookie
stolen later cannot attach the thief's key to a session that never bound one.
`bindSessionKey()` still refuses to replace a key already bound.

---

## 9. Where the door is offered

- **On configured origins only.** `public_url` is a list of origins
  (`https://host[:port]`, no path); each one's `<origin>/auth/oidc/callback` is
  registered with the provider. `/auth/oidc/start` builds `redirect_uri` from the
  origin that matches the request's `Host` (compared the way
  `RequestGuard::normalizeAuthority` compares authorities: lowercased, default
  port dropped). No match: 403 and no attempt is created, and
  `/api/auth/status` reports `oidc.enabled: false` for that request — the button
  is not shown where it could not work. A `redirect_uri` is never built from a
  `Host` header the operator did not list. A LAN address is offered when it is
  listed, like any other origin.
- **A flow finishes where it started.** The attempt records its `redirect_uri`;
  a callback arriving on another origin is refused (`wrong_origin`), and the code
  is exchanged with the recorded `redirect_uri`, never a recomputed one.
- **Not through the rendezvous tunnel.** A tunnelled page lives on the
  introduction server's origin, where no `redirect_uri` can point, and an
  `oidcAdmin` cookie minted on our origin says nothing about who holds it over
  there. Rendezvous visitors keep the PIN.
- **Not in booth mode** (`--noadmin`): a booth lets no new device in, through
  this door either.

---

## 10. Talking to the provider

- **https only, with one exception: plain http to a loopback host**
  (`localhost`, `127.0.0.0/8`, `::1`) — a provider on this very machine, where
  there is no wire to read. The rule applies to the issuer *and* to every
  endpoint the discovery document names; a document whose `issuer` is not the
  configured one (a trailing slash aside) is refused. Self-signed provider
  certificates are not accepted: the system trust store decides.
- No redirect is followed, no reply over 512 KiB is read, each exchange times
  out after 6 s (four of them fit in `HttpServer`'s 30 s), and no token, code or
  secret is ever logged.
- **Client authentication**: `client_secret_basic` (RFC 6749 §2.3.1, id and
  secret form-encoded before Base64) when a secret is set; otherwise a **public
  client**, `client_id` in the body, protected by PKCE alone. PKCE `S256` is
  always sent; a provider that advertises `code_challenge_methods_supported`
  without `S256` is refused rather than silently losing the protection.
- **The secret** may live in `settings.json` (`oidc_client_secret`, next to the
  HMAC key and the rendezvous token it already protects), but
  `MW_OIDC_CLIENT_SECRET` or `MW_OIDC_CLIENT_SECRET_FILE` take precedence and
  keep it out of the file. The admin API never returns it — only whether one is
  set and where it comes from.
- **LAN-only** (`MW_LAN_ONLY`) forbids contact with the project's own
  infrastructure. An identity provider is a URL the operator typed in, so it is
  allowed there, like a Sunshine host's address.

---

## 11. Threats, and what stops them

| An attacker tries to | What stops it |
|---|---|
| forge an ID token with `alg: none` or `HS256` keyed with the public key | §5 allow-list, before any key is used |
| use a token issued to another client of the same provider | `aud` / `azp` |
| replay an ID token from an earlier login | `nonce` is this attempt's, and single use |
| replay a callback URL (history, logs, shoulder) | `state` is removed on first use; the code is spent at the provider |
| log a victim in as the attacker (login CSRF) | `mw_oidc` binds the callback to the browser that started it |
| finish a flow on another origin of the same instance | the recorded `redirect_uri`, `wrong_origin` |
| point a `redirect_uri` at a host of their choosing via `Host` | only configured origins are used |
| substitute endpoints through a tampered discovery document | https, issuer equality, endpoint URL rule |
| downgrade PKCE | always S256; a provider without S256 is refused |
| brute-force or flood the callback | single-use states, bounded table, rate-limit bucket, `ConnectionGuard` |
| keep access after removal at the provider | absolute session end (24 h default); revoke on the admin page |
| bind their own MW-BIND key with a stolen cookie | 10-minute window, and no key replacement |
| ride an `oidcAdmin` cookie through the tunnel | `oidcAdmin` is honoured off the tunnel only |
| take admin writes via CSRF from another page | unchanged: cross-site gate + `X-MW-Admin-Key` |

## 12. Out of scope for v1, stated

- **RP-initiated and back-channel logout.** Logging out ends the MoonlightWeb
  session only; the provider session lives on, so the next "Sign in with…" is
  silent. A user disabled at the provider keeps their session until its end
  (§6).
- **Re-reading groups during a session** (refresh tokens, userinfo). Groups
  come from the ID token at login.
- **Rendezvous visitors.** They keep the PIN (§9). With `oidc_disable_pin`
  they have no way in at all — leave it off if you use the entry link.
- `at_hash` / `c_hash` (optional for the code flow), `private_key_jwt`,
  `client_secret_post`, encrypted ID tokens.

## 13. `oidc_disable_pin`

When the provider is meant to be the only door, `oidc_disable_pin` makes
`POST /api/auth/validate` answer **403 `pin_disabled`** for the PIN *and* the
certificate file, before either is examined (so nothing is counted against
anyone), and `/api/auth/status` says `oidc.pin_disabled: true` so the page hides
the form. It only takes effect while the login is usable: a half-configured
provider can never shut every door at once. It does not touch the host key
(that proves the host machine, not a person) nor the host machine's own
loopback access, which stays the way back in when the provider is down.
The remote admin password still promotes an existing session; it never
admitted anyone on its own.

## 14. Where it lives, and how it is tested

| Piece | Where | Tests |
|---|---|---|
| ID-token verification | `backend/src/common/JwtVerifier.{h,cpp}` | `test_jwt_verifier.cpp` (security suite) |
| Settings, URL rules, origins, attempts, groups, admin updates | `backend/src/server/OidcFlow.{h,cpp}` | `test_oidc_rules.cpp` (security suite) |
| Discovery, JWKS, token exchange | `backend/src/network/OidcClient.{h,cpp}` | `test_oidc_flow.cpp`, against a stand-in provider on 127.0.0.1 |
| The start / callback flow | `backend/src/server/OidcLogin.{h,cpp}` | `test_oidc_flow.cpp` |
| Routes and admin API | `backend/src/server/routes/OidcRoutes.cpp`, dispatch in `HttpServer::serveRequest` | E2E (below) |
| Sessions | `SessionInfo`, `AuthManager::createOidcSession()` … | `test_oidc_flow.cpp` |
| Admin privilege | `RequestGuard::Context::oidcAdmin` | `test_request_guard.cpp` (security suite) |
| PIN refusal, status, pairing-key rule | `routes/AuthRoutes.cpp` | E2E |

The flow tests cover, with keys generated in the test: a full login (public
and confidential client, env secret first), `state` replay, missing or foreign
`mw_oidc`, expired attempt, provider error, `nonce` mismatch, a token for
another login, wrong `aud`/`iss`/`azp`, expired, future `iat`, `none`,
alg confusion, an unpublished key, rotation (one refetch on an unknown `kid`,
never a loop), ES256, disallowed and admin groups, TTL, token endpoint 4xx/5xx,
oversized key set, discovery issuer mismatch and redirect, provider down, rate
limit, several origins (match, no match, cross-origin callback). The request
guard tests pin that the callback passes the cross-site check while `/api/*`
does not, and that an `oidcAdmin` session needs the admin key to write and is
never the host machine.

End to end, a small mock provider and Playwright drive Chromium and Firefox
through the real server: button → provider → callback → signed-in app; admin
group gets the admin page, a plain group does not, a disallowed group is
refused; `oidc_disable_pin` hides the PIN form; logout.
