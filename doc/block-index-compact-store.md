# Compact block-index store

## Goal

The residency experiment proved that DigiByte can keep only the live
multi-algorithm working set resident without affecting the mining path. The
next step is to stop materializing one `CBlockIndex` object for every known
historical block.

This branch introduces the Generation 2 representation incrementally. It must
remain possible to compare the compact representation against the existing
`CBlockIndex` graph until the compact store has proved equivalent.

## Representation

The v1 compact store uses a 32-bit `BlockIndexId`. A 32-bit identifier covers
more than four billion records and halves active-chain parent/index references
relative to 64-bit pointers.

A historical slot contains:

- authoritative block hash;
- parent and skip record ids;
- height;
- block/undo file positions;
- persisted chain work;
- transaction counts and status;
- full block-header payload;
- persisted derived `nTimeMax`.

The v1 slot is fixed at 160 bytes. At roughly 24.3 million blocks this is about
3.6 GiB on disk, but the important distinction is that the store is intended
to be mmap/file-backed. Clean historical pages can therefore be reclaimed by
the operating system instead of remaining anonymous process memory.

The active-chain representation should ultimately be
`std::vector<BlockIndexId>`, around 93 MiB at the current mainnet height.

## Exact-hash lookup

Network-facing block existence checks must not turn arbitrary peer-provided
hashes into synchronous random disk reads.

The target lookup path is:

1. keyed in-memory fingerprint lookup;
2. fingerprint miss means the block is unknown with no storage access;
3. fingerprint hit returns a `BlockIndexId`;
4. verify the full 256-bit hash in the mapped compact slot.

The fingerprint key must be process-random/keyed so a peer cannot deliberately
manufacture collisions that force backing-store work.

## Hot materialization

Only blocks needed by live validation/mining/network state should have
materialized `CBlockIndex` objects:

- the active consensus/versionbits working window;
- competing candidate branches;
- blocks in flight;
- peer-referenced tips;
- blocks being connected/disconnected;
- explicitly pinned RPC/index work;
- a bounded historical cache.

Normal tip extension must remain a no-I/O path.

## Migration stages

1. Define and test the compact on-disk format and stable ids.
2. Build a streaming shadow writer from the existing fully materialized index.
3. Re-open the generated store and verify every sampled/full record against the
   existing `CBlockIndex` graph.
4. Introduce compact hash-to-id lookup and an active-chain id mirror.
5. Convert long-lived pointer owners to ids/leases.
6. Start in a hybrid mode where history is compact but hot objects are
   materialized.
7. Disable full historical `CBlockIndex` construction on normal startup.
8. Remove the old whole-map startup passes when their remaining users are
   converted.

## Oracle startup state is separate

Oracle reconstruction is not part of the block-index memory problem.

Current startup scans up to 172,800 recent blocks, reads each relevant block
from the block files, extracts/validates oracle data, and rebuilds volatility
state. The steady price cache itself is very small: only the recent price
window is retained, while the volatility reconstruction vector is temporary.

Therefore compact block-index work may make access to block metadata cheaper,
but it will not remove the approximately two-minute oracle scan because that
code reads full block data.

Oracle startup should be optimized independently by persisting the derived
price/volatility checkpoint as blocks connect and disconnect, then validating
and loading that checkpoint on startup with a bounded fallback scan.


## Current implementation status

The first shadow-store milestone is implemented.

- Each loaded `CBlockIndex` receives a deterministic 32-bit compact id after
  sorting by `(height, block hash)`. On 64-bit builds the id consumes existing
  alignment padding and does not grow the 152-byte balanced shell.
- `-blockindexcompactshadow=build` writes `blocks/index.compact.tmp`
  sequentially, fsyncs it, atomically publishes `blocks/index.compact`, then
  immediately re-opens and verifies every record against the live
  `CBlockIndex` graph.
- `-blockindexcompactshadow=verify` verifies an existing shadow independently.
- The shadow path is disabled by default and is not yet used for normal startup
  lookup or materialization. Building/verifying it is therefore a migration
  experiment, not a startup optimization yet.
- No additional 24-million-entry pointer/id map is allocated while building:
  parent and skip ids are read from the compact id embedded in each existing
  shell.

The next milestone is to open this verified representation through a
file-backed reader, add compact hash-to-id lookup and an active-chain id view,
then begin replacing historical pointer ownership.


## Startup parallelism follow-up

The current startup path is predominantly serial and several expensive phases
run while `cs_main` is held. This is worth auditing after the representation
changes above, but parallelism should not be used to preserve work that the
compact-store design can eliminate entirely.

Priority order:

1. First remove obsolete whole-history work through the compact store and
   persisted derived state.
2. Measure remaining phases using wall time, process CPU time and per-thread CPU
   utilization.
3. Parallelize only phases whose inputs are immutable and whose outputs can be
   reduced deterministically without extending `cs_main` contention.

Likely candidates include read-only compact-store verification, construction of
auxiliary hash/id lookup tables, and independent validation/reduction passes.
Poor candidates include chain-dependent reconstruction of `nChainWork`,
`nTimeMax`, `nChainTx`, failed-child state and skip links, where each height
depends on preceding state.

Oracle startup is currently also a serial loop under `cs_main`, but persisting
its derived price/volatility state is preferable to making the 172,800-block
reconstruction scan multithreaded. If a bounded fallback scan remains after
checkpointing, that fallback can then be evaluated separately for safe
parallel I/O/validation.


## Persistent hash lookup experiment

A debug-only persistent `hash -> BlockIndexId` lookup now complements the
mapped compact store.

- `-blockindexcompactlookup=build` builds
  `blocks/index.compact.lookup` from the existing compact generation, reopens
  it read-only, verifies every persisted entry, and benchmarks sampled positive
  lookups against the legacy `unordered_map<uint256, CBlockIndex>`.
- `-blockindexcompactlookup=verify` reopens and verifies an existing lookup.
- The table uses power-of-two open addressing at no more than 75% load.
  At roughly 24.3 million records this is 33,554,432 16-byte slots, about
  512 MiB of file-backed storage.
- Each slot stores a keyed 64-bit SipHash fingerprint plus a 32-bit
  `BlockIndexId`. Fingerprint matches are always verified against the full
  authoritative 256-bit hash in the compact mapped record.
- The lookup is a derived cache tied to the compact source generation and size;
  failure to build or open it is non-fatal.
- Normal block-index lookup is not switched to this table yet. The first goal is
  to establish correctness, lookup latency, and file-backed residency before
  replacing the legacy historical map.
