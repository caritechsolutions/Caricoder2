#!/bin/bash
#
# caricoder3 - the load test: 20 of 110.41's transcoded services on this server
# Copyright (c) 2026 CariTech Solutions
#
#   sudo bash /opt/caricoder3/scripts/load-test-41.sh           # add (or update) them
#   sudo bash /opt/caricoder3/scripts/load-test-41.sh --remove  # take them away again
#
# Each is set up as on 110.41 (its source program, its renditions and
# rates, audio copied; HBO Mundi and HBO Extreme from the IRD feeds on
# 224.5.5.5 that 110.41 prefers, the MPTS copies of them being scrambled)
# and sent to 224.100.200.N:5000, a group neither
# 110.41 nor 110.42 uses, with the same output options (interface, TTL)
# as godtv's first UDP output here. All 60 renditions on card 0.
#
set -euo pipefail

CFG="${CC3_CONFIG:+--config $CC3_CONFIG}"
PW="$(caricoder3 serve $CFG --show-password | awk '{print $3}')"
PORT="$(caricoder3 serve $CFG --show-port)"
API="http://127.0.0.1:$PORT/api"
call() { curl -fsS -u "admin:$PW" -H 'Content-Type: application/json' "$@"; }

CHANNELS=$(cat <<'LIST'
paramount264 udp://224.100.200.1:5000 {"input":"udp://224.100.1.5:49500","video":["h264:2500k","960x540:fit:h264:2200k","-1x360:h264:864k","512x288:fit:h264:500k"],"enabled":true,"program":2000,"bframes":2,"gop":2000}
tntseries264 udp://224.100.200.2:5000 {"input":"udp://224.100.1.5:49500","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":2010}
universaltv264 udp://224.100.200.3:5000 {"input":"udp://224.100.1.5:49500","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":2080}
ae-lat264 udp://224.100.200.4:5000 {"input":"udp://224.100.1.5:49500","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":2160}
aljazeera264 udp://224.100.200.5:5000 {"input":"udp://224.100.1.5:49500","video":["-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":2030}
discoveyi264 udp://224.100.200.6:5000 {"input":"udp://224.100.1.5:49500","video":["-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":2040}
msnbc264 udp://224.100.200.7:5000 {"input":"udp://224.100.1.6:49500","video":["1280x720:fit:h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":2190}
studiouniv264 udp://224.100.200.8:5000 {"input":"udp://224.100.1.6:49500","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":2220}
cinemax264 udp://224.100.200.9:5000 {"input":"udp://224.100.1.6:49500","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":2230}
nickelodeon264 udp://224.100.200.10:5000 {"input":"udp://224.100.1.6:49500","video":["h264:2000k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":2240}
comedy264 udp://224.100.200.11:5000 {"input":"udp://224.100.1.6:49500","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":2250}
tcm264 udp://224.100.200.12:5000 {"input":"udp://224.2.2.2:2000","video":["h264:2000k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":1210}
natgeo264 udp://224.100.200.13:5000 {"input":"udp://224.2.2.2:2000","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":1230}
hbomun264 udp://224.100.200.14:5000 {"input":"udp://224.5.5.5:8012","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true}
hboext264 udp://224.100.200.15:5000 {"input":"udp://224.5.5.5:8013","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true}
espn2264 udp://224.100.200.16:5000 {"input":"udp://224.2.2.2:2000","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":510}
tnt264 udp://224.100.200.17:5000 {"input":"udp://224.2.2.2:2000","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":990}
history264 udp://224.100.200.18:5000 {"input":"udp://224.2.2.2:2000","video":["h264:2000k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":1120}
axn264 udp://224.100.200.19:5000 {"input":"udp://224.2.2.3:2000","video":["h264:2500k","-1x480:h264:1000k","-1x360:h264:700k"],"enabled":true,"program":1790}
bbcw264 udp://224.100.200.20:5000 {"input":"udp://224.2.2.3:2000","video":["h264:2500k","960x540:fit:h264:2200k","-1x360:h264:864k","512x288:fit:h264:500k"],"enabled":true,"program":1810,"bframes":2,"gop":2000}
LIST
)

if [ "${1:-}" = "--remove" ]; then
    while read -r name out cfg; do
        call -X DELETE "$API/channels/$name" >/dev/null && echo "removed $name" || echo "$name: not there"
    done <<<"$CHANNELS"
    exit 0
fi

# the output options godtv uses (what follows the ? of its first udp:// output)
OPTS="$(call "$API/channels/godtv" | python3 -c '
import json, sys
c = json.load(sys.stdin)["channel"]
outs = ((c.get("outputs") or c.get("saved_outputs") or {}).get("outputs") or [])
u = next((o["url"] for o in outs if o.get("url", "").startswith("udp://")), "")
print(u.split("?", 1)[1] if "?" in u else "")')"
echo "output options (from godtv): ${OPTS:-none}"

n=0
while read -r name out cfg; do
    url="$out${OPTS:+?$OPTS}"
    if ! call -X PUT "$API/channels/$name" -d "$cfg" >/dev/null; then
        echo "$name: not set up (the supervisor refused its settings)"
        continue
    fi
    # an output is added to a running channel: a moment for it to start
    state=""
    for _ in $(seq 1 50); do
        state="$(call "$API/channels/$name" | python3 -c 'import json, sys; print(json.load(sys.stdin)["channel"]["state"])')"
        [ "$state" = running ] && break
        sleep 0.2
    done
    have="$(call "$API/channels/$name" | python3 -c '
import json, sys
c = json.load(sys.stdin)["channel"]
outs = ((c.get("outputs") or c.get("saved_outputs") or {}).get("outputs") or [])
print(" ".join(o.get("url", "") for o in outs))')"
    case " $have " in
        *" $url "*) echo "$name -> $url (already)"; n=$((n + 1)) ;;
        *)  if call -X POST "$API/channels/$name/outputs" -d "{\"url\": \"$url\", \"save\": true}" >/dev/null; then
                echo "$name -> $url"
                n=$((n + 1))
            else
                echo "$name: running? ($state) the output was not added"
            fi ;;
    esac
done <<<"$CHANNELS"
echo "$n of 20 channels set up; the Channels page shows them"
