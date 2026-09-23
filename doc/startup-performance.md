# Startup performance investigation

This document tracks an ongoing investigation into DigiByte Core startup time and
block-index memory use. It records measurements and hypotheses so that individual
changes can be reviewed and benchmarked independently.

The investigation is based on DigiByte Core v9.26.5
(`05b50e229db5a3d1fb316c77f3f6c62efa879b96`). Measurements below are from a
single ARM64 system with a mainnet block index of approximately 24.26 million
entries. Absolute timings are machine-specific; the phase breakdown is the useful
part.

## Baseline observations

At approximately 24.26 million block-index entries:

- `sizeof(CBlockIndex) == 208` bytes on ARM64.
- `lastAlgoBlocks[NUM_ALGOS_IMPL]` contains 8 pointers and occupies 64 bytes,
  about 30.8% of each `CBlockIndex`.
- `BlockMap::value_type` is 240 bytes before unordered-map node/bucket allocator
  overhead.
- The active-chain pointer vector alone is approximately 185 MiB.
- Runtime anonymous RSS is approximately 7.6 GiB, indicating that the dominant
  memory cost is in-memory chain/index state rather than the configured block
  database cache.

A non-instrumented startup took approximately 465 seconds to RPC readiness.

## Instrumented baseline

Branch: `perf/startup-instrumentation`

Instrumented build commit: `b256eb67225887c40555c18cb322d3c613c517d6`

The instrumented run reached RPC readiness in 453.276 seconds. The block-index
contained 24,261,452 entries during the measured reconstruction stages.

| Startup phase | Time |
| --- | ---: |
| Block-index count-only DB pass | 13.296 s |
| Block-index deserialize pass | 72.548 s |
| First block-index pointer-vector construction | 3.676 s |
| First height sort | 12.494 s |
| Block-index reconstruction | 153.587 s |
| Second block-index pointer-vector construction | 3.654 s |
| Second height sort | 14.275 s |
| Candidate / best-header traversal | 27.899 s |
| Oracle price-history reconstruction | 120.674 s |
| System-health reconstruction | 12.346 s |

The existing outer startup timer reported 318.445 seconds for the block-index
stage. The measured subphases account for most of it.

Peak RSS (`VmHWM`) during this run was approximately 10.05 GiB. Settled
anonymous RSS was approximately 7.64 GiB.

## Current findings

### Block-index database loading

`BlockTreeDB::LoadBlockIndexGuts()` performs a complete count-only LevelDB pass
before the deserialize pass solely to calculate loading progress. The measured
cost of that extra traversal is 13.296 seconds.

### Duplicate height ordering

`BlockManager::LoadBlockIndex()` constructs and height-sorts a vector containing
every block index before reconstructing memory-only state.

Later, `ChainstateManager::LoadBlockIndex()` independently constructs another
full vector and sorts it by height again before candidate and best-header
processing.

The second vector construction plus sort cost 17.929 seconds in the measured run.
A future optimization should investigate preserving or reusing the first ordering
without changing initialization semantics or object lifetimes.

### Reconstruction loop

The largest block-index subphase is the 153.587-second height-ordered
reconstruction loop. It performs, among other work:

- propagation and update of `lastAlgoBlocks`;
- `GetBlockProof()` and `nChainWork` reconstruction;
- `nTimeMax` reconstruction;
- `nChainTx` and unlinked-block bookkeeping;
- failed-child propagation; and
- `BuildSkip()`.

Commit `c62a08b08d8f732874079cdb19bf85e6a06cf7ac` adds temporary profiling of
these groups. A follow-up benchmark is pending.

### Oracle startup scan

Startup synchronously scans the last 172,800 blocks for Oracle price history.
The measured scan took 120.674 seconds and recovered two oracle prices in that
run. System-health reconstruction then consumed another 12.346 seconds.

The branch `fix/dd-oracle-startup-scan-hang` currently points directly at the
v9.26.5 commit and contains no implementation changes. Oracle startup therefore
remains a separate optimization target.

### Multi-algorithm history memory

Every `CBlockIndex` stores `lastAlgoBlocks[NUM_ALGOS_IMPL]`. On ARM64 this is
64 bytes per entry, approximately 1.45 GiB of direct payload at 24.26 million
entries.

This structure is rebuilt during startup rather than serialized in
`CDiskBlockIndex`. It is used by DigiByte's fast last-block-per-algorithm lookup.
Changing it is potentially consensus-sensitive, especially around historical
algorithms and testnet minimum-difficulty behavior, so memory work should remain
separate from straightforward startup-pipeline optimizations.

## Planned experiments

1. Complete reconstruction-loop profiling and identify the dominant operations.
2. Optimize the startup pipeline independently, beginning with the count-only DB
   pass and duplicate vector/sort where semantics permit.
3. Investigate Oracle history reconstruction separately.
4. Prototype a lower-memory representation for per-algorithm history only after
   differential tests cover the fast and slow lookup behavior.
5. Benchmark every material change against the same chain state, recording RPC
   readiness, phase timings, peak RSS, settled anonymous/file-backed RSS, faults,
   I/O, and consensus-relevant behavior.

## Benchmark discipline

Startup optimizations should remain behavior-preserving unless explicitly
identified otherwise. Do not run two daemons against the same data directory.
Avoid reindexing for ordinary startup benchmarks. Record whether filesystem cache
conditions are warm or cold when comparing absolute timings.

Instrumentation commits should remain separate from optimization commits so that
measurement overhead can be removed when evaluating final performance.


## Reconstruction profiling follow-up

A follow-up run at 24,261,668 block-index entries instrumented the major groups
inside the height-ordered reconstruction loop:

| Reconstruction group | Time |
| --- | ---: |
| Per-algorithm history propagation/update | 1.233 s |
| Chain-work reconstruction / `GetBlockProof()` | 147.561 s |
| `nTimeMax` reconstruction | 0.853 s |
| Transaction linkage, failure propagation and skip-list work | 1.486 s |
| Entire instrumented reconstruction loop | 159.842 s |

The per-group timers account for 151.133 seconds. The remaining approximately
8.7 seconds includes loop/progress/interrupt/contiguity overhead and profiling
overhead.

This falsifies the initial hypothesis that copying `lastAlgoBlocks` is a major
startup-time cost. It remains a major memory target, but consumed less than one
percent of the measured reconstruction loop. The dominant startup CPU cost is
chain-work reconstruction, specifically the expression containing
`GetBlockProof(*pindex)`, at about 92% of the whole reconstruction loop.

The follow-up run reached RPC readiness in 461.300 seconds. Other major phases
were stable relative to the first instrumented run: count pass 13.497 s,
deserialize 72.449 s, first sort 12.235 s, second vector 3.619 s, second sort
13.828 s, candidate/header pass 27.866 s, Oracle history 120.660 s, and system
health 12.360 s. Peak RSS was approximately 10.08 GiB and settled anonymous RSS
approximately 7.64 GiB.

### Next profiling target

Before changing chain-work semantics, profile the internals of
`GetBlockProof(const CBlockIndex&)`. Determine whether the cost is dominated by
compact-target decoding, 256-bit division, algorithm-specific work adjustment,
or another operation. Any optimization here must preserve exact chain-work
values across all DigiByte algorithms and historical consensus transitions.


## Persisted chain-work investigation

The reconstruction profile changes the architectural question from merely making
`GetBlockProof()` faster to deciding whether historical chain work should be
recomputed on every normal startup.

### Why reconstruction is unusually expensive in DigiByte

For mainnet heights below `workComputationChangeTarget` (1,430,000),
`GetBlockProof()` derives work from the block's compact target and an
algorithm-dependent scale factor.

From height 1,430,000 onward, the implementation is substantially more expensive.
For every block it computes a geometric mean across the active proof-of-work
algorithms. For every active algorithm it calls `GetNextWorkRequired()`, which
selects the V4 difficulty algorithm in this era. V4 walks back
`NUM_ALGOS * nAveragingInterval` blocks (50 with the mainnet parameters),
locates the previous block for that algorithm, calculates median-time-past
values, and performs retarget arithmetic. The resulting targets are each passed
through `ApproxNthRoot(NUM_ALGOS)` before being multiplied together.

At a block-index size of roughly 24.26 million entries, more than 22.8 million
entries are in this post-DigiSpeed chain-work regime. This explains why an
operation that is cheap enough to reconstruct in Bitcoin Core has become a major
startup cost in DigiByte.

### Chain work is already known before it is discarded

Normal header insertion in `BlockManager::AddToBlockIndex()` computes

`parent.nChainWork + GetBlockProof(block)`

and then marks the new block-index entry dirty. `WriteBlockIndexDB()` later
passes dirty entries to `BlockTreeDB::WriteBatchSync()`, which serializes each
one as a `CDiskBlockIndex`.

However, `CBlockIndex::nChainWork` is explicitly marked memory-only and
`CDiskBlockIndex::SERIALIZE_METHODS` does not serialize it. Therefore the
expensive value is known during ordinary operation, but is discarded from the
persistent block index. On the next startup, all entries are height-sorted and
the value is recomputed from genesis.

### Candidate persistence design

A promising design is to persist the already-computed chain-work value with each
block-index record. This naturally handles side branches as well as the active
chain because chain work belongs to each block index entry, not merely to a
height.

The block-index record already begins with a serialized version value. A
versioned extension could allow new records to carry chain work while still
recognizing legacy records. Existing records would require a one-time
reconstruction/migration. New blocks would then persist their chain work through
the existing dirty-block-index write path.

A prototype must explicitly test mixed old/new records, downgrade behavior,
interrupted migration, reindex behavior, corrupted records, and all supported
networks before this can be treated as a production design.

### Alternative cache designs

A separate sidecar cache avoids changing `CDiskBlockIndex`, but needs its own
mapping from block hash to derived state or a robust scheme for maintaining
alignment with the block-index database. A hash-keyed sidecar duplicates a large
amount of key material and introduces another database to keep synchronized.

Persisting chain work directly in the existing block-index record appears
structurally simpler, but the compatibility and validation properties need to be
proven rather than assumed.

### Important separation of concerns

Persisted chain work would remove the dominant measured reconstruction cost, but
it does not replace the other startup work already identified:

- the count-only LevelDB pass;
- deserialization;
- height ordering;
- `lastAlgoBlocks`, `nTimeMax`, chain-transaction and skip-pointer
  reconstruction;
- duplicate vector/sort work;
- candidate/header processing; and
- Oracle and system-health reconstruction.

Those remain independent optimization targets. Likewise, reducing
`lastAlgoBlocks` memory remains valuable even though its reconstruction time is
small.


## Chain-work persistence prototype

Branch: `perf/persist-chainwork-cache`

The first prototype persists `CBlockIndex::nChainWork` as a trailing field in
`CDiskBlockIndex`. Existing records remain readable: legacy records use the
historical disk-index version value and have no trailing chain-work field, while
newly written records use a new format version and include the 256-bit cumulative
chain-work value.

Normal block-index writes therefore cache chain work incrementally without a
separate database or shutdown snapshot.

During startup, records with persisted non-zero chain work reuse it. Legacy
records fall back to the existing deterministic reconstruction. Reconstructed
legacy records are rewritten in bounded batches so migration is resumable:
an interrupted migration leaves a mixture of legacy and cached records, and the
next startup recomputes only the remaining legacy records.

This prototype intentionally leaves the other startup phases unchanged so the
effect of chain-work persistence can be measured independently. The first run on
an existing database is expected to be slower because it both performs the old
chain-work reconstruction and rewrites the historical block-index records. The
second run is the important benchmark.

Before treating this as production-ready, verify disk-format round trips,
old-version read compatibility, interrupted migration, reindex behavior, and
cache invalidation rules for any future change to historical chain-work
semantics.
