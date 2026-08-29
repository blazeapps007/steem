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
- `git`, `lz4`
- ~50GB free disk space (~19GB for the compressed snapshot download, ~28GB
  once extracted — you can delete the archive after extracting to get back
  down to ~28GB)
- 8GB+ RAM
- A few minutes to pull the pre-built image (or 20-30 minutes if you'd
  rather build it yourself — see
  [Optional: build the image yourself](#optional-build-the-image-yourself))

## 1. Get the code

```bash
git clone https://github.com/blazeapps007/steem.git
cd steem
git checkout trim-test-46c7d93d
```

The branch matters: it pins the exact source revision + patch that the
snapshot's on-disk state was written by. See
[Why the exact branch/flags matter](#why-the-exact-branchflags-matter) if
you're curious why this can't just be "any recent steemd." (No need to init
submodules for this path — those are only required if you build the image
yourself.)

## 2. Pull the image

```bash
docker pull steemblazer/trimmed-steem:latest
```

This is a pre-built image matching the exact `LOW_MEMORY_NODE` /
`ENABLE_MIRA` / `CLEAR_VOTES` / `SKIP_BY_TX_ID` flags the snapshot was
created with. If you'd rather build it from source yourself, skip this and
see [Optional: build the image yourself](#optional-build-the-image-yourself),
then point `docker-compose.yml`'s `image:` at your local tag instead.

## 3. Download and extract the snapshot

```bash
curl -L -o witness_node_data_dir.tar.lz4 https://pub-1f381b2bd7c04cbba634ee9deac91a06.r2.dev/witness_node_data_dir.tar.lz4
tar --use-compress-program=lz4 -xf witness_node_data_dir.tar.lz4
```

This extracts straight into `witness_node_data_dir/` in the current
directory. Verify it landed correctly — you should see a `block_log`,
`block_log.index`, `block_log.offset`, and a large number of `rocksdb_*`
directories:

```bash
ls witness_node_data_dir/blockchain
```

Once you've confirmed the node starts cleanly (step 5), you can delete the
archive to reclaim disk space:

```bash
rm witness_node_data_dir.tar.lz4
```

## 4. Create your config

Copy the template and use it as your `config.ini`:

```bash
cp contrib/trimmed_config.ini witness_node_data_dir/config.ini
```

This template comes from a real production witness config, so it already
has a wide/redundant seed-node list, sane `p2p`/`shared-file` tuning, and
split stderr+file logging. It ships with `plugin = witness` on but the
witness identity commented out — **before copying it in, edit these two
lines** to add your own witness account name and signing key:

```ini
# name of witness controlled by this node (e.g. initwitness )
witness = "your-witness-account"

# WIF PRIVATE KEY to be used by one or more witnesses or miners
private-key = 5Kxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
```

> **Security note:** `private-key` must be your witness's dedicated
> **signing key** (set via a `witness_set_properties` operation), never your
> account's owner/active key. Never commit `config.ini` to git or share it —
> anyone with the signing key can sign blocks (and get your witness
> penalized/disabled) on your behalf. Treat this file like a password.
>
> If you just want a syncing node with no witness identity yet, leave
> `witness`/`private-key` commented out (and drop `plugin = witness`) — safe
> to start and inspect before you commit to anything.

## 5. Start it

The repo already has a `docker-compose.yml` at its root, pointing at the
pre-built image and mounting `./witness_node_data_dir` (the directory you
just extracted into):

```yaml
services:
  witness:
    image: steemblazer/trimmed-steem:latest
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

If you built your own image instead of pulling (see
[Optional: build the image yourself](#optional-build-the-image-yourself)),
change `image:` to your local tag, e.g. `steem-witness:trimmed`. Otherwise,
just start it:

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

## Optional: build the image yourself

If you'd rather not pull `steemblazer/trimmed-steem`, you can build the same
image from source:

```bash
git submodule update --init --recursive
docker build -f Dockerfile.trimmed-witness -t steem-witness:trimmed .
```

This takes 20-30 minutes on a modern multi-core machine. Grab a coffee.

Don't edit the `LOW_MEMORY_NODE` / `ENABLE_MIRA` / `CLEAR_VOTES` /
`SKIP_BY_TX_ID` build args in `Dockerfile.trimmed-witness` — they must match
what the snapshot was created with, or steemd will fail to start with
`Column family not found` errors. See
[Why the exact branch/flags matter](#why-the-exact-branchflags-matter).

Then point `docker-compose.yml`'s `image:` at `steem-witness:trimmed`
instead of the pre-built image (see step 5).

## Troubleshooting

**`Column family not found: ...` / `You have to open all column families`
at startup.** The steemd build's schema doesn't match the snapshot. Make
sure you're using `steemblazer/trimmed-steem:latest` unmodified, or if you
built it yourself, that it was built from `Dockerfile.trimmed-witness` on
the exact `trim-test-46c7d93d` branch, unmodified. Don't mix a different
steemd build (a different image, a different branch, or the same branch
with different `cmake` flags) with this snapshot.

**`Too many open files` / `While open directory: ... Too many open files` /
`You have to open all column families` at startup.** steemd/MIRA opens
dozens of separate `rocksdb_*` column families, each with many files, and
the default per-container file-descriptor limit (often `1024` soft) isn't
enough. `docker-compose.yml` already sets a high `ulimits.nofile` for this —
if you're not using that compose file (e.g. a bare `docker run`), add
`--ulimit nofile=1048576:1048576` yourself.

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
