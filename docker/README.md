# MoonlightWeb in Docker

Official images for Linux servers, NAS boxes and mini PCs, built from the
[`Dockerfile`](../Dockerfile) at the repository root on **official Debian**
(`debian:trixie`), published to the GitHub Container Registry for
**`linux/amd64`** and **`linux/arm64`**.

---

## Quick start — headless Linux server (Ubuntu x64, Debian, a VM, a NUC)

Four steps. Copy them in order; the container prints the same list back at you
in `docker logs` if you lose your place.

### 1. Run it

```sh
sudo docker run -d --name moonlightweb \
  --network host \
  --cap-drop ALL --cap-add NET_BIND_SERVICE \
  -v mw-data:/data \
  --restart unless-stopped \
  ghcr.io/linckosz/moonlight-web:latest
```

`--network host` is what puts the container on your LAN — without it, it can
only reach machines by IP and will never discover them. See
[Why `--network host`](#why---network-host).

### 2. Open the ports — this is NOT automatic

The `.deb` and `.rpm` add firewall rules for you in their postinstall. **A
container cannot and must not touch the host's firewall**, so this one is
yours:

```sh
sudo ufw allow 443/tcp        # the web UI — required
sudo ufw allow 80/tcp         # HTTP-to-HTTPS redirect
sudo ufw reload
```

`firewall-cmd --permanent --add-port=443/tcp --add-port=80/tcp && firewall-cmd
--reload` on Fedora/RHEL. If `ufw status` says *inactive*, nothing is blocking
and there is nothing to do. Full table under [Ports](#ports).

### 3. Set the two credentials

```sh
sudo docker exec -it moonlightweb moonlightweb --set-admin-password
sudo docker exec     moonlightweb moonlightweb --new-pin
sudo docker exec     moonlightweb moonlightweb --status
```

`--status` prints the URL to open, the PIN, and what is still missing. Neither
credential has a default, and neither is optional — see
[First run](#first-run--the-pin-is-not-optional-here) for why a container gets
no free pass.

### 4. Pair a Sunshine machine and stream

1. Open **`https://<server-ip>`** in Chrome/Edge/Safari (use the IP, not
   `localhost` — the server is not the machine you are sitting at).
2. Accept the self-signed certificate warning. Expected on a LAN — the
   server serves a self-signed certificate unless you configure your own
   domain and certificate.
3. Type the **PIN** from step 3. One PIN per device — re-run `--new-pin` for
   the next phone or laptop.
4. Your Sunshine machines appear by themselves (mDNS). If one does not, or you
   are on a bridge network, **Add computer** → its IP address.
5. Click the machine → MoonlightWeb shows a **4-digit pairing PIN**. Type
   *that* one into Sunshine's own web UI at `https://<sunshine-machine>:47990`
   → *PIN*. (This is the reverse direction from step 3, and the one people mix
   up: MoonlightWeb issues it, Sunshine consumes it.)
6. Pick a game and launch.

---

## First run — the PIN is not optional here

On a desktop install the app trusts the machine it runs on: a browser on
`127.0.0.1` gets in without a PIN, and the setup wizard opens by itself. **A
container never sees that loopback peer.** Behind a bridge every request
arrives from the Docker gateway (`172.17.0.1`), and even on the host network a
headless server has no local browser to be trusted — so the request is remote,
`/api/setup/status` answers `401 authentication_required`, and the page loads
into an authentication wall rather than the wizard.

That is the design working as intended, not a bug: what would otherwise be a
"trusted local machine" is, in a container, every device that can reach the
port. So the first thing to do after `docker run` is `--new-pin`.

Two other commands are worth running while you are there — both talk to the
server over the container's own loopback, which is why they are allowed to:

```sh
docker exec -it moonlightweb moonlightweb --set-admin-password
```

Without an admin password **the admin page cannot be opened from anywhere** —
there is no built-in default, and the usual "you are on the machine itself"
exemption does not apply to a container. The PIN grants streaming; the password
grants administration.

Accept the self-signed certificate warning on first connection — expected on a
LAN, unless you configure your own domain and certificate.

---

## Tags

Prefer Compose: [`docker-compose.yml`](docker-compose.yml) is the quick start in
a file. [`docker-compose.bridge.yml`](docker-compose.bridge.yml) is the fallback
for platforms with no host networking — read the caveats in it first.

| Tag | What it is |
|---|---|
| `latest` | The newest release. What you want. |
| `0.4.1` | That exact release, forever. |
| `0.4` | The newest `0.4.x` — minor updates, no major jump. |
| `sha-<commit>` | One commit, pinned forever. |

**Every tag here is a release.** There is no `edge` and no nightly: an image is
published only from a `v*` tag that already passed the quality and test gates,
so `latest` can never hand you work in progress.

Development builds exist, in a **separate and private** package —
`ghcr.io/linckosz/moonlight-web-dev:dev`. It is produced only by running the
Docker workflow by hand with `publish` ticked, and pulling it needs
`docker login ghcr.io` with an account that has access: an anonymous request
for its tag list is refused with 403, where the public package answers 200.
Nothing you do with `docker pull …/moonlight-web:latest` can reach it.

**Architectures: `linux/amd64` and `linux/arm64` only.** Both are built on native
runners; no 32-bit `armv7`/`armhf` image is published. There is no hosted 32-bit
ARM runner, so it would have to be emulated under QEMU — and emulating a full
Qt + libdatachannel C++ build costs hours per commit for a target that Raspberry
Pi OS has shipped a 64-bit default for since 2022. A Pi 3, 4, 5 or Zero 2 W runs
the `arm64` image; genuinely 32-bit hardware can
[build from source](#building-it-yourself), which works — it is only the CI cost
that rules it out.

---

## Why `--network host`

MoonlightWeb is not a plain web server; it is one end of a WebRTC connection
and an mDNS client. Three things break behind a bridge, and publishing more
ports fixes none of them:

- **WebRTC media.** libdatachannel binds an **ephemeral** UDP port for the
  media and input transport, unless UPnP handed it a fixed one. An ephemeral
  port cannot be published in advance. When the browser cannot reach it, the
  stream falls back to its TCP/WebSocket transport — it works, but you pay for
  it in latency.
- **Host discovery.** Sunshine machines announce themselves over mDNS
  (multicast UDP 5353), which does not cross the default bridge. On a bridge,
  add your hosts by IP address instead — a one-time step, then everything else
  behaves normally.
- **UPnP.** The router is found by SSDP multicast — same story. Without it,
  Internet Access cannot open the per-session streaming port for you.

Host networking removes all three at once, and there is nothing to gain from
isolating a service whose whole job is to be reachable.

**This is not something the application can decide for itself.** A process
cannot move into another network namespace after it has started, so there is no
`--access-host-network` command to run and no setting to tick: it is chosen by
`docker run`, before the binary exists. What the image *can* do is tell you
which side of that line it landed on, which it does in the first lines of
`docker logs`.

### The three options, ranked

| | LAN reach | mDNS discovery | WebRTC | When |
|---|---|---|---|---|
| **`--network host`** | full | ✅ | direct | **Default. Any Linux host.** |
| **macvlan** | full, own LAN IP | ✅ | direct | You want the container isolated *and* on the LAN — a NAS with several services fighting over port 443. |
| **bridge** (published ports) | by IP only | ❌ | may fall back to TCP | Docker Desktop on macOS/Windows, rootless Podman, a NAS UI that only offers port mappings. |

macvlan gives the container its own MAC and its own address on your LAN, so
multicast works and nothing collides with the host's ports:

```sh
# Adjust parent= to your NIC (ip -br link) and the subnet to your LAN.
sudo docker network create -d macvlan \
  --subnet=10.0.0.0/24 --gateway=192.168.1.1 \
  -o parent=eth0 mw-lan

sudo docker run -d --name moonlightweb \
  --network mw-lan --ip 192.168.1.240 \
  -v mw-data:/data --restart unless-stopped \
  ghcr.io/linckosz/moonlight-web:latest
```

One caveat worth knowing before you pick it: by design, a macvlan container and
its **own host** cannot talk to each other. Fine when Sunshine runs elsewhere;
not fine when Sunshine runs on that same box — use host networking there.

If none of these are available, `docker-compose.bridge.yml` states its two
caveats up front and works for everything except discovery and the lowest
latency.

---

## Ports

### Inbound — what browsers connect to

These are the ports to publish (bridge) or to open in the **host's** firewall
(host networking), and to forward on your router if you want access from
outside the LAN.

| Port | Proto | Needed | What goes over it |
|---|---|---|---|
| **443** | TCP | **Always** | The web UI, the REST API, and the `wss://…/ws` upgrade that carries WebRTC signalling. The one port a browser cannot do without. |
| **80** | TCP | Recommended | Redirect to HTTPS, so `http://server` reaches the app instead of failing. |
| **48550–48573** | UDP | **Always** (host firewall) | WebRTC media, audio and input. Each stream slot binds `48550 + slot`, with or without UPnP, so a host firewall can let the block through; with UPnP the router forwards a port to it (the slot's own first). |
| **47999** | UDP | Legacy Internet Access | Mapped alongside 80/443 only by an install that still holds a legacy `moonlightweb.top` sub-domain. |
| *ephemeral* | UDP | Fallback | When the slot's port is already taken (a second instance on the same machine), the stream takes another free port of the block. Only when the whole block is taken does libdatachannel take whatever the kernel gives it, which a host firewall blocks. |

> **In short:** on a LAN, **443/tcp**, **80/tcp** and **48550–48573/udp**.
> The `.deb`, `.rpm` and AUR package open them for you in
> firewalld/ufw; a container does not touch the host's firewall, so that is
> yours to do.

Ports 80 and 443 are the defaults; `MW_HTTP_PORT` / `MW_HTTPS_PORT` move them.

### Outbound — what MoonlightWeb connects to

Nothing to publish, but these paths have to exist. On the host network they do.

| Destination | Port | Proto | What for |
|---|---|---|---|
| Sunshine host | 47989 | TCP | GameStream HTTP: pairing, app list, host info. |
| Sunshine host | 47984 · 47990 | TCP | GameStream HTTPS (GeForce Experience · Sunshine defaults). |
| Sunshine host | 48010 | TCP | RTSP handshake that sets the stream up. |
| Sunshine host | 47998 · 47999 · 48000 | UDP | Video · control/input · audio. |
| LAN multicast | 5353 | UDP | mDNS host discovery (host networking only). |
| LAN multicast | 1900 | UDP | SSDP, to find the UPnP router (host networking only). |
| Internet | 19302 | UDP | STUN, to learn the public address for a stream from outside the LAN. |
| Internet | 443 | TCP | Update check, and the held line to the rendezvous server when Internet Access is on. |

---

## Streaming from outside the LAN (Internet Access)

Internet Access is an opt-in consent, off by default. While it is on, the app
asks the router (UPnP, SSDP multicast — so **host networking**) to open a
streaming port during each session; whoever connects reaches this machine
directly, and each side of a peer-to-peer connection sees the other's public IP
address. No DNS record is created, no certificate is issued, and ports 80/443
stay closed:

```sh
docker exec -it moonlightweb moonlightweb --enable-internet
```

A new remote entry link — reaching the web UI itself from outside without
opening it to the internet — is in development and will be announced in the
release notes. Until then a remote browser has no way in unless you forward
TCP 443 yourself and accept the self-signed warning, or bring your own domain
and certificate.

**A container upgraded from v0.2.4 or earlier** stops using the
`moonlightweb.top` sub-domain it once registered: this version registers no
name, renews no certificate and forwards no web port. The sub-domain keeps
resolving — that is the DNS server's doing, not the container's — until the
shared service shuts down in February 2027. Nothing has to be reconfigured;
the consent checkbox is asked once more, because the wording it was ticked
under described a mechanism that no longer runs.

---

## Storage

One volume, `/data`, mounted at the container's `$HOME`. Qt derives every path
from it:

| Path inside `/data` | What lives there |
|---|---|
| `.local/share/MoonlightWeb/MoonlightWeb/settings.json` | Ports, codec, transport, admin password hash, access PIN. |
| `.local/share/MoonlightWeb/MoonlightWeb/cert/` | The TLS certificate and key the browser sees. |
| `.local/share/MoonlightWeb/MoonlightWeb/logs/` | `moonlightweb.log`, plus one file per stream worker. |
| `.config/MoonlightWeb/MoonlightWeb.conf` | Paired hosts **and the Moonlight client identity**. |

Losing the volume un-pairs every host — the identity is what Sunshine
recognises. Back it up with the container stopped:

```sh
docker run --rm -v mw-data:/data -v "$PWD:/backup" debian:trixie-slim \
  tar czf /backup/moonlightweb-data.tgz -C /data .
```

---

## Environment variables

| Variable | Default | What it does |
|---|---|---|
| `MW_HTTPS_PORT` | `443` | HTTPS listener. Written into `settings.json` on every start, so it survives the fallback the server applies when a port is taken. |
| `MW_HTTP_PORT` | `80` | HTTP listener (HTTP-to-HTTPS redirect). |
| `MW_UPNP` | `1` | `0` stops the app asking the router to map ports — set it on a bridge network, where the request cannot reach a router anyway. |
| `MW_ALLOWED_TRANSPORTS` | `all` | Which transports streams may use: `all`, `webrtc` (no WebSocket relay) or `wss` (WebSocket only, WebRTC off). Written into `settings.json` on every start; the admin page's **Allowed transports** is the same setting. |
| `MW_OIDC_ISSUER`, `MW_OIDC_CLIENT_ID` | unset | The OpenID Connect provider and the client id it knows MoonlightWeb as. Written into `settings.json` on every start; the login itself is switched on from the admin page. See [Security §6.2.3](../docs/wiki/06-Security.md#623-openid-connect-sign-in). |
| `MW_OIDC_CLIENT_SECRET`, `MW_OIDC_CLIENT_SECRET_FILE` | unset | The client secret, or the path of a file holding it (a Docker secret). Read by the server, never written to `settings.json`, and preferred over a secret saved on the admin page. Unset: a public client, protected by PKCE. |
| `MW_TRUSTED_PROXIES` | unset | The reverse proxies in front of the container (addresses or CIDRs, commas or spaces) whose `X-Forwarded-For` is believed, so each client behind them gets its own rate limit, ban and line in the sessions list. It grants no access. Written into `settings.json` on every start, and wins over it while set. See [Security §6.4a](../docs/wiki/06-Security.md#64a-behind-a-reverse-proxy-trusted_proxies). |
| `TZ` | `Etc/UTC` | Timestamps in the logs and the admin UI. |
| `MW_DOMAIN`, `MW_PDNS_TOKEN` | baked in | The rendezvous domain and the shared API key for the update relay and aggregate metrics. The official images carry the project's, exactly like the `.deb`; without them the container never contacts our infrastructure. |

Everything else is configured from the admin page, or by editing
`settings.json` in the volume and restarting — see
[`docs/wiki/07-Settings-Reference.md`](../docs/wiki/07-Settings-Reference.md).

Anything after the image name is passed straight to the binary:

```sh
docker run … ghcr.io/linckosz/moonlight-web:latest --port 8080
```

---

## Operating it

```sh
docker exec moonlightweb moonlightweb --status               # URLs, PIN, internet state
docker exec moonlightweb moonlightweb --new-pin              # let one more device in
docker exec -it moonlightweb moonlightweb --set-admin-password
docker exec -it moonlightweb moonlightweb --enable-internet  # allow the internet link
docker exec moonlightweb moonlightweb --list-sessions        # who is signed in
docker exec moonlightweb moonlightweb --revoke-session Ab3dEf_hIj-l   # sign one out
docker exec moonlightweb moonlightweb --get all              # the settings --set can change
docker exec moonlightweb moonlightweb --set transport_mode=wss --set cert_auth_enabled=on
docker logs -f moonlightweb
```

`--set` and `--get` go through the running server's admin API, like the admin
page, so a change applies at once and survives the server's next write to
`settings.json`. Editing that file in the volume while the container runs does
not: the server overwrites it. The supported keys are listed in
[Settings §7.6](../docs/wiki/07-Settings-Reference.md#76-from-a-terminal---get-and---set).
`--list-sessions --json` and `--get … --json` print JSON for scripts. Every one
of these commands exits `0` when done, `1` when the server is not running or
refused, and `2` when the command line is wrong.

The image's `HEALTHCHECK` is `moonlightweb --status`, which answers only once
the TLS listener is up — so `docker ps` showing *healthy* means a browser can
actually connect.

### Updating

`docker pull` + recreate. The volume carries the settings, the certificate and
the pairings across, so nothing is re-entered:

```sh
sudo docker pull ghcr.io/linckosz/moonlight-web:latest
sudo docker stop moonlightweb && sudo docker rm moonlightweb
sudo docker run -d --name moonlightweb … # the same command as step 1
```

With Compose: `docker compose pull && docker compose up -d`.

**The in-app update button is deliberately absent in a container.** On Windows
and macOS it downloads the installer and runs it; here that would install a
`.deb` into the image's writable layer, appear to work, and be discarded by the
next `docker run` — the version would silently go *backwards* on the next
restart. `SelfUpdater` detects the container and reports `supported: false`, so
the UI never offers a button that cannot keep its promise.

The update *check* still runs: you are told a new version exists, and the
registry is where you get it. Pin `0.2` instead of `latest` if you would rather
take minor releases and skip a major one, or a `sha-…` tag to freeze entirely.

---

## Hardware acceleration: none needed

There is nothing to pass through — no `--device /dev/dri`, no NVIDIA container
runtime, no VA-API packages.

MoonlightWeb never decodes or re-encodes the stream. Sunshine encodes on the
gaming PC's GPU; this server re-packetises the encrypted RTP into WebRTC and
the **browser** decodes it on the viewer's device. The upscaling and sharpening
(FSR1 / SGSRv1) run in the browser on WebGL2 (WebGPU in HDR), on the viewer's GPU. The
container's job is packet plumbing and TLS, and it is comfortable on a
Raspberry Pi 4 or an N100 mini PC.

Gamepads, keyboard and mouse are the same story: input is captured by the
browser, arrives over the WebRTC data channel and leaves as GameStream input
packets to Sunshine. No `/dev/input`, no `uinput`, no privileged container.

**Sunshine is not in this image and cannot be.** A container has no display to
capture. Run Sunshine on the machine with the GPU, and add it here by IP.

---

## Running as a service

`--restart unless-stopped` (or `restart: unless-stopped` in Compose) is enough
on any host where Docker itself starts at boot. For a systemd unit that owns
the Compose project instead:

```ini
# /etc/systemd/system/moonlightweb-docker.service
[Unit]
Description=MoonlightWeb (Docker Compose)
Requires=docker.service
After=docker.service network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
WorkingDirectory=/opt/moonlightweb
ExecStart=/usr/bin/docker compose up -d
ExecStop=/usr/bin/docker compose down
ExecReload=/usr/bin/docker compose pull && /usr/bin/docker compose up -d

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now moonlightweb-docker
```

Not running in Docker at all? [`../backend/packaging/systemd/moonlightweb.service`](../backend/packaging/systemd/moonlightweb.service)
is the native unit the `.deb` and `.rpm` install.

---

## Hardening, and volume permissions

### A hardened compose file

Everything the server writes goes to `/data` or to `/tmp`, so the image runs
with no capabilities at all, a read-only root filesystem and a user of your
choosing:

```yaml
services:
  moonlightweb:
    image: ghcr.io/linckosz/moonlight-web:latest
    container_name: moonlightweb
    restart: unless-stopped
    network_mode: host

    user: "1000:1000"                         # owns /opt/moonlightweb/data
    cap_drop: ["ALL"]
    security_opt: ["no-new-privileges:true"]
    read_only: true
    tmpfs:
      - /tmp                                  # Qt's runtime directory and temporary files

    volumes:
      - /opt/moonlightweb/data:/data

    environment:
      TZ: Etc/UTC
      # A user other than root cannot bind 80/443 on the host network.
      MW_HTTP_PORT: 8080
      MW_HTTPS_PORT: 8443

    healthcheck:
      test: ["CMD", "moonlightweb", "--status"]
      interval: 30s
      timeout: 10s
      start_period: 60s
      retries: 3
```

Give that user the directory **before** the first start — the container has no
privilege left to do it itself:

```sh
sudo mkdir -p /opt/moonlightweb/data
sudo chown 1000:1000 /opt/moonlightweb/data
docker compose up -d
```

The URL becomes `https://<server-ip>:8443`. Any uid works, as long as it owns
the directory; there is no user to create in the image.

**Why the ports move.** On the host network the kernel's
`net.ipv4.ip_unprivileged_port_start` is the host's (1024), and Docker does not
raise an added capability for a user other than root — `cap_add:
[NET_BIND_SERVICE]` shows up in the bounding set and nowhere else, so 443 still
refuses to bind. To keep 80/443, run as root instead: drop `user:`, add
`cap_add: ["NET_BIND_SERVICE"]`, and keep the rest (`cap_drop`, `security_opt`,
`read_only`, `tmpfs`). Root then needs the directory to be its own:
`sudo chown 0:0 /opt/moonlightweb/data`, or a named volume, which Docker creates
root-owned. On a bridge network Docker sets that sysctl to 0 inside the
container, which is why `docker-compose.bridge.yml` needs no capability.

**`read_only` needs the `tmpfs`.** Without it the entrypoint cannot create
`$XDG_RUNTIME_DIR` under `/tmp`, and says so instead of starting.

### Volume permissions

A container started with `cap_drop: ALL` cannot write to a directory its uid is
not allowed to write to — **including root**: dropping every capability drops
`CAP_DAC_OVERRIDE`, the one that lets root ignore file permissions. The usual
way to hit this is a bind mount owned by your own user (`/opt/moonlightweb/data`
created as uid 1000) with the container running as root, or the reverse: a
root-owned named volume with `user: "1000:1000"`.

The entrypoint checks before anything starts — the data directory, the
QSettings directory, every subdirectory an earlier run left there, and
`/tmp` — and stops with exit code 1 and a report like this in `docker logs`:

```text
entrypoint: cannot write to the volume at /data, where MoonlightWeb keeps
its settings, certificate and paired hosts. Not starting.

  needed      /data/.local/share/MoonlightWeb/MoonlightWeb
  found       /data  owner 1000:1000  mode 755
  error       Permission denied
  running as  uid 0, gid 0, without CAP_DAC_OVERRIDE (cap_drop: ALL):
              root can only write where uid 0 or gid 0 is allowed to

Fix one of these on the Docker host, then start the container again:

  1. Give the directory to the user the container runs as:
       sudo chown -R 0:0 <host directory mounted at /data>
       sudo chmod -R u+rwX <host directory mounted at /data>
  2. Run the container as the directory's owner instead:
       docker run --user 1000:1000 ...      compose: user: "1000:1000"
     (on the host network a user other than root cannot bind 80/443,
     even with NET_BIND_SERVICE: set MW_HTTP_PORT=8080, MW_HTTPS_PORT=8443)
  3. Give root back the right to override file permissions:
       docker run --cap-add DAC_OVERRIDE ...
       compose: cap_add: [NET_BIND_SERVICE, DAC_OVERRIDE]

See "Volume permissions" in docker/README.md.
```

In order of preference: **1** or **2** keep the container capability-free; **3**
works, but hands root back the right to write anywhere it can see. (`CHOWN` is
not needed: nothing in the image changes an owner.) A named volume has no host
directory to `chown`; fix it from a throwaway container:

```sh
docker run --rm -v mw-data:/data debian:trixie-slim chown -R 1000:1000 /data
```

With `restart: unless-stopped` Docker keeps retrying, with a back-off that grows
to a minute — the report repeats until the volume is fixed, and nothing is
written in the meantime. `docker exec … moonlightweb --status` and the other
operator commands skip the check: they talk to a running server and write
nothing.

---

## Building it yourself

```sh
git clone --recurse-submodules https://github.com/linckosz/moonlight-web
cd moonlight-web
docker build -t moonlightweb --build-arg MW_VERSION=0.0.0-dev .
```

The submodules are not optional: moonlight-common-c, libdatachannel, miniupnpc
and qmdnsengine are compiled into the binary, and `.dockerignore` sends only
`backend/`, `frontend/` and `docker/` to the daemon. `--build-arg
DEBIAN_RELEASE=bookworm` builds against Debian 12 (Qt 6.4) instead of trixie
(Qt 6.8) if you need the older base.

The build stage uses Debian's Qt 6 and OpenSSL development packages and the
runtime stage the matching runtime ones, so nothing is privately bundled:
rebuilding on a patched base is how this image gets its security updates.

---

## Troubleshooting

**The container starts and immediately exits 0.** A single-instance lock left
by a previous container. The entrypoint deletes it on every start — if you see
this, you are running the binary without the entrypoint. Delete
`/data/.local/share/MoonlightWeb/MoonlightWeb/moonlightweb.lock`.

**The container restarts, and `docker logs` says `entrypoint: cannot write to
the volume at /data`.** The volume belongs to another uid, and with
`cap_drop: ALL` not even root may write to it. The report names the owner it
found and the uid it runs as; see [Volume permissions](#volume-permissions).

**`HTTPS server started on port 8443`** (not 443). Something else on the host
already holds 443 — on host networking that is a real conflict with a reverse
proxy or another web server. Either free the port or set `MW_HTTPS_PORT` and
put your proxy in front.

**The web UI loads but the stream never starts.** The signalling worked and the
media path did not — the usual bridge-network symptom. Check
`docker logs` for the ICE state, and move to host networking.

**No hosts found.** mDNS does not cross a bridge, and some LANs block multicast
outright. Add the host by IP from *Add computer* — discovery is a convenience,
not a requirement.

**`Qt platform: offscreen (no display — headless)` in the log.** Expected. It
is what the image is supposed to say.
