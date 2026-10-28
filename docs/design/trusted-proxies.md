# `trusted_proxies` — the client behind a reverse proxy, for bookkeeping only

**Status**: implemented.
**Audience**: reviewers. User-facing behaviour is in
[Security §6.4a](../wiki/06-Security.md#64a-behind-a-reverse-proxy-trusted_proxies);
this page records the design and why it stays clear of identity.
**Date**: 2026-10-28.

---

## 1. The problem

Behind a reverse proxy (nginx, Caddy, Traefik, on this machine or another one
on the LAN) every request reaches MoonlightWeb from the proxy's address. Four
things then see one caller where there are many:

- the PIN and admin password rate limits (`AuthManager`, per address): one
  guesser locks every user out, the owner included;
- `ConnectionGuard`: a proxy on the LAN is fully exempt, so credential
  scanning through it is never banned; a proxy on a public address is banned
  for everybody;
- the sessions list: every row shows the proxy's address and location;
- the logs.

## 2. What exists before this change

Three readers of forwarding headers, each with its own rule:

| Where | Rule |
|---|---|
| `AuthManager::attemptAddress` | loopback peer only: `CF-Connecting-IP`, else the last `X-Forwarded-For`, else the last `Forwarded for=`. PIN buckets only. |
| `main.cpp` `streamClientKind` | any of four headers present → plan ICE for the internet. A hint, grants nothing. |
| `ShareRoutes` link builder | `X-Forwarded-Host` from a loopback or private peer names the board's origin. |

## 3. Design

**One helper, one precedence** (`backend/src/server/ClientAddress.{h,cpp}`,
pure Qt Core/Network, no state):

1. Over the rendezvous tunnel: the ICE address. No header is read.
2. Peer inside `trusted_proxies`: `X-Forwarded-For` when present, else
   `Forwarded for=`, walked **right-most untrusted** (skip entries that are
   themselves listed proxies; the first that is not is the client; all listed →
   the left-most). Malformed on the way → step 3, as if no header had come.
3. The peer. A loopback peer's PIN attempts keep the local-tunnel rule of
   §2, unchanged, so a default install behaves exactly as before.

`HttpServer` stamps each request with the result: `HttpRequest::sourceAddress`
(rate limits, bans, session address and geolocation, logs) and
`attemptAddress` (PIN and password buckets). `clientAddress` stays the socket
peer, and `RequestGuard::privilegeAddress(req)` names it as the only address a
privilege may read. The three readers of §2 now go through the helper.

**Never identity, never privilege.** A *trusted header* that authenticates a
user (forward auth) would take trust from where the packet came from, which is
the objection Security §6.2 makes to a proxy on this machine: it forwards the
whole Internet. This design believes a listed proxy about one thing only: whose
counter a request lands on. Every check of where the caller
sits (`isLocalRequest`, `peerLocal`, local and admin privilege, `hostTrusted`,
`canUnlockAdmin`, the pairing-key and host-key gates, the gamepad-driver offer,
every LAN-only route) keeps reading the peer.

**ConnectionGuard.**

- A listed proxy is exempt from both detectors: its traffic is its clients'.
- Failures are reported against the forwarded client. A banned client is
  refused per request (`allowRequest`, **429** with `Retry-After`, REST and
  WebSocket upgrade), because `accept()` only sees the proxy.
- A forwarded private client stays exempt, as on the LAN: a correctly appending
  proxy wrote that address after anything the client sent, so the client is on
  the LAN. An internet client cannot reach the exemption.
- Keys are canonical, so `::ffff:203.0.113.5` and `203.0.113.5` are one IP.

**Choices.**

- `X-Forwarded-For` before `Forwarded`: it is what nginx, Caddy, Traefik,
  HAProxy and Apache write by default. A proxy that writes only `Forwarded` must
  strip a client's `X-Forwarded-For` (documented).
- `CF-Connecting-IP` is not believed from a listed proxy: it is a single value
  that a proxy which does not strip it passes through unchanged.
- `/0` is refused (it would believe every client's header), at most 64 entries,
  host bits cleared rather than refused, IPv4-mapped networks stored as IPv4.
- Duplicate header lines: the parser keeps the last one, which is the line a
  proxy adds when it does not append.

## 4. Settings

`trusted_proxies` in `settings.json` (array, default `[]`, seeded), read and
written by `GET/POST /api/admin/settings` (validated whole, stored canonical,
applied to the next request), and the admin page's Advanced section.
`MW_TRUSTED_PROXIES` wins over the file while set; the POST then answers 409.
The Docker entrypoint writes the variable into `settings.json` on every start.

## 5. Tests

- `test_client_address.cpp`: CIDR parsing and matching (IPv4, IPv6, mapped,
  odd prefixes, refusals), node parsing (ports, brackets, quotes, `unknown`,
  obfuscated identifiers), `Forwarded` tokenising (quoted separators),
  right-most-untrusted with chains, junk and spoof attempts, the full
  precedence including the unchanged default and the tunnel.
- `test_connection_guard.cpp`: proxy exemption, per-client bans and
  `allowRequest`, the private-client decision, canonical keys.
- `test_api_csrf.cpp`: wire requests from a listed proxy claiming `127.0.0.1`
  and `::1` gain no privilege; `stamp()` changes nothing but the two
  bookkeeping fields.
- `test_app_settings.cpp`: validation, canonical storage, hand-edited files,
  the environment's precedence.
- `test_auth_manager.cpp`: the local-tunnel rule's existing cases, unchanged.
