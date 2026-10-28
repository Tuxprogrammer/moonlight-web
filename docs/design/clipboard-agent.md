# Clipboard agent: the host clipboard of a remote Sunshine host

## The problem

Clipboard sync (`ClipboardBridge`) reads and writes **this machine's** clipboard. That is the
streamed host's only when MoonlightWeb runs on the host itself (`Session.cpp`,
`ClipboardBridge::isSelfAddress`). A server that streams other PCs gets no `clipboardcaps`: Ctrl+V
forwards the chord, and the host pastes its own clipboard. The GameStream protocol has no clipboard
channel, so something has to run on the host.

## What runs where

```
 browser ──(stream transport: clipboardpaste / clipboardset / clipboard / clipboardcaps)──┐
                                                                                          │
 MoonlightWeb server                                                                      │
 ┌───────────────────────────────── main process ───────────────────────────┐             │
 │ HttpServer  /ws/clipboard-agent ──proxy──► ClipboardAgentServer (loopback)│             │
 │                 (TLS, no Origin, token)        │        ▲                  │             │
 │ ClipboardAgentRegistry (tokens, switch, state) │        │                  │             │
 │ ClipboardAgentRouter ◄─────────────────────────┘        │                  │             │
 │        ▲  stdin/stdout JSON lines (ClipboardIpc)        │                  │             │
 └────────┼────────────────────────────────────────────────┼──────────────────┘             │
          ▼                                                │                                │
 stream worker: RemoteClipboardProvider ◄── relays (RelayClipboard) ◄────────────────────────┘
                                                           │
 Sunshine host (the user's desktop session)                │
   moonlightweb-clipboard-agent ──── wss, outbound ────────┘
       └─ wl-clipboard | Klipper | QClipboard
```

- **The agent** (`backend/clipboard-agent`, its own small executable) runs on the host, inside the
  user's graphical session, and dials **out** to the server. Nothing listens on the host, and
  nothing is installed on the viewer's machine: the browser keeps using the clipboard messages it
  already has.
- **The server** (main process) authenticates it, keeps its state, and relays.
- **The stream worker** (one process per session) holds the relays. It reaches the agent through
  the server over the worker's existing stdin/stdout JSON-line protocol.

## Who gets the agent

`ClipboardRoute` (pure, tested over every combination):

| Host | Session | Agent | Route |
|---|---|---|---|
| this machine | unrestricted | — | **local** (`ClipboardBridge`, unchanged) |
| this machine | restricted | — | none |
| remote | owner's own, unrestricted | enabled by the admin **and** connected | **agent** |
| remote | owner's own, unrestricted | disabled, or not connected | none (yet: it switches on when the agent connects) |
| remote | invited player, whatever their permissions | — | none |
| remote | restricted | — | none |

The host clipboard is an exfiltration channel: a guest never reaches it through an agent, even with
full keyboard and mouse. The local rule is left exactly as it was (it lets an *unrestricted* invited
player use this machine's clipboard).

The worker of the owner's session on a remote host gets a `RemoteClipboardProvider` in its config
(`"clipboardAgent": <available now>`). The server switches it on and off as the agent comes and
goes, and the relays tell the browser: `clipboardcaps {available:true}` when the agent arrives, and
`{available:false}` when it leaves, mid-stream.

## Protocol (`backend/src/clipboard/ClipboardAgentProtocol.h`)

`wss://<server>/ws/clipboard-agent`, header `Authorization: Bearer <token>`, one JSON object per
text frame:

| Direction | Message | Meaning |
|---|---|---|
| agent → server | `hello {version, platform, backend, agent}` | First frame. Anything else first closes with 4400. |
| server → agent | `welcome {version, watch}` | Accepted; whether to watch already. |
| server → agent | `watch {on}` | Start / stop reading the clipboard and reporting changes. |
| server → agent | `announce {}` | Report the current clipboard once (a browser connected). |
| server → agent | `set {id, text}` | Write the clipboard. |
| agent → server | `ack {id, ok}` | The write took (or not). |
| agent → server | `changed {text}` | The clipboard changed (only while watching). |
| both | `ping {}` / `pong {}` | The agent pings every 30 s; the server closes a socket silent for 95 s. |

Close codes: 4400 protocol error, 4401 token revoked or rotated, 4409 replaced by another agent for
the same host, 4426 protocol version. Text is non-empty and capped at `kMaxTextChars` (256 Ki
UTF-16 units, never splitting a surrogate pair); ids are positive integers; labels are cut to 64
printable ASCII characters before they reach a log or the admin page. Frames over 2 MiB are not
read.

**Watching is the privacy switch.** The agent reads nothing unless told to watch, and the server
says watch only while (a) a session that may use the agent is streaming that host, and (b) the admin
switch is on. So the host's clipboard leaves the host only while someone allowed to read it is
looking — the same rule as `ClipboardBridge`, which does not read the clipboard while no relay
listens.

### Why `/ws/clipboard-agent` and not `/api/…`

Every upgrade is recognized by `HttpServer` before REST routing and proxied to a loopback WebSocket
server (`/ws`, `/ws/stream`, `/ws/control`); the agent's socket follows that shape. `/api/*` is where
`RequestGuard`'s browser rules apply (cross-site, JSON body, `Host` privileges, admin key), none of
which fit a non-browser client holding a bearer token. The agent path has its own gate instead
(`RequestGuard::clipboardAgentUpgrade`, a security test):

- an `Origin` header → 403: it came from a web page, and a page has no business there whatever it
  holds (a page cannot set `Authorization` on a WebSocket anyway);
- plain HTTP from another machine → 403: the token travels in the handshake;
- an unknown token, or one for a host no longer in the list → 401;
- both count as authentication failures in `ConnectionGuard`.

The loopback server checks the token again: it is what binds the socket to its host, and the port
is reachable from the machine itself. The path is not carried through the rendezvous tunnel
(`authorizeTunnelWebSocket` only opens stream slots).

## Credentials (`ClipboardAgentRegistry`)

- One token per host: `mwca_` + 43 base64url characters (256 random bits from the system RNG).
  Shown **once**, in the answer that issues it.
- Kept as `base64url(SHA-256(token))` in `clipboard-agents.json` (AppData, next to `sessions.json`,
  written whole with `QSaveFile`, mode 0600) — like session tokens: a leaked file names no usable
  credential. A plain hash is enough for a 256-bit random secret.
- Looked up in constant time against every host (no early exit).
- **Rotating** issues a new one and disconnects the agent using the old one (4401). **Revoking**
  forgets it and disconnects. Neither touches the admin switch.
- The switch (`enabled`) is off for every host until the admin turns it on.
- Runtime state (Disconnected → Connected → Ready, platform, backend, agent version) is never
  written, except `last_seen`.

## The server's key, pinned

The server serves a self-signed certificate, so the agent pins it instead of trusting a CA. It pins
the **key** — `sha256/<base64>` of the certificate's SubjectPublicKeyInfo — not the certificate:
the local certificate is re-issued at every start with fresh LAN SANs, but keeps its key
(`CertManager::ensureLocalSslConfig`; a test checks that the pin survives a re-issue and changes
when the key file is deleted). The admin page gives the pins of both certificates the server may
present (public and local, `CertManager::keyPins`). By hand:
`openssl s_client -connect <server>:<port> </dev/null | openssl x509 -pubkey -noout | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | base64`.

With a pin, the agent gives Qt **no CA at all**, so every certificate fails verification and every
handshake stops in `sslErrors`, where the pin is checked — before the HTTP upgrade, and the token in
it, is sent. Without a pin the system's CAs and the host name must verify (a server under a real
domain certificate). Verification is never switched off.

## Routing (`ClipboardAgentRouter`, `RemoteClipboardProvider`)

Worker ⇄ server lines (`ClipboardIpc.h`):

| Direction | Line | |
|---|---|---|
| server → worker | `{"cmd":"clipboardAgent","available":b}` | at attach, then at every change |
| server → worker | `{"cmd":"clipboardText","text":s}` | the agent reported a change |
| server → worker | `{"cmd":"clipboardAck","id":n,"ok":b}` | the answer to a write |
| worker → server | `{"event":"clipboardSet","id":n,"text":s,"paste":b}` | the browser's paste or set |
| worker → server | `{"event":"clipboardAnnounce"}` | a browser (re)connected |

- `clipboardpaste` → write through the agent, **wait for the ack**, then inject the paste chord in
  the worker: the keystrokes never overtake the write. The server waits 1.5 s for the agent, the
  worker 2.5 s for the server. A refusal or no answer still injects the chord: the host pastes what
  it already held, which is what Ctrl+V did before clipboard sync existed.
- `clipboardset` → write only.
- `changed` → every attached worker on that host → `clipboard {text}` to its browser.
- The first worker attached to a host turns the agent's watch on; the last one leaving turns it off.

`RelayClipboard` is the relays' shared end: provider, availability, caps and text messages, and the
dispatch of `clipboardpaste` / `clipboardset` (`ClipboardMsg::decode`). The relays do not know which
provider they hold.

## The agent

- **Backends**, chosen at runtime from the session (`BackendSelect`, pure), with a log line naming the
  one in use; the next is tried when one fails to start:

  | Session | Order |
  |---|---|
  | Wayland | wl-clipboard (`wl-paste --watch echo`, `wl-paste`, `wl-copy`: ext-data-control-v1 or wlr-data-control; wl-clipboard ≥ 2.2 speaks the former) → Klipper over D-Bus on KDE → QClipboard through XWayland (best effort) |
  | X11 | QClipboard (XFixes reports every owner change, window or not) |
  | Windows, macOS | QClipboard (polled every 500 ms while watching on macOS) |

  Text goes through pipes, D-Bus or Qt, **never** on a command line, where other users could read
  it. GNOME offers no data-control protocol: `wl-paste --watch` exits at once there and the agent
  moves on.
- **Echo suppression** (`ClipboardEcho`): a write comes back from every backend as a change; the
  agent remembers what it wrote and does not report it, nor the same text twice. An `announce` or a
  new watch resends the current text on purpose.
- **Reconnects** with jittered exponential backoff (1 s → 60 s). It **exits for good** on a refused
  or revoked token (2), a replacing agent (3) or another protocol version (4), so it does not fill
  the server's failed-login counters; the systemd unit does not restart on those codes.
- **Logs** never contain clipboard text — lengths at most.

Configuration: `--server`, `--pin`, `--token-file`, `--backend`, or `MW_CLIPBOARD_AGENT_SERVER` /
`_PIN` / `_BACKEND` in the environment or in `~/.config/moonlightweb/clipboard-agent.env`; the token
from `~/.config/moonlightweb/clipboard-agent.token` or `MW_CLIPBOARD_AGENT_TOKEN`. The admin page's
token answer carries a setup line that writes both files owner-only.

## Limits

- Text only. The legacy in-process streaming path (`stream_worker_enabled` off) gets no agent route.
- One agent per host; a second one replaces the first.
- The agent runs as the user signed in on the host. A host at the login screen has no clipboard to
  share.
- An agent on a host that also runs MoonlightWeb itself is never used: the local bridge wins.
