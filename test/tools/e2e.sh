#!/bin/sh
# End-to-end test of hubusd: a real mosquitto broker, the daemon on a simulated
# bus (relay, button, temperature sensor), and mosquitto_pub/sub as MQTT clients.
# Usage: sh test/tools/e2e.sh [build dir]     (needs mosquitto and mosquitto-clients)
set -u
trap "" PIPE
cd "$(dirname "$0")/../.."
build="${1:-build/host}"
port=18831
tmp="$(mktemp -d)"
fails=0

cleanup() {
    exec 3>&- 2>/dev/null
    kill $sub_pid $sim_pid $broker_pid 2>/dev/null
    wait 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT

printf 'listener %s 127.0.0.1\nallow_anonymous true\n' "$port" > "$tmp/broker.conf"
cat > "$tmp/hubusd.yaml" <<YAML
mqtt:
  host: 127.0.0.1
  port: $port
  stats_interval_s: 2
bus:
  ping_interval_s: 1
  ping_failures: 2
  scan_interval_s: 0
slaves:
  - mcid: 3
    name: Garden relay
  - mcid: 5
    name: Door button
  - mcid: 7
    name: Cellar
YAML

mosquitto -c "$tmp/broker.conf" >"$tmp/broker.log" 2>&1 &
broker_pid=$!
sleep 1
mosquitto_sub -p "$port" -v -t 'hubus/#' -t 'homeassistant/#' > "$tmp/sub.log" 2>&1 &
sub_pid=$!
mkfifo "$tmp/in"
"$build/test/hubusd_sim" -c "$tmp/hubusd.yaml" < "$tmp/in" > "$tmp/sim.out" 2> "$tmp/sim.err" &
sim_pid=$!
exec 3> "$tmp/in"
sleep 3

pub() { mosquitto_pub -p "$port" -t "$1" -m "$2"; }
expect() { # description pattern
    sleep 1
    if grep -q -E "$2" "$tmp/sub.log"; then
        echo "ok:   $1"
    else
        echo "FAIL: $1   (pattern: $2)"; fails=$((fails + 1))
    fi
}

expect "master online"               '^hubus/master/status online$'
expect "relay available"             '^hubus/slave/3/availability online$'
expect "initial relay state"         '^hubus/slave/3/state OFF$'
expect "temperature state"           '^hubus/slave/7/state 25$'
expect "slave info"                  '^hubus/slave/3/info .*"serial":"00000003"'
expect "discovery relay"             '^homeassistant/switch/hubus_3/config .*"command_topic":"hubus/slave/3/set"'
expect "discovery temperature"       '^homeassistant/sensor/hubus_7/config .*"device_class":"temperature"'
expect "unknown slave reported"      '^hubus/master/state .*"unknown_slaves":\[\{"mcid":9'

pub hubus/slave/3/set ON
expect "relay switched on"           '^hubus/slave/3/state ON$'
echo press >&3
expect "button press"                '^hubus/slave/5/state ON$'
echo release >&3
expect "button release"              '^hubus/slave/5/state OFF$'
echo "temp 303" >&3
pub hubus/slave/7/capture ""
expect "temperature capture"         '^hubus/slave/7/state 30$'
pub hubus/slave/3/set TOGGLE
expect "invalid command rejected"    '^hubus/master/log .*command rejected'
pub hubus/master/cmd/stats ""
expect "statistics"                  '^hubus/master/stats .*"bus":"ok"'
pub hubus/master/cmd/scan ""
expect "scan result"                 '^hubus/master/scan .*"mcid":9,"state":"present","configured":false'

echo "unplug 7" >&3
sleep 4
expect "slave goes offline"          '^hubus/slave/7/availability offline$'
echo "plug 7" >&3
sleep 3
expect "slave comes back"            '^hubus/slave/7/availability online$'

echo quit >&3
sleep 1
expect "offline on shutdown"         '^hubus/master/status offline$'

if [ "$fails" -ne 0 ]; then
    echo "$fails checks failed"; echo "--- daemon log"; cat "$tmp/sim.err"; echo "--- subscriber"; tail -30 "$tmp/sub.log"; exit 1
fi
echo "all checks passed"
