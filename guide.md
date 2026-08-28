# Run a Steem Witness Node in Minutes (Trimmed Snapshot + Docker)

This guide gets a Steem node up and syncing with mainnet in a few minutes,
using a pre-synced state snapshot instead of syncing ~110 million blocks
(~400GB+) from genesis. The snapshot only keeps the most recent ~1GB of raw
block history (via a trimmed `block_log`); full account/chain state
(balances, posts, votes, etc.) is complete and current as of the snapshot
date.

**What you get:** a node that validates and applies new blocks live, same as
any full node, but that cannot serve very old historical blocks to other
peers (see [Limitations](#limitations)). That's fine for voting/witness
production; it is not an archival/API node.

## Prerequisites

- Docker with the Compose plugin (`docker compose version` should work)
- `git`
- ~30GB free disk space
- 8GB+ RAM
- 20-30 minutes for the one-time image build

## 1. Get the code

```bash
git clone https://github.com/blazeapps007/steem.git
cd steem
git checkout trim-test-46c7d93d
git submodule update --init --recursive
```

The branch matters: it pins the exact source revision + patch that the
snapshot's on-disk state was written by. See
[Why the exact branch/flags matter](#why-the-exact-branchflags-matter) if
you're curious why this can't just be "any recent steemd."

## 2. Build the image

```bash
docker build -f Dockerfile.trimmed-witness -t steem-witness:trimmed .
```

This takes 20-30 minutes on a modern multi-core machine. Grab a coffee.

Don't edit the `LOW_MEMORY_NODE` / `ENABLE_MIRA` / `CLEAR_VOTES` /
`SKIP_BY_TX_ID` build args in `Dockerfile.trimmed-witness` — they must match
what the snapshot was created with, or steemd will fail to start with
`Column family not found` errors. See
[Why the exact branch/flags matter](#why-the-exact-branchflags-matter).

## 3. Download and extract the snapshot

```bash
mkdir -p witness_node_data_dir
curl -L -o snap.zip https://steemscanner.com/snap.zip
unzip snap.zip -d witness_node_data_dir
```

Verify it landed correctly — you should see a `block_log`, `block_log.index`,
`block_log.offset`, and a large number of `rocksdb_*` directories:

```bash
ls witness_node_data_dir/blockchain
```

If the zip already contains a top-level `witness_node_data_dir/` folder,
extract one level up instead (`unzip snap.zip -d .`) so you don't end up
with a nested `witness_node_data_dir/witness_node_data_dir/`.

## 4. Create your config

Create `witness_node_data_dir/config.ini`:

```ini
p2p-seed-node = seed.steemworld.org:2001
p2p-seed-node = seed.steemchat.org:2001
p2p-seed-node = seed.justyy.com:2001
p2p-seed-node = seed2.justyy.com:2001
p2p-seed-node = sn1.steemit.com:2001
p2p-seed-node = sn2.steemit.com:2001
p2p-seed-node = sn3.steemit.com:2001
p2p-seed-node = sn4.steemit.com:2001
p2p-seed-node = sn5.steemit.com:2001
p2p-seed-node = sn6.steemit.com:2001

log-appender = {"appender":"stderr","stream":"std_error"}
log-logger = {"name":"default","level":"info","appender":"stderr"}
```

This gets you a syncing node with **no witness identity configured** — safe
to start and inspect before you commit to anything. To actually produce
blocks as a registered witness, add (see the security note right after):

```ini
plugin = witness
witness = "your-witness-account"
private-key = 5Kxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

> **Security note:** `private-key` must be your witness's dedicated
> **signing key** (set via a `witness_set_properties` operation), never your
> account's owner/active key. Never commit `config.ini` to git or share it —
> anyone with this key can sign blocks (and get your witness penalized/
> disabled) on your behalf. Treat this file like a password.

## 5. docker-compose.yml

```yaml
services:
  witness:
    image: steem-witness:trimmed
    container_name: steem-witness
    # Uncomment to accept inbound P2P connections (recommended for a real
    # witness so peers can reach you directly; not required to sync out).
    # ports:
    #   - "2001:2001"
    volumes:
      - ./witness_node_data_dir:/steem
    command: --data-dir=/steem
    restart: unless-stopped
    logging:
      driver: "json-file"
      options:
        max-size: "50m"
        max-file: "3"
```

## 6. Start it

```bash
docker compose up -d
docker compose logs -f
```

You should see something like:

```
block_log.cpp:125  open  ] Using data offset: 411320856405
chain_plugin.cpp:616  plugin_startup  ] Started on blockchain with 109084430 blocks
p2p_plugin.cpp:688  plugin_startup  ] P2P Plugin started
p2p_plugin.cpp:212  handle_block  ] Got 3 transactions on block 109085716 ...
```

Block numbers climbing every ~3 seconds means you're synced and live. `Ctrl-C`
exits the log follow without stopping the container.

## Ongoing maintenance: re-trimming

`block_log` keeps growing forever as new blocks arrive — the snapshot only
buys you a fresh start, not a permanently-bounded disk footprint. Periodically
re-trim it, **with the node stopped** (trimming while steemd holds the file
open will silently detach its writes into the renamed backup file):

```bash
docker compose stop
cd witness_node_data_dir/blockchain
bash /path/to/steem/programs/util/trim_block_log.sh
cd -
docker compose start
```

By default this trims to ~1GB; edit `target_size` at the top of
`programs/util/trim_block_log.sh` to change that. It keeps a `block_log.org`
backup of what it cut — safe to delete once you've confirmed the node
restarts cleanly:

```bash
rm witness_node_data_dir/blockchain/block_log.org
```

## Troubleshooting

**`Column family not found: ...` / `You have to open all column families`
at startup.** The steemd build's schema doesn't match the snapshot. Make
sure you built from `Dockerfile.trimmed-witness` on the exact
`trim-test-46c7d93d` branch, unmodified. Don't mix a different steemd build
(a different image, a different branch, or the same branch with different
`cmake` flags) with this snapshot.

**Occasional `basic_ios::clear: iostream error` in the logs during normal
operation.** Expected and harmless — a peer asked this node for a block
older than the trim window, which it can no longer serve. Doesn't affect
sync or validation.

**`attempting to push a block that is too old` (`assert_exception`) during
the first few seconds after startup.** Also expected — normal noise from
multiple seed peers announcing blocks slightly out of order during the
initial P2P handshake. Sync continues normally past it.

## Why the exact branch/flags matter

RocksDB (via the MIRA storage layer) stores each chain object's indices in
separate column families whose expected set is derived from how the C++
object types are compiled — in particular, `comment_object` and
`transaction_object` gain or lose indices depending on the `LOW_MEMORY_NODE`
and `SKIP_BY_TX_ID` build flags. If a steemd binary built with different
flags (or from different-enough source) opens this snapshot, it expects a
different column family set than what's on disk and refuses to start. The
branch and Dockerfile in this guide are the exact combination validated
against this snapshot — that's why they're pinned rather than "use
whatever's on `master`."

## Limitations

- This node **cannot serve full historical blocks** to other peers — only
  the ~1GB (and growing, until next trim) tail of recent blocks is on disk.
  It's a full participant in consensus (validates and can produce blocks)
  but not an archival or block-explorer-API node.
- The untrimmed original block history isn't recoverable from this node once
  trimmed — if you need full archival history, run a separate full node from
  genesis instead.
