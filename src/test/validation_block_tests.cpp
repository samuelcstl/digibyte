// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <node/blockindex_compact.h>
#include <node/blockindex_compact_delta.h>
#include <node/blockindex_compact_delta_log.h>
#include <node/blockindex_compact_ids.h>
#include <node/blockindex_compact_lookup.h>
#include <node/blockindex_compact_store.h>
#include <node/miner.h>
#include <pow.h>
#include <random.h>
#include <script/standard.h>
#include <test/util/random.h>
#include <test/util/script.h>
#include <test/util/setup_common.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>

#include <thread>

#define ADVANCE() SetMockTime(GetTime() + Params().GetConsensus().nTargetSpacing * 2 + 1)
#define APPLY_BLOCK_TIME(block) SetMockTime((block)->nTime)

using node::BlockAssembler;
using node::BlockIndexId;
using node::CompactBlockIndexEntry;
using node::CompactBlockIndexRecord;
using node::BlockIndexResidencyMode;
using node::BlockIndexStore;

namespace validation_block_tests {
struct MinerTestingSetup : public RegTestingSetup {
    std::shared_ptr<CBlock> Block(const uint256& prev_hash);
    std::shared_ptr<const CBlock> GoodBlock(const uint256& prev_hash);
    std::shared_ptr<const CBlock> BadBlock(const uint256& prev_hash);
    std::shared_ptr<CBlock> FinalizeBlock(std::shared_ptr<CBlock> pblock);
    void BuildChain(const uint256& root, int height, const unsigned int invalid_rate, const unsigned int branch_rate, const unsigned int max_size, std::vector<std::shared_ptr<const CBlock>>& blocks);
};
} // namespace validation_block_tests

BOOST_FIXTURE_TEST_SUITE(validation_block_tests, MinerTestingSetup)

BOOST_AUTO_TEST_CASE(compact_block_index_record_snapshot)
{
    CBlockHeader header;
    CBlockIndex index{header};
    const BlockIndexId parent_id{123};
    const BlockIndexId skip_id{45};

    {
        LOCK(cs_main);
        index.nHeight = 456;
        index.StorageFile() = 7;
        index.DataPos() = 1234;
        index.UndoPos() = 5678;
        index.nChainWork = UintToArith256(uint256S("123456"));
        index.nTx = 11;
        index.nChainTx = 222;
        index.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO;
        index.nVersion = 0x20000000;
        index.MerkleRoot() = uint256S("abcdef");
        index.nTime = 1'700'000'000;
        index.nBits = 0x1d00ffff;
        index.nNonce = 42;
        index.TimeMax() = 1'700'000'123;

        const CompactBlockIndexRecord record{
            CompactBlockIndexRecord::FromBlockIndex(index, parent_id, skip_id)};

        BOOST_CHECK_EQUAL(record.parent, parent_id);
        BOOST_CHECK_EQUAL(record.skip, skip_id);
        BOOST_CHECK_EQUAL(record.height, index.nHeight);
        BOOST_CHECK_EQUAL(record.file, index.StorageFile());
        BOOST_CHECK_EQUAL(record.data_pos, index.DataPos());
        BOOST_CHECK_EQUAL(record.undo_pos, index.UndoPos());
        BOOST_CHECK(record.chain_work == ArithToUint256(index.nChainWork));
        BOOST_CHECK_EQUAL(record.tx_count, index.nTx);
        BOOST_CHECK_EQUAL(record.chain_tx_count, index.nChainTx);
        BOOST_CHECK_EQUAL(record.status, index.nStatus);
        BOOST_CHECK_EQUAL(record.version, index.nVersion);
        BOOST_CHECK(record.merkle_root == index.MerkleRoot());
        BOOST_CHECK_EQUAL(record.time, index.nTime);
        BOOST_CHECK_EQUAL(record.bits, index.nBits);
        BOOST_CHECK_EQUAL(record.nonce, index.nNonce);
        BOOST_CHECK_EQUAL(record.time_max, index.TimeMax());
    }

    BOOST_CHECK_EQUAL(sizeof(CompactBlockIndexRecord), 120U);
    BOOST_CHECK_EQUAL(sizeof(CompactBlockIndexEntry), 160U);
    BOOST_CHECK_EQUAL(sizeof(node::CompactBlockIndexFileHeader), 128U);
    BOOST_CHECK_EQUAL(sizeof(node::CompactBlockIndexLookupHeader), 128U);
    BOOST_CHECK_EQUAL(sizeof(node::CompactBlockIndexLookupSlot), 16U);
    BOOST_CHECK_EQUAL(sizeof(node::CompactBlockIndexIdsHeader), 128U);
    BOOST_CHECK_EQUAL(sizeof(node::CompactBlockIndexDeltaHeader), 128U);
    BOOST_CHECK_EQUAL(sizeof(node::CompactBlockIndexDeltaLogHeader), 128U);
    BOOST_CHECK_EQUAL(sizeof(node::CompactBlockIndexDeltaLogRecord), 168U);
    if constexpr (sizeof(void*) == 8) {
        // Historical storage/merkle/time-max domains now live behind the
        // residency payload. The stable identity/topology shell shrinks from
        // 152 to 112 bytes while retaining pointer identity.
        BOOST_CHECK_EQUAL(sizeof(CBlockIndex), 112U);
        BOOST_CHECK_LE(sizeof(BlockIndexResidentPayload), 56U);
        BOOST_CHECK_EQUAL(sizeof(BlockIndexAlgoHistory), sizeof(CBlockIndex*) * NUM_ALGOS_IMPL);
    }
}

BOOST_AUTO_TEST_CASE(compact_block_index_mapped_store)
{
    const fs::path path{m_path_root / "compact-index-test.dat"};

    node::CompactBlockIndexFileHeader header;
    header.entry_count = 2;
    header.genesis_hash = Params().GetConsensus().hashGenesisBlock;

    node::CompactBlockIndexEntry first;
    first.hash = uint256S("01");
    first.record.height = 10;

    node::CompactBlockIndexEntry second;
    second.hash = uint256S("02");
    second.record.height = 11;
    second.record.parent = 0;

    FILE* file{fsbridge::fopen(path, "wb")};
    BOOST_REQUIRE(file != nullptr);
    BOOST_REQUIRE_EQUAL(std::fwrite(&header, sizeof(header), 1, file), 1U);
    BOOST_REQUIRE_EQUAL(std::fwrite(&first, sizeof(first), 1, file), 1U);
    BOOST_REQUIRE_EQUAL(std::fwrite(&second, sizeof(second), 1, file), 1U);
    BOOST_REQUIRE_EQUAL(std::fclose(file), 0);

    node::CompactBlockIndexStore store;
    std::string error;
    BOOST_REQUIRE_MESSAGE(
        store.Open(path, header.genesis_hash, error),
        "failed to open mapped compact store: " << error);

    BOOST_CHECK(store.IsOpen());
    BOOST_CHECK_EQUAL(store.EntryCount(), 2U);
    BOOST_CHECK_EQUAL(store.SizeBytes(),
                      sizeof(header) + 2 * sizeof(node::CompactBlockIndexEntry));

    const auto* got_first{store.Get(0)};
    const auto* got_second{store.Get(1)};
    BOOST_REQUIRE(got_first);
    BOOST_REQUIRE(got_second);
    BOOST_CHECK(got_first->hash == first.hash);
    BOOST_CHECK_EQUAL(got_first->record.height, 10);
    BOOST_CHECK(got_second->hash == second.hash);
    BOOST_CHECK_EQUAL(got_second->record.parent, 0U);
    BOOST_CHECK(store.Get(node::INVALID_BLOCK_INDEX_ID) == nullptr);
    BOOST_CHECK(store.Get(2) == nullptr);
}

BOOST_AUTO_TEST_CASE(compact_block_index_metadata_delta_log)
{
    const fs::path log_path{m_path_root / "compact-index-delta.log"};
    const uint256 genesis{Params().GetConsensus().hashGenesisBlock};
    std::string error;

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDeltaLog::Create(
            log_path,
            /*base_generation=*/7,
            /*base_entry_count=*/10,
            /*snapshot_tail_entry_count=*/3,
            genesis,
            error),
        "failed to create compact metadata delta log: " << error);

    node::CompactBlockIndexDeltaLog log;
    BOOST_REQUIRE_MESSAGE(
        log.Open(
            log_path,
            /*expected_base_generation=*/7,
            /*expected_base_entry_count=*/10,
            /*expected_snapshot_tail_entry_count=*/3,
            genesis,
            error),
        "failed to open compact metadata delta log: " << error);

    std::vector<node::CompactBlockIndexDeltaLogRecord> records(4);

    // Immutable base records may acquire mutable status/position updates too.
    records[0].id = 2;
    records[0].entry.hash = uint256S("9999");
    records[0].entry.record.height = 2;
    records[0].entry.record.status = BLOCK_VALID_TREE;

    records[1].id = 11;
    records[1].entry.hash = uint256S("aaaa");
    records[1].entry.record.height = 101;

    // Same id again: replay order deliberately permits metadata replacement.
    records[2].id = 11;
    records[2].entry.hash = uint256S("aaaa");
    records[2].entry.record.height = 101;
    records[2].entry.record.status = BLOCK_VALID_TREE;

    // First id after the snapshot tail [10, 13).
    records[3].id = 13;
    records[3].entry.hash = uint256S("bbbb");
    records[3].entry.record.height = 103;

    BOOST_REQUIRE_MESSAGE(log.Append(records, error), "append failed: " << error);
    BOOST_CHECK_EQUAL(log.BaseGeneration(), 7U);
    BOOST_CHECK_EQUAL(log.BaseEntryCount(), 10U);
    BOOST_CHECK_EQUAL(log.SnapshotTailEntryCount(), 3U);
    BOOST_CHECK_EQUAL(log.RecordCount(), records.size());
    BOOST_CHECK_EQUAL(
        log.SizeBytes(),
        sizeof(node::CompactBlockIndexDeltaLogHeader) +
            records.size() * sizeof(node::CompactBlockIndexDeltaLogRecord));

    std::vector<node::CompactBlockIndexDeltaLogRecord> read;
    BOOST_REQUIRE_MESSAGE(
        log.ForEach(
            [&](const node::CompactBlockIndexDeltaLogRecord& record) {
                read.push_back(record);
                return true;
            },
            error),
        "delta log replay failed: " << error);

    BOOST_REQUIRE_EQUAL(read.size(), records.size());
    BOOST_CHECK_EQUAL(read[0].id, 2U);
    BOOST_CHECK_EQUAL(read[0].entry.record.status, BLOCK_VALID_TREE);
    BOOST_CHECK_EQUAL(read[1].id, 11U);
    BOOST_CHECK_EQUAL(read[2].id, 11U);
    BOOST_CHECK_EQUAL(read[2].entry.record.status, BLOCK_VALID_TREE);
    BOOST_CHECK_EQUAL(read[3].id, 13U);
    BOOST_CHECK(read[3].entry.hash == uint256S("bbbb"));

    node::CompactBlockIndexDeltaLog reopened;
    BOOST_REQUIRE_MESSAGE(
        reopened.Open(log_path, 7, 10, 3, genesis, error),
        "reopen failed: " << error);
    BOOST_CHECK_EQUAL(reopened.RecordCount(), records.size());
}

BOOST_AUTO_TEST_CASE(compact_block_index_metadata_pending_batch)
{
    const fs::path log_path{m_path_root / "compact-index-delta-main.log"};
    const fs::path pending_path{m_path_root / "compact-index-delta.pending"};
    const uint256 genesis{Params().GetConsensus().hashGenesisBlock};
    std::string error;

    BOOST_REQUIRE(node::CompactBlockIndexDeltaLog::Create(
        log_path, 7, 10, 3, genesis, error));
    BOOST_REQUIRE(node::CompactBlockIndexDeltaLog::Create(
        pending_path, 7, 10, 3, genesis, error));

    node::CompactBlockIndexDeltaLog log;
    node::CompactBlockIndexDeltaLog pending;
    BOOST_REQUIRE(log.Open(log_path, 7, 10, 3, genesis, error));
    BOOST_REQUIRE(pending.Open(pending_path, 7, 10, 3, genesis, error));

    std::vector<node::CompactBlockIndexDeltaLogRecord> staged(2);
    staged[0].id = 2;
    staged[0].entry.hash = uint256S("aaaa");
    staged[0].entry.record.height = 2;
    staged[0].entry.record.status = BLOCK_VALID_TREE;
    staged[1].id = 13;
    staged[1].entry.hash = uint256S("bbbb");
    staged[1].entry.record.height = 103;

    BOOST_REQUIRE_MESSAGE(pending.Append(staged, error), error);
    BOOST_CHECK_EQUAL(pending.RecordCount(), staged.size());

    std::vector<node::CompactBlockIndexDeltaLogRecord> recovered;
    BOOST_REQUIRE_MESSAGE(
        pending.ForEach(
            [&](const node::CompactBlockIndexDeltaLogRecord& record) {
                recovered.push_back(record);
                return true;
            },
            error),
        error);

    BOOST_REQUIRE_EQUAL(recovered.size(), staged.size());
    BOOST_REQUIRE_MESSAGE(log.Append(recovered, error), error);
    BOOST_CHECK_EQUAL(log.RecordCount(), staged.size());

    std::vector<node::CompactBlockIndexDeltaLogRecord> published;
    BOOST_REQUIRE_MESSAGE(
        log.ForEach(
            [&](const node::CompactBlockIndexDeltaLogRecord& record) {
                published.push_back(record);
                return true;
            },
            error),
        error);

    BOOST_REQUIRE_EQUAL(published.size(), staged.size());
    BOOST_CHECK_EQUAL(published[0].id, 2U);
    BOOST_CHECK(published[0].entry.hash == uint256S("aaaa"));
    BOOST_CHECK_EQUAL(published[1].id, 13U);
    BOOST_CHECK(published[1].entry.hash == uint256S("bbbb"));
}

BOOST_AUTO_TEST_CASE(compact_block_index_metadata_delta_compaction_publish)
{
    const fs::path delta_base{m_path_root / "compact-index-delta-publish"};
    const fs::path log_base{m_path_root / "compact-index-delta-publish.log"};
    const fs::path state_path{m_path_root / "compact-index-delta-publish.state"};
    const uint256 genesis{Params().GetConsensus().hashGenesisBlock};
    std::string error;

    const auto slot_a{node::CompactBlockIndexDeltaSlot::A};
    const auto slot_b{node::CompactBlockIndexDeltaSlot::B};

    std::vector<node::CompactBlockIndexEntry> initial(2);
    initial[0].hash = uint256S("10");
    initial[0].record.height = 100;
    initial[1].hash = uint256S("11");
    initial[1].record.height = 101;

    const fs::path delta_a{node::CompactBlockIndexDeltaState::SlotPath(delta_base, slot_a)};
    const fs::path log_a{node::CompactBlockIndexDeltaState::SlotPath(log_base, slot_a)};
    BOOST_REQUIRE_MESSAGE(node::CompactBlockIndexDelta::Build(delta_a, 7, 10, genesis, initial, error), error);
    BOOST_REQUIRE_MESSAGE(node::CompactBlockIndexDeltaLog::Create(log_a, 7, 10, 2, genesis, error), error);

    node::CompactBlockIndexDeltaLog source_log;
    BOOST_REQUIRE_MESSAGE(source_log.Open(log_a, 7, 10, 2, genesis, error), error);

    std::vector<node::CompactBlockIndexDeltaLogRecord> updates(4);
    updates[0].id = 2;
    updates[0].entry.hash = uint256S("02");
    updates[0].entry.record.height = 2;
    updates[0].entry.record.status = BLOCK_VALID_TREE;
    updates[1].id = 11;
    updates[1].entry.hash = uint256S("11");
    updates[1].entry.record.height = 101;
    updates[1].entry.record.status = BLOCK_VALID_CHAIN;
    updates[2].id = 12;
    updates[2].entry.hash = uint256S("12");
    updates[2].entry.record.height = 102;
    updates[3] = updates[2];
    updates[3].entry.record.status = BLOCK_VALID_SCRIPTS;
    BOOST_REQUIRE_MESSAGE(source_log.Append(updates, error), error);

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDeltaState::Publish(
            state_path, slot_a, 1, 7, 10, 2, genesis, error),
        error);

    node::CompactBlockIndexDelta source_delta;
    BOOST_REQUIRE_MESSAGE(source_delta.Open(delta_a, 7, 10, genesis, error), error);

    node::CompactBlockIndexDeltaCompaction plan;
    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDelta::PlanCompaction(
            source_delta, source_log, 13, plan, error),
        error);

    BOOST_REQUIRE_EQUAL(plan.tail_entries.size(), 3U);
    BOOST_REQUIRE_EQUAL(plan.base_updates.size(), 1U);

    const fs::path delta_b{node::CompactBlockIndexDeltaState::SlotPath(delta_base, slot_b)};
    const fs::path log_b{node::CompactBlockIndexDeltaState::SlotPath(log_base, slot_b)};
    BOOST_REQUIRE_MESSAGE(node::CompactBlockIndexDelta::Build(delta_b, 7, 10, genesis, plan.tail_entries, error), error);
    BOOST_REQUIRE_MESSAGE(node::CompactBlockIndexDeltaLog::Create(log_b, 7, 10, 3, genesis, error), error);

    node::CompactBlockIndexDeltaLog compacted_log;
    BOOST_REQUIRE_MESSAGE(compacted_log.Open(log_b, 7, 10, 3, genesis, error), error);
    BOOST_REQUIRE_MESSAGE(compacted_log.Append(plan.base_updates, error), error);

    // Preparing B does not change authority.
    node::CompactBlockIndexDeltaState state;
    BOOST_REQUIRE_MESSAGE(state.Open(state_path, 7, 10, genesis, error), error);
    BOOST_CHECK(state.ActiveSlot() == slot_a);

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDeltaState::Publish(
            state_path, slot_b, 2, 7, 10, 3, genesis, error),
        error);
    BOOST_REQUIRE_MESSAGE(state.Open(state_path, 7, 10, genesis, error), error);

    BOOST_CHECK(state.ActiveSlot() == slot_b);
    BOOST_CHECK_EQUAL(state.Sequence(), 2U);
    BOOST_CHECK_EQUAL(state.SnapshotTailEntryCount(), 3U);

    node::CompactBlockIndexDelta compacted_delta;
    node::CompactBlockIndexDeltaLog reopened_log;
    BOOST_REQUIRE_MESSAGE(compacted_delta.Open(delta_b, 7, 10, genesis, error), error);
    BOOST_REQUIRE_MESSAGE(reopened_log.Open(log_b, 7, 10, 3, genesis, error), error);
    BOOST_CHECK_EQUAL(reopened_log.RecordCount(), 1U);

    node::CompactBlockIndexDeltaCompaction stable;
    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDelta::PlanCompaction(
            compacted_delta, reopened_log, 13, stable, error),
        error);
    BOOST_CHECK_EQUAL(stable.tail_entries.size(), 3U);
    BOOST_CHECK_EQUAL(stable.base_updates.size(), 1U);
    BOOST_CHECK_EQUAL(stable.base_updates[0].id, 2U);
}

BOOST_AUTO_TEST_CASE(compact_block_index_metadata_delta_legacy_migration)
{
    const fs::path legacy_delta{m_path_root / "compact-index-delta-legacy"};
    const fs::path legacy_log{m_path_root / "compact-index-delta-legacy.log"};
    const fs::path state_path{m_path_root / "compact-index-delta-legacy.state"};
    const uint256 genesis{Params().GetConsensus().hashGenesisBlock};
    std::string error;

    std::vector<node::CompactBlockIndexEntry> entries(2);
    entries[0].hash = uint256S("10");
    entries[0].record.height = 100;
    entries[1].hash = uint256S("11");
    entries[1].record.height = 101;

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDelta::Build(
            legacy_delta, 7, 10, genesis, entries, error),
        error);
    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDeltaLog::Create(
            legacy_log, 7, 10, 2, genesis, error),
        error);

    node::CompactBlockIndexDeltaLog log;
    BOOST_REQUIRE_MESSAGE(log.Open(legacy_log, 7, 10, 2, genesis, error), error);

    std::vector<node::CompactBlockIndexDeltaLogRecord> updates(2);
    updates[0].id = 11;
    updates[0].entry.hash = uint256S("11");
    updates[0].entry.record.height = 101;
    updates[0].entry.record.status = BLOCK_VALID_CHAIN;
    updates[1].id = 12;
    updates[1].entry.hash = uint256S("12");
    updates[1].entry.record.height = 102;
    BOOST_REQUIRE_MESSAGE(log.Append(updates, error), error);

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDeltaState::MigrateLegacyPair(
            state_path,
            legacy_delta,
            legacy_log,
            legacy_delta,
            legacy_log,
            7,
            10,
            genesis,
            error),
        error);

    BOOST_CHECK(fs::exists(legacy_delta));
    BOOST_CHECK(fs::exists(legacy_log));

    node::CompactBlockIndexDeltaState state;
    BOOST_REQUIRE_MESSAGE(state.Open(state_path, 7, 10, genesis, error), error);
    BOOST_CHECK(state.ActiveSlot() == node::CompactBlockIndexDeltaSlot::A);
    BOOST_CHECK_EQUAL(state.Sequence(), 1U);
    BOOST_CHECK_EQUAL(state.SnapshotTailEntryCount(), 2U);

    const fs::path migrated_delta{
        node::CompactBlockIndexDeltaState::SlotPath(
            legacy_delta, node::CompactBlockIndexDeltaSlot::A)};
    const fs::path migrated_log{
        node::CompactBlockIndexDeltaState::SlotPath(
            legacy_log, node::CompactBlockIndexDeltaSlot::A)};

    node::CompactBlockIndexDelta delta;
    node::CompactBlockIndexDeltaLog migrated;
    BOOST_REQUIRE_MESSAGE(delta.Open(migrated_delta, 7, 10, genesis, error), error);
    BOOST_REQUIRE_MESSAGE(migrated.Open(migrated_log, 7, 10, 2, genesis, error), error);
    BOOST_CHECK_EQUAL(delta.TailEntryCount(), 2U);
    BOOST_CHECK_EQUAL(migrated.RecordCount(), 2U);
    BOOST_CHECK(migrated.SizeBytes() == log.SizeBytes());
}

BOOST_AUTO_TEST_CASE(compact_block_index_metadata_delta_pair_selector)
{
    const fs::path delta_base{m_path_root / "compact-index-delta-pair"};
    const fs::path log_base{m_path_root / "compact-index-delta-pair.log"};
    const fs::path state_path{m_path_root / "compact-index-delta-pair.state"};
    const uint256 genesis{Params().GetConsensus().hashGenesisBlock};
    std::string error;

    const auto slot_a{node::CompactBlockIndexDeltaSlot::A};
    const auto slot_b{node::CompactBlockIndexDeltaSlot::B};

    const fs::path delta_a{
        node::CompactBlockIndexDeltaState::SlotPath(delta_base, slot_a)};
    const fs::path log_a{
        node::CompactBlockIndexDeltaState::SlotPath(log_base, slot_a)};
    const fs::path delta_b{
        node::CompactBlockIndexDeltaState::SlotPath(delta_base, slot_b)};
    const fs::path log_b{
        node::CompactBlockIndexDeltaState::SlotPath(log_base, slot_b)};

    std::vector<node::CompactBlockIndexEntry> entries_a(2);
    entries_a[0].hash = uint256S("10");
    entries_a[1].hash = uint256S("11");

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDelta::Build(
            delta_a, 7, 10, genesis, entries_a, error),
        error);
    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDeltaLog::Create(
            log_a, 7, 10, 2, genesis, error),
        error);

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDeltaState::Publish(
            state_path, slot_a, /*sequence=*/1, 7, 10, 2, genesis, error),
        error);

    node::CompactBlockIndexDeltaState state;
    BOOST_REQUIRE_MESSAGE(state.Open(state_path, 7, 10, genesis, error), error);
    BOOST_CHECK(state.IsOpen());
    BOOST_CHECK(state.ActiveSlot() == slot_a);
    BOOST_CHECK_EQUAL(state.Sequence(), 1U);
    BOOST_CHECK_EQUAL(state.SnapshotTailEntryCount(), 2U);
    BOOST_CHECK(
        node::CompactBlockIndexDeltaState::OtherSlot(state.ActiveSlot()) == slot_b);

    node::CompactBlockIndexDelta opened_a;
    node::CompactBlockIndexDeltaLog opened_log_a;
    BOOST_REQUIRE_MESSAGE(opened_a.Open(delta_a, 7, 10, genesis, error), error);
    BOOST_REQUIRE_MESSAGE(opened_log_a.Open(log_a, 7, 10, 2, genesis, error), error);

    // Prepare a complete inactive B pair before publishing the selector.
    std::vector<node::CompactBlockIndexEntry> entries_b(3);
    entries_b[0].hash = uint256S("10");
    entries_b[1].hash = uint256S("11");
    entries_b[2].hash = uint256S("12");

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDelta::Build(
            delta_b, 7, 10, genesis, entries_b, error),
        error);
    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDeltaLog::Create(
            log_b, 7, 10, 3, genesis, error),
        error);

    // Until this atomic state-file replacement, A remains authoritative.
    BOOST_REQUIRE_MESSAGE(state.Open(state_path, 7, 10, genesis, error), error);
    BOOST_CHECK(state.ActiveSlot() == slot_a);
    BOOST_CHECK_EQUAL(state.Sequence(), 1U);

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDeltaState::Publish(
            state_path, slot_b, /*sequence=*/2, 7, 10, 3, genesis, error),
        error);

    BOOST_REQUIRE_MESSAGE(state.Open(state_path, 7, 10, genesis, error), error);
    BOOST_CHECK(state.ActiveSlot() == slot_b);
    BOOST_CHECK_EQUAL(state.Sequence(), 2U);
    BOOST_CHECK_EQUAL(state.SnapshotTailEntryCount(), 3U);

    node::CompactBlockIndexDelta opened_b;
    node::CompactBlockIndexDeltaLog opened_log_b;
    BOOST_REQUIRE_MESSAGE(opened_b.Open(delta_b, 7, 10, genesis, error), error);
    BOOST_REQUIRE_MESSAGE(opened_log_b.Open(log_b, 7, 10, 3, genesis, error), error);

    // The previous A pair remains intact, making an interrupted next
    // compaction harmless until a later selector publication chooses it again.
    BOOST_CHECK_EQUAL(opened_a.TailEntryCount(), 2U);
    BOOST_CHECK_EQUAL(opened_log_a.RecordCount(), 0U);
    BOOST_CHECK_EQUAL(opened_b.TailEntryCount(), 3U);
    BOOST_CHECK_EQUAL(opened_log_b.RecordCount(), 0U);

    BOOST_CHECK(
        node::CompactBlockIndexDeltaState::SlotPath(delta_base, slot_a) !=
        node::CompactBlockIndexDeltaState::SlotPath(delta_base, slot_b));
}

BOOST_AUTO_TEST_CASE(compact_block_index_metadata_delta_compaction_plan)
{
    const fs::path delta_path{m_path_root / "compact-index-delta-compact.dat"};
    const fs::path log_path{m_path_root / "compact-index-delta-compact.log"};
    const uint256 genesis{Params().GetConsensus().hashGenesisBlock};
    std::string error;

    std::vector<node::CompactBlockIndexEntry> snapshot_entries(3);
    snapshot_entries[0].hash = uint256S("10");
    snapshot_entries[0].record.height = 100;
    snapshot_entries[1].hash = uint256S("11");
    snapshot_entries[1].record.height = 101;
    snapshot_entries[2].hash = uint256S("12");
    snapshot_entries[2].record.height = 102;

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDelta::Build(
            delta_path, 7, 10, genesis, snapshot_entries, error),
        error);

    node::CompactBlockIndexDelta delta;
    BOOST_REQUIRE_MESSAGE(delta.Open(delta_path, 7, 10, genesis, error), error);

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDeltaLog::Create(
            log_path, 7, 10, 3, genesis, error),
        error);

    node::CompactBlockIndexDeltaLog log;
    BOOST_REQUIRE_MESSAGE(log.Open(log_path, 7, 10, 3, genesis, error), error);

    std::vector<node::CompactBlockIndexDeltaLogRecord> updates(6);

    // Base update survives compaction as a sparse overlay.
    updates[0].id = 2;
    updates[0].entry.hash = uint256S("02");
    updates[0].entry.record.height = 2;
    updates[0].entry.record.status = BLOCK_VALID_TREE;

    // Two writes to the same checkpoint-tail id: last one must win.
    updates[1].id = 11;
    updates[1].entry.hash = uint256S("11");
    updates[1].entry.record.height = 101;
    updates[1].entry.record.status = BLOCK_VALID_TREE;

    updates[2] = updates[1];
    updates[2].entry.record.status = BLOCK_VALID_SCRIPTS;

    // Post-checkpoint extensions.
    updates[3].id = 13;
    updates[3].entry.hash = uint256S("13");
    updates[3].entry.record.height = 103;

    updates[4] = updates[3];
    updates[4].entry.record.status = BLOCK_VALID_CHAIN;

    updates[5].id = 14;
    updates[5].entry.hash = uint256S("14");
    updates[5].entry.record.height = 104;

    BOOST_REQUIRE_MESSAGE(log.Append(updates, error), error);

    node::CompactBlockIndexDeltaCompaction compacted;
    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDelta::PlanCompaction(
            delta, log, /*expected_next_id=*/15, compacted, error),
        error);

    BOOST_REQUIRE_EQUAL(compacted.tail_entries.size(), 5U);
    BOOST_CHECK(compacted.tail_entries[0].hash == uint256S("10"));
    BOOST_CHECK(compacted.tail_entries[1].hash == uint256S("11"));
    BOOST_CHECK_EQUAL(
        compacted.tail_entries[1].record.status,
        BLOCK_VALID_SCRIPTS);
    BOOST_CHECK(compacted.tail_entries[2].hash == uint256S("12"));
    BOOST_CHECK(compacted.tail_entries[3].hash == uint256S("13"));
    BOOST_CHECK_EQUAL(
        compacted.tail_entries[3].record.status,
        BLOCK_VALID_CHAIN);
    BOOST_CHECK(compacted.tail_entries[4].hash == uint256S("14"));

    BOOST_REQUIRE_EQUAL(compacted.base_updates.size(), 1U);
    BOOST_CHECK_EQUAL(compacted.base_updates[0].id, 2U);
    BOOST_CHECK(compacted.base_updates[0].entry.hash == uint256S("02"));

    node::CompactBlockIndexDeltaCompaction invalid;
    error.clear();
    BOOST_CHECK(
        !node::CompactBlockIndexDelta::PlanCompaction(
            delta, log, /*expected_next_id=*/16, invalid, error));
    BOOST_CHECK(!error.empty());
}

BOOST_AUTO_TEST_CASE(compact_block_index_metadata_delta_snapshot)
{
    const fs::path delta_path{m_path_root / "compact-index-delta.dat"};
    const uint256 genesis{Params().GetConsensus().hashGenesisBlock};
    std::string error;

    std::vector<node::CompactBlockIndexEntry> entries(3);
    entries[0].hash = uint256S("11");
    entries[0].record.height = 100;
    entries[0].record.parent = 9;
    entries[1].hash = uint256S("12");
    entries[1].record.height = 101;
    entries[1].record.parent = 10;
    entries[2].hash = uint256S("13");
    entries[2].record.height = 102;
    entries[2].record.parent = 11;

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexDelta::Build(
            delta_path,
            /*base_generation=*/7,
            /*base_entry_count=*/10,
            genesis,
            entries,
            error),
        "failed to build compact metadata delta: " << error);

    node::CompactBlockIndexDelta delta;
    BOOST_REQUIRE_MESSAGE(
        delta.Open(
            delta_path,
            /*expected_base_generation=*/7,
            /*expected_base_entry_count=*/10,
            genesis,
            error),
        "failed to open compact metadata delta: " << error);

    BOOST_CHECK(delta.IsOpen());
    BOOST_CHECK_EQUAL(delta.BaseEntryCount(), 10U);
    BOOST_CHECK_EQUAL(delta.TailEntryCount(), entries.size());
    BOOST_CHECK_EQUAL(
        delta.SizeBytes(),
        sizeof(node::CompactBlockIndexDeltaHeader) +
            entries.size() * sizeof(node::CompactBlockIndexEntry));

    BOOST_CHECK(delta.Get(9) == nullptr);
    BOOST_REQUIRE(delta.Get(10));
    BOOST_REQUIRE(delta.Get(11));
    BOOST_REQUIRE(delta.Get(12));
    BOOST_CHECK(delta.Get(13) == nullptr);

    BOOST_CHECK(delta.Get(10)->hash == entries[0].hash);
    BOOST_CHECK_EQUAL(delta.Get(10)->record.height, 100);
    BOOST_CHECK_EQUAL(delta.Get(11)->record.parent, 10U);
    BOOST_CHECK(delta.Get(12)->hash == entries[2].hash);
}

BOOST_AUTO_TEST_CASE(compact_block_index_persistent_ids)
{
    const fs::path ids_path{m_path_root / "compact-index-ids.dat"};
    const uint256 genesis{Params().GetConsensus().hashGenesisBlock};
    std::string error;

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexIds::Create(
            ids_path,
            /*base_generation=*/7,
            /*base_entry_count=*/3,
            genesis,
            error),
        "failed to create compact id tail: " << error);

    node::CompactBlockIndexIds ids;
    BOOST_REQUIRE_MESSAGE(
        ids.Open(ids_path, 7, 3, genesis, error),
        "failed to open compact id tail: " << error);

    BOOST_CHECK(ids.IsOpen());
    BOOST_CHECK_EQUAL(ids.BaseEntryCount(), 3U);
    BOOST_CHECK_EQUAL(ids.TailEntryCount(), 0U);
    BOOST_CHECK_EQUAL(ids.NextId(), 3U);

    const std::vector<uint256> hashes{
        uint256S("04"),
        uint256S("05"),
        uint256S("06"),
    };
    BOOST_REQUIRE_MESSAGE(ids.Append(hashes, error), "append failed: " << error);
    BOOST_CHECK_EQUAL(ids.TailEntryCount(), 3U);
    BOOST_CHECK_EQUAL(ids.NextId(), 6U);
    BOOST_CHECK_EQUAL(
        ids.SizeBytes(),
        sizeof(node::CompactBlockIndexIdsHeader) +
            hashes.size() * sizeof(uint256));

    std::vector<std::pair<BlockIndexId, uint256>> read;
    BOOST_REQUIRE_MESSAGE(
        ids.ForEachTail(
            [&](BlockIndexId id, const uint256& hash) {
                read.emplace_back(id, hash);
                return true;
            },
            error),
        "tail read failed: " << error);

    BOOST_REQUIRE_EQUAL(read.size(), hashes.size());
    for (size_t i = 0; i < hashes.size(); ++i) {
        BOOST_CHECK_EQUAL(read[i].first, static_cast<BlockIndexId>(3 + i));
        BOOST_CHECK(read[i].second == hashes[i]);
    }

    BOOST_REQUIRE_MESSAGE(ids.TruncateTail(2, error), "truncate failed: " << error);
    BOOST_CHECK_EQUAL(ids.TailEntryCount(), 2U);
    BOOST_CHECK_EQUAL(ids.NextId(), 5U);

    node::CompactBlockIndexIds reopened;
    BOOST_REQUIRE_MESSAGE(
        reopened.Open(ids_path, 7, 3, genesis, error),
        "reopen failed: " << error);
    BOOST_CHECK_EQUAL(reopened.TailEntryCount(), 2U);
    BOOST_CHECK_EQUAL(reopened.NextId(), 5U);
}

BOOST_AUTO_TEST_CASE(compact_block_index_persistent_lookup)
{
    const fs::path compact_path{m_path_root / "compact-index-lookup-source.dat"};
    const fs::path lookup_path{m_path_root / "compact-index-lookup.dat"};

    node::CompactBlockIndexFileHeader header;
    header.entry_count = 3;
    header.generation = 7;
    header.genesis_hash = Params().GetConsensus().hashGenesisBlock;

    std::array<node::CompactBlockIndexEntry, 3> entries{};
    entries[0].hash = uint256S("01");
    entries[1].hash = uint256S("02");
    entries[2].hash = uint256S("abcdef");

    FILE* file{fsbridge::fopen(compact_path, "wb")};
    BOOST_REQUIRE(file != nullptr);
    BOOST_REQUIRE_EQUAL(std::fwrite(&header, sizeof(header), 1, file), 1U);
    BOOST_REQUIRE_EQUAL(std::fwrite(entries.data(), sizeof(entries[0]), entries.size(), file), entries.size());
    BOOST_REQUIRE_EQUAL(std::fclose(file), 0);

    node::CompactBlockIndexStore store;
    std::string error;
    BOOST_REQUIRE_MESSAGE(
        store.Open(compact_path, header.genesis_hash, error),
        "failed to open compact lookup source: " << error);

    BOOST_REQUIRE_MESSAGE(
        node::CompactBlockIndexLookup::Build(lookup_path, store, error),
        "failed to build compact lookup: " << error);

    node::CompactBlockIndexLookup lookup;
    BOOST_REQUIRE_MESSAGE(
        lookup.Open(lookup_path, store, error),
        "failed to open compact lookup: " << error);

    BOOST_CHECK(lookup.IsOpen());
    BOOST_CHECK_EQUAL(lookup.EntryCount(), entries.size());
    BOOST_CHECK(lookup.SlotCount() >= entries.size());

    for (BlockIndexId id = 0; id < entries.size(); ++id) {
        const auto found{lookup.Find(entries[id].hash, store)};
        BOOST_REQUIRE(found.has_value());
        BOOST_CHECK_EQUAL(*found, id);
    }

    BOOST_CHECK(!lookup.Find(uint256S("deadbeef"), store).has_value());

    BOOST_REQUIRE_MESSAGE(
        lookup.LoadResidentProbeFront(error),
        "failed to load resident compact lookup front: " << error);
    BOOST_CHECK(lookup.HasResidentProbeFront());
    BOOST_CHECK(lookup.ResidentProbeFrontBytes() > 0U);

    for (BlockIndexId id = 0; id < entries.size(); ++id) {
        const auto found{lookup.FindResident(entries[id].hash, store)};
        BOOST_REQUIRE(found.has_value());
        BOOST_CHECK_EQUAL(*found, id);
    }

    BOOST_CHECK(!lookup.FindResident(uint256S("deadbeef"), store).has_value());
}

BOOST_AUTO_TEST_CASE(block_index_store_compact_identity_cutover)
{
    BlockIndexStore store{BlockIndexResidencyMode::BALANCED, 2};

    const std::array<uint256, 3> hashes{
        uint256S("01"), uint256S("02"), uint256S("03")};

    auto [it_a, inserted_a] = store.try_emplace(hashes[0]);
    auto [it_b, inserted_b] = store.try_emplace(hashes[1]);
    auto [it_c, inserted_c] = store.try_emplace(hashes[2]);
    BOOST_REQUIRE(inserted_a && inserted_b && inserted_c);

    CBlockIndex& a{it_a->second};
    CBlockIndex& b{it_b->second};
    CBlockIndex& c{it_c->second};

    a.phashBlock = &it_a->first;
    b.phashBlock = &it_b->first;
    c.phashBlock = &it_c->first;
    a.m_compact_id = 0;
    b.m_compact_id = 1;
    c.m_compact_id = 2;
    a.nHeight = 0;
    b.nHeight = 1;
    c.nHeight = 2;
    b.pprev = &a;
    c.pprev = &b;
    c.pskip = &a;
    a.StorageFile() = 10;
    b.StorageFile() = 11;
    c.StorageFile() = 12;

    std::vector<CBlockIndex*> by_id{&a, &b, &c};

    BOOST_REQUIRE(store.PrepareCompactIdentityCutover(
        by_id,
        /*immutable_count=*/2,
        [&](BlockIndexId id) -> const uint256* {
            return id < 2 ? &hashes[id] : nullptr;
        }));

    CBlockIndex* new_a{store.RemapPreparedIdentity(&a)};
    CBlockIndex* new_b{store.RemapPreparedIdentity(&b)};
    CBlockIndex* new_c{store.RemapPreparedIdentity(&c)};

    BOOST_REQUIRE(new_a && new_b && new_c);
    BOOST_CHECK_NE(new_a, &a);
    BOOST_CHECK_NE(new_b, &b);
    BOOST_CHECK_NE(new_c, &c);
    BOOST_CHECK_EQUAL(new_b->pprev, new_a);
    BOOST_CHECK_EQUAL(new_c->pprev, new_b);
    BOOST_CHECK_EQUAL(new_c->pskip, new_a);
    BOOST_CHECK_EQUAL(new_c->StorageFile(), 12);
    BOOST_CHECK_EQUAL(new_a->GetBlockHash(), hashes[0]);
    BOOST_CHECK_EQUAL(new_c->GetBlockHash(), hashes[2]);

    store.CommitCompactIdentityCutover();

    BOOST_CHECK(store.CompactIdentityActive());
    BOOST_CHECK(store.RawMap().empty());
    BOOST_CHECK_EQUAL(store.size(), 3U);
    BOOST_CHECK_EQUAL(store.ImmutableIdentityCount(), 2U);
    BOOST_CHECK_EQUAL(store.ByCompactId(0), new_a);
    BOOST_CHECK_EQUAL(store.ByCompactId(2), new_c);

    // Immutable-base lookup is supplied by BlockManager's compact lookup.
    BOOST_CHECK(store.Lookup(hashes[0]) == nullptr);
    // Tail/live identity remains exact and fully resident.
    BOOST_CHECK_EQUAL(store.Lookup(hashes[2]), new_c);

    store.UpdateHotWindow(new_b);
    // Once the hot front is built, recent immutable identities are resident.
    BOOST_CHECK_EQUAL(store.Lookup(hashes[0]), new_a);
    BOOST_CHECK_EQUAL(store.Lookup(hashes[1]), new_b);
}

BOOST_AUTO_TEST_CASE(block_index_store_payload_cache_indirection)
{
    const size_t budget{2 * sizeof(BlockIndexResidentPayload)};
    BlockIndexStore store{
        BlockIndexResidencyMode::LOWMEM,
        /*hot_depth=*/1,
        budget};

    const uint256 hash_a{uint256S("01")};
    const uint256 hash_b{uint256S("02")};
    const uint256 hash_c{uint256S("03")};

    auto [it_a, inserted_a] = store.try_emplace(hash_a);
    auto [it_b, inserted_b] = store.try_emplace(hash_b);
    auto [it_c, inserted_c] = store.try_emplace(hash_c);
    BOOST_REQUIRE(inserted_a && inserted_b && inserted_c);

    CBlockIndex& a{it_a->second};
    CBlockIndex& b{it_b->second};
    CBlockIndex& c{it_c->second};

    a.m_compact_id = 0;
    b.m_compact_id = 1;
    c.m_compact_id = 2;
    a.nHeight = 0;
    b.nHeight = 1;
    c.nHeight = 2;
    b.pprev = &a;
    c.pprev = &b;

    a.StorageFile() = 10;
    b.StorageFile() = 11;
    c.StorageFile() = 12;
    a.TimeMax() = 100;
    b.TimeMax() = 101;
    c.TimeMax() = 102;

    std::array<BlockIndexResidentPayload, 3> backing{
        a.ResidentPayload(),
        b.ResidentPayload(),
        c.ResidentPayload()};
    size_t loads{0};

    BOOST_REQUIRE(store.ActivatePayloadCache(
        &c,
        [&](const CBlockIndex& index, BlockIndexResidentPayload& payload) {
            BOOST_REQUIRE(index.m_compact_id < backing.size());
            payload = backing[index.m_compact_id];
            ++loads;
            return true;
        }));

    // Only the hot tip remains pinned after the bootstrap arena is released.
    BOOST_CHECK_EQUAL(store.ResidentPayloads(), 1U);
    BOOST_CHECK(c.HasResidentPayload());
    BOOST_CHECK(!a.HasResidentPayload());
    BOOST_CHECK(!b.HasResidentPayload());

    BOOST_CHECK_EQUAL(a.StorageFile(), 10);
    BOOST_CHECK_EQUAL(loads, 1U);
    BOOST_CHECK_EQUAL(store.ResidentPayloads(), 2U);

    // The one-entry discretionary cache evicts A when B is materialized.
    BOOST_CHECK_EQUAL(b.TimeMax(), 101U);
    BOOST_CHECK_EQUAL(loads, 2U);
    BOOST_CHECK(!a.HasResidentPayload());
    BOOST_CHECK(b.HasResidentPayload());
    BOOST_CHECK(c.HasResidentPayload());

    const auto stats{store.GetResidencyStats()};
    BOOST_CHECK_EQUAL(stats.backing_reads, 2U);
    BOOST_CHECK_EQUAL(stats.payload_cache_evictions, 1U);
    BOOST_CHECK_EQUAL(stats.no_io_violations, 0U);

    const uint256 hash_d{uint256S("04")};
    auto [it_d, inserted_d] = store.try_emplace(hash_d);
    BOOST_REQUIRE(inserted_d);
    CBlockIndex& d{it_d->second};
    d.m_compact_id = 3;
    d.nHeight = 3;
    d.pprev = &c;
    d.StorageFile() = 13;

    store.UpdateHotWindow(&d);
    BOOST_CHECK(d.HasResidentPayload());

    BOOST_CHECK_EQUAL(a.StorageFile(), 10);
    BOOST_CHECK(d.HasResidentPayload());
    BOOST_CHECK(!c.HasResidentPayload());

    const size_t resident_before_disable{store.ResidentPayloads()};
    store.DisablePayloadEviction();
    BOOST_CHECK_EQUAL(b.TimeMax(), 101U);
    BOOST_CHECK(store.ResidentPayloads() > resident_before_disable);
    BOOST_CHECK(!store.PayloadEvictionEnabled());
}

BOOST_AUTO_TEST_CASE(block_index_store_cold_known_payload_pin_before_no_io)
{
    const size_t budget{2 * sizeof(BlockIndexResidentPayload)};
    BlockIndexStore store{
        BlockIndexResidencyMode::LOWMEM,
        /*hot_depth=*/1,
        budget};

    const uint256 hash_a{uint256S("01")};
    const uint256 hash_b{uint256S("02")};
    const uint256 hash_c{uint256S("03")};

    auto [it_a, inserted_a] = store.try_emplace(hash_a);
    auto [it_b, inserted_b] = store.try_emplace(hash_b);
    auto [it_c, inserted_c] = store.try_emplace(hash_c);
    BOOST_REQUIRE(inserted_a && inserted_b && inserted_c);

    CBlockIndex& a{it_a->second};
    CBlockIndex& b{it_b->second};
    CBlockIndex& c{it_c->second};

    a.m_compact_id = 0;
    b.m_compact_id = 1;
    c.m_compact_id = 2;
    a.nHeight = 0;
    b.nHeight = 1;
    c.nHeight = 2;
    b.pprev = &a;
    c.pprev = &b;

    a.StorageFile() = 10;
    b.StorageFile() = 11;
    c.StorageFile() = 12;

    std::array<BlockIndexResidentPayload, 3> backing{
        a.ResidentPayload(),
        b.ResidentPayload(),
        c.ResidentPayload()};
    size_t loads{0};

    // Model an already-known header ahead of the active tip: B is the active
    // hot tip, while C exists in the block index but becomes cold when the
    // historical bootstrap payload arena is released.
    BOOST_REQUIRE(store.ActivatePayloadCache(
        &b,
        [&](const CBlockIndex& index, BlockIndexResidentPayload& payload) {
            BOOST_REQUIRE(index.m_compact_id < backing.size());
            payload = backing[index.m_compact_id];
            ++loads;
            return true;
        }));

    BOOST_CHECK(b.HasResidentPayload());
    BOOST_CHECK(!c.HasResidentPayload());

    {
        // AcceptBlock must do this before entering its no-I/O section.
        auto pre_pin = store.PinPayloadScoped(c);
        BOOST_CHECK(c.HasResidentPayload());
        BOOST_CHECK_EQUAL(loads, 1U);

        auto no_io = store.EnterNoIO();
        BOOST_CHECK(!store.BackingReadAllowed());

        // Model ReceivedBlockTransactions taking its dirty pin and mutating the
        // payload while the no-I/O guard is active.
        store.PinPayload(c);
        c.StorageFile() = 42;
        c.DataPos() = 43;
        c.UndoPos() = 44;
        store.ReleasePayloadPin(c);

        BOOST_CHECK_EQUAL(c.StorageFile(), 42);
        BOOST_CHECK_EQUAL(c.DataPos(), 43U);
        BOOST_CHECK_EQUAL(c.UndoPos(), 44U);
    }

    const auto stats{store.GetResidencyStats()};
    BOOST_CHECK_EQUAL(stats.backing_reads, 1U);
    BOOST_CHECK_EQUAL(stats.no_io_violations, 0U);
}

BOOST_AUTO_TEST_CASE(block_index_store_bootstrap_arena_pointer_stability)
{
    BlockIndexStore store{BlockIndexResidencyMode::BALANCED, 8};

    const uint256 first_hash{uint256S("01")};
    auto [first_it, first_inserted] = store.try_emplace(first_hash);
    BOOST_REQUIRE(first_inserted);
    CBlockIndex& first{first_it->second};
    first.StorageFile() = 77;
    BlockIndexResidentPayload* first_payload{first.m_resident_payload};
    BOOST_REQUIRE(first_payload);

    for (uint32_t i = 2; i < 140000; ++i) {
        uint256 hash;
        WriteLE32(hash.data(), i);
        auto [it, inserted] = store.try_emplace(hash);
        BOOST_REQUIRE(inserted);
        it->second.StorageFile() = static_cast<int>(i & 0x7fffffff);
    }

    BOOST_CHECK(first.m_resident_payload == first_payload);
    BOOST_CHECK_EQUAL(first.StorageFile(), 77);
    BOOST_CHECK_EQUAL(store.ResidentPayloads(), 139999U);
}

BOOST_AUTO_TEST_CASE(block_index_store_full_residency_invariants)
{
    BlockIndexStore store;
    BOOST_CHECK(store.GetMode() == BlockIndexResidencyMode::FULL);
    BOOST_CHECK(store.empty());
    BOOST_CHECK(store.BackingReadAllowed());

    const uint256 hash_a{uint256S("01")};
    const uint256 hash_b{uint256S("02")};

    auto [it_a, inserted_a] = store.try_emplace(hash_a);
    BOOST_REQUIRE(inserted_a);
    CBlockIndex* stable_a = &it_a->second;

    auto [it_b, inserted_b] = store.try_emplace(hash_b);
    BOOST_REQUIRE(inserted_b);
    BOOST_CHECK_EQUAL(store.size(), 2U);

    {
        auto no_io = store.EnterNoIO();
        BOOST_CHECK(!store.BackingReadAllowed());

        auto found = store.find(hash_a);
        BOOST_REQUIRE(found != store.end());
        BOOST_CHECK_EQUAL(&found->second, stable_a);
    }

    BOOST_CHECK(store.BackingReadAllowed());

    const auto stats{store.GetResidencyStats()};
    BOOST_CHECK_EQUAL(stats.insertions, 2U);
    BOOST_CHECK_EQUAL(stats.no_io_scopes, 1U);
    BOOST_CHECK_EQUAL(stats.backing_reads, 0U);
    BOOST_CHECK_EQUAL(stats.no_io_violations, 0U);
    BOOST_CHECK(stats.lookup_hits >= 1U);
}

struct TestSubscriber final : public CValidationInterface {
    uint256 m_expected_tip;

    explicit TestSubscriber(uint256 tip) : m_expected_tip(tip) {}

    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, pindexNew->GetBlockHash());
    }

    void BlockConnected(ChainstateRole role, const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, block->hashPrevBlock);
        BOOST_CHECK_EQUAL(m_expected_tip, pindex->pprev->GetBlockHash());

        m_expected_tip = block->GetHash();
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        BOOST_CHECK_EQUAL(m_expected_tip, block->GetHash());
        BOOST_CHECK_EQUAL(m_expected_tip, pindex->GetBlockHash());

        m_expected_tip = block->hashPrevBlock;
    }
};

std::shared_ptr<CBlock> MinerTestingSetup::Block(const uint256& prev_hash)
{
    static int i = 0;
    static uint64_t time = Params().GenesisBlock().nTime;
    
    // Reset counter periodically to avoid overflow and ensure deterministic behavior
    if (i > 10000) i = 0;

    // Determine which algorithm to use based on the current chain height
    int algo = ALGO_SCRYPT; // Default to SCRYPT for early blocks
    int nHeight = 0;
    
    {
        LOCK(cs_main);
        const CBlockIndex* pindexPrev = m_node.chainman->m_blockman.LookupBlockIndex(prev_hash);
        if (pindexPrev) {
            nHeight = pindexPrev->nHeight;
            const auto& consensus = Params().GetConsensus();
            
            // Only use multi-algo after the activation height
            if (nHeight >= consensus.multiAlgoDiffChangeTarget) {
                // Rotate through all algorithms based on height for deterministic behavior
                static const int algos[] = {ALGO_SHA256D, ALGO_SCRYPT, ALGO_GROESTL, ALGO_SKEIN, ALGO_QUBIT};
                algo = algos[(nHeight + 1) % 5];
            }
        }
    }
    
    auto ptemplate = BlockAssembler{m_node.chainman->ActiveChainstate(), m_node.mempool.get()}.CreateNewBlock(CScript{} << i++ << OP_TRUE, algo);
    auto pblock = std::make_shared<CBlock>(ptemplate->block);
    pblock->hashPrevBlock = prev_hash;
    pblock->nTime = ++time;

    // Make the coinbase transaction with two outputs:
    // One zero-value one that has a unique pubkey to make sure that blocks at the same height can have a different hash
    // Another one that has the coinbase reward in a P2WSH with OP_TRUE as witness program to make it easy to spend
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vout.resize(2);
    txCoinbase.vout[1].scriptPubKey = P2WSH_OP_TRUE;
    txCoinbase.vout[1].nValue = txCoinbase.vout[0].nValue;
    txCoinbase.vout[0].nValue = 0;
    txCoinbase.vin[0].scriptWitness.SetNull();
    // Always pad with OP_0 at the end to avoid bad-cb-length error
    txCoinbase.vin[0].scriptSig = CScript{} << (nHeight + 1) << OP_0;
    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));

    return pblock;
}

std::shared_ptr<CBlock> MinerTestingSetup::FinalizeBlock(std::shared_ptr<CBlock> pblock)
{
    auto consensus = Params().GetConsensus();
    
    ADVANCE();
    pblock->nTime = GetTime();

    {
        LOCK(cs_main); // For m_node.chainman->m_blockman.LookupBlockIndex
        const CBlockIndex* prev_block = m_node.chainman->m_blockman.LookupBlockIndex(pblock->hashPrevBlock);
        m_node.chainman->GenerateCoinbaseCommitment(*pblock, prev_block);

        CBlockHeader header = pblock->GetBlockHeader();
        pblock->nBits = GetNextWorkRequired(prev_block, &header, consensus, pblock->GetAlgo());
    }

    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);

    while (!CheckProofOfWork(GetPoWAlgoHash(pblock->GetBlockHeader()), pblock->nBits, Params().GetConsensus())) {
        ++(pblock->nNonce);
    }

    // submit block header, so that miner can get the block height from the
    // global state and the node has the topology of the chain
    BlockValidationState ignored;
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlockHeaders({pblock->GetBlockHeader()}, true, ignored));

    return pblock;
}

// construct a valid block
std::shared_ptr<const CBlock> MinerTestingSetup::GoodBlock(const uint256& prev_hash)
{
    return FinalizeBlock(Block(prev_hash));
}

// construct an invalid block (but with a valid header)
std::shared_ptr<const CBlock> MinerTestingSetup::BadBlock(const uint256& prev_hash)
{
    auto pblock = Block(prev_hash);

    CMutableTransaction coinbase_spend;
    coinbase_spend.vin.emplace_back(COutPoint(pblock->vtx[0]->GetHash(), 0), CScript(), 0);
    coinbase_spend.vout.push_back(pblock->vtx[0]->vout[0]);

    CTransactionRef tx = MakeTransactionRef(coinbase_spend);
    pblock->vtx.push_back(tx);

    auto ret = FinalizeBlock(pblock);
    return ret;
}

void MinerTestingSetup::BuildChain(const uint256& root, int height, const unsigned int invalid_rate, const unsigned int branch_rate, const unsigned int max_size, std::vector<std::shared_ptr<const CBlock>>& blocks)
{
    if (height <= 0 || blocks.size() >= max_size) return;

    bool gen_invalid = InsecureRandRange(100) < invalid_rate;
    bool gen_fork = InsecureRandRange(100) < branch_rate;

    const std::shared_ptr<const CBlock> pblock = gen_invalid ? BadBlock(root) : GoodBlock(root);
    blocks.push_back(pblock);
    if (!gen_invalid) {
        BuildChain(pblock->GetHash(), height - 1, invalid_rate, branch_rate, max_size, blocks);
    }

    if (gen_fork) {
        blocks.push_back(GoodBlock(root));
        BuildChain(blocks.back()->GetHash(), height - 1, invalid_rate, branch_rate, max_size, blocks);
    }
}

BOOST_AUTO_TEST_CASE(processnewblock_signals_ordering)
{
    // build a large-ish chain that's likely to have some forks
    std::vector<std::shared_ptr<const CBlock>> blocks;
    while (blocks.size() < 50) {
        blocks.clear();
        BuildChain(Params().GenesisBlock().GetHash(), 100, 15, 10, 500, blocks);
    }

    bool ignored;
    BlockValidationState state;
    std::vector<CBlockHeader> headers;
    std::transform(blocks.begin(), blocks.end(), std::back_inserter(headers), [](std::shared_ptr<const CBlock> b) { return b->GetBlockHeader(); });

    // Process all the headers so we understand the toplogy of the chain
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlockHeaders(headers, true, state));

    // Connect the genesis block and drain any outstanding events
    BOOST_CHECK(Assert(m_node.chainman)->ProcessNewBlock(std::make_shared<CBlock>(Params().GenesisBlock()), true, true, &ignored));
    SyncWithValidationInterfaceQueue();

    // subscribe to events (this subscriber will validate event ordering)
    const CBlockIndex* initial_tip = nullptr;
    {
        LOCK(cs_main);
        initial_tip = m_node.chainman->ActiveChain().Tip();
    }
    auto sub = std::make_shared<TestSubscriber>(initial_tip->GetBlockHash());
    RegisterSharedValidationInterface(sub);

    // create a bunch of threads that repeatedly process a block generated above at random
    // this will create parallelism and randomness inside validation - the ValidationInterface
    // will subscribe to events generated during block validation and assert on ordering invariance
    std::vector<std::thread> threads;
    threads.reserve(10);
    for (int i = 0; i < 10; i++) {
        threads.emplace_back([&]() {
            bool ignored;
            FastRandomContext insecure;
            for (int i = 0; i < 1000; i++) {
                auto block = blocks[insecure.randrange(blocks.size() - 1)];
                Assert(m_node.chainman)->ProcessNewBlock(block, true, true, &ignored);
            }

            // to make sure that eventually we process the full chain - do it here
            for (const auto& block : blocks) {
                if (block->vtx.size() == 1) {
                    bool processed = Assert(m_node.chainman)->ProcessNewBlock(block, true, true, &ignored);
                    assert(processed);
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
    SyncWithValidationInterfaceQueue();

    UnregisterSharedValidationInterface(sub);

    LOCK(cs_main);
    BOOST_CHECK_EQUAL(sub->m_expected_tip, m_node.chainman->ActiveChain().Tip()->GetBlockHash());
}

/**
 * Test that mempool updates happen atomically with reorgs.
 *
 * This prevents RPC clients, among others, from retrieving immediately-out-of-date mempool data
 * during large reorgs.
 *
 * The test verifies this by creating a chain of `num_txs` blocks, matures their coinbases, and then
 * submits txns spending from their coinbase to the mempool. A fork chain is then processed,
 * invalidating the txns and evicting them from the mempool.
 *
 * We verify that the mempool updates atomically by polling it continuously
 * from another thread during the reorg and checking that its size only changes
 * once. The size changing exactly once indicates that the polling thread's
 * view of the mempool is either consistent with the chain state before reorg,
 * or consistent with the chain state after the reorg, and not just consistent
 * with some intermediate state during the reorg.
 */
BOOST_AUTO_TEST_CASE(mempool_locks_reorg)
{
    bool ignored;
    auto ProcessBlock = [&](std::shared_ptr<const CBlock> block) -> bool {
        APPLY_BLOCK_TIME(block);
        return Assert(m_node.chainman)->ProcessNewBlock(block, /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&ignored);
    };

    // Process all mined blocks
    BOOST_REQUIRE(ProcessBlock(std::make_shared<CBlock>(Params().GenesisBlock())));
    auto last_mined = GoodBlock(Params().GenesisBlock().GetHash());
    BOOST_REQUIRE(ProcessBlock(last_mined));

    // Run the test multiple times
    for (int test_runs = 3; test_runs > 0; --test_runs) {
        BOOST_CHECK_EQUAL(last_mined->GetHash(), WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip()->GetBlockHash()));

        // Later on split from here
        const uint256 split_hash{last_mined->hashPrevBlock};

        // Create a bunch of transactions to spend the miner rewards of the
        // most recent blocks
        std::vector<CTransactionRef> txs;
        for (int num_txs = 22; num_txs > 0; --num_txs) {
            CMutableTransaction mtx;
            mtx.vin.emplace_back(COutPoint{last_mined->vtx[0]->GetHash(), 1}, CScript{});
            mtx.vin[0].scriptWitness.stack.push_back(WITNESS_STACK_ELEM_OP_TRUE);
            mtx.vout.push_back(last_mined->vtx[0]->vout[1]);
            mtx.vout[0].nValue -= 10000;
            txs.push_back(MakeTransactionRef(mtx));

            last_mined = GoodBlock(last_mined->GetHash());
            BOOST_REQUIRE(ProcessBlock(last_mined));
        }

        // Mature the inputs of the txs
        for (int j = COINBASE_MATURITY_2; j > 0; --j) {
            last_mined = GoodBlock(last_mined->GetHash());
            BOOST_REQUIRE(ProcessBlock(last_mined));
        }

        // Mine a reorg (and hold it back) before adding the txs to the mempool
        const uint256 tip_init{last_mined->GetHash()};

        std::vector<std::shared_ptr<const CBlock>> reorg;
        last_mined = GoodBlock(split_hash);
        reorg.push_back(last_mined);
        for (size_t j = COINBASE_MATURITY_2 + txs.size() + 1; j > 0; --j) {
            last_mined = GoodBlock(last_mined->GetHash());
            reorg.push_back(last_mined);
        }

        // Add the txs to the tx pool
        {
            LOCK(cs_main);
            for (const auto& tx : txs) {
                const MempoolAcceptResult result = m_node.chainman->ProcessTransaction(tx);
                BOOST_REQUIRE(result.m_result_type == MempoolAcceptResult::ResultType::VALID);
            }
        }

        // Check that all txs are in the pool
        {
            LOCK(m_node.mempool->cs);
            BOOST_CHECK_EQUAL(m_node.mempool->mapTx.size(), txs.size());
        }

        // Run a thread that simulates an RPC caller that is polling while
        // validation is doing a reorg
        std::thread rpc_thread{[&]() {
            // This thread is checking that the mempool either contains all of
            // the transactions invalidated by the reorg, or none of them, and
            // not some intermediate amount.
            while (true) {
                LOCK(m_node.mempool->cs);
                if (m_node.mempool->mapTx.size() == 0) {
                    // We are done with the reorg
                    break;
                }
                // Internally, we might be in the middle of the reorg, but
                // externally the reorg to the most-proof-of-work chain should
                // be atomic. So the caller assumes that the returned mempool
                // is consistent. That is, it has all txs that were there
                // before the reorg.
                assert(m_node.mempool->mapTx.size() == txs.size());
                continue;
            }
            LOCK(cs_main);
            // We are done with the reorg, so the tip must have changed
            assert(tip_init != m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }};

        // Submit the reorg in this thread to invalidate and remove the txs from the tx pool
        for (const auto& b : reorg) {
            ProcessBlock(b);
        }
        // Check that the reorg was eventually successful
        BOOST_CHECK_EQUAL(last_mined->GetHash(), WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip()->GetBlockHash()));

        // We can join the other thread, which returns when the reorg was successful
        rpc_thread.join();
    }
}

BOOST_AUTO_TEST_CASE(witness_commitment_index)
{
    LOCK(Assert(m_node.chainman)->GetMutex());
    CScript pubKey;
    pubKey << 1 << OP_TRUE;
    
    // Use SCRYPT since this test runs on genesis (height 0) where only SCRYPT is active
    auto ptemplate = BlockAssembler{m_node.chainman->ActiveChainstate(), m_node.mempool.get()}.CreateNewBlock(pubKey, ALGO_SCRYPT);
    CBlock pblock = ptemplate->block;

    CTxOut witness;
    witness.scriptPubKey.resize(MINIMUM_WITNESS_COMMITMENT);
    witness.scriptPubKey[0] = OP_RETURN;
    witness.scriptPubKey[1] = 0x24;
    witness.scriptPubKey[2] = 0xaa;
    witness.scriptPubKey[3] = 0x21;
    witness.scriptPubKey[4] = 0xa9;
    witness.scriptPubKey[5] = 0xed;

    // A witness larger than the minimum size is still valid
    CTxOut min_plus_one = witness;
    min_plus_one.scriptPubKey.resize(MINIMUM_WITNESS_COMMITMENT + 1);

    CTxOut invalid = witness;
    invalid.scriptPubKey[0] = OP_VERIFY;

    CMutableTransaction txCoinbase(*pblock.vtx[0]);
    txCoinbase.vout.resize(4);
    txCoinbase.vout[0] = witness;
    txCoinbase.vout[1] = witness;
    txCoinbase.vout[2] = min_plus_one;
    txCoinbase.vout[3] = invalid;
    pblock.vtx[0] = MakeTransactionRef(std::move(txCoinbase));

    BOOST_CHECK_EQUAL(GetWitnessCommitmentIndex(pblock), 2);
}
BOOST_AUTO_TEST_SUITE_END()
