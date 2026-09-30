// Copyright (c) 2018-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <boost/test/unit_test.hpp>

#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <node/blockindex_compact.h>
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
    CBlockIndex index;
    const BlockIndexId parent_id{123};
    const BlockIndexId skip_id{45};

    {
        LOCK(cs_main);
        index.nHeight = 456;
        index.nFile = 7;
        index.nDataPos = 1234;
        index.nUndoPos = 5678;
        index.nChainWork = UintToArith256(uint256S("123456"));
        index.nTx = 11;
        index.nChainTx = 222;
        index.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO;
        index.nVersion = 0x20000000;
        index.hashMerkleRoot = uint256S("abcdef");
        index.nTime = 1'700'000'000;
        index.nBits = 0x1d00ffff;
        index.nNonce = 42;
        index.nTimeMax = 1'700'000'123;

        const CompactBlockIndexRecord record{
            CompactBlockIndexRecord::FromBlockIndex(index, parent_id, skip_id)};

        BOOST_CHECK_EQUAL(record.parent, parent_id);
        BOOST_CHECK_EQUAL(record.skip, skip_id);
        BOOST_CHECK_EQUAL(record.height, index.nHeight);
        BOOST_CHECK_EQUAL(record.file, index.nFile);
        BOOST_CHECK_EQUAL(record.data_pos, index.nDataPos);
        BOOST_CHECK_EQUAL(record.undo_pos, index.nUndoPos);
        BOOST_CHECK(record.chain_work == ArithToUint256(index.nChainWork));
        BOOST_CHECK_EQUAL(record.tx_count, index.nTx);
        BOOST_CHECK_EQUAL(record.chain_tx_count, index.nChainTx);
        BOOST_CHECK_EQUAL(record.status, index.nStatus);
        BOOST_CHECK_EQUAL(record.version, index.nVersion);
        BOOST_CHECK(record.merkle_root == index.hashMerkleRoot);
        BOOST_CHECK_EQUAL(record.time, index.nTime);
        BOOST_CHECK_EQUAL(record.bits, index.nBits);
        BOOST_CHECK_EQUAL(record.nonce, index.nNonce);
        BOOST_CHECK_EQUAL(record.time_max, index.nTimeMax);
    }

    BOOST_CHECK_EQUAL(sizeof(CompactBlockIndexRecord), 120U);
    BOOST_CHECK_EQUAL(sizeof(CompactBlockIndexEntry), 160U);
    BOOST_CHECK_EQUAL(sizeof(node::CompactBlockIndexFileHeader), 128U);
    if constexpr (sizeof(void*) == 8) {
        // m_compact_id consumes the former alignment padding before the payload
        // pointer; generation-2 preparation must not grow the balanced shell.
        BOOST_CHECK_EQUAL(sizeof(CBlockIndex), 152U);
    }
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
