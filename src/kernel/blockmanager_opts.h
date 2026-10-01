// Copyright (c) 2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_KERNEL_BLOCKMANAGER_OPTS_H
#define DIGIBYTE_KERNEL_BLOCKMANAGER_OPTS_H

#include <kernel/notifications_interface.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>

class CChainParams;

namespace kernel {

enum class BlockIndexResidencyMode {
    FULL,
    BALANCED,
    LOWMEM,
};

enum class BlockIndexCompactShadowMode {
    OFF,
    BUILD,
    VERIFY,
};

enum class BlockIndexCompactLookupMode {
    OFF,
    BUILD,
    VERIFY,
};

enum class BlockIndexCompactIdsMode {
    OFF,
    BUILD,
    VERIFY,
};

enum class BlockIndexCompactDeltaMode {
    OFF,
    BUILD,
    VERIFY,
};

static constexpr size_t DEFAULT_BLOCK_INDEX_HOT_DEPTH{40320};

/**
 * An options struct for `BlockManager`, more ergonomically referred to as
 * `BlockManager::Options` due to the using-declaration in `BlockManager`.
 */
struct BlockManagerOpts {
    const CChainParams& chainparams;
    uint64_t prune_target{0};
    bool fast_prune{false};
    BlockIndexResidencyMode block_index_mode{BlockIndexResidencyMode::FULL};
    size_t block_index_hot_depth{DEFAULT_BLOCK_INDEX_HOT_DEPTH};
    BlockIndexCompactShadowMode block_index_compact_shadow{BlockIndexCompactShadowMode::OFF};
    BlockIndexCompactLookupMode block_index_compact_lookup{BlockIndexCompactLookupMode::OFF};
    BlockIndexCompactIdsMode block_index_compact_ids{BlockIndexCompactIdsMode::OFF};
    BlockIndexCompactDeltaMode block_index_compact_delta{BlockIndexCompactDeltaMode::OFF};
    const fs::path blocks_dir;
    Notifications& notifications;
};

} // namespace kernel

#endif // DIGIBYTE_KERNEL_BLOCKMANAGER_OPTS_H
