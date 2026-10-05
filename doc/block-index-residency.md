# Block-index residency audit

> **Status note (Generation 2):** this document remains the governing residency
> design. The compact store is the backing representation for this policy, not
> a replacement API that validation/mining code must adopt directly. Existing
> hot code continues to use stable `CBlockIndex*` identities; residency
> indirection decides which payload domains are resident. Long-lived owners are
> converted to ids/leases only when their lifetime would otherwise prevent a
> desired eviction.
>
> The first real historical payload-cache cut-over is now implemented for
> measurement: `nFile/nDataPos/nUndoPos`, `hashMerkleRoot` and `nTimeMax`
> moved behind `BlockIndexStore`; shell-v2 then collapses the mutually
> exclusive resident-payload/provider pointers into one tagged word, reducing
> the 64-bit stable shell from 152 to 104 bytes. `full` retains eager
> compatibility residency while
> `balanced` and `lowmem` pin the active hot window and use a bounded
> historical payload cache backed by the verified compact base plus live delta.
> The missing `-blockindexcache=<MiB>` policy knob has been restored.
>
> The immediate acceptance gate is measured RSS and live-path behavior. Lookup
> generation rollover, deeper shell eviction and exhaustive lifecycle hardening
> are deliberately deferred unless required by that measured hybrid path.

## Scope

This document audits the current block-index access patterns with one goal:
separate block-index data that must be immediately available for consensus,
mining and block propagation from data that may safely be cached or loaded on
request.

The intended end state is a configurable residency layer. A compatibility mode
must be able to keep the same effective in-memory state as today, while lower
memory modes progressively move selected data domains behind cache/prefetch/lazy
loading.

The design constraint is stronger than merely "make old block indexes lazy":
network-facing code must not be able to trigger uncontrolled synchronous storage
I/O while holding `cs_main`, and mining/validation critical paths must not
suffer cache misses.

## Current lifetime constraint

`BlockManager::m_block_index` is currently

```
std::unordered_map<uint256, CBlockIndex, BlockHasher>
```

and its comment explicitly requires stable addressing because validation code
passes `CBlockIndex*` throughout the node.

`CBlockIndex` also documents pointer identity as a correctness assumption and
stores direct pointers in `pprev`, `pskip` and
`lastAlgoBlocks[NUM_ALGOS_IMPL]`.

This makes transparent eviction of complete `CBlockIndex` objects unsafe:
long-lived raw pointers exist outside the block map.

Examples include:

- `CChain::vChain`, one pointer for every active-chain height;
- `Chainstate::setBlockIndexCandidates`;
- `BlockManager::m_blocks_unlinked`;
- `ChainstateManager::m_best_header`, `m_best_invalid` and failed-block state;
- peer state such as `pindexBestKnownBlock`, `pindexLastCommonBlock`,
  `pindexBestHeaderSent` and chain-sync work headers;
- versionbits caches keyed by `const CBlockIndex*`;
- transient validation-interface callbacks.

A first implementation therefore should preserve stable identity while
introducing residency indirection. Fully evicting the identity object is a
second-generation change that requires replacing or pinning these raw-pointer
references.

### Shell-v2 compatibility-first compaction

The shell-v2 work keeps that rule. `CBlockIndex*` identity remains stable and
the ordinary active-chain pointer vector remains canonical.

Two pieces of generation-2 metadata can be reduced without changing any
consensus or pointer-owner behavior:

- the resident payload pointer and payload-provider pointer are mutually
  exclusive states, so they share one tagged machine word; hot payload access
  remains a direct pointer dereference and cold entries retain the provider
  needed for lazy materialization;
- the added `CChain` compact-id mirror is redundant because every pointed-to
  `CBlockIndex` already carries its compact id. Compact-id accessors derive the
  id from the canonical pointer vector instead of permanently duplicating four
  bytes per active-chain height.

On 64-bit builds these changes reduce the stable shell from 112 to 104 bytes
without removing any upstream `CBlockIndex` identity/topology/chain-selection
field. At roughly 24.3 million entries the one-word shell reduction is about
186 MiB, and removing the active-chain id mirror saves another roughly 93 MiB.

`nChainWork` is intentionally *not* moved out of the shell by this step.
Although it is the largest single remaining field, it is Tier-0 state on
candidate, peer, header-validation, mining and anti-DoS paths. A naive compact
backing accessor would reintroduce synchronous mmap/storage faults under
`cs_main`, violating the original compatibility/performance contract. A later
chain-work compaction must first provide explicit resident pinning or another
exact no-I/O representation for every live owner.

## Current object contents

The current in-memory `CBlockIndex` contains several distinct data domains:

### Identity and topology

- block hash identity through `phashBlock`;
- `pprev`;
- `pskip`;
- `nHeight`.

These are heavily used for chain membership, ancestry, locators, fork finding
and peer download state.

### Chain-selection state

- `nChainWork`;
- `nStatus`;
- `nTx`;
- `nChainTx`;
- `nSequenceId`.

These feed candidate ordering, anti-DoS work checks, best-header tracking,
availability checks and active-chain selection.

### Header/consensus data

- `nVersion`;
- `hashMerkleRoot`;
- `nTime`;
- `nBits`;
- `nNonce`;
- `nTimeMax`.

Only a subset is required on every hot-path operation, but historical header
serving and versionbits/difficulty logic can touch these values.

### Storage-location data

- `nFile`;
- `nDataPos`;
- `nUndoPos`.

These are needed to read block/undo data, not to identify a block or compare
chain work.

### DigiByte per-algorithm history

- `lastAlgoBlocks[NUM_ALGOS_IMPL]`.

On 64-bit systems this is 64 bytes per `CBlockIndex`. It is a major memory
consumer but is cheap to reconstruct relative to the historical chain-work
computation.

## Mining and propagation critical path

### Template creation

`BlockAssembler::CreateNewBlock()` operates from the active tip and performs
operations including:

- reading tip height and hash;
- `ComputeBlockVersion()`;
- `GetMedianTimePast()`;
- `GetNextWorkRequired()`;
- test-block validation;
- DigiDollar/oracle checks.

For current mainnet DigiByte difficulty V4,
`GetNextWorkRequiredV4()` walks back
`NUM_ALGOS * nAveragingInterval` blocks, currently 50, then performs median
time calculations. A mining-safe hot window must cover this lookback plus
median-time ancestors.

Versionbits can require substantially more history. Mainnet's
`nMinerConfirmationWindow` is 40,320 blocks. `GetStateFor()` may walk period
boundaries and, while a deployment is STARTED, count an entire period through
`pprev`. A newly started process with cold versionbits state must therefore
not rely on a tiny 50- or 100-block hot window.

A practical mining-safe baseline is to keep at least the current versionbits
period resident, or persist/prewarm versionbits state before mining is enabled.
Keeping roughly 40k full block-index records hot is inexpensive compared with
keeping 24+ million full records hot.

### Incoming full block

The P2P BLOCK path:

1. looks up the previous block by hash;
2. uses previous-block deployment state for mutation checks;
3. compares parent chain work against anti-DoS thresholds;
4. calls `ProcessNewBlock()`;
5. validates/accepts the header and block;
6. for a block extending the active tip, calls
   `NewPoWValidBlock()` before writing the block to history;
7. then activates the best chain.

The immediate relay path is explicitly latency-sensitive. In
`AcceptBlock()`, a valid block extending the tip triggers
`GetMainSignals().NewPoWValidBlock()` before the block is written to the block
files. `PeerManagerImpl::NewPoWValidBlock()` then fast-announces compact blocks
to high-bandwidth peers.

A storage fetch introduced anywhere in the parent/header/chainwork/deployment
lookups on this path would directly increase block relay latency.

### Compact-block path

Compact-block processing uses the received header, block-index lookup,
`nChainWork`, `nStatus`, `nTx`, height, ancestry and peer in-flight state.
It intentionally tries to reconstruct and process blocks with minimal network
round trips.

The corresponding index state must therefore already be resident for recent
candidate blocks and blocks in flight.

### New header path

`AcceptBlockHeader()` and `AddToBlockIndex()` require the parent and perform
contextual header validation, ancestry checks, chain-work calculation,
best-header maintenance, skip-pointer construction and DigiByte algorithm
history updates.

The parent of an ordinary new tip header must never require synchronous disk
loading.

## Remotely triggerable access that must not become naive lazy I/O

### Exact hash lookup

`LookupBlockIndex()` is currently an in-memory hash-table lookup.

It is used from network-facing paths including:

- block INV handling through `AlreadyHaveBlock()`;
- peer block availability tracking;
- incoming BLOCK parent lookup;
- incoming header-chain attachment;
- compact-block processing;
- GETHEADERS/GETBLOCKTXN and other request handling.

For block INVs, `AlreadyHaveBlock(inv.hash)` runs for each advertised block
while `cs_main` is held.

Replacing this with a synchronous LevelDB read would make untrusted network
messages capable of causing serialized random storage I/O. This is a
high-priority anti-pattern.

The residency design therefore needs an exact block-identity lookup that is
effectively non-blocking. Options include a compact resident index, a
file-backed/memory-mapped hash index with a recent resident front cache, or a
compact fingerprint-to-record-id table with full-hash verification.

### Historical GETHEADERS

A peer may request up to `MAX_HEADERS_RESULTS` headers. The current path walks
the active chain and calls `GetBlockHeader()` for each entry while
`cs_main` is held.

If old header fields become individually lazy and each call performs a LevelDB
lookup, a peer can force thousands of synchronous random reads while blocking
other validation work.

Historical headers therefore need a bulk/sequential path rather than scalar
lazy loads under the global validation lock. Suitable implementations include a
compact header store ordered by active-chain height, a memory-mapped header
array, or batched block-index reads performed outside the critical section.

### Peer chain state

`CNodeState` stores raw block-index pointers for best-known block, last common
block, best header sent and chain-sync work thresholds.

These references can remain valid for a long-lived connection. Any object these
pointers reference must either remain stable/resident or be represented by a
stable handle with explicit pinning.

### Candidate and unlinked structures

`setBlockIndexCandidates` stores `CBlockIndex*` and orders by
`nChainWork`, `nSequenceId`, then pointer address.

`m_blocks_unlinked` stores parent/child block-index pointers.

Entries in these structures must be pinned in a first-generation lazy design.
Moving them to stable record IDs/handles can come later.

## Data residency classification

### Tier 0: must be no-I/O on live consensus/network paths

These values must be available without synchronous backing-store access for
the active tip, its consensus lookback window, active candidate branches,
blocks in flight and peer-referenced tips:

- stable block identity;
- parent/topology relation;
- height;
- chain work;
- validity/data-availability status;
- sequence/candidate ordering state;
- version/time/bits needed by header validation and difficulty;
- the recent history required by median-time and difficulty calculations;
- versionbits state or the current versionbits period;
- all state needed by compact-block reconstruction and immediate block relay.

The invariant should be explicit: a cache miss from a "no-I/O" block-index
scope is a bug in the residency policy.

### Tier 1: hot/prefetched, allowed to be evictable outside the working set

Good candidates:

- complete header data for recent active-chain and candidate blocks;
- block/undo storage locations for recent active-chain and candidate blocks;
- per-algorithm lookup acceleration;
- recent `nTimeMax` and other derived values used by common RPC/network paths.

These should be prefetched whenever a branch becomes a candidate or a peer
begins downloading toward it.

### Tier 2: cacheable historical data

These may be decoded on demand, but accesses should go through explicit
store/cache APIs rather than hidden field-level page faults:

- old full header payloads;
- old storage positions;
- historical undo locations;
- old derived metadata used mainly by RPC/index maintenance;
- historical per-algorithm acceleration data if retained at all.

Network bulk requests should use batch interfaces.

### Tier 3: lazy/admin-only traversal

Operations that already tolerate being expensive may scan the persistent index
instead of forcing all entries resident:

- invalidate/reconsider/reset-failure operations that currently walk the full
  block map;
- pruning maintenance that scans all block-index records;
- startup consistency/debug scans;
- deep historical RPCs;
- unusual deep-reorg preparation before the branch is activated.

These paths should be explicit about their cost.

## `lastAlgoBlocks`

This field is the clearest first memory target.

It occupies eight pointers per block index, yet current mainnet difficulty V4
only needs recent algorithm history on the live/candidate branches.

A global per-block eight-pointer array is unnecessary for ordinary tip mining
and propagation.

Possible replacements, in increasing order of change:

1. Keep the array only in hot payloads and reconstruct/prefetch it for candidate
   branches.
2. Use a per-branch/tip algorithm-history context.
3. Fall back to a short `pprev` scan when a hot accelerated lookup is absent.
   With five active algorithms, the expected same-algorithm lookback is small;
   this must be benchmarked under header-sync/flood workloads before adoption.
4. Store compact record IDs rather than raw pointers if historical fast lookup
   remains useful.

Mainnet mining must never discover that the required algorithm-history context
is cold only after a share/block has been found.

## Suggested architecture

### BlockIndexStore

Introduce a single owner for persistent and resident block-index state:

```
BlockIndexStore
    identity index
    persistent record store
    decoded payload cache
    prefetch queue
    residency policy
    pin accounting
    instrumentation
```

The first integration should preserve the current `CBlockIndex*` identity
contract rather than attempting immediate full eviction.

### Stable shell plus payload

Generation 1:

```
CBlockIndex shell
    stable address
    minimal always-available identity/topology/core state
    payload/store identifier

BlockIndexPayload
    optional header/storage/derived domains
    owned/pinned through BlockIndexStore
```

This permits a compatibility mode in which every payload is eagerly loaded and
pinned, while lower-memory modes use identical APIs with smaller residency.

It also allows memory reduction before the much larger task of replacing every
long-lived raw pointer.

### Explicit payload access

Do not introduce scalar wrappers that silently fault from disk when a field is
read.

Prefer explicit grouped access such as:

```
GetHeaderData()
GetStorageData()
GetAlgoHistory()
```

or store-level equivalents.

That makes blocking points auditable and allows batch/prefetch APIs.

### Pinning

Pin at least:

- active consensus hot window;
- active-chain tip;
- every candidate tip and the branch segment needed to reach the active chain;
- every block in flight;
- all block indexes stored in peer long-lived pointer fields;
- best header / best invalid / failed-block references;
- entries in candidate and unlinked collections;
- snapshot/background-validation working branches.

Pins should be reference-counted or represented by scoped leases when possible.

### Prefetch

Before entering latency-sensitive activation/validation:

- prefetch the candidate branch;
- prefetch storage positions for blocks that may need to be connected;
- prefetch the consensus lookback window;
- prepare/prefetch per-algorithm state;
- prewarm versionbits state.

Deep reorgs may take longer to prepare; once activation starts, the required
metadata should already be resident.

## Configuration model

A useful initial surface would expose policy rather than individual field
switches:

```
-blockindexmode=full|balanced|lowmem
-blockindexcache=<MiB>
-blockindexhotdepth=<blocks>
```

`full` should reproduce today's effective residency and provide the regression
baseline.

`balanced` should pin all consensus/mining/P2P working state and cache
historical payloads.

`lowmem` may use a smaller historical cache, but must retain the same no-I/O
guarantees for mining and normal tip propagation.

An expert/debug interface can later expose per-domain residency if useful.

## Required instrumentation

Before enabling eviction, add counters for:

- resident entries and bytes by data domain;
- pins by reason;
- cache hits/misses by domain;
- storage bytes/records loaded;
- prefetch requests and useful-prefetch rate;
- eviction count;
- cache high-water mark;
- time spent waiting for block-index storage;
- **misses inside no-I/O critical scopes**.

The final counter should normally remain zero.

## Performance acceptance criteria

A lower-memory mode should not be considered successful solely because RSS
falls.

It must also preserve or improve:

- incoming valid block -> `NewPoWValidBlock` fast-relay latency;
- incoming header validation throughput;
- compact-block reconstruction latency;
- `CreateNewBlock()` / GBT refresh latency;
- local found-block submission/activation latency;
- lock hold time around `cs_main`;
- peer sync/download scheduling throughput.

Historical RPC latency may intentionally trade off against RAM use.

## Recommended implementation order

1. Introduce `BlockIndexStore`, residency policy, instrumentation and
   `full` mode with no change in effective residency.
2. Add explicit no-I/O critical scopes and prove the full mode produces zero
   misses.
3. Add pin accounting for existing raw-pointer holders.
4. Move `lastAlgoBlocks` behind the residency layer first and measure memory
   and live-path latency.
5. Move storage-location data and old header payloads behind explicit cache
   access, with special bulk handling for GETHEADERS.
6. Replace full-map/admin scans with persistent-index iterators.
7. Only after the above is stable, consider replacing long-lived
   `CBlockIndex*` identities with record IDs/handles so cold identity shells
   themselves can be evicted.

This staging gives useful memory reductions early while keeping the current
pointer/lifetime model intact until the residency layer has proved itself.


## Implementation status

The first structural milestone is now represented on this branch.

- `BlockIndexStore` owns the existing `BlockMap` while preserving stable
  `CBlockIndex*` identity and the current full-residency behavior.
- The existing `ChainstateManager::BlockIndex()` API still exposes the raw map
  as a temporary compatibility escape hatch. New residency-aware code should use
  `BlockIndexStore` instead.
- Lookup, insertion and no-I/O scope counters are present.
- `RecordBackingRead()` is the mandatory hook for future lazy payload reads.
  Debug builds assert if a backing read occurs inside a no-I/O scope, while
  release builds retain a violation counter.
- Mining template creation, header/block acceptance, best-chain activation and
  fast compact-block relay are marked as no-I/O block-index paths.
- A unit test covers full-mode store behavior, stable object identity and nested
  no-I/O policy state.

This milestone intentionally does not reduce memory yet. Its purpose is to
insert and test the indirection boundary before moving the first real payload,
with `lastAlgoBlocks` remaining the planned first memory-saving target.
