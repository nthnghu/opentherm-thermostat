#!/bin/bash
# Fault tests for the over-the-air update (REVIEW.md T15, T16). Run from firmware/, after a normal build:
#
#   tools/ota_faults.sh truncated    half of the image uploaded                 -> refused, current firmware stays
#   tools/ota_faults.sh chip         a valid image built for another chip      -> refused, current firmware stays
#   tools/ota_faults.sh garbage      not a firmware image at all               -> refused at the first block
#   tools/ota_faults.sh stall        upload stops halfway, socket kept open    -> dropped after 30 s, page answers again
#   tools/ota_faults.sh powercut     YOU pull the power during the upload      -> boots the current firmware
#   tools/ota_faults.sh nowifi       YOU block the device's Wi-Fi after an update -> rolls back after 5 min (M3)
#   tools/ota_faults.sh all          the four automatic ones (truncated, chip, garbage, stall)
#
# Uses the same login file as ota.sh (~/.config/thermostat.netrc); the password is never printed.
# Host: $THERMOSTAT_HOST or 10.10.102.121.
set -uo pipefail
cd "$(dirname "$0")/.."

HOST="${THERMOSTAT_HOST:-10.10.102.121}"
NETRC="$HOME/.config/thermostat.netrc"
BIN=build/thermostat.bin
BASE="https://$HOST"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
[ -f "$NETRC" ] || { echo "Missing $NETRC (see tools/ota.sh)"; exit 2; }
[ -f "$BIN" ] || { echo "Missing $BIN: build first (idf.py build)"; exit 2; }

api() { curl -sk --max-time "${2:-10}" --netrc-file "$NETRC" "$BASE$1"; }
field() { sed -n "s/.*\"$1\":\"\{0,1\}\([^\",}]*\).*/\1/p"; }
upload() {  # file [curl options...]: prints the device's answer
    local f=$1; shift
    curl -sk --max-time 120 --netrc-file "$NETRC" --data-binary "@$f" -H "Content-Type: application/octet-stream" "$@" \
        "$BASE/ota"
}
PASS=0; FAIL=0
ok()  { echo "  PASS: $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }

# Remembers what runs now; the tests compare against it
snapshot() {
    local v
    v=$(api /api/version) || { echo "Device not reachable at $HOST"; exit 2; }
    SHA0=$(echo "$v" | field elf_sha); PART0=$(echo "$v" | field partition); UP0=$(echo "$v" | field uptime_s)
    echo "Running: build $SHA0 on $PART0, $(echo "$v" | field ota), up ${UP0} s"
}

# Waits up to $1 s for the web page; then checks the same build on the same partition, and (with $2=norestart)
# that it did not restart
expect_unchanged() {
    local wait=$1 mode=${2:-} t0=$SECONDS v=""
    while [ $((SECONDS - t0)) -le "$wait" ]; do
        v=$(api /api/version 5) && break
        sleep 2
    done
    [ -n "$v" ] || { bad "web page not answering within $wait s"; return; }
    echo "  web page answering after $((SECONDS - t0)) s"
    local sha part up
    sha=$(echo "$v" | field elf_sha); part=$(echo "$v" | field partition); up=$(echo "$v" | field uptime_s)
    [ "$sha" = "$SHA0" ] && [ "$part" = "$PART0" ] && ok "build $sha still runs from $part" ||
        bad "now build $sha on $part (was $SHA0 on $PART0)"
    if [ "$mode" = norestart ]; then
        [ "$up" -ge "$UP0" ] && ok "no restart (up $up s)" || bad "the device restarted (up $up s)"
    fi
}

log_tail() { api /api/log 10 | grep -iE "ota|install|stall|image|rollback|rolling" | tail -"${1:-3}" | sed 's/^/  log: /'; }

t_truncated() {
    echo "== T15a: truncated image"; snapshot
    head -c $(($(stat -f%z "$BIN") / 2)) "$BIN" > "$TMP/half.bin"
    echo "  device: $(upload "$TMP/half.bin")"
    expect_unchanged 30 norestart; log_tail
}

t_chip() {
    echo "== T15b: image for another chip (header chip id set to ESP32, appended hash recomputed)"; snapshot
    python3 -I - "$BIN" "$TMP/chip.bin" <<'EOF'
import sys, hashlib
b = bytearray(open(sys.argv[1], 'rb').read())
assert b[0] == 0xE9, "not an ESP image"
b[12:14] = (0).to_bytes(2, 'little')  # esp_image_header_t.chip_id: 0 = ESP32 (the P4 is 18)
if b[23] == 1:                         # hash_appended: keep the image otherwise valid
    b[-32:] = hashlib.sha256(bytes(b[:-32])).digest()
open(sys.argv[2], 'wb').write(b)
EOF
    echo "  device: $(upload "$TMP/chip.bin")"
    expect_unchanged 30 norestart; log_tail
}

t_garbage() {
    echo "== T15c: not a firmware image"; snapshot
    head -c 300000 /dev/urandom > "$TMP/garbage.bin"
    echo "  device: $(upload "$TMP/garbage.bin")"
    expect_unchanged 30 norestart; log_tail
}

t_stall() {
    echo "== T15d: upload stalls halfway (socket stays open 60 s)"; snapshot
    python3 -I - "$HOST" "$NETRC" "$BIN" <<'EOF' &
import sys, ssl, socket, base64, netrc, time
host, rc, path = sys.argv[1:4]
login, _, pw = netrc.netrc(rc).authenticators(host)
data = open(path, 'rb').read()
ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
s = ctx.wrap_socket(socket.create_connection((host, 443), timeout=90), server_hostname=host)
auth = base64.b64encode(f"{login}:{pw}".encode()).decode()
s.sendall((f"POST /ota HTTP/1.1\r\nHost: {host}\r\nAuthorization: Basic {auth}\r\n"
           f"Content-Type: application/octet-stream\r\nContent-Length: {len(data)}\r\n\r\n").encode())
s.sendall(data[:len(data) // 2])
print(f"  sent {len(data) // 2} of {len(data)} bytes, now silent", flush=True)
try:
    s.settimeout(60)
    ans = s.recv(400)
    print("  device closed the upload:", ans.split(b"\r\n\r\n")[-1][:100].decode(errors="replace") or "(no body)", flush=True)
except Exception as e:
    print("  no answer to the stalled upload:", type(e).__name__, flush=True)
s.close()
EOF
    local py=$!
    sleep 5
    echo "  the web page must be busy now, and answer again ~30 s after the stall"
    api /api/version 3 > /dev/null && echo "  (it answered already: the stall did not block it)"
    expect_unchanged 45 norestart
    wait $py; log_tail
}

t_powercut() {
    echo "== T15e: power cut during the write (needs you)"; snapshot
    echo "  Get ready to pull the device's power. Press Enter to start the upload."; read -r
    upload "$BIN" > "$TMP/pc.out" 2>&1 &
    local cu=$!
    sleep 4
    echo; echo "  >>> PULL THE POWER NOW <<<"; echo
    wait $cu
    if grep -q Installed "$TMP/pc.out"; then
        echo "  The upload finished before the cut: the test did not hit the write. Restore power and run it again"
        echo "  (the device boots the same build on the other partition, on trial; that is harmless)."
        return
    fi
    echo "  upload broken off as expected. Wait 5 s, restore power, then press Enter."; read -r
    expect_unchanged 90
    local up; up=$(api /api/version | field uptime_s)
    [ "${up:-999}" -lt 120 ] && ok "fresh boot (up $up s): the cut really happened" ||
        bad "uptime $up s: the device did not lose power?"
}

t_nowifi() {
    echo "== T16: update, then no Wi-Fi during the 5 min trial (needs you)"; snapshot
    cat <<EOF
  The device accepts a new build as soon as Wi-Fi is up and its web server answers, a few seconds after the
  restart. So its Wi-Fi must be gone BEFORE it comes back:
    1. Open the router / AP page where you can block this device ($HOST), ready to click.
    2. Press Enter: the current build is uploaded again (into the other partition).
    3. The moment you see INSTALLED, block the device (you have ~5 s).
    4. Keep it blocked for 6 minutes. Meanwhile check the screen: it says "on trial", the room and the
       boiler values keep updating (the control loop must keep running without Wi-Fi).
    5. Unblock; this script then checks that it rolled back to $PART0.
  Expected (M3, intentional): no Wi-Fi for 5 minutes means rollback, because later the device can only be
  updated over the air.
EOF
    read -r
    local a; a=$(upload "$BIN"); echo "  device: $a"
    echo "$a" | grep -q Installed || { bad "upload refused"; return; }
    echo; echo "  >>> INSTALLED: BLOCK THE DEVICE NOW <<<"; echo
    sleep 20
    if v=$(api /api/version 4); then
        echo "  It is still reachable: $(echo "$v" | field ota_text)"
        echo "  Too late: it accepted the build (same build, harmless). Run it again."
        return
    fi
    local t0=$SECONDS
    while [ $((SECONDS - t0)) -lt 360 ]; do
        printf "\r  blocked for %3d s of 360 " $((SECONDS - t0)); sleep 5
    done
    echo; echo "  Unblock the device now, then press Enter."; read -r
    expect_unchanged 120
    local v up; v=$(api /api/version); up=$(echo "$v" | field uptime_s)
    [ "${up:-999}" -lt 400 ] && ok "it restarted during the trial (up $up s): the rollback happened" ||
        bad "up $up s: no rollback restart?"
    echo "  ota: $(echo "$v" | field ota_text)"
}

case "${1:-}" in
    truncated) t_truncated ;;
    chip) t_chip ;;
    garbage) t_garbage ;;
    stall) t_stall ;;
    powercut) t_powercut ;;
    nowifi) t_nowifi ;;
    all) t_truncated; t_chip; t_garbage; t_stall ;;
    *) sed -n '2,12p' "$0"; exit 2 ;;
esac
echo "== $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
