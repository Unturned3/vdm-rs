# VDM-RS Stream Demo

This file documents how to build and run the UDP Reed-Solomon stream demo.

## Build

From the repo root:

```bash
cd vdm-rs

cmake -S . -B build \
  -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++

cmake --build build
```

This builds:

- `build/tx`
- `build/rx`
- `build/link_sim`
- `build/unit_tests`

## Run Tests

```bash
./build/unit_tests
```

## Demo 1: No Loss

Prepare an input file:

```bash
dd if=/dev/urandom of=/tmp/in.bin bs=4096 count=16
```

Start the receiver in one terminal:

```bash
./build/rx --exit-on-eof localhost 1235 > /tmp/out.bin
```

Send the file in another terminal:

```bash
cat /tmp/in.bin | ./build/tx localhost 1235
```

Verify the output:

```bash
cmp /tmp/in.bin /tmp/out.bin
```

Expected behavior:

- `cmp` should report no output, which means the files are identical.
- `tx` should print a `tx config:` line first, then `tx stats:` lines to `stderr`.
- `rx` should print a `rx config:` line first, then `rx stats:` lines to `stderr`.
- The output file should match exactly because there is no simulated loss.

Useful fields to watch:

- `tx stats`: `packets_sent`, `input_rate`, `output_rate`, `fec_overhead_pct`
- `rx stats`: `reconstructed_stripes`, `lost_stripes`, `avg_latency_ms`,
  `max_latency_ms`, `goodput`

## Demo 2: Moderate Loss With Recovery

Prepare an input file:

```bash
dd if=/dev/urandom of=/tmp/in.bin bs=4096 count=16
```

Start the receiver in terminal 1:

```bash
./build/rx --exit-on-eof localhost 1235 > /tmp/out.bin
```

Start the simulated link in terminal 2:

```bash
./build/link_sim --drop 0.10 --latency-ms 25 --jitter-ms 10 localhost 1234 1235
```

Send the file in terminal 3:

```bash
cat /tmp/in.bin | ./build/tx localhost 1234
```

Verify the output:

```bash
cmp /tmp/in.bin /tmp/out.bin
```

Expected behavior:

- `cmp` will often still report no output.
- `link_sim` should print a `link_sim config:` line first, then `link_sim stats:`
  lines showing some `random_drops` and nonzero `total_drop_pct`.
- `rx` may show reconstructed stripes, nonzero latency, and still `lost_stripes=0`.
- Even with packet loss, the output file can still be identical if the loss per
  stripe stays within the Reed-Solomon recovery budget.

With the default settings:

- `k = 8`
- `t = 4`
- each stripe sends `12` shards
- `rx` can recover from any `8` received shards
- so each stripe can tolerate up to `4` lost packets

That is why moderate loss, such as `--drop 0.10`, often still produces a
perfect output file.

Useful fields to watch:

- `link_sim stats`: `random_drops`, `queue_drops`, `total_drop_pct`,
  `observed_rate`
- `rx stats`: `reconstructed_stripes`, `lost_stripes`, `avg_latency_ms`,
  `max_latency_ms`

## Demo 3: High Loss Failure

This demo increases drop probability enough that some stripes are likely to lose
more than `t` shards and become unrecoverable.

Prepare an input file:

```bash
dd if=/dev/urandom of=/tmp/in.bin bs=4096 count=16
```

Start the receiver in terminal 1:

```bash
./build/rx --exit-on-eof localhost 1235 > /tmp/out.bin
```

Start the simulated link in terminal 2:

```bash
./build/link_sim --drop 0.45 --latency-ms 25 --jitter-ms 10 localhost 1234 1235
```

Send the file in terminal 3:

```bash
cat /tmp/in.bin | ./build/tx localhost 1234
```

Inspect the result:

```bash
cmp /tmp/in.bin /tmp/out.bin
wc -c /tmp/in.bin /tmp/out.bin
```

Expected behavior:

- `link_sim` should report many random drops.
- `rx` should report some lost stripes.
- `cmp` may report a difference.
- `wc -c` may show that `/tmp/out.bin` is smaller because `rx` does not invent
  bytes for unrecoverable stripes.
- Failure is probabilistic, so if one run still succeeds, run it again or raise
  `--drop` further.

Useful fields to watch:

- `link_sim stats`: `total_drop_pct` should be much higher than in the moderate
  loss demo
- `rx stats`: `lost_stripes` and `stripe_loss_pct` should become nonzero
- `rx stats`: `bytes_written` may stop below the input size

## Common Options

### `tx`

```bash
./build/tx [options] <host> <port>
```

Options:

- `-k <count>`: data shards per stripe, default `8`
- `-t <count>`: parity shards per stripe, default `4`
- `-s <bytes>`: shard payload size, default `1400`
- `-p <id>`: stream id, default `0`
- `-x <count>`: repeat each stripe, default `1`
- `--stats-ms <ms>`: stats interval, default `1000`

Example:

```bash
cat /tmp/in.bin | ./build/tx -k 8 -t 4 -s 1400 localhost 1234
```

### `rx`

```bash
./build/rx [options] <host> <port> > /tmp/out.bin
```

Options:

- `-k <count>`: data shards per stripe, default `8`
- `-t <count>`: parity shards per stripe, default `4`
- `-s <bytes>`: shard payload size, default `1400`
- `-p <id>`: stream id, default `0`
- `-d <count>`: max in-flight stripes, default `32`
- `--timeout-ms <ms>`: stripe timeout, default `1000`
- `--exit-on-eof`: exit after final stripe is emitted
- `--stats-ms <ms>`: stats interval, default `1000`

Example:

```bash
./build/rx -k 8 -t 4 -s 1400 --exit-on-eof localhost 1235 > /tmp/out.bin
```

### `link_sim`

```bash
./build/link_sim [options] <host> <in-port> <out-port>
```

Options:

- `--drop <p>`: random packet drop probability in `[0, 1]`, default `0`
- `--latency-ms <ms>`: fixed latency, default `0`
- `--jitter-ms <ms>`: extra random latency, default `0`
- `--rate-bps <bps>`: output bandwidth limit, `0` means unlimited
- `--queue <count>`: max queued packets, default `1024`
- `--seed <value>`: RNG seed, default `1`
- `--stats-ms <ms>`: stats interval, default `1000`

Example:

```bash
./build/link_sim --drop 0.05 --latency-ms 10 --jitter-ms 5 localhost 1234 1235
```

## Notes

- `tx`, `rx`, and `link_sim` write logs and stats to `stderr`.
- `rx` writes only recovered stream bytes to `stdout`.
- `tx` and `rx` must use matching `-k`, `-t`, `-s`, and `-p` values.
- `--exit-on-eof` is useful for file-based demos so `rx` terminates cleanly.
- Low or moderate packet loss may still produce a byte-identical output file.
- To force visible failure, use a drop rate high enough that some stripes lose
  more than `t` shards.

## Reading The Logs

Each program now prints one startup line followed by periodic stats lines.

- `tx config:` shows the transmitter settings actually in use.
- `rx config:` shows the receiver settings actually in use.
- `link_sim config:` shows the simulator parameters actually in use.

Stats lines are cumulative since process start.

- `fec_overhead_pct` in `tx` shows how much extra packet payload is being sent
  beyond the original input bytes.
- `total_drop_pct` in `link_sim` shows the fraction of received packets that
  were dropped by random loss or queue overflow.
- `stripe_loss_pct` in `rx` shows the fraction of completed stripes that were
  declared lost instead of reconstructed.
- `avg_latency_ms` and `max_latency_ms` in `rx` show end-to-end stripe
  completion latency based on the transmitter timestamps.
