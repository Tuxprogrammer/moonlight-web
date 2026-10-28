#!/bin/sh
# =============================================================================
#  MoonlightWeb container entrypoint.
#
#  Three jobs, then it gets out of the way with exec so the server is PID 1 and
#  receives Docker's signals directly:
#
#    1. make $HOME (the mounted volume) usable, or say exactly why it is not,
#    2. apply the handful of settings that have to be decided before the server
#       starts — the listener ports and UPnP — from environment variables,
#    3. drop the single-instance lock left behind by a container that was
#       killed rather than stopped.
#
#  Anything after the image name is passed straight through to the binary, so
#  `docker run … moonlightweb-image --port 8080` and
#  `docker exec <c> moonlightweb --status` both behave like the packaged app.
# =============================================================================
set -eu

APP=/opt/moonlightweb/MoonlightWeb
# Qt's AppDataLocation for organization=application=MoonlightWeb, under the
# $HOME the image sets to the volume. settings.json, cert/, the access PIN and
# the logs all live here; QSettings (paired hosts + the Moonlight client
# identity) sits next door in $HOME/.config/MoonlightWeb.
DATA="$HOME/.local/share/MoonlightWeb/MoonlightWeb"
SETTINGS="$DATA/settings.json"

# ── Operator commands run against the *other* process ────────────────────────
# `--status`, `--new-pin`, `--set-admin-password`, `--clear-admin-password` and
# `--enable-internet` talk to the instance that is already running and exit.
# None of the preparation
# below applies to them — and the lock cleanup would be actively wrong, since
# the lock they must not disturb belongs to a live server.
for arg in "$@"; do
    case "$arg" in
        --status|--new-pin|--set-admin-password|--clear-admin-password|--enable-internet|--version|-v|--help|-h)
            exec "$APP" "$@"
            ;;
    esac
done

# ── The volume must be writable, or nothing starts ───────────────────────────
# Everything that has to survive a `docker rm` lives under $HOME, so a volume
# the container cannot write to is fatal. The usual cause is a bind mount owned
# by another uid while root runs with cap_drop: ALL: that removes
# CAP_DAC_OVERRIDE, the capability that lets root write where its uid may not.
# It used to show as a bare "mkdir: Permission denied", repeated by the restart
# policy forever; it is now one report saying who we are, what we found and
# what to change, then exit 1.
_uid=$(id -u)
_gid=$(id -g)

# Creates $1 if needed and proves a file can be made in it, by making one. On
# failure _why holds the system's reason (Permission denied, Read-only …).
probe_dir() {
    _why=''
    if ! _out=$(mkdir -p "$1" 2>&1); then
        _why=${_out##*: }
        return 1
    fi
    _probe="$1/.moonlightweb-write-test.$$"
    if ! _out=$(touch "$_probe" 2>&1); then
        _why=${_out##*: }
        return 1
    fi
    rm -f "$_probe"
}

# Bit 1 of the effective capability set is CAP_DAC_OVERRIDE.
has_dac_override() {
    _eff=$(sed -n 's/^CapEff:[[:space:]]*//p' /proc/self/status 2>/dev/null) || return 1
    case "$_eff" in '' | *[!0-9a-fA-F]*) return 1 ;; esac
    [ $((0x$_eff & 2)) -ne 0 ]
}

# The deepest part of $1 that exists: whose directory refused us.
nearest_existing() {
    _p=$1
    while [ ! -e "$_p" ] && [ "$_p" != / ]; do
        _p=$(dirname "$_p")
    done
    printf '%s\n' "$_p"
}

refuse_unwritable() {
    _dir=$1 _reason=$2
    _at=$(nearest_existing "$_dir")
    _owner=$(stat -c '%u:%g' "$_at" 2>/dev/null) || _owner='?'
    _mode=$(stat -c '%a' "$_at" 2>/dev/null) || _mode='?'
    _root_capless=no
    [ "$_uid" -eq 0 ] && ! has_dac_override && _root_capless=yes
    {
        echo "entrypoint: cannot write to the volume at $HOME, where MoonlightWeb keeps"
        echo "its settings, certificate and paired hosts. Not starting."
        echo
        echo "  needed      $_dir"
        echo "  found       $_at  owner $_owner  mode $_mode"
        echo "  error       ${_reason:-not writable}"
        if [ "$_root_capless" = yes ]; then
            echo "  running as  uid 0, gid $_gid, without CAP_DAC_OVERRIDE (cap_drop: ALL):"
            echo "              root can only write where uid 0 or gid $_gid is allowed to"
        else
            echo "  running as  uid $_uid, gid $_gid"
        fi
        echo
        case "$_reason" in
            *Read-only*)
                echo "The mount itself is read-only: remove :ro (or read_only: true) from"
                echo "the $HOME volume. If that is not it, see below."
                echo
                ;;
        esac
        echo "Fix one of these on the Docker host, then start the container again:"
        echo
        echo "  1. Give the directory to the user the container runs as:"
        echo "       sudo chown -R $_uid:$_gid <host directory mounted at $HOME>"
        echo "       sudo chmod -R u+rwX <host directory mounted at $HOME>"
        case "$_owner" in
            "$_uid:"* | '?' | 0:*) ;;
            *)
                echo "  2. Run the container as the directory's owner instead:"
                echo "       docker run --user $_owner ...      compose: user: \"$_owner\""
                echo "     (on the host network a user other than root cannot bind 80/443,"
                echo "     even with NET_BIND_SERVICE: set MW_HTTP_PORT=8080, MW_HTTPS_PORT=8443)"
                ;;
        esac
        if [ "$_root_capless" = yes ]; then
            echo "  3. Give root back the right to override file permissions:"
            echo "       docker run --cap-add DAC_OVERRIDE ..."
            echo "       compose: cap_add: [NET_BIND_SERVICE, DAC_OVERRIDE]"
        fi
        echo
        echo "See \"Volume permissions\" in docker/README.md."
    } >&2
    exit 1
}

for _dir in "$DATA" "$HOME/.config/MoonlightWeb"; do
    probe_dir "$_dir" || refuse_unwritable "$_dir" "$_why"
done
# What a previous run left below them (cert/, logs/, …) counts too: a backup
# restored as another user would otherwise pass here and fail mid-start.
_bad=$(find "$HOME/.local/share/MoonlightWeb" "$HOME/.config/MoonlightWeb" \
    -mindepth 1 -type d ! -writable -print 2>/dev/null | head -n 1)
if [ -n "$_bad" ]; then
    probe_dir "$_bad" || true
    refuse_unwritable "$_bad" "$_why"
fi

# Qt's runtime directory is on the image's own filesystem, not the volume: a
# read-only root filesystem needs a tmpfs on /tmp for it.
if ! probe_dir "$XDG_RUNTIME_DIR" || ! chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null; then
    {
        echo "entrypoint: cannot prepare $XDG_RUNTIME_DIR (${_why:-chmod failed}),"
        echo "running as uid $_uid, gid $_gid. Not starting."
        echo
        echo "With a read-only root filesystem, give the container a writable /tmp:"
        echo "  docker run --read-only --tmpfs /tmp ...      compose: tmpfs: [/tmp]"
    } >&2
    exit 1
fi

# ── Settings that must be decided before the listeners bind ──────────────────
# Everything else is configured from the admin page or by editing settings.json
# in the volume; these three are the ones you cannot reach the admin page
# without having set correctly, so they are worth an environment variable.
#
# Re-applied on every start rather than seeded once: the server writes the port
# it actually bound back into settings.json, so a seed-if-absent rule would let
# a fallback port outlive the compose file that asked for 443.
apply_setting() {
    _key=$1 _value=$2
    [ -n "$_value" ] || return 0
    [ -f "$SETTINGS" ] || echo '{}' > "$SETTINGS"
    _tmp="$SETTINGS.tmp"
    if jq --argjson v "$_value" ".\"$_key\" = \$v" "$SETTINGS" > "$_tmp" 2>/dev/null; then
        mv "$_tmp" "$SETTINGS"
    else
        rm -f "$_tmp"
        echo "entrypoint: could not set $_key in $SETTINGS (is it valid JSON?)" >&2
    fi
}

# Same, for a string: the value is passed as a JSON string, never parsed as JSON.
apply_string_setting() {
    _key=$1 _value=$2
    [ -n "$_value" ] || return 0
    [ -f "$SETTINGS" ] || echo '{}' > "$SETTINGS"
    _tmp="$SETTINGS.tmp"
    if jq --arg v "$_value" ".\"$_key\" = \$v" "$SETTINGS" > "$_tmp" 2>/dev/null; then
        mv "$_tmp" "$SETTINGS"
    else
        rm -f "$_tmp"
        echo "entrypoint: could not set $_key in $SETTINGS (is it valid JSON?)" >&2
    fi
}

apply_setting http_port "${MW_HTTP_PORT:-}"
apply_setting https_port "${MW_HTTPS_PORT:-}"
# UPnP port mapping is how a stream reaches a browser outside the LAN without
# hand-written router rules, and it is driven by SSDP multicast — which reaches
# a router from the host network and from nowhere else. Left on by default (the
# image is meant to be run with --network host); MW_UPNP=0 stops a bridged
# container from asking a router it cannot see to map ports for it. The
# capability probe InternetAccessManager runs at startup is separate and always
# runs, so "No UPnP devices found" in the log is expected either way.
case "${MW_UPNP:-}" in
    0|no|false) apply_setting upnp_enabled false ;;
    1|yes|true) apply_setting upnp_enabled true ;;
esac
# Which transports streams may use: all (default), webrtc, or wss — "wss"
# turns WebRTC off for a LAN-only container, "webrtc" takes the WebSocket relay
# away. The server applies MW_ALLOWED_TRANSPORTS itself too; refused here first
# so a typo shows at the top of `docker logs`, not as a policy that never came.
_transports=$(printf '%s' "${MW_ALLOWED_TRANSPORTS:-}" | tr 'A-Z' 'a-z')
case "$_transports" in
    "") ;;
    all|webrtc|wss) apply_setting allowed_transports "\"$_transports\"" ;;
    *) echo "entrypoint: MW_ALLOWED_TRANSPORTS=$MW_ALLOWED_TRANSPORTS ignored (all, webrtc or wss)" >&2 ;;
esac

# OpenID Connect login (docs/design/oidc-login.md): the provider and the client
# it knows us as, so a compose file can carry them. Everything else, enabling
# the login included, is on the admin page. The client secret is NOT written
# here: the server reads MW_OIDC_CLIENT_SECRET or MW_OIDC_CLIENT_SECRET_FILE
# (a Docker secret) itself, and it never reaches settings.json.
apply_string_setting oidc_issuer "${MW_OIDC_ISSUER:-}"
apply_string_setting oidc_client_id "${MW_OIDC_CLIENT_ID:-}"

# ── Stale single-instance lock ───────────────────────────────────────────────
# main.cpp guards against a second server with a QLockFile whose staleness test
# is "is that PID still alive". In a container the server is PID 1 — so after a
# `docker kill`, an OOM kill or a host reboot, the lock left on the volume names
# PID 1 and the *replacement* container is also PID 1. The new server would
# conclude that it is the second instance and exit 0, and the container would
# quietly refuse to come up until someone deleted the file by hand.
#
# Safe to delete unconditionally at this point: the entrypoint runs once per
# container start, before any server exists, and the operator commands above
# already left via exec.
rm -f "$DATA/moonlightweb.lock" "$DATA/moonlightweb-tray.lock"

# ── What the operator still has to do ────────────────────────────────────────
# A container cannot finish its own setup. Two of the three steps below need a
# human (a password, and a PIN typed into a browser), and the third — putting
# the container on the LAN — was decided by `docker run` before this process
# existed: nothing the app can do about its own network namespace afterwards.
#
# So it says so, once, at the top of `docker logs`, rather than leaving someone
# to discover it from a page that will not load. Printed before exec, so it is
# the first thing in the log and never buried under the server's own output.
#
# Network mode is a heuristic, and phrased as one. A bridged container sees
# exactly one interface of its own; sharing the host's namespace means seeing
# the host's — the docker/podman bridge among them, which is the tell.
lan_mode() {
    for _if in /sys/class/net/*; do
        case "${_if##*/}" in
            lo | eth0) ;;
            docker0 | br-* | cni-* | podman*) echo host; return ;;
            *) echo host; return ;;
        esac
    done
    echo bridge
}

# Only the persisted half is checkable here: the access PIN is single-use and
# lives in memory, so it is always worth (re)issuing. `admin_password` is a
# settings key, so a second start does not nag about something already done.
_admin_set=no
[ -f "$SETTINGS" ] && jq -e 'has("admin_password")' "$SETTINGS" > /dev/null 2>&1 && _admin_set=yes

# The container id, unless --hostname was given. Not as pretty as the name the
# operator chose — which the container has no way to know — but it is what
# `docker exec` accepts, so every line below is copy-pasteable as-is.
# shellcheck disable=SC3028  # HOSTNAME is not a POSIX shell variable, but Docker
# puts it in the environment of every container, so this is an env var like any
# other here. /etc/hostname covers a runtime that does not.
_name="${HOSTNAME:-}"
[ -n "$_name" ] || _name="$(cat /etc/hostname 2>/dev/null)"
[ -n "$_name" ] || _name="<container>"

# Drawn through one padding helper rather than by hand: every line is one
# `%-69s`, so the right-hand border cannot drift when a container id is a
# different length or a sentence gets reworded.
#
# KEEP THE TEXT PASSED TO row() PURE ASCII. dash's printf pads to a byte count,
# not a column count, so one em dash (3 bytes, 1 column) pulls that line's
# border two columns left. The box characters themselves are safe: they live in
# the format string, at a fixed position, and are never padded.
# 71 = the 69-wide text field plus the space either side of it. Built rather
# than typed: a hand-counted run of box characters is the one thing here that
# would silently go crooked, and nobody re-counts it after an edit.
_bar=''
_i=0
while [ "$_i" -lt 71 ]; do
    _bar="$_bar─"
    _i=$((_i + 1))
done
row() { printf '  │ %-69s │\n' "$1"; }
echo
printf '  ┌%s┐\n' "$_bar"
if [ "$(lan_mode)" = host ]; then
    row "Network   host: this container is ON your LAN."
    row "          Sunshine machines are discovered automatically (mDNS)."
else
    row "Network   bridge: this container is NOT on your LAN."
    row "          Other machines are reachable by IP but will NOT be"
    row "          discovered: mDNS is multicast and stops at the bridge,"
    row "          and WebRTC media may fall back to TCP (more latency)."
    row "          Fix: recreate with --network host (Linux hosts), or use"
    row "          a macvlan network. See docker/README.md."
fi
printf '  ├%s┤\n' "$_bar"
row "Still to do, on the Docker host:"
row ""
[ "$_admin_set" = yes ] || row "  docker exec -it $_name moonlightweb --set-admin-password"
row "  docker exec $_name moonlightweb --new-pin"
row "  docker exec $_name moonlightweb --status"
row ""
row "--status prints the URL to open. The PIN is single-use: one per"
row "device. Without it the page stops at an authentication wall: a"
row "container has no trusted local browser to let in."
printf '  └%s┘\n' "$_bar"
echo

exec "$APP" "$@"
