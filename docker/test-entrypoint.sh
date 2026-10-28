#!/usr/bin/env bash
# =============================================================================
#  Tests for docker/entrypoint.sh: the volume preflight, and that nothing
#  around it changed. No image build needed.
#
#      bash docker/test-entrypoint.sh
#
#  Part 1 runs the entrypoint on this machine, against a stub server, with
#  directories made unwritable by chmod. It needs a non-root user (root ignores
#  the modes) and is skipped as root.
#
#  Part 2 runs it in a debian:trixie-slim container with the stub mounted where
#  the server lives, for what only a container can show: root with
#  `--cap-drop ALL` on a volume owned by another uid, `--user`, a read-only
#  root filesystem and a read-only mount. Skipped without a usable docker, or
#  with MW_TEST_DOCKER=0; MW_TEST_IMAGE picks another base image.
#
#  Every run is bounded by `timeout`: an entrypoint that loops fails the test
#  instead of hanging it.
# =============================================================================
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENTRYPOINT="$HERE/entrypoint.sh"
IMAGE="${MW_TEST_IMAGE:-debian:trixie-slim}"

pass=0
fail=0
n=0
ok() {
    n=$((n + 1))
    pass=$((pass + 1))
    echo "ok $n - $1"
}
not_ok() {
    n=$((n + 1))
    fail=$((fail + 1))
    echo "not ok $n - $1"
    [ -z "${2:-}" ] || printf '%s\n' "$2" | sed 's/^/#   /'
}
# check <description> <command…>: passes when the command succeeds.
check() {
    _desc=$1
    shift
    if "$@"; then ok "$_desc"; else not_ok "$_desc" "$out"; fi
}
has() { case "$out" in *"$1"*) return 0 ;; *) return 1 ;; esac }
lacks() { ! has "$1"; }
# The two outcomes: refused (exit 1, server never ran) or started (exit 0).
refused() { [ "$rc" -eq 1 ] && lacks stub-ran && { [ -z "${1:-}" ] || has "$1"; }; }
started() { [ "$rc" -eq 0 ] && { [ -z "${1:-}" ] || has "$1"; }; }

TMP="$(mktemp -d)"
uid="$(id -u)"
gid="$(id -g)"

# The stub stands in for the server: it says it ran, as whom and with what,
# then proves the preflight was right by writing where the server would.
STUB="$TMP/MoonlightWeb"
cat > "$STUB" << 'EOF'
#!/bin/sh
echo "stub-ran uid=$(id -u) args=$*"
echo ok > "$HOME/.local/share/MoonlightWeb/MoonlightWeb/stub-wrote" && echo stub-wrote
EOF
chmod 755 "$STUB"

# ── Part 1: on this machine ──────────────────────────────────────────────────
# The entrypoint execs a fixed path; this copy execs the stub instead.
EP="$TMP/entrypoint.sh"
sed "s|^APP=/opt/moonlightweb/MoonlightWeb\$|APP=$STUB|" "$ENTRYPOINT" > "$EP"
if ! grep -q "^APP=$STUB\$" "$EP"; then
    not_ok "the test can point the entrypoint at a stub (APP= line not found)"
fi

# run <home> [args…]: the entrypoint with a clean environment; sets out, rc and
# rt (the runtime directory it was given).
runs=0
run() {
    _home=$1
    shift
    runs=$((runs + 1))
    rt="${XDG_RUN:-$TMP/run-$runs/runtime}"
    out=$(env -i PATH="$PATH" HOME="$_home" XDG_RUNTIME_DIR="$rt" \
        HOSTNAME=mw-test MW_HTTPS_PORT="${MW_HTTPS_PORT:-}" \
        timeout 20 sh "$EP" "$@" 2>&1)
    rc=$?
}
data() { echo "$1/.local/share/MoonlightWeb/MoonlightWeb"; }
no_probe_left() { [ -z "$(find "$1" -name '.moonlightweb-write-test.*' 2>/dev/null)" ]; }

if [ "$uid" -eq 0 ]; then
    echo "# part 1 skipped: running as root, which ignores directory modes"
else
    h="$TMP/fresh"
    mkdir -p "$h"
    run "$h"
    check "a fresh writable volume starts the server" started
    check "the server ran and could write its data directory" has stub-wrote
    check "the AppData and QSettings directories are created" \
        test -d "$(data "$h")" -a -d "$h/.config/MoonlightWeb"
    check "the runtime directory is private (700)" [ "$(stat -c %a "$rt" 2>/dev/null)" = 700 ]
    check "no write probe is left behind" no_probe_left "$h"
    run "$h"
    check "a second start on the same volume starts again" started stub-wrote

    h="$TMP/ro-home"
    mkdir -p "$h"
    chmod 555 "$h"
    run "$h"
    check "an unwritable volume exits 1 without starting the server" refused
    check "... naming the directory it needed" has "needed      $(data "$h")"
    check "... and the owner and mode it found" has "found       $h  owner $uid:$gid  mode 555"
    check "... and the reason" has "error       Permission denied"
    check "... and who the container runs as" has "running as  uid $uid, gid $gid"
    check "... and the chown fix" has "sudo chown -R $uid:$gid <host directory mounted at $h>"
    check "... with no --user fix when we already own it" lacks "--user"
    check "... and no capability fix when not root" lacks DAC_OVERRIDE
    check "... and the README section" has '"Volume permissions" in docker/README.md'
    check "... and nothing else: no repeated mkdir error" lacks "mkdir:"

    run "$h" --status
    check "operator commands skip the preflight (--status still runs)" has "stub-ran uid=$uid args=--status"

    h="$TMP/ro-data"
    mkdir -p "$(data "$h")"
    chmod 555 "$(data "$h")"
    run "$h"
    check "an existing but unwritable AppData directory is refused" \
        refused "needed      $(data "$h")"

    h="$TMP/ro-config"
    mkdir -p "$h/.config/MoonlightWeb"
    chmod 555 "$h/.config/MoonlightWeb"
    run "$h"
    check "an unwritable QSettings directory is refused" \
        refused "needed      $h/.config/MoonlightWeb"

    h="$TMP/ro-cert"
    mkdir -p "$(data "$h")/cert"
    chmod 555 "$(data "$h")/cert"
    run "$h"
    check "an unwritable subdirectory left by an earlier run (cert/) is refused" \
        refused "needed      $(data "$h")/cert"
    check "... and the probe left nothing behind" no_probe_left "$h"

    h="$TMP/ro-deep"
    mkdir -p "$h/.config/MoonlightWeb/a/b"
    chmod 555 "$h/.config/MoonlightWeb/a/b"
    run "$h"
    check "a deeper unwritable subdirectory is refused too" \
        refused "needed      $h/.config/MoonlightWeb/a/b"

    h="$TMP/runtime"
    mkdir -p "$h" "$TMP/ro-tmp"
    chmod 555 "$TMP/ro-tmp"
    XDG_RUN="$TMP/ro-tmp/runtime" run "$h"
    check "an uncreatable runtime directory exits 1, pointing at a tmpfs for /tmp" \
        refused "--tmpfs /tmp"

    if command -v jq > /dev/null; then
        h="$TMP/ports"
        mkdir -p "$h"
        MW_HTTPS_PORT=8443 run "$h"
        check "MW_HTTPS_PORT is still applied once the preflight passes" \
            [ "$(jq -r .https_port "$(data "$h")/settings.json" 2>/dev/null)" = 8443 ]
    else
        echo "# jq not installed: settings test skipped"
    fi

    chmod -R u+w "$TMP"
fi

# ── Part 2: in a container ───────────────────────────────────────────────────
docker_ok() {
    [ "${MW_TEST_DOCKER:-1}" != 0 ] && command -v docker > /dev/null && docker info > /dev/null 2>&1
}
# A directory under $TMP owned by 4242:4242, made by a container so that the
# owner is "someone else" whoever runs this.
volume() {
    mkdir -p "$TMP/vol"
    docker run --rm -v "$TMP/vol:/v" "$IMAGE" sh -c "mkdir /v/$1 && chown 4242:4242 /v/$1"
    echo "$TMP/vol/$1"
}
# drun <docker run options…> -- : the entrypoint and the stub in a container.
drun() {
    out=$(timeout 60 docker run --rm \
        -v "$ENTRYPOINT:/usr/local/bin/entrypoint.sh:ro" \
        -v "$STUB:/opt/moonlightweb/MoonlightWeb:ro" \
        -e HOME=/data -e XDG_RUNTIME_DIR=/tmp/runtime-moonlightweb \
        --entrypoint sh "$@" "$IMAGE" /usr/local/bin/entrypoint.sh 2>&1)
    rc=$?
}

have_image() { docker image inspect "$IMAGE" > /dev/null 2>&1 || docker pull -q "$IMAGE" > /dev/null; }
if docker_ok && have_image; then
    v=$(volume capless)
    drun --cap-drop ALL -v "$v:/data"
    check "root with cap_drop ALL on another uid's volume exits 1" refused
    check "... saying root lacks CAP_DAC_OVERRIDE" has "running as  uid 0, gid 0, without CAP_DAC_OVERRIDE"
    check "... with the owner it found" has "found       /data  owner 4242:4242  mode 755"
    check "... suggesting the owner as --user" has 'compose: user: "4242:4242"'
    check "... suggesting DAC_OVERRIDE" has "compose: cap_add: [NET_BIND_SERVICE, DAC_OVERRIDE]"

    drun --cap-drop ALL --cap-add DAC_OVERRIDE -v "$v:/data"
    check "... and with DAC_OVERRIDE added back, it starts" started stub-wrote

    v=$(volume user)
    drun --user 4242:4242 --cap-drop ALL --security-opt no-new-privileges:true -v "$v:/data"
    check "--user matching the volume's owner starts" started "stub-ran uid=4242"

    v=$(volume rootfs)
    drun --user 4242:4242 --cap-drop ALL --read-only --tmpfs /tmp -v "$v:/data"
    check "a read-only root filesystem with a tmpfs /tmp starts" started stub-wrote

    drun --user 4242:4242 --cap-drop ALL --read-only -v "$v:/data"
    check "a read-only root filesystem without a tmpfs exits 1" refused "Read-only file system"
    check "... asking for a tmpfs on /tmp" has "--tmpfs /tmp"

    drun --user 4242:4242 --cap-drop ALL -v "$TMP/vol/rootfs:/data:ro"
    check "a volume mounted :ro is refused as read-only" refused "remove :ro"

    # Hand the files back so they can be deleted from here.
    docker run --rm -v "$TMP/vol:/v" "$IMAGE" chown -R "$uid:$gid" /v
else
    echo "# part 2 skipped: no usable docker (or MW_TEST_DOCKER=0)"
fi

[ -n "$TMP" ] && [ -d "$TMP" ] && rm -rf -- "$TMP"

echo "# $pass passed, $fail failed"
[ "$fail" -eq 0 ] && [ "$n" -gt 0 ]
