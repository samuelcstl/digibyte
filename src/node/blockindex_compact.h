// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_NODE_BLOCKINDEX_COMPACT_H
#define DIGIBYTE_NODE_BLOCKINDEX_COMPACT_H

#include <arith_uint256.h>
#include <chain.h>
#include <uint256.h>

#include <cstdint>
#include <limits>
#include <type_traits>

namespace node {

/**
 * Stable identifier for one historical block-index record.
 *
 * A 32-bit id is intentionally sufficient for the first compact-store format:
 * it addresses more than four billion records while keeping active-chain and
 * parent/skip references half the size of native pointers on 64-bit systems.
 */
using BlockIndexId = uint32_t;
static constexpr BlockIndexId INVALID_BLOCK_INDEX_ID{
    std::numeric_limits<BlockIndexId>::max()};

/**
 * Fixed-width historical metadata.
 *
 * This is not a CBlockIndex replacement exposed to validation code. It is the
 * persistent/file-backed representation from which hot CBlockIndex objects can
 * eventually be materialized. Pointer identity, hash ownership and runtime-only
 * candidate ordering deliberately stay outside this record.
 */
struct CompactBlockIndexRecord
{
    BlockIndexId parent{INVALID_BLOCK_INDEX_ID};
    BlockIndexId skip{INVALID_BLOCK_INDEX_ID};

    int32_t height{0};

    int32_t file{0};
    uint32_t data_pos{0};
    uint32_t undo_pos{0};

    uint256 chain_work{};

    uint32_t tx_count{0};
    uint32_t chain_tx_count{0};
    uint32_t status{0};

    int32_t version{0};
    uint256 merkle_root{};
    uint32_t time{0};
    uint32_t bits{0};
    uint32_t nonce{0};

    uint32_t time_max{0};

    static CompactBlockIndexRecord FromBlockIndex(
        const CBlockIndex& index,
        BlockIndexId parent_id,
        BlockIndexId skip_id)
    {
        CompactBlockIndexRecord record;
        record.parent = parent_id;
        record.skip = skip_id;
        record.height = index.nHeight;
        record.file = index.nFile;
        record.data_pos = index.nDataPos;
        record.undo_pos = index.nUndoPos;
        record.chain_work = ArithToUint256(index.nChainWork);
        record.tx_count = index.nTx;
        record.chain_tx_count = index.nChainTx;
        record.status = index.nStatus;
        record.version = index.nVersion;
        record.merkle_root = index.hashMerkleRoot;
        record.time = index.nTime;
        record.bits = index.nBits;
        record.nonce = index.nNonce;
        record.time_max = index.nTimeMax;
        return record;
    }
};

/**
 * One fixed-size slot in the future mmap-able historical store.
 *
 * The authoritative full block hash lives beside its compact metadata. The
 * trailing reserved words make the v1 slot exactly 160 bytes and leave format
 * space for flags/checksums without changing record stride.
 */
struct CompactBlockIndexEntry
{
    uint256 hash{};
    CompactBlockIndexRecord record{};
    uint32_t reserved[2]{};
};

static_assert(std::is_standard_layout_v<CompactBlockIndexRecord>);
static_assert(std::is_standard_layout_v<CompactBlockIndexEntry>);
static_assert(sizeof(CompactBlockIndexRecord) == 120,
              "compact block-index v1 record size changed");
static_assert(sizeof(CompactBlockIndexEntry) == 160,
              "compact block-index v1 entry size changed");

static constexpr uint32_t COMPACT_BLOCK_INDEX_FORMAT_VERSION{1};

} // namespace node

#endif // DIGIBYTE_NODE_BLOCKINDEX_COMPACT_H
