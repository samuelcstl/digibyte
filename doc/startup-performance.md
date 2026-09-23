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
