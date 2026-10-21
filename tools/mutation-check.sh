#!/bin/sh
# Sanity check of the test suite: applies small deliberate bugs ("mutants") to a
# scratch copy of the sources and verifies that the tests notice each of them.
# Usage: sh tools/mutation-check.sh        (ONLY=text runs the mutants whose name contains the text)
set -u
src="$(cd "$(dirname "$0")/.." && pwd)"
work="${TMPDIR:-/tmp}/hubus-mutation"
survived=0
total=0

run_mutant() { # name file sed-expression
    name="$1"; file="$2"; expr="$3"
    case "$name" in *"${ONLY:-}"*) ;; *) return ;; esac
    rm -rf "$work"; mkdir -p "$work"
    (cd "$src" && tar --exclude=./build --exclude=./.git -cf - .) | (cd "$work" && tar xf -)
    sed -i "$expr" "$work/$file"
    if cmp -s "$src/$file" "$work/$file"; then
        echo "MUTANT NOT APPLIED: $name"; survived=$((survived + 1)); total=$((total + 1)); return
    fi
    total=$((total + 1))
    if ! (cd "$work" && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null 2>&1 && cmake --build build >/dev/null 2>&1); then
        echo "INVALID (does not build): $name"; survived=$((survived + 1)); return
    fi
    if (cd "$work" && ctest --test-dir build -j4 --timeout 60 -E hubusd_e2e >/dev/null 2>&1); then
        echo "SURVIVED: $name"; survived=$((survived + 1))
    else
        echo "killed:   $name"
    fi
}

run_mutant "arbitration: lower MCID wins"          core/hubus_slave_core.c 's/(c->mcid >> k) \& 1u));/!((c->mcid >> k) \& 1u)));/'
run_mutant "no handoff slot"                       core/hubus_slave_core.c 's/c->data_low = (uint8_t)(c->lost == k);/c->data_low = 0;/'
run_mutant "STOP never clears the buffer"          core/hubus_slave_core.c 's/if (c->tx_complete \&\& c->tx_type == HUBUS_S_DATA) {/if (0) {/'
run_mutant "partial response counts as complete"   core/hubus_slave_core.c 's/if (c->tx_idx >= c->tx_total \&\& c->tx_nbits == 0u \&\& c->tx_type == HUBUS_S_DATA) {/if (c->tx_type == HUBUS_S_DATA) {/'
run_mutant "write does not protect new content"    core/hubus_slave_core.c 's/    c->tx_complete = 0; \/\* a complete response must not clear the new content \*\//    \/\* mutant \*\//'
run_mutant "write allowed during response"         core/hubus_slave_core.c 's/if (c->state == HUBUS_ST_TX \&\& c->tx_type == HUBUS_S_DATA) {/if (0) {/'
run_mutant "GEN not incremented"                   core/hubus_slave_core.c 's/c->gen = (uint8_t)((c->gen + 1u) \& 3u);/c->gen = c->gen;/'
run_mutant "group request answered"                core/hubus_slave_core.c 's/h->field != HUBUS_TARGET_LEN_EXACT)) {/0)) {/;s/tlen != HUBUS_TARGET_LEN_EXACT)) {/0)) {/'
run_mutant "bad CRC accepted"                      core/hubus_slave_core.c 's/rx_done(c, (uint8_t)(b == hubus_crc8_final(c->crc)));/rx_done(c, 1);/'
run_mutant "reserved opcodes executed"             core/hubus_slave_core.c 's/} else if (op >= HUBUS_OP_APP_MIN \&\& c->on_command) {/} else if (c->on_command) {/'
run_mutant "event released while CLK high"         core/hubus_slave_core.c 's/    case HUBUS_ST_EVENT:/    case 99:/'
run_mutant "oversized command executed"           core/hubus_slave_core.c 's/if (c->rx_len >= 1u \&\& c->rx_len <= HUBUS_RX_BUF_SIZE) {/if (c->rx_len >= 1u) {/'
run_mutant "CRC polynomial"                        core/hubus_crc.c 's/HUBUS_CRC_POLY)/0x07)/'
run_mutant "prefix mask off by one"                core/hubus_frame.h 's/(0x3Fu << (HUBUS_MCID_BITS - target_len))/(0x3Fu << (HUBUS_MCID_BITS - target_len + 1))/'
run_mutant "mask stack is FIFO"                    core/hubus_arb.c 's/\*m = s->item\[--s->n\];/*m = s->item[0]; { uint8_t i; for (i = 1; i < s->n; i++) s->item[i - 1] = s->item[i]; } s->n--;/'
run_mutant "handoff mask without cleared bit"      core/hubus_arb.c 's/ \& ~(1u << k));/);/'
run_mutant "duplicates not detected"               core/hubus_gen.c 's/            return HUBUS_GEN_DUPLICATE;/            res = HUBUS_GEN_WRAPPED;/'
run_mutant "master ends bad response with STOP"    master/hubus_master.c 's/    if (cond_reset(m) != HUBUS_OK) {\n        (void)recover(m);/XX/;s/^    \/\* Invalid response: RESET keeps buffer and pending flag in the slave. \*\//    if (cond_stop(m) == HUBUS_OK) { return r; }/'
run_mutant "master never retries"                  master/hubus_master.c 's/if (attempts >= m->cfg.retry_budget) {/if (1) {/'
run_mutant "master ignores readback"               master/hubus_master.c 's/return (high == (bit ? 0u : 1u)) ? HUBUS_OK : HUBUS_ERR_LINE;/return HUBUS_OK;/'
run_mutant "serve order not descending"            master/hubus_master.c 's/hubus_mask_stack_clear(\&m->stack);/hubus_mask_stack_clear(\&m->stack); (void)0;/;s/if (!hubus_mask_stack_pop(\&m->stack, \&mask)) {/if (m->stack.n == 0 || (mask = m->stack.item[0], 0)) {/'

run_mutant "daemon: no republish after reconnect"     daemon/hubusd_core.c 's/    d->republish = 1;/    d->republish = 0;/'
run_mutant "daemon: slave never goes offline"          daemon/hubusd_core.c 's/if (++s->ping_failures >= threshold) {/if (0) {/'
run_mutant "daemon: wrong discovery payload"           daemon/hubusd_core.c 's/"payload_on", "ON"/"payload_on", "on"/'
run_mutant "daemon: set without profile accepted"         daemon/hubusd_core.c 's/} else if (!s->profile) {/} else if (0) {/'
run_mutant "daemon: queue never reports overflow"      daemon/hubusd_core.c 's/        d->stats.queue_full++;/        (void)0;/'
run_mutant "daemon: state published without readback"  daemon/hubusd_core.c 's/    if (slave_readback(d, mcid) && info_can_capture(s)) {/    if (0) {/'
run_mutant "config: duplicate MCID accepted"           daemon/hubusd_config.c 's/if (cfg->slaves\[j\].mcid == mcid) {/if (0) {/'
run_mutant "profile: byte order ignored"               daemon/hubusd_profile.c 's/uint16_t hi = p->big_endian ? /uint16_t hi = 1 ? /'

echo "$survived of $total mutants survived"
[ "$survived" -eq 0 ]
