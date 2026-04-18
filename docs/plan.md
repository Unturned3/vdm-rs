# Reed-Solomon UDP Stream Demo Plan

## Goal

Build a minimal end-to-end Reed-Solomon forward-error-correction demo using three
small command-line programs:

```bash
rx localhost 1235 > file_out
link_sim localhost 1234 1235
cat file_in | tx localhost 1234
```

The programs are intentionally simple:

- `tx` reads a byte stream from `stdin`, encodes it into Reed-Solomon stripes, and
  sends UDP packets.
- `link_sim` receives UDP packets on one local port, simulates a radio link, and
  forwards packets to another UDP port.
- `rx` receives UDP packets, reconstructs Reed-Solomon stripes, and writes the
  recovered byte stream to `stdout`.

Logs, stats, warnings, and progress messages must go to `stderr`. `stdout` is
reserved for decoded stream bytes.

## Reed-Solomon Basics

The demo uses Reed-Solomon forward error correction. The key idea is:

- Split the input stream into groups called stripes.
- Each stripe has `k` data shards.
- The encoder generates `t` extra parity shards.
- The transmitter sends all `k + t` shards.
- The receiver can recover the original stripe if it receives any `k` of those
  `k + t` shards.

Example:

```text
k = 8
t = 4
total shards sent per stripe = 12
receiver needs any 8 shards to recover the stripe
the stripe can survive up to 4 lost UDP packets
```

Tradeoff:

- Larger `t` means more loss protection, but more bandwidth overhead.
- Larger `k` means less relative overhead for the same `t`, but each stripe takes
  longer to fill and may increase latency.
- `tx` and `rx` must use matching `k`, `t`, and shard-size settings.

## Demo Shape

Run the receiver first:

```bash
rx localhost 1235 > file_out
```

Run the simulated link:

```bash
link_sim localhost 1234 1235
```

Send a file:

```bash
cat file_in | tx localhost 1234
```

Verify the output:

```bash
cmp file_in file_out
```

This is not a shell pipeline from `tx` to `link_sim` to `rx`. The only pipe is
from a normal Linux producer, such as `cat`, into `tx`. After that, everything is
UDP.

## Team Split

### Person 1: `tx`

Build the transmitter.

Basic command:

```bash
cat file_in | tx localhost 1234
```

Suggested full command:

```bash
cat file_in | tx [options] <host> <port>
```

Suggested options:

```text
-k <count>       Data shards per stripe. Default: 8
-t <count>       Parity shards per stripe. Default: 4
-s <bytes>       Shard payload size. Default: 1400
-p <id>          Stream id. Default: 0
-x <count>       Repeat each encoded stripe this many times. Default: 1
--stats-ms <ms>  Stats interval. Default: 1000
```

Responsibilities:

- Read arbitrary binary bytes from `stdin`.
- Segment bytes into fixed-size Reed-Solomon data shards.
- Group `k` data shards into one stripe. These are the original bytes we need to
  recover on the receiver.
- Add a small protected per-shard payload header so `rx` can remove padding.
- Pad incomplete shards with zeroes.
- Flush a final partial stripe on EOF.
- Use the Reed-Solomon codec to generate `t` parity shards. These are extra
  repair shards that let `rx` recover from packet loss.
- Send `k + t` UDP packets per stripe to `<host>:<port>`.
- Include an unprotected packet header on every UDP packet so `rx` can identify
  stripe id, shard index, shard size, stream id, and FEC parameters.
- Put all logs and stats on `stderr`.

Important behavior:

- `tx` must work with any stdin source: `cat`, `dd`, `ffmpeg`, generated data,
  or another program.
- `tx` must not print anything to `stdout`.
- The `k`, `t`, and `-s` shard-size values must match the receiver's settings.
- `tx` should send stripes in increasing `stripe_id` order.
- `tx` should timestamp packets or stripes so `rx` can estimate latency.
- `tx` should reject invalid options early, especially impossible `k/t` values
  and shard sizes too large for UDP.

### Person 2: `rx`

Build the receiver.

Basic command:

```bash
rx localhost 1235 > file_out
```

Suggested full command:

```bash
rx [options] <host> <port> > file_out
```

Suggested options:

```text
-k <count>          Data shards per stripe. Default: 8
-t <count>          Parity shards per stripe. Default: 4
-s <bytes>          Shard payload size. Default: 1400
-p <id>             Stream id. Default: 0
-d <count>          Max in-flight stripes to buffer. Default: 32
--timeout-ms <ms>   Stripe timeout before declaring loss. Default: 1000
--exit-on-eof       Exit after receiving and emitting the final stripe
--stats-ms <ms>     Stats interval. Default: 1000
```

Responsibilities:

- Bind/listen on `<host>:<port>`.
- Receive UDP packets from `tx` or `link_sim`.
- Parse and validate the packet header.
- Buffer in-flight stripes by `stripe_id`.
- Deduplicate repeated shards.
- Reconstruct a stripe as soon as at least `k` unique shards are available. It
  does not matter whether those are original data shards or parity shards.
- Emit recovered data shards to `stdout` in original stream order.
- Trim padding using the protected per-shard payload length.
- Mark old unrecoverable stripes as lost after timeout/window expiration.
- Report stats to `stderr`.

Important behavior:

- `stdout` must contain only decoded stream bytes.
- The `k`, `t`, and `-s` shard-size values must match the transmitter's settings.
- `rx` should tolerate packet loss, duplicates, and out-of-order delivery.
- `rx` should output stripes in increasing `stripe_id` order.
- `rx` should never output made-up bytes for an unrecoverable stripe.
- `rx` should reject packets with the wrong magic, version, `k`, `t`, shard size,
  stream id, or invalid shard index.
- For file demos, `--exit-on-eof` should let the process terminate cleanly after
  the final stripe is emitted.

### Person 3: `link_sim`

Build the UDP radio-link simulator.

Basic command:

```bash
link_sim localhost 1234 1235
```

Suggested full command:

```bash
link_sim [options] <host> <in-port> <out-port>
```

Meaning:

- Listen for UDP packets on `<host>:<in-port>`.
- Forward surviving UDP packets to `<host>:<out-port>`.

Suggested options:

```text
--drop <p>          Independent packet drop probability in [0, 1]. Default: 0
--latency-ms <ms>   Fixed latency. Default: 0
--jitter-ms <ms>    Random extra latency in [0, jitter]. Default: 0
--rate-bps <bps>    Bandwidth limit. 0 means unlimited. Default: 0
--queue <count>     Max queued UDP packets before queue drops. Default: 1024
--seed <value>      RNG seed for repeatable tests. Default: fixed seed
--stats-ms <ms>     Stats interval. Default: 1000
```

Responsibilities:

- Receive UDP packets on `<host>:<in-port>`.
- Randomly drop packets according to `--drop`.
- Delay packets according to `--latency-ms` and `--jitter-ms`.
- Limit output bandwidth according to `--rate-bps`.
- Bound the internal queue and count queue drops.
- Forward surviving packets to `<host>:<out-port>`.
- Report stats to `stderr`.

Important behavior:

- `link_sim` should be protocol-agnostic. It should not parse Reed-Solomon
  headers.
- It must preserve UDP datagram boundaries.
- It may reorder packets if jitter makes later packets ready before earlier
  packets.
- It should be deterministic when given the same `--seed`.

## Shared Packet Format

All multi-byte integer fields should use one explicit byte order. Pick little
endian or network byte order, document it in code, and use it everywhere.

### UDP Packet Header

Every UDP packet sent by `tx` should start with an unprotected header:

```text
magic              uint32  identifies our packets, e.g. "VDMR"
version            uint16  packet format version
header_size        uint16  bytes before shard payload starts
stream_id          uint16  logical stream id
flags              uint16  packet/stripe flags
stripe_id          uint64  monotonically increasing stripe number
tx_time_ns         uint64  transmit timestamp for latency stats
shard_index        uint16  0 through k+t-1
k                  uint16  number of data shards
t                  uint16  number of parity shards
shard_payload_size uint16  bytes in each shard payload
header_crc32       uint32  optional header checksum
```

`shard_index < k` means the UDP packet carries a data shard.
`shard_index >= k` means it carries a parity shard.

The Reed-Solomon-protected shard payload immediately follows this header.

### Protected Data Shard Header

Each data shard should begin with a small header that is included in the
Reed-Solomon-protected bytes:

```text
data_length        uint32  number of real stream bytes in this shard
flags              uint16  EOF/final-stripe marker, if needed
payload_crc32      uint32  optional checksum of real stream bytes
```

After this protected header:

```text
real stream bytes
zero padding
```

The entire protected data shard must be exactly `shard_payload_size` bytes.
Parity shards must also be exactly `shard_payload_size` bytes.

The protected `data_length` is how `rx` knows how many bytes to write from each
recovered data shard.

## Encoding Rules

For each stripe:

1. Fill up to `k` data shards from stdin.
2. Prefix each data shard with the protected data-shard header.
3. Zero-pad all data shards to `shard_payload_size`.
4. Generate `t` parity shards with the Reed-Solomon codec.
5. Send all `k + t` shards as UDP packets.

Recommended send order:

```text
data0, parity0, data1, parity1, data2, parity2, ...
```

If `k` and `t` differ, keep interleaving while both remain, then send the
remaining shards. This mirrors the useful part of wifibroadcast's block behavior:
avoid putting all parity at the very end of a burst.

## Decoding Rules

For each received UDP packet:

1. Validate the unprotected UDP packet header.
2. Find or create the in-flight stripe for `stripe_id`.
3. Ignore duplicate `shard_index` values for the same stripe.
4. Store the shard payload.
5. Once at least `k` unique shards are present, reconstruct the full stripe.
6. Hold reconstructed stripes until all earlier stripes are emitted or declared
   lost.
7. Write each recovered data shard's real bytes to `stdout` using `data_length`.
8. Stop on final EOF only when `--exit-on-eof` is enabled.

## Statistics

All stats should go to `stderr`.

`tx` should report:

- bytes read from stdin
- stripes sent
- UDP packets sent
- input bitrate
- output bitrate, including parity and headers

`rx` should report:

- UDP packets received
- duplicate shards
- reconstructed stripes
- lost stripes
- bytes written to stdout
- average stripe completion latency
- max stripe completion latency
- current in-flight stripe count

`link_sim` should report:

- UDP packets received
- packets forwarded
- random drops
- queue drops
- packets currently queued
- configured output bandwidth
- observed output bandwidth

## Testing

### Local No-Loss Test

Terminal 1:

```bash
rx --exit-on-eof localhost 1235 > /tmp/out.bin
```

Terminal 2:

```bash
cat /tmp/in.bin | tx localhost 1235
```

Verify:

```bash
cmp /tmp/in.bin /tmp/out.bin
```

### Link Simulator Test

Terminal 1:

```bash
rx --exit-on-eof localhost 1235 > /tmp/out.bin
```

Terminal 2:

```bash
link_sim --drop 0.10 --latency-ms 25 --jitter-ms 10 localhost 1234 1235
```

Terminal 3:

```bash
cat /tmp/in.bin | tx localhost 1234
```

Verify:

```bash
cmp /tmp/in.bin /tmp/out.bin
```

### Suggested Test Cases

- Empty input file.
- Tiny input smaller than one shard.
- Input exactly one full stripe.
- Input ending in a partial shard.
- Large random input.
- Loss below FEC capacity.
- Loss above FEC capacity.
- Duplicate packet injection.
- Out-of-order delivery from jitter.
- Bandwidth-limited link.

## Milestones

1. Agree on CLI shape and packet format.
2. Implement `tx` and `rx` with no-loss UDP on localhost.
3. Add EOF/final-stripe behavior and padding trim.
4. Implement `link_sim` with UDP forwarding only.
5. Add drop, latency, jitter, bandwidth, queue limit, and stats to `link_sim`.
6. Add integration tests for file round trips.
7. Tune defaults for the final demo.

## Open Decisions

- Use little endian or network byte order for packet fields?
- Should default `rx` behavior be to run forever, with `--exit-on-eof` only for
  file demos?
- Should `stream_id` be fully supported now or hardcoded to zero until needed?
- Should timestamps be same-machine monotonic timestamps only, or wall-clock
  timestamps for multi-machine experiments?
- Should repeated transmission with `-x` be part of the first version or left as
  a stretch feature?
