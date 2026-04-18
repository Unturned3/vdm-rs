#!/usr/bin/env bash

set -euo pipefail

mkdir -p artifacts

link_sim_pid=""
rx_pid=""

cleanup() {
    if [[ -n "${rx_pid}" ]] && kill -0 "${rx_pid}" 2>/dev/null; then
        kill "${rx_pid}" 2>/dev/null || true
        wait "${rx_pid}" 2>/dev/null || true
    fi
    rx_pid=""

    if [[ -n "${link_sim_pid}" ]] && kill -0 "${link_sim_pid}" 2>/dev/null; then
        kill "${link_sim_pid}" 2>/dev/null || true
        wait "${link_sim_pid}" 2>/dev/null || true
    fi
    link_sim_pid=""
}

handle_interrupt() {
    cleanup
    exit 130
}

handle_term() {
    cleanup
    exit 143
}

trap cleanup EXIT
trap handle_interrupt INT
trap handle_term TERM

./build/link_sim \
    localhost 1234 1235 \
    --drop 0.1 --latency-ms 25 --jitter-ms 10 \
    2> artifacts/link_sim.log \
    &
link_sim_pid="$!"

sleep 0.1

./build/rx \
    localhost 1235 \
    --exit-on-eof \
    -k 60 -t 40 \
    > artifacts/out.img \
    2> artifacts/rx.log \
    &
rx_pid="$!"

sleep 0.1

./build/tx \
    localhost 1234 \
    -k 60 -t 40 \
    < artifacts/in.img \
    2> artifacts/tx.log

wait "${rx_pid}"
rx_pid=""

# Check if the output file matches the input file
if cmp -s artifacts/in.img artifacts/out.img; then
    echo "Success: Output file matches input file"
else
    echo "Error: Output file does not match input file"
    exit 1
fi
