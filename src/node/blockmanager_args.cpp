// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <node/blockmanager_args.h>

#include <common/args.h>
#include <node/blockstorage.h>
#include <tinyformat.h>
#include <util/result.h>
#include <util/translation.h>
#include <validation.h>

#include <cstdint>

namespace node {
util::Result<void> ApplyArgsManOptions(const ArgsManager& args, BlockManager::Options& opts)
{
    // block pruning; get the amount of disk space (in MiB) to allot for block & undo files
    int64_t nPruneArg{args.GetIntArg("-prune", opts.prune_target)};
    if (nPruneArg < 0) {
        return util::Error{_("Prune cannot be configured with a negative value.")};
    }
    uint64_t nPruneTarget{uint64_t(nPruneArg) * 1024 * 1024};
    if (nPruneArg == 1) { // manual pruning: -prune=1
        nPruneTarget = BlockManager::PRUNE_TARGET_MANUAL;
    } else if (nPruneTarget) {
        if (nPruneTarget < MIN_DISK_SPACE_FOR_BLOCK_FILES) {
            return util::Error{strprintf(_("Prune configured below the minimum of %d MiB.  Please use a higher number."), MIN_DISK_SPACE_FOR_BLOCK_FILES / 1024 / 1024)};
        }
    }
    opts.prune_target = nPruneTarget;

    if (auto value{args.GetBoolArg("-fastprune")}) opts.fast_prune = *value;

    if (auto value{args.GetArg("-blockindexmode")}) {
        if (*value == "full") {
            opts.block_index_mode = kernel::BlockIndexResidencyMode::FULL;
        } else if (*value == "balanced") {
            opts.block_index_mode = kernel::BlockIndexResidencyMode::BALANCED;
        } else if (*value == "lowmem") {
            opts.block_index_mode = kernel::BlockIndexResidencyMode::LOWMEM;
        } else {
            return util::Error{strprintf(Untranslated("Invalid -blockindexmode=%s (expected full, balanced, or lowmem)"), *value)};
        }
    }

    if (auto value{args.GetIntArg("-blockindexhotdepth")}) {
        if (*value < 0) {
            return util::Error{Untranslated("-blockindexhotdepth cannot be negative")};
        }
        opts.block_index_hot_depth = static_cast<size_t>(*value);
    }

    if (auto value{args.GetArg("-blockindexcompactshadow")}) {
        if (*value == "off") {
            opts.block_index_compact_shadow = kernel::BlockIndexCompactShadowMode::OFF;
        } else if (*value == "build") {
            opts.block_index_compact_shadow = kernel::BlockIndexCompactShadowMode::BUILD;
        } else if (*value == "verify") {
            opts.block_index_compact_shadow = kernel::BlockIndexCompactShadowMode::VERIFY;
        } else {
            return util::Error{strprintf(Untranslated("Invalid -blockindexcompactshadow=%s (expected off, build, or verify)"), *value)};
        }
    }

    if (auto value{args.GetArg("-blockindexcompactlookup")}) {
        if (*value == "off") {
            opts.block_index_compact_lookup = kernel::BlockIndexCompactLookupMode::OFF;
        } else if (*value == "build") {
            opts.block_index_compact_lookup = kernel::BlockIndexCompactLookupMode::BUILD;
        } else if (*value == "verify") {
            opts.block_index_compact_lookup = kernel::BlockIndexCompactLookupMode::VERIFY;
        } else {
            return util::Error{strprintf(Untranslated("Invalid -blockindexcompactlookup=%s (expected off, build, or verify)"), *value)};
        }
    }

    if (auto value{args.GetArg("-blockindexcompactids")}) {
        if (*value == "off") {
            opts.block_index_compact_ids = kernel::BlockIndexCompactIdsMode::OFF;
        } else if (*value == "build") {
            opts.block_index_compact_ids = kernel::BlockIndexCompactIdsMode::BUILD;
        } else if (*value == "verify") {
            opts.block_index_compact_ids = kernel::BlockIndexCompactIdsMode::VERIFY;
        } else {
            return util::Error{strprintf(Untranslated("Invalid -blockindexcompactids=%s (expected off, build, or verify)"), *value)};
        }
    }

    if (auto value{args.GetArg("-blockindexcompactdelta")}) {
        if (*value == "off") {
            opts.block_index_compact_delta = kernel::BlockIndexCompactDeltaMode::OFF;
        } else if (*value == "build") {
            opts.block_index_compact_delta = kernel::BlockIndexCompactDeltaMode::BUILD;
        } else if (*value == "verify") {
            opts.block_index_compact_delta = kernel::BlockIndexCompactDeltaMode::VERIFY;
        } else {
            return util::Error{strprintf(Untranslated("Invalid -blockindexcompactdelta=%s (expected off, build, or verify)"), *value)};
        }
    }

    return {};
}
} // namespace node
