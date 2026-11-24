#!/bin/bash
# Tests du demon iotd sur le PC (Ubuntu), sans Yocto ni QEMU.
# Prerequis : sudo apt install gcc libmosquitto-dev mosquitto mosquitto-clients
# Usage : ./tests/run_tests.sh
set -u

REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
SRC="$REPO_DIR/recipes-iot/iotd/files/iotd.c"
TMP=$(mktemp -d)
trap 'kill $BROKER_PID 2>/dev/null; rm -rf "$TMP"' EXIT
BROKER_PID=""
PASS=0
FAIL=0

check() {   # check <nom> <condition...>
    local name=$1; shift
    if "$@"; then echo "OK    $name"; PASS=$((PASS + 1))
    else          echo "ECHEC $name"; FAIL=$((FAIL + 1)); fi
}

gcc -Wall -Wextra -Werror -o "$TMP/iotd" "$SRC" -lmosquitto || exit 1

# Lance iotd avec une configuration ; enabled = 0 le fait s'arreter
# juste apres la lecture : on teste la configuration sans broker.
run_conf() {   # run_conf "<contenu>" -> $LOG et $CODE
    printf '%s\n' "$1" > "$TMP/test.conf"
    LOG=$("$TMP/iotd" -c "$TMP/test.conf" 2>&1); CODE=$?
}
has() { grep -qF -- "$1" <<< "$LOG"; }

echo "--- Configuration"
run_conf "enabled = 0
broker = 10.0.2.2
port = 1884
topic = test/status
interval = 3"
check "configuration valide lue"     has "broker=10.0.2.2 port=1884 topic=test/status interval=3"
check "code 0 si desactive"          [ "$CODE" -eq 0 ]

run_conf "enabled=0
port=abc"
check "port=abc : defaut garde"      has "port=1883"
check "port=abc : avertissement"     has "invalid value for 'port'"

run_conf "enabled=0
port=99999"
check "port=99999 : refus, code 1"   [ "$CODE" -eq 1 ]

run_conf "enabled=0
port=99999999999999999999"
check "port trop grand : defaut garde" has "port=1883"

run_conf "enabled=0
interval=0"
check "interval=0 : refus, code 1"   [ "$CODE" -eq 1 ]

run_conf "enabled=0
topic=gateway/#"
check "topic avec joker : refus"     [ "$CODE" -eq 1 ]

run_conf "  # commentaire
enabled=0

nimporte=1
ligne sans egal"
check "option inconnue signalee"     has "unknown option 'nimporte'"
check "ligne sans = signalee"        has "missing '='"
check "commentaire ignore"           [ "$CODE" -eq 0 ]

LOG=$("$TMP/iotd" -c "$TMP/absent.conf" -h 2>&1)
check "option -h"                    has "Usage:"

# --- MQTT : un broker local sur un port libre
if command -v mosquitto > /dev/null; then
    echo "--- MQTT"
    PORT=18830
    mosquitto -p "$PORT" > /dev/null 2>&1 & BROKER_PID=$!
    sleep 1
    printf 'broker=127.0.0.1\nport=%s\ntopic=test/gw\ninterval=1\n' "$PORT" > "$TMP/mqtt.conf"
    sub() { mosquitto_sub -p "$PORT" -t "$1" -C 1 -W 5 2>/dev/null; }

    "$TMP/iotd" -c "$TMP/mqtt.conf" 2> "$TMP/iotd.log" & IOTD_PID=$!
    MSG=$(sub test/gw)
    check "etat JSON publie"         grep -q '"interfaces":\[' <<< "$MSG"
    check "disponibilite online"     [ "$(sub test/gw/availability)" = "online" ]

    kill -TERM "$IOTD_PID"; wait "$IOTD_PID"
    check "SIGTERM : offline publie par le demon" [ "$(sub test/gw/availability)" = "offline" ]

    "$TMP/iotd" -c "$TMP/mqtt.conf" 2>> "$TMP/iotd.log" & IOTD_PID=$!
    sleep 2
    kill -KILL "$IOTD_PID"; wait "$IOTD_PID" 2>/dev/null
    sleep 1
    check "kill -9 : offline publie par le broker (testament)" [ "$(sub test/gw/availability)" = "offline" ]
else
    echo "--- MQTT : mosquitto absent, tests ignores"
fi

echo "--- $PASS reussis, $FAIL en echec"
[ "$FAIL" -eq 0 ]
