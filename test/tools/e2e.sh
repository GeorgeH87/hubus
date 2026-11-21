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
runtime_file: $tmp/runtime.yaml
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
expect_since() { # line description pattern: only what came after that line of the subscriber
    sleep 1
    if tail -n "+$1" "$tmp/sub.log" | grep -q -E "$3"; then
        echo "ok:   $2"
    else
        echo "FAIL: $2   (pattern: $3)"; fails=$((fails + 1))
    fi
}
expect_file() { # description pattern
    if grep -q -E "$2" "$tmp/runtime.yaml" 2>/dev/null; then
        echo "ok:   $1"
    else
        echo "FAIL: $1   (pattern in the runtime file: $2)"; fails=$((fails + 1))
    fi
}

expect "master online"               '^hubus/master/status online$'
expect "relay available"             '^hubus/slave/3/availability online$'
expect "initial relay state"         '^hubus/slave/3/relay/state OFF$'
expect "temperature state"           '^hubus/slave/7/temperature/state 25$'
expect "slave info"                  '^hubus/slave/3/info .*"serial":"00000003"'
expect "discovery relay"             '^homeassistant/switch/hubus_3_relay/config .*"command_topic":"hubus/slave/3/relay/set"'
expect "discovery temperature"       '^homeassistant/sensor/hubus_7_temperature/config .*"device_class":"temperature"'
expect "unknown slave reported"      '^hubus/master/state .*"unknown_slaves":\[\{"mcid":9'

pub hubus/slave/3/relay/set ON
expect "relay switched on"           '^hubus/slave/3/relay/state ON$'
echo press >&3
expect "button press"                '^hubus/slave/5/button/state ON$'
echo release >&3
expect "button release"              '^hubus/slave/5/button/state OFF$'
echo "temp 303" >&3
pub hubus/slave/7/capture ""
expect "temperature capture"         '^hubus/slave/7/temperature/state 30$'
echo "temp 311" >&3
pub hubus/slave/7/read ""
expect "temperature read (REQUEST_STATE)" '^hubus/slave/7/temperature/state 38$'
pub hubus/slave/5/read ""
expect "read of a slave without a state is rejected" '^hubus/master/log .*has no state'
pub hubus/master/cmd/probe 9
expect "probe of an unconfigured slave" '^hubus/master/probe .*"mcid":9,"state":"present","configured":false'
pub hubus/master/cmd/probe 40
expect "probe of an empty MCID" '^hubus/master/probe .*"mcid":40,"state":"absent"'
pub hubus/slave/3/relay/set TOGGLE
expect "invalid command rejected"    '^hubus/master/log .*command rejected'
pub hubus/master/cmd/stats ""
expect "statistics"                  '^hubus/master/stats .*"bus":"ok"'
pub hubus/master/cmd/scan ""
expect "scan result"                 '^hubus/master/scan .*"mcid":9,"state":"present","configured":false'

pub hubus/slave/9/config/set '{"name":"Attic"}'
expect "slave added over MQTT"       '^hubus/master/config/result .*"topic":"hubus/slave/9/config/set","result":"ok","saved":true'
expect "configuration published"     '^hubus/master/config .*\{"mcid":9,"name":"Attic"'
expect "added slave available"       '^hubus/slave/9/availability online$'
expect_file "runtime file written"   'name: "Attic"'
pub hubus/master/config/set '{"bus":{"ping_failures":3},"gpio":{"clk":1}}'
expect "fixed part refused"          '^hubus/master/config/result .*"result":"error","error":".*can only be set in the main configuration file'

echo "unplug 7" >&3
sleep 4
expect "slave goes offline"          '^hubus/slave/7/availability offline$'
echo "plug 7" >&3
sleep 3
expect "slave comes back"            '^hubus/slave/7/availability online$'

echo quit >&3
sleep 1
expect "offline on shutdown"         '^hubus/master/status offline$'

# a restart: the runtime file has the slave added over MQTT, the main file does not
exec 3>&-
wait $sim_pid 2>/dev/null
mark=$(($(wc -l < "$tmp/sub.log") + 1))
"$build/test/hubusd_sim" -c "$tmp/hubusd.yaml" < "$tmp/in" > "$tmp/sim.out" 2>> "$tmp/sim.err" &
sim_pid=$!
exec 3> "$tmp/in"
sleep 3
expect_since "$mark" "restart keeps the change" '^hubus/master/config .*\{"mcid":9,"name":"Attic"'
expect_since "$mark" "added slave online after the restart" '^hubus/slave/9/availability online$'
echo quit >&3
sleep 1

if [ "$fails" -ne 0 ]; then
    echo "$fails checks failed"; echo "--- daemon log"; cat "$tmp/sim.err"; echo "--- subscriber"; tail -30 "$tmp/sub.log"; exit 1
fi
echo "all checks passed"
