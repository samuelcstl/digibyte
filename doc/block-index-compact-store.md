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
