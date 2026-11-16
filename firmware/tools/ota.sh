#!/bin/bash
# Over-the-air update from the Mac: build, upload, wait for the restart, check that the new build runs and that the
# device accepted it (Wi-Fi + web page checked within 5 min; otherwise it rolls back by itself).
#
#   tools/ota.sh [--no-build] [host]          host default: $THERMOSTAT_HOST or 10.10.102.121
#
# Web login: ~/.config/thermostat.netrc (chmod 600), one line:
#   machine 10.10.102.121 login <user> password <password>
# Without that file curl asks for the password.
#
# Remote diagnostics (same login):
#   C="curl -k --pinnedpubkey $(cat ~/.config/thermostat.pin) --netrc-file ~/.config/thermostat.netrc"
#   $C -d STAT https://HOST/api/cmd          any serial command
#   $C https://HOST/api/log                  device log
#   $C -o s.bmp https://HOST/api/screen.bmp
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=1
if [ "${1:-}" = "--no-build" ]; then BUILD=0; shift; fi
HOST="${1:-${THERMOSTAT_HOST:-10.10.102.121}}"
NETRC="$HOME/.config/thermostat.netrc"
if [ -f "$NETRC" ]; then AUTH=(--netrc-file "$NETRC"); else read -rp "Web user: " U; AUTH=(-u "$U"); fi
# The device serves HTTPS with its own self-signed certificate. Its public key is pinned: stored in PINFILE at the
# first contact (compare it with the device: console command "TLS", or the line "HTTPS public-key pin" in the log)
# and checked on every later one. Firmware older than 0.4 has plain HTTP only; that is used when HTTPS is absent.
PINFILE="$HOME/.config/thermostat.pin"
BASE=""; TLS=()
fetch_pin() {
    python3 -c 'import ssl,sys; sys.stdout.write(ssl.get_server_certificate((sys.argv[1], 443), timeout=8))' "$HOST" 2>/dev/null |
        openssl x509 -pubkey -noout 2>/dev/null | openssl pkey -pubin -outform der 2>/dev/null |
        openssl dgst -sha256 -binary | base64
}
detect() {
    if curl -sk --max-time 5 -o /dev/null "https://$HOST/" 2>/dev/null; then
        if [ ! -s "$PINFILE" ]; then
            pin=$(fetch_pin)
            [ -n "$pin" ] || return 1
            (umask 077; echo "sha256//$pin" > "$PINFILE")
            echo "HTTPS key of $HOST trusted on first use and saved to $PINFILE:" >&2
            echo "  sha256//$pin   (must equal the device's \"TLS\" console output)" >&2
        fi
        BASE="https://$HOST"; TLS=(-k --pinnedpubkey "$(cat "$PINFILE")")
    else
        BASE="http://$HOST"; TLS=()
    fi
}
api() { curl -sS --max-time "${2:-10}" "${AUTH[@]}" ${TLS[@]+"${TLS[@]}"} "$BASE$1"; }
field() { sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p"; }

if [ $BUILD = 1 ]; then
    # shellcheck disable=SC1091
    . "$HOME/esp/esp-idf/export.sh" >/dev/null 2>&1
    idf.py build | tail -2
fi
BIN=build/thermostat.bin
WANT=$(shasum -a 256 build/thermostat.elf | cut -c1-64)
# The device reports the first CONFIG_APP_RETRIEVE_LEN_ELF_SHA characters (9): compare that prefix.
same() { [ -n "$1" ] && [ "${WANT#"$1"}" != "$WANT" ]; }

detect || { echo "Device not reachable at $HOST"; exit 1; }
before=$(api /api/version) || { echo "Device not reachable at $HOST (or wrong login)"; exit 1; }
echo "Running now: $(echo "$before" | field version) build $(echo "$before" | field elf_sha) ($(echo "$before" | field ota))"
if echo "$before" | grep -q '"hist":false'; then
    echo "Note: the device has no history partition; flash once over USB (idf.py flash) to keep history across restarts."
fi
if same "$(echo "$before" | field elf_sha)"; then echo "This build is already running."; exit 0; fi

echo "Uploading $BIN ($(($(stat -f%z "$BIN") / 1024)) KB) to $HOST ..."
curl -sS --max-time 300 "${AUTH[@]}" ${TLS[@]+"${TLS[@]}"} --data-binary "@$BIN" -H "Content-Type: application/octet-stream" \
    "$BASE/ota"
echo

echo "Waiting for the restart and the device's own check (up to 6 min) ..."
deadline=$((SECONDS + 360))
while [ $SECONDS -lt $deadline ]; do
    sleep 5
    detect 2>/dev/null || continue  # the new firmware may answer on https where the old one used http
    v=$(api /api/version 5 2>/dev/null) || continue
    sha=$(echo "$v" | field elf_sha)
    state=$(echo "$v" | field ota)
    if same "$sha" && [ "$state" = "accepted" ]; then
        echo "OK: build $sha is running and accepted."
        exit 0
    fi
    if ! same "$sha" && [ "$(echo "$v" | sed -n 's/.*"uptime_s":\([0-9]*\).*/\1/p')" -lt 120 ] 2>/dev/null; then
        echo "FAILED: the device restarted on the old build $sha (rolled back). See: curl ... /api/log"
        exit 1
    fi
    echo "  $(echo "$v" | field ota_text)"
done
echo "TIMEOUT: no confirmation within 6 min. Check the web page (Firmware, Diagnostics)."
exit 1
