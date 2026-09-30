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

1. Define and prove the compact historical record and 32-bit id model.
2. Build and fully verify a shadow generation against the legacy
   `CBlockIndex` graph.
3. Re-open the generation through a file-backed reader and prove exact
   hash-to-id lookup independently of the legacy map.
4. Make ids persistent and monotonic: an existing generation owns historical
   ids and newly learned blocks, including old-height forks, receive later ids
   without renumbering history.
5. Add an active-chain id view and convert long-lived pointer owners to
   ids/leases or explicitly pinned hot materializations.
6. Make the compact store live and crash-consistent so normal tip growth and
   IBD maintain it incrementally rather than requiring snapshot rebuilds.
7. Start in hybrid mode where cold history exists only as compact records while
   consensus/mining/P2P working state is materialized and pinned.
8. Make compact-native startup the normal path and stop constructing the full
   historical `CBlockIndex` graph.
9. Remove whole-history startup passes whose users have been converted.
10. Retain an automatic compatibility/recovery path from the ordinary upstream
    block-index database.

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

Generation 2 has passed the shadow-equivalence and first lookup milestones, but
normal startup still materializes the complete legacy block-index graph.

Measured on mainnet at approximately 24.3 million records:

- the Generation 1 residency extraction reduced the ARM64 `CBlockIndex` shell
  from 208 to 152 bytes and moved the 64-byte per-algorithm accelerator out of
  cold historical objects;
- settled anonymous RSS fell from roughly 7.6 GiB to roughly 6.2-6.3 GiB;
- the compact generation contains 24,301,588 fixed 160-byte records plus a
  128-byte header, exactly 3,888,254,208 bytes;
- the complete generation was written in about 10.1 s and round-trip verified
  against every live legacy record;
- the read-only mmap reader successfully verifies the generation;
- generation ids now survive normal chain growth: a 24,301,588-record
  generation was restored against 24,301,752 live records and the 164 newer
  records were assigned tail ids without renumbering persisted history;
- the second 24-million-entry height sort has been eliminated. The replacement
  compact-id pointer view took about 3.9 s in the measured run;
- a persistent 33,554,432-slot hash lookup was built from the compact
  generation. At 16 bytes per slot the file is exactly 536,871,040 bytes;
- lookup construction took about 9.3 s and complete 24.3-million-entry
  verification took about 4.9 s;
- after the verification pass had warmed the mappings, one million sampled
  positive lookups took 397 ms through the legacy
  `unordered_map<uint256, CBlockIndex>` and 214 ms through the compact
  fingerprint/id lookup. This demonstrates promising lookup CPU cost but is
  explicitly a warm-cache result, not proof of the no-I/O network invariant.

The lookup currently uses a randomly generated SipHash key persisted in the
derived lookup header. A remote peer does not know that local key under the
normal threat model, but the final design must document this explicitly and
must not rely on secrecy against a local attacker.

Normal `LookupBlockIndex()` is still backed by the legacy map. Normal startup
still performs the LevelDB count pass, deserializes roughly 24.3 million
records, constructs the first pointer vector, performs the first height sort,
reconstructs legacy linkage state, and scans all records for candidate/header
state. The compact files are therefore still migration/measurement sidecars,
not yet the normal runtime representation.

The current compact file is also an experimental local-ABI format: writer and
reader currently copy C++ structs directly. Before this becomes a durable
public format it must use explicit endian-stable byte serialization with
versioned decode rules rather than host layout.



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

A debug-only persistent `hash -> BlockIndexId` lookup complements the mapped
compact store.

- `-blockindexcompactlookup=build` builds
  `blocks/index.compact.lookup` from the existing compact generation, reopens
  it read-only, verifies every persisted entry, and benchmarks sampled lookups
  against the legacy `unordered_map<uint256, CBlockIndex>`.
- `-blockindexcompactlookup=verify` reopens and verifies an existing lookup.
- The table uses power-of-two open addressing at no more than 75% load.
  At roughly 24.3 million records this is 33,554,432 16-byte slots, exactly
  536,871,040 bytes including the header.
- Each slot stores a keyed 64-bit SipHash fingerprint plus a 32-bit
  `BlockIndexId`. Fingerprint matches are always verified against the full
  authoritative 256-bit hash in the compact mapped record.
- The lookup is a derived cache tied to the compact source generation and size;
  failure to build or open it is non-fatal.
- The first full-mainnet build completed in about 9.3 s. Full verification
  measured 4.9 s in the first build run and 10.0 s in a later verify-only run,
  illustrating the sensitivity of these full scans to cache/storage state.
- In the later probe-instrumented run, one million sampled positive lookups
  measured 1,706 ms through the legacy map and 418 ms through the compact
  lookup. Successful compact lookups averaged 2.315 probes with a maximum of
  168.
- One million proven-absent hashes measured 258 ms through the legacy map and
  244 ms through the compact lookup. Compact misses averaged 7.069 probes with
  a maximum of 247.
- Those probe counts are consistent with the expected behavior of linear
  probing near the table's ~72.4% occupancy and show no evidence of a table
  pathology. The timings remain warm-mapping measurements because full lookup
  verification had just touched the mapped table and source records.
- These results make the compact exact lookup CPU-competitive, and often
  substantially faster than the legacy unordered map, but are still
  insufficient for network-path adoption because they do not demonstrate
  behavior after page-cache eviction or under memory pressure.

Before routing arbitrary network hashes through the compact lookup, measure
negative-lookup cost and page-fault behavior under cold and reclaim-pressure
conditions. The original invariant remains stronger than "mmap is usually
fast": an untrusted peer must not be able to turn arbitrary unknown hashes into
serialized random storage I/O while `cs_main` is held.

The measured miss path strengthens the case for a genuinely resident keyed
fingerprint front in front of the exact mapped lookup. The current interleaved
table is 16 bytes per slot, but an 8-byte fingerprint array for all 2^25 slots
would be 256 MiB, plus only a few MiB for exact occupancy metadata. Unknown
peer hashes could then probe exclusively resident memory. Only a keyed
fingerprint match would touch the mapped id/full-hash backing data.

This is preferable to a probabilistic Bloom-only front if the goal is to avoid
peer-controllable storage reads: a Bloom false positive is expected by design,
whereas a keyed 64-bit fingerprint collision is computationally infeasible for
a remote peer that does not know the local key. Full 256-bit verification still
remains mandatory before returning a positive result. If the threat model
requires still more margin, two independent keyed fingerprints can be
evaluated against the extra resident-memory cost.

Cold/page-cache-eviction behavior still needs measurement before selecting the
final split/residency policy.

## Compatibility, clean IBD, migration and recovery

Generation 2 must not require the manual transformation sequence used by these
experiments.

The intended compatibility contract is:

- **Clean node / full IBD:** an empty upstream-style datadir must initialize
  normally. As headers and blocks arrive, compact ids and compact records are
  maintained incrementally. Finishing IBD must not require a separate
  whole-history conversion before the next restart.
- **Upgrade from an upstream node:** if the ordinary DigiByte block-index
  database exists but no valid compact generation exists, the node can build a
  compact generation automatically as a one-time resumable migration. The
  existing upstream database remains sufficient to recover.
- **Normal restart:** when a valid compact generation exists, startup should use
  it directly and should not deserialize/materialize every historical
  `CBlockIndex`.
- **Reindex:** rebuilding from block files must populate both the ordinary
  compatibility index and the compact representation as records are accepted.
- **Reindex-chainstate:** should not rebuild compact history unless compact
  metadata is itself invalid.
- **Corruption/interrupted migration:** a compact generation or lookup is a
  derived structure. Failure must fall back to the ordinary block-index source
  or resume/rebuild the derived generation; it must not falsely instruct the
  operator to reindex an otherwise healthy block database.
- **Pruning and assumeutxo:** compact metadata must remain sufficient for
  historical identity/topology even when block payloads are absent. Snapshot
  and background-validation branches need explicit hot/pin semantics.
- **Downgrade:** the preferred design keeps the ordinary upstream-compatible
  block-index database readable by an upstream binary. Gen2 sidecars can then
  be ignored by older software. Any future change that breaks this property
  needs an explicit format/version and downgrade policy.

This compatibility goal suggests harvesting the result of the persisted
chain-work experiment without necessarily keeping its LevelDB format extension.
The experiment proved that persisting cumulative chain work removes roughly
147 seconds of normal-start reconstruction. The compact record already contains
`nChainWork`, so compact-native startup can obtain the same benefit while the
ordinary compatibility database remains closer to upstream format.

## Live-update and crash-consistency work still required

The verified compact file is currently a generation snapshot. The production
store must define how new records and mutable metadata are published without
renumbering historical ids or exposing torn state after a crash.

Requirements include:

- monotonic id allocation for every newly learned block, including historical
  side branches;
- append/update semantics for new headers and later block-data/status changes;
- a generation/checkpoint model or journal that makes interrupted updates
  detectable and recoverable;
- lookup updates for newly appended ids, with bounded rebuild/resize policy;
- active-tip/best-header identity and generation metadata;
- safe fsync/publish ordering;
- periodic compaction if a delta/journal design is used;
- no requirement to scan or rewrite the complete history for ordinary tip
  growth.

A base-generation plus small live delta/overlay is a viable design candidate:
the large compact generation remains immutable and mmap-friendly while recent
changes are maintained in a small resident/persisted delta and periodically
folded into a new generation. This is especially attractive during migration,
but it must also scale through a clean full IBD.

## Pointer-owner conversion and hot leases

Cold `CBlockIndex` objects cannot disappear until all long-lived pointer
owners have an explicit replacement. The conversion inventory includes:

- `CChain::vChain`, ultimately an id vector/view;
- block-index candidate sets;
- `m_blocks_unlinked`;
- best-header, best-invalid and failed-block state;
- peer best-known/last-common/best-header-sent state;
- blocks in flight and compact-block reconstruction state;
- versionbits caches currently keyed by block-index pointers;
- snapshot/background-validation working branches;
- validation-interface callbacks and transient RPC/index users.

The target is not a permanent proxy object for every historical block. That
would preserve most of the memory problem. Long-lived state should use ids or
stable handles; a `CBlockIndex*` should represent an explicitly materialized
hot object with pin/lease lifetime.

Deep reorganizations may legitimately require preparation. Discover the branch,
pin the required segment, prefetch/materialize it, validate residency, then
activate it. Once activation begins, consensus/mining/relay paths must not
discover cold metadata mid-flight.

## Historical network/admin access

Several older residency-audit ideas remain required and must not be lost during
the Gen2 cut-over:

- historical `GETHEADERS` must use a bulk/sequential compact path rather than
  thousands of scalar lazy reads under `cs_main`;
- arbitrary INV/header existence checks need the no-I/O negative-lookup
  property described above;
- pruning and failure-reset operations should iterate compact persistent
  records rather than force all history into objects;
- historical RPC/index work may use explicit cache/prefetch APIs and tolerate
  storage latency;
- instrumentation must retain counters for hot materializations, pins by reason,
  cache hits/misses, bytes faulted/read, prefetch usefulness, evictions and any
  storage wait observed inside a no-I/O scope.

## Startup work outside the compact representation

The compact cut-over should eliminate work rather than merely optimize it:

- the count-only LevelDB progress pass;
- full historical LevelDB deserialization on normal restart;
- first pointer-vector construction and height sort;
- whole-history `nTimeMax`/`nChainTx`/skip reconstruction where the compact
  representation already persists or can derive the required state;
- the second height sort, already eliminated experimentally;
- the whole-history candidate/header pass once candidate state can be rebuilt
  from compact metadata without materializing every object.

Independent startup work remains:

- Oracle price/volatility reconstruction, currently about 117-120 s, should
  load a persisted derived checkpoint with bounded deterministic fallback;
- system-health reconstruction is currently about 12-13 s and needs the same
  audit: persist derived state where safe, otherwise bound/reduce the scan;
- startup parallelism should be revisited only after obsolete serial work is
  removed. Read-only verification, lookup construction and independent
  reductions are candidates; chain-dependent reconstruction should preferably
  disappear through persistence rather than be parallelized.

## Acceptance criteria for the Gen2 cut-over

The compact architecture is not complete merely when RSS falls. A release-ready
cut-over must demonstrate:

- clean full IBD from an empty datadir;
- automatic migration from an ordinary upstream node;
- interrupted migration/update recovery;
- reindex and reindex-chainstate behavior;
- prune and assumeutxo behavior;
- restart without full historical `CBlockIndex` materialization;
- no-I/O guarantees for normal tip mining, incoming block validation,
  compact-block fast relay and arbitrary unknown peer hashes;
- mining template / GBT latency comparable to the compatibility path;
- correct historical GETHEADERS and RPC behavior;
- deterministic deep-reorg preparation;
- corruption detection with fallback that does not misdiagnose the upstream
  database;
- measured anonymous RSS, file-backed RSS, startup wall time, page faults,
  storage I/O and `cs_main` hold time;
- differential consensus tests for every algorithm and historical transition.

## Ideas inventory: harvested, pending, superseded

This section exists specifically to prevent useful experiments from being lost.

**Harvested into the current architecture**

- extract `lastAlgoBlocks` from cold historical objects;
- explicit no-I/O scopes around mining/validation/relay;
- persisted cumulative chain work;
- stable 32-bit historical ids;
- mmap/file-backed compact records;
- compact exact hash lookup with full-hash verification;
- reuse compact ordering to eliminate duplicate height sorting.

**Still pending / intended for later**

- active-chain id mirror;
- pointer-owner to id/lease conversion;
- hot materialization cache and pin accounting;
- deep-reorg prefetch;
- bulk historical GETHEADERS;
- resident negative-membership protection for peer-provided unknown hashes;
- compact-native pruning/admin iterators;
- cache/pin/page-fault instrumentation;
- endian-stable durable serialization;
- crash-consistent live appends/delta generations;
- clean IBD and automatic upstream migration;
- Oracle and system-health derived-state persistence;
- post-cut-over parallel startup work.

**Experiments whose result should be kept even if their implementation is
superseded**

- the LevelDB chain-work extension proved that persistence cuts chain-work
  reconstruction from roughly 147 s to roughly 1 s, but Gen2 can carry the
  value in compact records instead;
- deterministic `(height, hash)` id assignment was useful for the first shadow
  generation, but final ids must be persisted/monotonic and must not shift when
  an old-height fork is learned;
- full shadow verification proved data equivalence, but normal startup should
  not scan/fault the complete compact store;
- the mapped exact lookup demonstrates promising warm CPU cost, but its
  file-backed nature alone does not satisfy the stronger no-I/O network
  requirement.
