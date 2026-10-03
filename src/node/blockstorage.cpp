// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <node/blockstorage.h>

#include <chain.h>
#include <clientversion.h>
#include <consensus/validation.h>
#include <crypto/common.h>
#include <dbwrapper.h>
#include <flatfile.h>
#include <hash.h>
#include <kernel/chain.h>
#include <kernel/chainparams.h>
#include <kernel/messagestartchars.h>
#include <logging.h>
#include <node/blockindex_compact.h>
#include <node/interface_ui.h>
#include <pow.h>
#include <primitives/block.h>
#include <reverse_iterator.h>
#include <signet.h>
#include <streams.h>
#include <sync.h>
#include <undo.h>
#include <util/batchpriority.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/signalinterrupt.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

namespace kernel {
static constexpr uint8_t DB_BLOCK_FILES{'f'};
static constexpr uint8_t DB_BLOCK_INDEX{'b'};
static constexpr uint8_t DB_FLAG{'F'};
static constexpr uint8_t DB_REINDEX_FLAG{'R'};
static constexpr uint8_t DB_LAST_BLOCK{'l'};
// Keys used in previous version that might still be found in the DB:
// BlockTreeDB::DB_TXINDEX_BLOCK{'T'};
// BlockTreeDB::DB_TXINDEX{'t'}
// BlockTreeDB::ReadFlag("txindex")

bool BlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo& info)
{
    return Read(std::make_pair(DB_BLOCK_FILES, nFile), info);
}

bool BlockTreeDB::WriteReindexing(bool fReindexing)
{
    if (fReindexing) {
        return Write(DB_REINDEX_FLAG, uint8_t{'1'});
    } else {
        return Erase(DB_REINDEX_FLAG);
    }
}

void BlockTreeDB::ReadReindexing(bool& fReindexing)
{
    fReindexing = Exists(DB_REINDEX_FLAG);
}

bool BlockTreeDB::ReadLastBlockFile(int& nFile)
{
    return Read(DB_LAST_BLOCK, nFile);
}

bool BlockTreeDB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*>>& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo)
{
    CDBBatch batch(*this);
    for (const auto& [file, info] : fileInfo) {
        batch.Write(std::make_pair(DB_BLOCK_FILES, file), *info);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (const CBlockIndex* bi : blockinfo) {
        batch.Write(std::make_pair(DB_BLOCK_INDEX, bi->GetBlockHash()), CDiskBlockIndex{bi});
    }
    return WriteBatch(batch, true);
}

bool BlockTreeDB::WriteBlockIndexBatch(const std::vector<const CBlockIndex*>& blockinfo, bool sync)
{
    CDBBatch batch(*this);
    for (const CBlockIndex* bi : blockinfo) {
        batch.Write(std::make_pair(DB_BLOCK_INDEX, bi->GetBlockHash()), CDiskBlockIndex{bi});
    }
    return WriteBatch(batch, sync);
}

bool BlockTreeDB::WriteFlag(const std::string& name, bool fValue)
{
    return Write(std::make_pair(DB_FLAG, name), fValue ? uint8_t{'1'} : uint8_t{'0'});
}

bool BlockTreeDB::ReadFlag(const std::string& name, bool& fValue)
{
    uint8_t ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch)) {
        return false;
    }
    fValue = ch == uint8_t{'1'};
    return true;
}

bool BlockTreeDB::LoadBlockIndexGuts(const Consensus::Params& consensusParams, std::function<CBlockIndex*(const uint256&)> insertBlockIndex, const util::SignalInterrupt& interrupt)
{
    AssertLockHeld(::cs_main);
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    pcursor->Seek(std::make_pair(DB_BLOCK_INDEX, uint256()));

    // Do not make a complete LevelDB pass merely to calculate a cosmetic
    // percentage. At mainnet scale that duplicate traversal costs tens of
    // seconds before we deserialize a single record.
    int nCount = 0;
    int nChainWorkCached = 0;
    const auto deserialize_start{SteadyClock::now()};

    // Load m_block_index
    while (pcursor->Valid()) {
        if (interrupt) return false;

        nCount++;
        std::pair<uint8_t, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_BLOCK_INDEX) {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = insertBlockIndex(diskindex.ConstructBlockHash());
                pindexNew->pprev          = insertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->StorageFile()  = diskindex.StorageFile();
                pindexNew->DataPos()      = diskindex.DataPos();
                pindexNew->UndoPos()      = diskindex.UndoPos();
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->MerkleRoot()   = diskindex.MerkleRoot();
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nTx            = diskindex.nTx;
                if (diskindex.HasPersistedChainWork()) {
                    pindexNew->nChainWork = diskindex.nChainWork;
                    ++nChainWorkCached;
                }

                // Only apply PoW optimization for mainnet
                // Check if this is mainnet by comparing genesis block hash
                const bool isMainnet = (consensusParams.hashGenesisBlock == uint256S("0x7497ea1b465eb39f1c8f507bc877078fe016d6fcb6dfad3a64c98dcc6e1e8496"));

                if (isMainnet) {
                    // Only check proof of work for genesis block and checkpoints to speed up loading
                    // This dramatically improves wallet startup time by skipping PoW checks for ~21M blocks
                    // while still validating critical blocks (genesis + checkpoints)
                    bool shouldCheckPoW = false;

                    // Always check genesis block (height 0)
                    if (pindexNew->nHeight == 0) {
                        shouldCheckPoW = true;
                        LogPrint(BCLog::BLOCKSTORAGE, "LoadBlockIndex: Checking PoW for genesis block at height %d\n", pindexNew->nHeight);
                    }

                    // Hardcoded critical checkpoints for DigiByte mainnet
                    // These must match exactly with checkpoints in chainparams.cpp
                    static const std::map<int, uint256> criticalCheckpoints = {
                        {0, uint256S("0x7497ea1b465eb39f1c8f507bc877078fe016d6fcb6dfad3a64c98dcc6e1e8496")}, // Genesis
                        {5000, uint256S("0x95753d284404118788a799ac754a3fdb5d817f5bd73a78697dfe40985c085596")},
                        {10000, uint256S("0x12f90b8744f3b965e107ad9fd8b33ba6d95a91882fbc4b5f8588d70d494bed88")},
                        {12000, uint256S("0xa1266acba91dc3d5737d9e8c6e21b7a91901f7f4c48082ce3d84dd394a13e415")},
                        {14300, uint256S("0x24f665d71b0c6c88f6f72a863e9f1ba8e835cc52d13ad895dc5426021c7d2c48")},
                        {30000, uint256S("0x17c69ef6b403571b1bd333c91fbe116e451ba8281be12aa6bafb0486764bb315")},
                        {60000, uint256S("0x57b2c612b60462a3d6c388c8b30a68cb6f7e2034eea962b12b7ef506454fa2c1")},
                        {110000, uint256S("0xab2da24656493015f2fd288994661e1cc657d90aa34c755514af044aaaf1569d")},
                        {141100, uint256S("0x145c2cb5239a4e019c730ce8468d927a3955529c2bae077850783da97ddbca05")},
                        {141656, uint256S("0x683d27720429f28bcfa22d8385b7a06f307c8fd918d49215148fbd41a0dda595")},
                        {245000, uint256S("0x852c475c605e1f20bbe60219c811abaeef08bf0d4ff87eef59200fd7a7567fa7")},
                        {302000, uint256S("0xfb6d14ac5e0208f00d941db1fcbfe050f093cfd0c05ed151c809e4428bc14286")},
                        {331000, uint256S("0xbd1a1d002750e1648746eb29c78d30fa1043c8b6f89d82924c4488be06fa3d19")},
                        {360000, uint256S("0x8fee7e3f6c38dccd3047a3e4667c63406f835c2890024030a2ab2dc6dba7c912")},
                        {400100, uint256S("0x82325a97cd97ac14b0a57408f881b1a9fc40174f8430a4580429499ac5d153c8")},
                        {521000, uint256S("0xd23fd1e1f994c0586d761b71bb3530e9ab45bd0fabda3a5a2e394f3dc4d9bb04")},
                        {1380000, uint256S("0x00000000000001969b1e5836dd8bf6a001d96f4a16d336e09405b62b29feead6")},
                        {2000000, uint256S("0x10f522ec60d8af2e2cbd9e2268260c33fb8bbf9cd9f176b4fddcae7493c6791d")},
                        {3500000, uint256S("0xbece76f2a3f53637e2ea84837a45a6ffdc0c86372ab4701c3146094f65832c80")},
                        {5000000, uint256S("0x1dd2fdf6416343688eed463a7bc70b298a4f872e941e36f85cda0915d6488e25")},
                        {6500000, uint256S("0xb168b7f70cbfd2e5fea07da55d9fa90dc7c65599ceb2700efe04ee6c45692e52")},
                        {8000000, uint256S("0x1af919cb004bb05c369a862cb5ded70aaa123d0eac2432ceec859f6f42880660")},
                        {9500000, uint256S("0x5b0351361414e520e9132ba6c5c4926d6f9ee55c41b77fffce3a16ea15d4a1be")},
                        {11000000, uint256S("0x0f4ad10ae49b504246c0175f6cbab9b0f91b6568a88931e6341a83a731701054")},
                        {12500000, uint256S("0x697a015b62140c9549fbc8d8b3c1d027626b2f94d337db32115e429fbf233ed7")},
                        {14000000, uint256S("0xa33861c857eed46191cf6cdaf81693e0dfcd00b3a11133821b0c73fe1d7769d9")},
                        {15500000, uint256S("0x000000000000000439d5c66b2fb3ec50f50a68b65f5790d338150b63488de645")},
                        {17000000, uint256S("0xf167688cc0102743b135499ed9f9eff9c5bad096203150e438be0a6e783d5587")},
                        {18500000, uint256S("0x745dc7b89208de482071a3a8d13eb5596d55bedc4f5ba2fa74cbea9ecf91169e")},
                        {20000000, uint256S("0xf530a66ba6fe93e647f7d88a9b3f22bfe8c2c2ab1ec1b0286286f86b82d6a10f")},
                        {21000000, uint256S("0x0000000000000001cb40d3be76bf601d98555a069669d963060d633ea3a140e8")},
                        {21700000, uint256S("0x457f6864b52e5076a433afe3c28e3ae0bbeeaba9036a782ddb691242326fcb80")}
                    };

                    // Check if this height is a critical checkpoint
                    auto checkpointIt = criticalCheckpoints.find(pindexNew->nHeight);
                    if (checkpointIt != criticalCheckpoints.end()) {
                        uint256 blockHash = diskindex.ConstructBlockHash();
                        if (checkpointIt->second == blockHash) {
                            shouldCheckPoW = true;
                            LogPrint(BCLog::BLOCKSTORAGE, "LoadBlockIndex: Checking PoW for checkpoint at height %d\n", pindexNew->nHeight);
                        } else {
                            // Hash mismatch at checkpoint - log details to help debug
                            LogPrintf("LoadBlockIndex: ERROR - Block hash mismatch at checkpoint height %d\n", pindexNew->nHeight);
                            LogPrintf("  Expected hash: %s\n", checkpointIt->second.ToString());
                            LogPrintf("  Actual hash:   %s\n", blockHash.ToString());
                            LogPrintf("  Block details: version=%d, time=%d, bits=%08x, nonce=%u\n",
                                     pindexNew->nVersion, pindexNew->nTime, pindexNew->nBits, pindexNew->nNonce);

                            // This is a critical error - checkpoint hashes must match
                            return error("%s: Block hash mismatch at checkpoint height %d: expected %s, got %s",
                                       __func__, pindexNew->nHeight,
                                       checkpointIt->second.ToString(),
                                       blockHash.ToString());
                        }
                    }

                    // Only perform expensive proof of work check for genesis and checkpoints
                    if (shouldCheckPoW) {
                        if (!CheckProofOfWork(pindexNew->GetBlockPoWHash(), pindexNew->nBits, consensusParams)) {
                            return error("%s: CheckProofOfWork failed for checkpoint/genesis block at height %d: %s",
                                       __func__, pindexNew->nHeight, pindexNew->ToString());
                        }
                    }
                } else {
                    // For testnet and regtest, always check proof of work (original behavior)
                    if (!CheckProofOfWork(pindexNew->GetBlockPoWHash(), pindexNew->nBits, consensusParams)) {
                        return error("%s: CheckProofOfWork failed: %s", __func__, pindexNew->ToString());
                    }
                }

                pcursor->Next();
            } else {
                return error("%s: failed to read value", __func__);
            }
        } else {
            break;
        }
    }

    LogPrintf("Startup timing: block index deserialize pass: %d entries in %d ms\n",
              nCount, Ticks<std::chrono::milliseconds>(SteadyClock::now() - deserialize_start));
    LogPrintf("LoadBlockIndex: loaded cached chain work for %d of %d block-index records\n",
              nChainWorkCached, nCount);
    return true;
}
} // namespace kernel

namespace node {
std::atomic_bool fReindex(false);

bool CBlockIndexWorkComparator::operator()(const CBlockIndex* pa, const CBlockIndex* pb) const
{
    // First sort by most total work, ...
    if (pa->nChainWork > pb->nChainWork) return false;
    if (pa->nChainWork < pb->nChainWork) return true;

    // ... then by earliest time received, ...
    if (pa->nSequenceId < pb->nSequenceId) return false;
    if (pa->nSequenceId > pb->nSequenceId) return true;

    // Use pointer address as tie breaker (should only happen with blocks
    // loaded from disk, as those all have id 0).
    if (pa < pb) return false;
    if (pa > pb) return true;

    // Identical blocks.
    return false;
}

bool CBlockIndexHeightOnlyComparator::operator()(const CBlockIndex* pa, const CBlockIndex* pb) const
{
    return pa->nHeight < pb->nHeight;
}

std::vector<CBlockIndex*> BlockManager::GetAllBlockIndices()
{
    AssertLockHeld(cs_main);
    std::vector<CBlockIndex*> rv;
    rv.reserve(m_block_index.size());
    m_block_index.ForEach([&](CBlockIndex& block_index) {
        rv.push_back(&block_index);
    });
    return rv;
}

std::vector<CBlockIndex*> BlockManager::GetAllBlockIndicesByCompactId()
{
    AssertLockHeld(cs_main);

    std::vector<CBlockIndex*> ordered(m_block_index.size(), nullptr);
    m_block_index.ForEach([&](CBlockIndex& block_index) {
        const BlockIndexId id{block_index.m_compact_id};
        Assert(id != INVALID_BLOCK_INDEX_ID);
        Assert(static_cast<size_t>(id) < ordered.size());
        Assert(ordered[id] == nullptr);
        ordered[id] = &block_index;
    });

    for (CBlockIndex* index : ordered) {
        Assert(index != nullptr);
    }
    return ordered;
}

std::vector<CBlockIndex*> BlockManager::TakeStartupBlockIndexView()
{
    AssertLockHeld(cs_main);

    std::vector<CBlockIndex*> view{std::move(m_startup_block_index_view)};
    m_startup_block_index_view.clear();
    return view;
}

fs::path BlockManager::CompactBlockIndexShadowPath() const
{
    return m_opts.blocks_dir / "index.compact";
}

fs::path BlockManager::CompactBlockIndexLookupPath() const
{
    return m_opts.blocks_dir / "index.compact.lookup";
}

fs::path BlockManager::CompactBlockIndexIdsPath() const
{
    return m_opts.blocks_dir / "index.compact.ids";
}

fs::path BlockManager::CompactBlockIndexDeltaPath() const
{
    return m_opts.blocks_dir / "index.compact.delta";
}

fs::path BlockManager::CompactBlockIndexDeltaLogPath() const
{
    return m_opts.blocks_dir / "index.compact.delta.log";
}

fs::path BlockManager::CompactBlockIndexDeltaStatePath() const
{
    return m_opts.blocks_dir / "index.compact.delta.state";
}

fs::path BlockManager::CompactBlockIndexDeltaPendingPath() const
{
    return m_opts.blocks_dir / "index.compact.delta.pending";
}

bool BlockManager::OpenCompactBlockIndexMapped()
{
    AssertLockHeld(cs_main);

    if (m_compact_block_index && m_compact_block_index->IsOpen()) {
        return true;
    }

    auto mapped = std::make_unique<CompactBlockIndexStore>();
    std::string open_error;
    const fs::path path{CompactBlockIndexShadowPath()};
    if (!mapped->Open(path, GetConsensus().hashGenesisBlock, open_error)) {
        LogPrintf("Compact block index: cannot map source %s: %s\n",
                  fs::PathToString(path), open_error);
        return false;
    }

    LogPrintf("Compact block index: opened mapped source entries=%u bytes=%u path=%s\n",
              mapped->EntryCount(), mapped->SizeBytes(), fs::PathToString(path));
    m_compact_block_index = std::move(mapped);
    return true;
}

BlockIndexId BlockManager::AllocateCompactId()
{
    AssertLockHeld(cs_main);
    Assert(m_next_compact_id < static_cast<uint64_t>(INVALID_BLOCK_INDEX_ID));
    return static_cast<BlockIndexId>(m_next_compact_id++);
}

void BlockManager::AssignCompactIdsDeterministic(const std::vector<CBlockIndex*>& sorted)
{
    AssertLockHeld(cs_main);
    Assert(sorted.size() < static_cast<size_t>(INVALID_BLOCK_INDEX_ID));

    for (size_t id = 0; id < sorted.size(); ++id) {
        sorted[id]->m_compact_id = static_cast<BlockIndexId>(id);
    }
    m_next_compact_id = sorted.size();

    LogPrintf("LoadBlockIndex: assigned deterministic compact ids to %u block indices next=%u\n",
              sorted.size(), m_next_compact_id);
}

bool BlockManager::BuildCompactBlockIndexShadow(const std::vector<CBlockIndex*>& sorted)
{
    AssertLockHeld(cs_main);

    const fs::path final_path{CompactBlockIndexShadowPath()};
    fs::path tmp_path{final_path};
    tmp_path += ".tmp";

    FILE* file{fsbridge::fopen(tmp_path, "wb")};
    if (!file) {
        return error("%s: cannot open compact shadow temp file %s",
                     __func__, fs::PathToString(tmp_path));
    }

    auto fail = [&](const char* reason) {
        std::fclose(file);
        file = nullptr;
        fs::remove(tmp_path);
        return error("%s: %s", __func__, reason);
    };

    CompactBlockIndexFileHeader header;
    header.entry_count = sorted.size();
    header.genesis_hash = GetConsensus().hashGenesisBlock;
    header.generation = 1;

    if (std::fwrite(&header, sizeof(header), 1, file) != 1) {
        return fail("failed to write compact shadow header");
    }

    const auto start{SteadyClock::now()};
    size_t written{0};
    int last_percent{-1};

    for (const CBlockIndex* index : sorted) {
        if (m_interrupt) {
            return fail("interrupted while writing compact shadow");
        }

        const BlockIndexId parent_id{
            index->pprev ? index->pprev->m_compact_id : INVALID_BLOCK_INDEX_ID};
        const BlockIndexId skip_id{
            index->pskip ? index->pskip->m_compact_id : INVALID_BLOCK_INDEX_ID};

        if ((index->pprev && parent_id == INVALID_BLOCK_INDEX_ID) ||
            (index->pskip && skip_id == INVALID_BLOCK_INDEX_ID)) {
            return fail("encountered unresolved parent/skip compact id");
        }

        CompactBlockIndexEntry entry;
        entry.hash = index->GetBlockHash();
        entry.record = CompactBlockIndexRecord::FromBlockIndex(*index, parent_id, skip_id);

        if (std::fwrite(&entry, sizeof(entry), 1, file) != 1) {
            return fail("failed to write compact shadow entry");
        }

        ++written;
        if (!sorted.empty()) {
            const int percent{static_cast<int>((100 * written) / sorted.size())};
            if (percent != last_percent && percent % 10 == 0) {
                LogPrintf("Compact block index: shadow build %d%%\n", percent);
                last_percent = percent;
            }
        }
    }

    if (!FileCommit(file)) {
        return fail("failed to fsync compact shadow");
    }
    if (std::fclose(file) != 0) {
        file = nullptr;
        fs::remove(tmp_path);
        return error("%s: failed to close compact shadow temp file", __func__);
    }
    file = nullptr;

    if (!RenameOver(tmp_path, final_path)) {
        fs::remove(tmp_path);
        return error("%s: failed to publish compact shadow %s",
                     __func__, fs::PathToString(final_path));
    }
    DirectoryCommit(final_path.parent_path());

    const uint64_t bytes{
        sizeof(CompactBlockIndexFileHeader) +
        static_cast<uint64_t>(written) * sizeof(CompactBlockIndexEntry)};
    LogPrintf("Compact block index: built shadow entries=%u bytes=%u path=%s in %d ms\n",
              written,
              bytes,
              fs::PathToString(final_path),
              Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));
    return true;
}

bool BlockManager::VerifyCompactBlockIndexShadow(const std::vector<CBlockIndex*>& sorted)
{
    AssertLockHeld(cs_main);

    const fs::path path{CompactBlockIndexShadowPath()};
    auto mapped = std::make_unique<CompactBlockIndexStore>();
    std::string open_error;
    if (!mapped->Open(path, GetConsensus().hashGenesisBlock, open_error)) {
        LogPrintf("Compact block index: ignoring unusable shadow %s: %s\n",
                  fs::PathToString(path), open_error);
        return false;
    }

    if (mapped->EntryCount() > sorted.size()) {
        LogPrintf("Compact block index: ignoring shadow newer/larger than live index: file=%u live=%u\n",
                  mapped->EntryCount(), sorted.size());
        return false;
    }

    for (CBlockIndex* index : sorted) {
        index->m_compact_id = INVALID_BLOCK_INDEX_ID;
    }

    const auto start{SteadyClock::now()};
    const uint64_t stored_count{mapped->EntryCount()};

    // First restore the ids already published by the compact-store generation.
    // Lookups still use the legacy in-memory map during this migration stage.
    for (uint64_t raw_id = 0; raw_id < stored_count; ++raw_id) {
        if (m_interrupt) {
            LogPrintf("Compact block index: verification interrupted\n");
            return false;
        }

        const BlockIndexId id{static_cast<BlockIndexId>(raw_id)};
        const CompactBlockIndexEntry* entry{mapped->Get(id)};
        if (!entry) {
            LogPrintf("Compact block index: missing mapped entry id=%u\n", id);
            return false;
        }

        CBlockIndex* live{LookupBlockIndex(entry->hash)};
        if (!live) {
            LogPrintf("Compact block index: shadow id=%u references unknown hash=%s; ignoring shadow\n",
                      id, entry->hash.ToString());
            return false;
        }
        if (live->m_compact_id != INVALID_BLOCK_INDEX_ID) {
            LogPrintf("Compact block index: duplicate live hash/id assignment at id=%u hash=%s; ignoring shadow\n",
                      id, entry->hash.ToString());
            return false;
        }
        live->m_compact_id = id;
    }

    // Blocks learned since this compact generation are a live tail. Give them
    // ids after the persisted generation without renumbering historical ids.
    uint64_t next_id{stored_count};
    for (CBlockIndex* index : sorted) {
        if (index->m_compact_id != INVALID_BLOCK_INDEX_ID) continue;
        if (next_id >= static_cast<uint64_t>(INVALID_BLOCK_INDEX_ID)) {
            LogPrintf("Compact block index: id space exhausted while assigning live tail\n");
            return false;
        }
        index->m_compact_id = static_cast<BlockIndexId>(next_id++);
    }

    size_t verified{0};
    int last_percent{-1};

    // Now that every live block has an id, compare all persisted records,
    // including parent and skip references, against reconstructed live state.
    for (uint64_t raw_id = 0; raw_id < stored_count; ++raw_id) {
        if (m_interrupt) {
            LogPrintf("Compact block index: verification interrupted\n");
            return false;
        }

        const BlockIndexId id{static_cast<BlockIndexId>(raw_id)};
        const CompactBlockIndexEntry* entry{mapped->Get(id)};
        CBlockIndex* index{LookupBlockIndex(entry->hash)};
        Assert(index != nullptr);

        const BlockIndexId parent_id{
            index->pprev ? index->pprev->m_compact_id : INVALID_BLOCK_INDEX_ID};
        const BlockIndexId skip_id{
            index->pskip ? index->pskip->m_compact_id : INVALID_BLOCK_INDEX_ID};

        if (!entry->record.MatchesBlockIndex(*index, parent_id, skip_id)) {
            LogPrintf("Compact block index: mismatch at persisted id=%u height=%d hash=%s; ignoring shadow\n",
                      id, index->nHeight, index->GetBlockHash().ToString());
            return false;
        }

        ++verified;
        if (stored_count > 0) {
            const int percent{static_cast<int>((100 * verified) / stored_count)};
            if (percent != last_percent && percent % 10 == 0) {
                LogPrintf("Compact block index: mapped verify %d%%\n", percent);
                last_percent = percent;
            }
        }
    }

    const uint64_t live_tail{sorted.size() - stored_count};
    m_next_compact_id = next_id;
    LogPrintf("Compact block index: mapped shadow entries=%u bytes=%u path=%s\n",
              stored_count,
              mapped->SizeBytes(),
              fs::PathToString(path));
    LogPrintf("Compact block index: verified persisted generation entries=%u live=%u tail=%u next=%u in %d ms\n",
              verified,
              sorted.size(),
              live_tail,
              m_next_compact_id,
              Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));

    m_compact_block_index = std::move(mapped);
    return true;
}

bool BlockManager::RestoreCompactIds(
    const std::vector<CBlockIndex*>& sorted,
    bool allow_create)
{
    AssertLockHeld(cs_main);

    uint64_t base_generation{0};
    uint64_t base_entry_count{0};

    const fs::path compact_path{CompactBlockIndexShadowPath()};
    if (fs::exists(compact_path)) {
        if (!OpenCompactBlockIndexMapped()) {
            LogPrintf("Compact block index: cannot restore persistent ids because compact base is unusable\n");
            return false;
        }
        base_generation = m_compact_block_index->Header()->generation;
        base_entry_count = m_compact_block_index->EntryCount();
    }

    if (base_entry_count > sorted.size()) {
        LogPrintf("Compact block index: id base newer/larger than live index: base=%u live=%u\n",
                  base_entry_count, sorted.size());
        return false;
    }

    const fs::path ids_path{CompactBlockIndexIdsPath()};
    auto ids = std::make_unique<CompactBlockIndexIds>();
    std::string ids_error;

    if (!ids->Open(
            ids_path,
            base_generation,
            base_entry_count,
            GetConsensus().hashGenesisBlock,
            ids_error)) {
        if (!allow_create) {
            LogPrintf("Compact block index: cannot open persistent ids %s: %s\n",
                      fs::PathToString(ids_path), ids_error);
            return false;
        }

        LogPrintf("Compact block index: creating persistent ids %s base_generation=%u base=%u (%s)\n",
                  fs::PathToString(ids_path),
                  base_generation,
                  base_entry_count,
                  ids_error);

        if (!CompactBlockIndexIds::Create(
                ids_path,
                base_generation,
                base_entry_count,
                GetConsensus().hashGenesisBlock,
                ids_error) ||
            !ids->Open(
                ids_path,
                base_generation,
                base_entry_count,
                GetConsensus().hashGenesisBlock,
                ids_error)) {
            LogPrintf("Compact block index: failed to create/open persistent ids: %s\n",
                      ids_error);
            return false;
        }
    }

    for (CBlockIndex* index : sorted) {
        index->m_compact_id = INVALID_BLOCK_INDEX_ID;
    }

    const auto start{SteadyClock::now()};

    // The immutable compact generation owns ids [0, base_entry_count).
    for (uint64_t raw_id = 0; raw_id < base_entry_count; ++raw_id) {
        const BlockIndexId id{static_cast<BlockIndexId>(raw_id)};
        const CompactBlockIndexEntry* entry{m_compact_block_index->Get(id)};
        if (!entry) {
            LogPrintf("Compact block index: persistent id base missing id=%u\n", id);
            return false;
        }

        CBlockIndex* live{LookupBlockIndex(entry->hash)};
        if (!live || live->m_compact_id != INVALID_BLOCK_INDEX_ID) {
            LogPrintf("Compact block index: persistent id base mismatch id=%u hash=%s\n",
                      id, entry->hash.ToString());
            return false;
        }
        live->m_compact_id = id;
    }

    uint64_t valid_tail{0};
    bool saw_missing_suffix{false};
    bool invalid_tail{false};
    ids_error.clear();

    if (!ids->ForEachTail(
            [&](BlockIndexId id, const uint256& hash) {
                CBlockIndex* live{LookupBlockIndex(hash)};
                if (!live) {
                    saw_missing_suffix = true;
                    return true;
                }
                if (saw_missing_suffix ||
                    live->m_compact_id != INVALID_BLOCK_INDEX_ID) {
                    invalid_tail = true;
                    return false;
                }
                live->m_compact_id = id;
                ++valid_tail;
                return true;
            },
            ids_error)) {
        LogPrintf("Compact block index: failed reading persistent id tail: %s\n",
                  ids_error);
        return false;
    }

    if (invalid_tail) {
        LogPrintf("Compact block index: persistent id tail has non-suffix mismatch or duplicate\n");
        return false;
    }

    if (saw_missing_suffix) {
        if (!allow_create) {
            LogPrintf("Compact block index: persistent id tail contains unpublished suffix\n");
            return false;
        }
        ids_error.clear();
        if (!ids->TruncateTail(valid_tail, ids_error)) {
            LogPrintf("Compact block index: failed truncating unpublished id suffix: %s\n",
                      ids_error);
            return false;
        }
        LogPrintf("Compact block index: truncated unpublished id suffix tail=%u\n",
                  valid_tail);
    }

    std::vector<uint256> missing_hashes;
    uint64_t next_id{ids->NextId()};
    for (CBlockIndex* index : sorted) {
        if (index->m_compact_id != INVALID_BLOCK_INDEX_ID) continue;

        if (next_id >= static_cast<uint64_t>(INVALID_BLOCK_INDEX_ID)) {
            LogPrintf("Compact block index: persistent id space exhausted\n");
            return false;
        }

        index->m_compact_id = static_cast<BlockIndexId>(next_id++);
        missing_hashes.push_back(index->GetBlockHash());
    }

    if (!missing_hashes.empty() && !allow_create) {
        LogPrintf("Compact block index: persistent ids missing %u live records\n",
                  missing_hashes.size());
        return false;
    }

    if (!missing_hashes.empty()) {
        ids_error.clear();
        if (!ids->Append(missing_hashes, ids_error)) {
            LogPrintf("Compact block index: failed appending migrated id tail: %s\n",
                      ids_error);
            return false;
        }
    }

    m_next_compact_id = ids->NextId();
    LogPrintf("Compact block index: persistent ids restored base=%u tail=%u migrated=%u next=%u bytes=%u in %d ms\n",
              ids->BaseEntryCount(),
              ids->TailEntryCount(),
              missing_hashes.size(),
              m_next_compact_id,
              ids->SizeBytes(),
              Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));

    m_compact_block_ids = std::move(ids);
    return true;
}

bool BlockManager::PersistCompactIds(
    const std::vector<const CBlockIndex*>& blockinfo)
{
    AssertLockHeld(cs_main);

    // Explicit reindex skips LoadBlockIndexDB(), so initialize a fresh
    // zero-base identity tail lazily on the first block-index flush.
    if ((!m_compact_block_ids || !m_compact_block_ids->IsOpen()) &&
        m_opts.block_index_compact_ids == kernel::BlockIndexCompactIdsMode::BUILD &&
        fReindex) {
        const fs::path path{CompactBlockIndexIdsPath()};
        std::string error;
        if (!CompactBlockIndexIds::Create(
                path,
                /*base_generation=*/0,
                /*base_entry_count=*/0,
                GetConsensus().hashGenesisBlock,
                error)) {
            LogPrintf("Compact block index: failed creating reindex id tail: %s\n",
                      error);
            return false;
        }

        auto ids = std::make_unique<CompactBlockIndexIds>();
        if (!ids->Open(
                path,
                /*expected_base_generation=*/0,
                /*expected_base_entry_count=*/0,
                GetConsensus().hashGenesisBlock,
                error)) {
            LogPrintf("Compact block index: failed opening reindex id tail: %s\n",
                      error);
            return false;
        }
        m_compact_block_ids = std::move(ids);
        LogPrintf("Compact block index: initialized zero-base persistent ids for reindex\n");
    }

    if (!m_compact_block_ids || !m_compact_block_ids->IsOpen()) {
        return true;
    }

    const uint64_t persisted_next{m_compact_block_ids->NextId()};
    std::vector<const CBlockIndex*> pending;

    for (const CBlockIndex* index : blockinfo) {
        if (index->m_compact_id == INVALID_BLOCK_INDEX_ID) continue;
        if (static_cast<uint64_t>(index->m_compact_id) >= persisted_next) {
            pending.push_back(index);
        }
    }

    if (pending.empty()) return true;

    std::sort(
        pending.begin(),
        pending.end(),
        [](const CBlockIndex* a, const CBlockIndex* b) {
            return a->m_compact_id < b->m_compact_id;
        });

    std::vector<uint256> hashes;
    hashes.reserve(pending.size());
    uint64_t expected_id{persisted_next};

    for (const CBlockIndex* index : pending) {
        if (index->m_compact_id != expected_id) {
            LogPrintf("Compact block index: cannot persist non-contiguous id tail expected=%u got=%u\n",
                      expected_id, index->m_compact_id);
            return false;
        }
        hashes.push_back(index->GetBlockHash());
        ++expected_id;
    }

    std::string error;
    if (!m_compact_block_ids->Append(hashes, error)) {
        LogPrintf("Compact block index: failed persisting live id tail: %s\n",
                  error);
        return false;
    }

    LogPrint(BCLog::BLOCKSTORAGE,
             "Compact block index: persisted live ids count=%u next=%u bytes=%u\n",
             hashes.size(),
             m_compact_block_ids->NextId(),
             m_compact_block_ids->SizeBytes());
    return true;
}

bool BlockManager::OpenCompactBlockIndexDeltaState(bool migrate_legacy)
{
    AssertLockHeld(cs_main);

    if (m_compact_block_delta_state && m_compact_block_delta_state->IsOpen()) {
        return true;
    }

    uint64_t base_generation{0};
    uint64_t base_entry_count{0};
    const fs::path compact_path{CompactBlockIndexShadowPath()};
    if (fs::exists(compact_path)) {
        if (!OpenCompactBlockIndexMapped()) {
            LogPrintf("Compact block index: cannot open metadata selector because compact base is unusable\n");
            return false;
        }
        base_generation = m_compact_block_index->Header()->generation;
        base_entry_count = m_compact_block_index->EntryCount();
    }

    const fs::path state_path{CompactBlockIndexDeltaStatePath()};
    std::string error;

    if (!fs::exists(state_path)) {
        if (!migrate_legacy) {
            LogPrintf("Compact block index: metadata selector is missing\n");
            return false;
        }

        const fs::path legacy_delta{CompactBlockIndexDeltaPath()};
        const fs::path legacy_log{CompactBlockIndexDeltaLogPath()};
        if (!fs::exists(legacy_delta) || !fs::exists(legacy_log)) {
            LogPrintf("Compact block index: cannot migrate metadata selector because legacy pair is incomplete\n");
            return false;
        }

        if (!CompactBlockIndexDeltaState::MigrateLegacyPair(
                state_path,
                legacy_delta,
                legacy_log,
                CompactBlockIndexDeltaPath(),
                CompactBlockIndexDeltaLogPath(),
                base_generation,
                base_entry_count,
                GetConsensus().hashGenesisBlock,
                error)) {
            LogPrintf("Compact block index: metadata pair migration failed: %s\n", error);
            return false;
        }

        LogPrintf("Compact block index: migrated legacy metadata pair into slot A selector=%s\n",
                  fs::PathToString(state_path));
    }

    auto state = std::make_unique<CompactBlockIndexDeltaState>();
    if (!state->Open(
            state_path,
            base_generation,
            base_entry_count,
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: cannot open metadata selector %s: %s\n",
                  fs::PathToString(state_path), error);
        return false;
    }

    LogPrintf("Compact block index: opened metadata selector slot=%s sequence=%u tail=%u path=%s\n",
              state->ActiveSlot() == CompactBlockIndexDeltaSlot::A ? "A" : "B",
              state->Sequence(),
              state->SnapshotTailEntryCount(),
              fs::PathToString(state_path));

    m_compact_block_delta_state = std::move(state);
    return true;
}

bool BlockManager::OpenCompactBlockIndexDeltaLog(bool create)
{
    AssertLockHeld(cs_main);

    if (!m_compact_block_delta || !m_compact_block_delta->IsOpen()) {
        LogPrintf("Compact block index: cannot open metadata delta log without delta snapshot\n");
        return false;
    }
    if (!OpenCompactBlockIndexDeltaState(/*migrate_legacy=*/!create)) {
        return false;
    }

    const uint64_t base_generation{m_compact_block_delta->BaseGeneration()};
    const uint64_t base_entry_count{m_compact_block_delta->BaseEntryCount()};
    const uint64_t snapshot_tail_entry_count{m_compact_block_delta->TailEntryCount()};
    const fs::path path{
        CompactBlockIndexDeltaState::SlotPath(
            CompactBlockIndexDeltaLogPath(),
            m_compact_block_delta_state->ActiveSlot())};
    std::string error;

    if (create) {
        if (!CompactBlockIndexDeltaLog::Create(
                path,
                base_generation,
                base_entry_count,
                snapshot_tail_entry_count,
                GetConsensus().hashGenesisBlock,
                error)) {
            LogPrintf("Compact block index: failed creating metadata delta log %s: %s\n",
                      fs::PathToString(path), error);
            return false;
        }
    }

    auto log = std::make_unique<CompactBlockIndexDeltaLog>();
    if (!log->Open(
            path,
            base_generation,
            base_entry_count,
            snapshot_tail_entry_count,
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: cannot open metadata delta log %s: %s\n",
                  fs::PathToString(path), error);
        return false;
    }

    LogPrintf("Compact block index: opened metadata delta log slot=%s records=%u bytes=%u path=%s\n",
              m_compact_block_delta_state->ActiveSlot() == CompactBlockIndexDeltaSlot::A ? "A" : "B",
              log->RecordCount(),
              log->SizeBytes(),
              fs::PathToString(path));
    m_compact_block_delta_log = std::move(log);
    return true;
}

bool BlockManager::BuildCompactBlockIndexDeltaLogRecords(
    const std::vector<const CBlockIndex*>& blockinfo,
    std::vector<CompactBlockIndexDeltaLogRecord>& updates)
{
    AssertLockHeld(cs_main);

    updates.clear();

    std::vector<const CBlockIndex*> ordered;
    ordered.reserve(blockinfo.size());
    for (const CBlockIndex* index : blockinfo) {
        if (index->m_compact_id == INVALID_BLOCK_INDEX_ID) continue;
        ordered.push_back(index);
    }

    if (ordered.empty()) return true;

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const CBlockIndex* a, const CBlockIndex* b) {
            return a->m_compact_id < b->m_compact_id;
        });

    updates.reserve(ordered.size());

    for (const CBlockIndex* index : ordered) {
        const BlockIndexId parent_id{
            index->pprev ? index->pprev->m_compact_id : INVALID_BLOCK_INDEX_ID};
        const BlockIndexId skip_id{
            index->pskip ? index->pskip->m_compact_id : INVALID_BLOCK_INDEX_ID};

        if ((index->pprev && parent_id == INVALID_BLOCK_INDEX_ID) ||
            (index->pskip && skip_id == INVALID_BLOCK_INDEX_ID)) {
            LogPrintf("Compact block index: cannot persist metadata update with unresolved linkage id=%u\n",
                      index->m_compact_id);
            return false;
        }

        CompactBlockIndexDeltaLogRecord update;
        update.id = index->m_compact_id;
        update.entry.hash = index->GetBlockHash();
        update.entry.record = CompactBlockIndexRecord::FromBlockIndex(
            *index, parent_id, skip_id);
        updates.push_back(update);
    }

    return true;
}

bool BlockManager::ClearCompactBlockIndexDeltaPending()
{
    AssertLockHeld(cs_main);

    const fs::path path{CompactBlockIndexDeltaPendingPath()};
    try {
        if (!fs::exists(path)) return true;
        fs::remove(path);
        DirectoryCommit(path.parent_path());
        return true;
    } catch (const std::exception& e) {
        LogPrintf("Compact block index: failed clearing metadata pending batch %s: %s\n",
                  fs::PathToString(path), e.what());
        return false;
    }
}

void BlockManager::InvalidateCompactBlockIndexDeltaOverlay()
{
    AssertLockHeld(cs_main);

    m_compact_block_delta_log.reset();
    m_compact_block_delta.reset();
    m_compact_block_delta_overlay.clear();

    fs::path path{CompactBlockIndexDeltaLogPath()};
    if (m_compact_block_delta_state && m_compact_block_delta_state->IsOpen()) {
        path = CompactBlockIndexDeltaState::SlotPath(
            path, m_compact_block_delta_state->ActiveSlot());
    }
    try {
        if (fs::exists(path)) {
            fs::remove(path);
            DirectoryCommit(path.parent_path());
        }
    } catch (const std::exception& e) {
        LogPrintf("Compact block index: failed removing invalid metadata delta log %s: %s\n",
                  fs::PathToString(path), e.what());
    }
}

bool BlockManager::StageCompactBlockIndexDeltaPending(
    const std::vector<const CBlockIndex*>& blockinfo)
{
    AssertLockHeld(cs_main);

    if (!m_compact_block_delta_log || !m_compact_block_delta_log->IsOpen()) {
        return true;
    }

    std::vector<CompactBlockIndexDeltaLogRecord> updates;
    if (!BuildCompactBlockIndexDeltaLogRecords(blockinfo, updates)) {
        return false;
    }

    if (updates.empty()) {
        return ClearCompactBlockIndexDeltaPending();
    }

    const fs::path path{CompactBlockIndexDeltaPendingPath()};
    std::string error;

    if (!CompactBlockIndexDeltaLog::Create(
            path,
            m_compact_block_delta_log->BaseGeneration(),
            m_compact_block_delta_log->BaseEntryCount(),
            m_compact_block_delta_log->SnapshotTailEntryCount(),
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: failed creating metadata pending batch %s: %s\n",
                  fs::PathToString(path), error);
        return false;
    }

    CompactBlockIndexDeltaLog pending;
    if (!pending.Open(
            path,
            m_compact_block_delta_log->BaseGeneration(),
            m_compact_block_delta_log->BaseEntryCount(),
            m_compact_block_delta_log->SnapshotTailEntryCount(),
            GetConsensus().hashGenesisBlock,
            error) ||
        !pending.Append(updates, error)) {
        LogPrintf("Compact block index: failed staging metadata pending batch %s: %s\n",
                  fs::PathToString(path), error);
        return false;
    }

    LogPrintf("Compact block index: staged metadata pending batch count=%u bytes=%u\n",
              pending.RecordCount(), pending.SizeBytes());
    return true;
}

bool BlockManager::PublishCompactBlockIndexDeltaPending()
{
    AssertLockHeld(cs_main);

    if (!m_compact_block_delta_log || !m_compact_block_delta_log->IsOpen()) {
        return true;
    }

    const fs::path path{CompactBlockIndexDeltaPendingPath()};
    if (!fs::exists(path)) return true;

    CompactBlockIndexDeltaLog pending;
    std::string error;
    if (!pending.Open(
            path,
            m_compact_block_delta_log->BaseGeneration(),
            m_compact_block_delta_log->BaseEntryCount(),
            m_compact_block_delta_log->SnapshotTailEntryCount(),
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: failed opening metadata pending batch %s: %s\n",
                  fs::PathToString(path), error);
        return false;
    }

    std::vector<CompactBlockIndexDeltaLogRecord> updates;
    updates.reserve(static_cast<size_t>(pending.RecordCount()));
    if (!pending.ForEach(
            [&](const CompactBlockIndexDeltaLogRecord& update) {
                updates.push_back(update);
                return true;
            },
            error)) {
        LogPrintf("Compact block index: failed reading metadata pending batch: %s\n",
                  error);
        return false;
    }

    if (!updates.empty() && !m_compact_block_delta_log->Append(updates, error)) {
        LogPrintf("Compact block index: failed publishing metadata pending batch: %s\n",
                  error);
        return false;
    }

    if (!ClearCompactBlockIndexDeltaPending()) {
        return false;
    }

    for (const CompactBlockIndexDeltaLogRecord& update : updates) {
        m_compact_block_delta_overlay[update.id] = update.entry;
    }

    LogPrintf("Compact block index: published metadata pending batch count=%u records=%u bytes=%u\n",
              updates.size(),
              m_compact_block_delta_log->RecordCount(),
              m_compact_block_delta_log->SizeBytes());
    return true;
}

bool BlockManager::ReconcileCompactBlockIndexDeltaPending()
{
    AssertLockHeld(cs_main);

    const fs::path path{CompactBlockIndexDeltaPendingPath()};
    if (!fs::exists(path)) return true;

    if (!m_compact_block_delta_log || !m_compact_block_delta_log->IsOpen()) {
        LogPrintf("Compact block index: cannot reconcile metadata pending batch without delta log\n");
        return false;
    }

    CompactBlockIndexDeltaLog pending;
    std::string error;
    if (!pending.Open(
            path,
            m_compact_block_delta_log->BaseGeneration(),
            m_compact_block_delta_log->BaseEntryCount(),
            m_compact_block_delta_log->SnapshotTailEntryCount(),
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: cannot open metadata pending batch %s: %s\n",
                  fs::PathToString(path), error);
        return false;
    }

    std::vector<CompactBlockIndexDeltaLogRecord> updates;
    updates.reserve(static_cast<size_t>(pending.RecordCount()));
    if (!pending.ForEach(
            [&](const CompactBlockIndexDeltaLogRecord& update) {
                updates.push_back(update);
                return true;
            },
            error)) {
        LogPrintf("Compact block index: failed reading metadata pending batch: %s\n",
                  error);
        return false;
    }

    bool canonical_match{true};
    for (const CompactBlockIndexDeltaLogRecord& update : updates) {
        if (static_cast<uint64_t>(update.id) >= m_next_compact_id) {
            canonical_match = false;
            break;
        }

        CBlockIndex* index{LookupBlockIndex(update.entry.hash)};
        if (!index || index->m_compact_id != update.id) {
            canonical_match = false;
            break;
        }

        const BlockIndexId parent_id{
            index->pprev ? index->pprev->m_compact_id : INVALID_BLOCK_INDEX_ID};
        const BlockIndexId skip_id{
            index->pskip ? index->pskip->m_compact_id : INVALID_BLOCK_INDEX_ID};

        if (!update.entry.record.MatchesBlockIndex(*index, parent_id, skip_id)) {
            canonical_match = false;
            break;
        }
    }

    if (canonical_match && !updates.empty()) {
        if (!m_compact_block_delta_log->Append(updates, error)) {
            LogPrintf("Compact block index: failed recovering committed metadata pending batch: %s\n",
                      error);
            return false;
        }
        for (const CompactBlockIndexDeltaLogRecord& update : updates) {
            m_compact_block_delta_overlay[update.id] = update.entry;
        }
        LogPrintf("Compact block index: recovered committed metadata pending batch count=%u records=%u bytes=%u\n",
                  updates.size(),
                  m_compact_block_delta_log->RecordCount(),
                  m_compact_block_delta_log->SizeBytes());
    } else if (!updates.empty()) {
        LogPrintf("Compact block index: discarded uncommitted metadata pending batch count=%u\n",
                  updates.size());
    }

    return ClearCompactBlockIndexDeltaPending();
}

bool BlockManager::BuildCompactBlockIndexDelta(
    const std::vector<CBlockIndex*>& sorted)
{
    AssertLockHeld(cs_main);

    uint64_t base_generation{0};
    uint64_t base_entry_count{0};

    const fs::path compact_path{CompactBlockIndexShadowPath()};
    if (fs::exists(compact_path)) {
        if (!OpenCompactBlockIndexMapped()) {
            LogPrintf("Compact block index: cannot build metadata delta because compact base is unusable\n");
            return false;
        }
        base_generation = m_compact_block_index->Header()->generation;
        base_entry_count = m_compact_block_index->EntryCount();
    }

    if (base_entry_count > sorted.size()) {
        LogPrintf("Compact block index: metadata delta base newer/larger than live index base=%u live=%u\n",
                  base_entry_count, sorted.size());
        return false;
    }

    if (m_next_compact_id < base_entry_count) {
        LogPrintf("Compact block index: metadata delta id namespace precedes base next=%u base=%u\n",
                  m_next_compact_id, base_entry_count);
        return false;
    }

    const uint64_t tail_count{m_next_compact_id - base_entry_count};
    if (tail_count != sorted.size() - base_entry_count) {
        LogPrintf("Compact block index: metadata delta tail cardinality mismatch tail=%u live_minus_base=%u\n",
                  tail_count, sorted.size() - base_entry_count);
        return false;
    }

    std::vector<const CBlockIndex*> by_tail_id(tail_count, nullptr);
    for (CBlockIndex* index : sorted) {
        const BlockIndexId id{index->m_compact_id};
        if (id == INVALID_BLOCK_INDEX_ID) {
            LogPrintf("Compact block index: metadata delta encountered unassigned id hash=%s\n",
                      index->GetBlockHash().ToString());
            return false;
        }
        if (id < base_entry_count) continue;

        const uint64_t offset{static_cast<uint64_t>(id) - base_entry_count};
        if (offset >= by_tail_id.size() || by_tail_id[offset] != nullptr) {
            LogPrintf("Compact block index: metadata delta duplicate/out-of-range id=%u\n", id);
            return false;
        }
        by_tail_id[offset] = index;
    }

    std::vector<CompactBlockIndexEntry> entries;
    entries.reserve(tail_count);
    for (uint64_t offset = 0; offset < tail_count; ++offset) {
        if (m_interrupt) {
            LogPrintf("Compact block index: metadata delta build interrupted\n");
            return false;
        }

        const CBlockIndex* index{by_tail_id[offset]};
        if (!index) {
            LogPrintf("Compact block index: metadata delta missing tail id=%u\n",
                      base_entry_count + offset);
            return false;
        }

        const BlockIndexId parent_id{
            index->pprev ? index->pprev->m_compact_id : INVALID_BLOCK_INDEX_ID};
        const BlockIndexId skip_id{
            index->pskip ? index->pskip->m_compact_id : INVALID_BLOCK_INDEX_ID};

        if ((index->pprev && parent_id == INVALID_BLOCK_INDEX_ID) ||
            (index->pskip && skip_id == INVALID_BLOCK_INDEX_ID)) {
            LogPrintf("Compact block index: metadata delta unresolved linkage id=%u\n",
                      index->m_compact_id);
            return false;
        }

        CompactBlockIndexEntry entry;
        entry.hash = index->GetBlockHash();
        entry.record = CompactBlockIndexRecord::FromBlockIndex(
            *index, parent_id, skip_id);
        entries.push_back(entry);
    }

    const auto start{SteadyClock::now()};
    std::string error;

    CompactBlockIndexDeltaSlot target_slot{CompactBlockIndexDeltaSlot::A};
    uint64_t sequence{1};
    const fs::path state_path{CompactBlockIndexDeltaStatePath()};
    if (fs::exists(state_path)) {
        auto current_state = std::make_unique<CompactBlockIndexDeltaState>();
        if (!current_state->Open(
                state_path,
                base_generation,
                base_entry_count,
                GetConsensus().hashGenesisBlock,
                error)) {
            LogPrintf("Compact block index: cannot open current metadata selector before rebuild: %s\n",
                      error);
            return false;
        }
        target_slot = CompactBlockIndexDeltaState::OtherSlot(
            current_state->ActiveSlot());
        sequence = current_state->Sequence() + 1;
    }

    const fs::path delta_path{
        CompactBlockIndexDeltaState::SlotPath(
            CompactBlockIndexDeltaPath(), target_slot)};
    const fs::path log_path{
        CompactBlockIndexDeltaState::SlotPath(
            CompactBlockIndexDeltaLogPath(), target_slot)};

    if (!CompactBlockIndexDelta::Build(
            delta_path,
            base_generation,
            base_entry_count,
            GetConsensus().hashGenesisBlock,
            entries,
            error)) {
        LogPrintf("Compact block index: metadata delta build failed for %s: %s\n",
                  fs::PathToString(delta_path), error);
        return false;
    }

    if (!CompactBlockIndexDeltaLog::Create(
            log_path,
            base_generation,
            base_entry_count,
            entries.size(),
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: metadata delta log build failed for %s: %s\n",
                  fs::PathToString(log_path), error);
        return false;
    }

    auto delta = std::make_unique<CompactBlockIndexDelta>();
    auto log = std::make_unique<CompactBlockIndexDeltaLog>();
    if (!delta->Open(
            delta_path,
            base_generation,
            base_entry_count,
            GetConsensus().hashGenesisBlock,
            error) ||
        !log->Open(
            log_path,
            base_generation,
            base_entry_count,
            entries.size(),
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: built metadata pair but could not reopen it: %s\n",
                  error);
        return false;
    }

    if (!CompactBlockIndexDeltaState::Publish(
            state_path,
            target_slot,
            sequence,
            base_generation,
            base_entry_count,
            entries.size(),
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: failed publishing metadata selector: %s\n",
                  error);
        return false;
    }

    auto state = std::make_unique<CompactBlockIndexDeltaState>();
    if (!state->Open(
            state_path,
            base_generation,
            base_entry_count,
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: published metadata selector but could not reopen it: %s\n",
                  error);
        return false;
    }

    LogPrintf("Compact block index: built metadata pair slot=%s sequence=%u base=%u tail=%u delta_bytes=%u log_bytes=%u in %d ms\n",
              target_slot == CompactBlockIndexDeltaSlot::A ? "A" : "B",
              sequence,
              base_entry_count,
              delta->TailEntryCount(),
              delta->SizeBytes(),
              log->SizeBytes(),
              Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));

    m_compact_block_delta_state = std::move(state);
    m_compact_block_delta = std::move(delta);
    m_compact_block_delta_log = std::move(log);

    if (!ClearCompactBlockIndexDeltaPending()) {
        m_compact_block_delta_log.reset();
        m_compact_block_delta.reset();
        m_compact_block_delta_state.reset();
        return false;
    }
    return true;
}

bool BlockManager::CompactBlockIndexMetadata()
{
    AssertLockHeld(cs_main);

    if (!m_compact_block_delta_state || !m_compact_block_delta_state->IsOpen() ||
        !m_compact_block_delta || !m_compact_block_delta->IsOpen() ||
        !m_compact_block_delta_log || !m_compact_block_delta_log->IsOpen()) {
        LogPrintf("Compact block index: cannot compact metadata without an open selected pair\n");
        return false;
    }

    const fs::path pending_path{CompactBlockIndexDeltaPendingPath()};
    if (fs::exists(pending_path)) {
        LogPrintf("Compact block index: refusing metadata compaction while pending batch exists\n");
        return false;
    }

    CompactBlockIndexDeltaCompaction compacted;
    std::string error;
    if (!CompactBlockIndexDelta::PlanCompaction(
            *m_compact_block_delta,
            *m_compact_block_delta_log,
            m_next_compact_id,
            compacted,
            error)) {
        LogPrintf("Compact block index: metadata compaction planning failed: %s\n",
                  error);
        return false;
    }

    const auto start{SteadyClock::now()};
    const auto old_slot{m_compact_block_delta_state->ActiveSlot()};
    const auto target_slot{CompactBlockIndexDeltaState::OtherSlot(old_slot)};
    const uint64_t sequence{m_compact_block_delta_state->Sequence() + 1};
    const uint64_t base_generation{m_compact_block_delta->BaseGeneration()};
    const uint64_t base_entry_count{m_compact_block_delta->BaseEntryCount()};
    const uint64_t old_records{m_compact_block_delta_log->RecordCount()};

    const fs::path delta_path{
        CompactBlockIndexDeltaState::SlotPath(
            CompactBlockIndexDeltaPath(), target_slot)};
    const fs::path log_path{
        CompactBlockIndexDeltaState::SlotPath(
            CompactBlockIndexDeltaLogPath(), target_slot)};
    const fs::path state_path{CompactBlockIndexDeltaStatePath()};

    if (!CompactBlockIndexDelta::Build(
            delta_path,
            base_generation,
            base_entry_count,
            GetConsensus().hashGenesisBlock,
            compacted.tail_entries,
            error)) {
        LogPrintf("Compact block index: failed building compacted metadata checkpoint %s: %s\n",
                  fs::PathToString(delta_path), error);
        return false;
    }

    if (!CompactBlockIndexDeltaLog::Create(
            log_path,
            base_generation,
            base_entry_count,
            compacted.tail_entries.size(),
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: failed creating compacted metadata log %s: %s\n",
                  fs::PathToString(log_path), error);
        return false;
    }

    CompactBlockIndexDeltaLog target_log;
    if (!target_log.Open(
            log_path,
            base_generation,
            base_entry_count,
            compacted.tail_entries.size(),
            GetConsensus().hashGenesisBlock,
            error) ||
        !target_log.Append(compacted.base_updates, error)) {
        LogPrintf("Compact block index: failed writing compacted metadata base overlay: %s\n",
                  error);
        return false;
    }

    auto target_delta = std::make_unique<CompactBlockIndexDelta>();
    auto verified_log = std::make_unique<CompactBlockIndexDeltaLog>();
    if (!target_delta->Open(
            delta_path,
            base_generation,
            base_entry_count,
            GetConsensus().hashGenesisBlock,
            error) ||
        !verified_log->Open(
            log_path,
            base_generation,
            base_entry_count,
            compacted.tail_entries.size(),
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: compacted metadata pair failed reopen validation: %s\n",
                  error);
        return false;
    }

    if (target_delta->TailEntryCount() != compacted.tail_entries.size() ||
        verified_log->RecordCount() != compacted.base_updates.size()) {
        LogPrintf("Compact block index: compacted metadata pair cardinality mismatch\n");
        return false;
    }

    for (uint64_t offset = 0; offset < compacted.tail_entries.size(); ++offset) {
        const BlockIndexId id{
            static_cast<BlockIndexId>(base_entry_count + offset)};
        const CompactBlockIndexEntry* stored{target_delta->Get(id)};
        if (!stored ||
            std::memcmp(
                stored,
                &compacted.tail_entries[offset],
                sizeof(CompactBlockIndexEntry)) != 0) {
            LogPrintf("Compact block index: compacted metadata checkpoint content mismatch id=%u\n",
                      id);
            return false;
        }
    }

    size_t base_update_index{0};
    bool base_update_mismatch{false};
    error.clear();
    if (!verified_log->ForEach(
            [&](const CompactBlockIndexDeltaLogRecord& stored) {
                if (base_update_index >= compacted.base_updates.size() ||
                    std::memcmp(
                        &stored,
                        &compacted.base_updates[base_update_index],
                        sizeof(CompactBlockIndexDeltaLogRecord)) != 0) {
                    base_update_mismatch = true;
                    return false;
                }
                ++base_update_index;
                return true;
            },
            error)) {
        LogPrintf("Compact block index: compacted metadata log content verification failed: %s\n",
                  error);
        return false;
    }
    if (base_update_mismatch ||
        base_update_index != compacted.base_updates.size()) {
        LogPrintf("Compact block index: compacted metadata base overlay content mismatch\n");
        return false;
    }

    CompactBlockIndexDeltaCompaction verify_plan;
    error.clear();
    if (!CompactBlockIndexDelta::PlanCompaction(
            *target_delta,
            *verified_log,
            m_next_compact_id,
            verify_plan,
            error) ||
        verify_plan.tail_entries.size() != compacted.tail_entries.size() ||
        verify_plan.base_updates.size() != compacted.base_updates.size()) {
        LogPrintf("Compact block index: compacted metadata pair self-verification failed: %s\n",
                  error);
        return false;
    }

    LogPrintf("Compact block index: prepared compacted metadata pair from_slot=%s to_slot=%s sequence=%u old_records=%u tail=%u base_updates=%u delta_bytes=%u log_bytes=%u\n",
              old_slot == CompactBlockIndexDeltaSlot::A ? "A" : "B",
              target_slot == CompactBlockIndexDeltaSlot::A ? "A" : "B",
              sequence,
              old_records,
              target_delta->TailEntryCount(),
              verified_log->RecordCount(),
              target_delta->SizeBytes(),
              verified_log->SizeBytes());

    if (m_opts.block_index_compact_fault ==
        kernel::BlockIndexCompactFaultMode::BEFORE_COMPACTION_SELECTOR) {
        LogPrintf("Compact block index: fault injection before compaction selector publish; terminating with previous slot authoritative\n");
        std::_Exit(87);
    }

    if (!CompactBlockIndexDeltaState::Publish(
            state_path,
            target_slot,
            sequence,
            base_generation,
            base_entry_count,
            compacted.tail_entries.size(),
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: failed publishing compacted metadata selector: %s\n",
                  error);
        return false;
    }

    if (m_opts.block_index_compact_fault ==
        kernel::BlockIndexCompactFaultMode::AFTER_COMPACTION_SELECTOR) {
        LogPrintf("Compact block index: fault injection after compaction selector publish; terminating with new slot authoritative\n");
        std::_Exit(88);
    }

    auto state = std::make_unique<CompactBlockIndexDeltaState>();
    if (!state->Open(
            state_path,
            base_generation,
            base_entry_count,
            GetConsensus().hashGenesisBlock,
            error)) {
        LogPrintf("Compact block index: compacted selector published but could not reopen it: %s\n",
                  error);
        return false;
    }

    LogPrintf("Compact block index: compacted metadata pair active slot=%s sequence=%u old_records=%u new_records=%u tail=%u base_updates=%u in %d ms\n",
              target_slot == CompactBlockIndexDeltaSlot::A ? "A" : "B",
              sequence,
              old_records,
              verified_log->RecordCount(),
              target_delta->TailEntryCount(),
              compacted.base_updates.size(),
              Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));

    m_compact_block_delta_state = std::move(state);
    m_compact_block_delta = std::move(target_delta);
    m_compact_block_delta_log = std::move(verified_log);
    return true;
}

bool BlockManager::VerifyCompactBlockIndexDelta()
{
    AssertLockHeld(cs_main);

    uint64_t base_generation{0};
    uint64_t base_entry_count{0};
    const fs::path compact_path{CompactBlockIndexShadowPath()};
    if (fs::exists(compact_path)) {
        if (!OpenCompactBlockIndexMapped()) {
            LogPrintf("Compact block index: cannot verify metadata delta because compact base is unusable\n");
            return false;
        }
        base_generation = m_compact_block_index->Header()->generation;
        base_entry_count = m_compact_block_index->EntryCount();
    }

    if (!OpenCompactBlockIndexDeltaState(/*migrate_legacy=*/true)) {
        return false;
    }

    if (!m_compact_block_delta || !m_compact_block_delta->IsOpen()) {
        auto delta = std::make_unique<CompactBlockIndexDelta>();
        std::string error;
        const fs::path path{
            CompactBlockIndexDeltaState::SlotPath(
                CompactBlockIndexDeltaPath(),
                m_compact_block_delta_state->ActiveSlot())};
        if (!delta->Open(
                path,
                base_generation,
                base_entry_count,
                GetConsensus().hashGenesisBlock,
                error)) {
            LogPrintf("Compact block index: cannot open metadata delta %s: %s\n",
                      fs::PathToString(path), error);
            return false;
        }

        if (delta->TailEntryCount() !=
            m_compact_block_delta_state->SnapshotTailEntryCount()) {
            LogPrintf("Compact block index: metadata selector tail mismatch selector=%u delta=%u\n",
                      m_compact_block_delta_state->SnapshotTailEntryCount(),
                      delta->TailEntryCount());
            return false;
        }

        m_compact_block_delta = std::move(delta);
    }

    if (m_compact_block_delta->BaseEntryCount() != base_entry_count) {
        LogPrintf("Compact block index: metadata delta base count mismatch\n");
        return false;
    }

    const uint64_t snapshot_tail_count{m_compact_block_delta->TailEntryCount()};
    const uint64_t snapshot_next{base_entry_count + snapshot_tail_count};
    if (snapshot_next > m_next_compact_id) {
        LogPrintf("Compact block index: metadata delta snapshot newer than id namespace base=%u tail=%u next=%u\n",
                  base_entry_count, snapshot_tail_count, m_next_compact_id);
        return false;
    }

    if (!m_compact_block_delta_log || !m_compact_block_delta_log->IsOpen()) {
        if (!OpenCompactBlockIndexDeltaLog(/*create=*/false)) {
            return false;
        }
    }

    if (!ReconcileCompactBlockIndexDeltaPending()) {
        return false;
    }

    const auto start{SteadyClock::now()};

    // Last published update wins. Keep only the sparse overlay in memory so
    // base-generation updates do not require a 24-million-entry RAM vector.
    std::unordered_map<BlockIndexId, CompactBlockIndexEntry> overlay;
    overlay.reserve(static_cast<size_t>(m_compact_block_delta_log->RecordCount()));

    uint64_t replayed{0};
    std::string error;
    bool replay_invalid{false};
    if (!m_compact_block_delta_log->ForEach(
            [&](const CompactBlockIndexDeltaLogRecord& update) {
                if (static_cast<uint64_t>(update.id) >= m_next_compact_id) {
                    LogPrintf("Compact block index: metadata delta log references unpublished id=%u next=%u\n",
                              update.id, m_next_compact_id);
                    replay_invalid = true;
                    return false;
                }

                CBlockIndex* index{LookupBlockIndex(update.entry.hash)};
                if (!index || index->m_compact_id != update.id) {
                    LogPrintf("Compact block index: metadata delta log identity mismatch id=%u hash=%s\n",
                              update.id, update.entry.hash.ToString());
                    replay_invalid = true;
                    return false;
                }

                overlay[update.id] = update.entry;
                ++replayed;
                return true;
            },
            error)) {
        LogPrintf("Compact block index: metadata delta log replay failed: %s\n",
                  error);
        return false;
    }
    if (replay_invalid) return false;

    uint64_t verified_snapshot{0};
    uint64_t verified_updates{0};
    uint64_t verified_extensions{0};

    // Verify the snapshot tail, substituting a newer logged record when one
    // exists for the same id.
    for (uint64_t offset = 0; offset < snapshot_tail_count; ++offset) {
        if (m_interrupt) {
            LogPrintf("Compact block index: metadata delta verification interrupted\n");
            return false;
        }

        const BlockIndexId id{
            static_cast<BlockIndexId>(base_entry_count + offset)};
        const CompactBlockIndexEntry* entry{nullptr};

        const auto update{overlay.find(id)};
        if (update != overlay.end()) {
            entry = &update->second;
            ++verified_updates;
        } else {
            entry = m_compact_block_delta->Get(id);
        }

        if (!entry) {
            LogPrintf("Compact block index: metadata delta missing id=%u\n", id);
            return false;
        }

        CBlockIndex* index{LookupBlockIndex(entry->hash)};
        if (!index || index->m_compact_id != id) {
            LogPrintf("Compact block index: metadata delta identity mismatch id=%u hash=%s\n",
                      id, entry->hash.ToString());
            return false;
        }

        const BlockIndexId parent_id{
            index->pprev ? index->pprev->m_compact_id : INVALID_BLOCK_INDEX_ID};
        const BlockIndexId skip_id{
            index->pskip ? index->pskip->m_compact_id : INVALID_BLOCK_INDEX_ID};

        if (!entry->record.MatchesBlockIndex(*index, parent_id, skip_id)) {
            LogPrintf("Compact block index: metadata delta record mismatch id=%u height=%d hash=%s\n",
                      id, index->nHeight, index->GetBlockHash().ToString());
            return false;
        }
        ++verified_snapshot;
    }

    // Every id learned after the snapshot must have a full logged record.
    for (uint64_t raw_id = snapshot_next; raw_id < m_next_compact_id; ++raw_id) {
        if (m_interrupt) {
            LogPrintf("Compact block index: metadata delta extension verification interrupted\n");
            return false;
        }

        const BlockIndexId id{static_cast<BlockIndexId>(raw_id)};
        const auto update{overlay.find(id)};
        if (update == overlay.end()) {
            LogPrintf("Compact block index: metadata delta log missing extension id=%u\n",
                      id);
            return false;
        }

        const CompactBlockIndexEntry& entry{update->second};
        CBlockIndex* index{LookupBlockIndex(entry.hash)};
        if (!index || index->m_compact_id != id) {
            LogPrintf("Compact block index: metadata delta extension identity mismatch id=%u hash=%s\n",
                      id, entry.hash.ToString());
            return false;
        }

        const BlockIndexId parent_id{
            index->pprev ? index->pprev->m_compact_id : INVALID_BLOCK_INDEX_ID};
        const BlockIndexId skip_id{
            index->pskip ? index->pskip->m_compact_id : INVALID_BLOCK_INDEX_ID};

        if (!entry.record.MatchesBlockIndex(*index, parent_id, skip_id)) {
            LogPrintf("Compact block index: metadata delta extension mismatch id=%u height=%d hash=%s\n",
                      id, index->nHeight, index->GetBlockHash().ToString());
            return false;
        }
        ++verified_extensions;
    }

    // Logged updates to immutable-base ids are sparse. Verify their latest
    // state separately; untouched base records remain owned by index.compact.
    for (const auto& [id, entry] : overlay) {
        if (static_cast<uint64_t>(id) >= base_entry_count) continue;

        CBlockIndex* index{LookupBlockIndex(entry.hash)};
        if (!index || index->m_compact_id != id) {
            LogPrintf("Compact block index: metadata base overlay identity mismatch id=%u hash=%s\n",
                      id, entry.hash.ToString());
            return false;
        }

        const BlockIndexId parent_id{
            index->pprev ? index->pprev->m_compact_id : INVALID_BLOCK_INDEX_ID};
        const BlockIndexId skip_id{
            index->pskip ? index->pskip->m_compact_id : INVALID_BLOCK_INDEX_ID};

        if (!entry.record.MatchesBlockIndex(*index, parent_id, skip_id)) {
            LogPrintf("Compact block index: metadata base overlay mismatch id=%u height=%d hash=%s\n",
                      id, index->nHeight, index->GetBlockHash().ToString());
            return false;
        }
        ++verified_updates;
    }

    m_compact_block_delta_overlay = overlay;

    LogPrintf("Compact block index: verified metadata delta base=%u snapshot_tail=%u next=%u log_records=%u replayed=%u overlay=%u snapshot=%u updates=%u extensions=%u bytes=%u log_bytes=%u in %d ms\n",
              base_entry_count,
              snapshot_tail_count,
              m_next_compact_id,
              m_compact_block_delta_log->RecordCount(),
              replayed,
              overlay.size(),
              verified_snapshot,
              verified_updates,
              verified_extensions,
              m_compact_block_delta->SizeBytes(),
              m_compact_block_delta_log->SizeBytes(),
              Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));
    return true;
}

bool BlockManager::BuildCompactBlockIndexLookup()
{
    AssertLockHeld(cs_main);

    if (!OpenCompactBlockIndexMapped()) return false;

    const fs::path path{CompactBlockIndexLookupPath()};
    const auto start{SteadyClock::now()};
    std::string build_error;
    if (!CompactBlockIndexLookup::Build(path, *m_compact_block_index, build_error)) {
        LogPrintf("Compact block index: lookup build failed for %s: %s\n",
                  fs::PathToString(path), build_error);
        return false;
    }

    auto lookup = std::make_unique<CompactBlockIndexLookup>();
    std::string open_error;
    if (!lookup->Open(path, *m_compact_block_index, open_error)) {
        LogPrintf("Compact block index: built lookup but could not reopen %s: %s\n",
                  fs::PathToString(path), open_error);
        return false;
    }

    LogPrintf("Compact block index: built lookup entries=%u slots=%u bytes=%u path=%s in %d ms\n",
              lookup->EntryCount(),
              lookup->SlotCount(),
              lookup->SizeBytes(),
              fs::PathToString(path),
              Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));

    m_compact_block_lookup = std::move(lookup);
    return true;
}

bool BlockManager::VerifyCompactBlockIndexLookup()
{
    AssertLockHeld(cs_main);

    if (!OpenCompactBlockIndexMapped()) return false;

    if (!m_compact_block_lookup || !m_compact_block_lookup->IsOpen()) {
        auto lookup = std::make_unique<CompactBlockIndexLookup>();
        std::string open_error;
        const fs::path path{CompactBlockIndexLookupPath()};
        if (!lookup->Open(path, *m_compact_block_index, open_error)) {
            LogPrintf("Compact block index: cannot open lookup %s: %s\n",
                      fs::PathToString(path), open_error);
            return false;
        }
        m_compact_block_lookup = std::move(lookup);
    }

    const uint64_t count{m_compact_block_index->EntryCount()};
    const auto verify_start{SteadyClock::now()};
    int last_percent{-1};

    for (uint64_t raw_id = 0; raw_id < count; ++raw_id) {
        if (m_interrupt) {
            LogPrintf("Compact block index: lookup verification interrupted\n");
            return false;
        }

        const BlockIndexId id{static_cast<BlockIndexId>(raw_id)};
        const CompactBlockIndexEntry* entry{m_compact_block_index->Get(id)};
        if (!entry) {
            LogPrintf("Compact block index: lookup verify missing source id=%u\n", id);
            return false;
        }

        const std::optional<BlockIndexId> found{
            m_compact_block_lookup->Find(entry->hash, *m_compact_block_index)};
        if (!found || *found != id) {
            LogPrintf("Compact block index: lookup mismatch id=%u hash=%s\n",
                      id, entry->hash.ToString());
            return false;
        }

        if (count > 0) {
            const int percent{static_cast<int>((100 * (raw_id + 1)) / count)};
            if (percent != last_percent && percent % 10 == 0) {
                LogPrintf("Compact block index: lookup verify %d%%\n", percent);
                last_percent = percent;
            }
        }
    }

    const auto verify_elapsed{SteadyClock::now() - verify_start};
    LogPrintf("Compact block index: verified lookup entries=%u slots=%u bytes=%u in %d ms\n",
              count,
              m_compact_block_lookup->SlotCount(),
              m_compact_block_lookup->SizeBytes(),
              Ticks<std::chrono::milliseconds>(verify_elapsed));

    // Compare lookup cost against the legacy unordered_map while both
    // representations coexist. Sample uniformly through the persisted
    // generation to avoid an additional allocation.
    const uint64_t samples{std::min<uint64_t>(1'000'000, count)};
    if (samples > 0) {
        uint64_t legacy_hits{0};
        uint64_t compact_hits{0};
        uint64_t positive_probes{0};
        uint32_t positive_max_probes{0};

        const auto legacy_start{SteadyClock::now()};
        for (uint64_t sample = 0; sample < samples; ++sample) {
            const BlockIndexId id{static_cast<BlockIndexId>((sample * count) / samples)};
            const CompactBlockIndexEntry* entry{m_compact_block_index->Get(id)};
            legacy_hits += m_block_index.find(entry->hash) != m_block_index.end();
        }
        const auto legacy_elapsed{SteadyClock::now() - legacy_start};

        const auto compact_start{SteadyClock::now()};
        for (uint64_t sample = 0; sample < samples; ++sample) {
            const BlockIndexId id{static_cast<BlockIndexId>((sample * count) / samples)};
            const CompactBlockIndexEntry* entry{m_compact_block_index->Get(id)};
            uint32_t probes{0};
            const auto found{m_compact_block_lookup->Find(
                entry->hash, *m_compact_block_index, &probes)};
            compact_hits += found && *found == id;
            positive_probes += probes;
            positive_max_probes = std::max(positive_max_probes, probes);
        }
        const auto compact_elapsed{SteadyClock::now() - compact_start};

        LogPrintf("Compact block index: lookup benchmark positive samples=%u legacy_hits=%u legacy=%d ms compact_hits=%u compact=%d ms probes_avg=%.3f probes_max=%u\n",
                  samples,
                  legacy_hits,
                  Ticks<std::chrono::milliseconds>(legacy_elapsed),
                  compact_hits,
                  Ticks<std::chrono::milliseconds>(compact_elapsed),
                  static_cast<double>(positive_probes) / samples,
                  positive_max_probes);

        // Exercise the network-facing miss shape separately. Generate a stable
        // set of hashes that the legacy map proves are absent, then time both
        // implementations against exactly the same miss set.
        std::vector<uint256> negative_hashes;
        negative_hashes.reserve(samples);
        for (uint64_t nonce = 1; negative_hashes.size() < samples; ++nonce) {
            uint256 candidate;
            uint64_t x{nonce};
            for (size_t word = 0; word < 4; ++word) {
                // SplitMix64-style diffusion gives SipHash a well-spread set
                // without putting RNG/string construction in the timed loops.
                x += 0x9e3779b97f4a7c15ULL;
                uint64_t z{x};
                z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
                z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
                z ^= z >> 31;
                WriteLE64(candidate.data() + word * 8, z);
            }
            if (m_block_index.find(candidate) == m_block_index.end()) {
                negative_hashes.push_back(candidate);
            }
        }

        uint64_t legacy_misses{0};
        const auto legacy_negative_start{SteadyClock::now()};
        for (const uint256& hash : negative_hashes) {
            legacy_misses += m_block_index.find(hash) == m_block_index.end();
        }
        const auto legacy_negative_elapsed{SteadyClock::now() - legacy_negative_start};

        uint64_t compact_misses{0};
        uint64_t negative_probes{0};
        uint32_t negative_max_probes{0};
        const auto compact_negative_start{SteadyClock::now()};
        for (const uint256& hash : negative_hashes) {
            uint32_t probes{0};
            const auto found{m_compact_block_lookup->Find(
                hash, *m_compact_block_index, &probes)};
            compact_misses += !found;
            negative_probes += probes;
            negative_max_probes = std::max(negative_max_probes, probes);
        }
        const auto compact_negative_elapsed{SteadyClock::now() - compact_negative_start};

        LogPrintf("Compact block index: lookup benchmark negative samples=%u legacy_misses=%u legacy=%d ms compact_misses=%u compact=%d ms probes_avg=%.3f probes_max=%u\n",
                  samples,
                  legacy_misses,
                  Ticks<std::chrono::milliseconds>(legacy_negative_elapsed),
                  compact_misses,
                  Ticks<std::chrono::milliseconds>(compact_negative_elapsed),
                  static_cast<double>(negative_probes) / samples,
                  negative_max_probes);

        // Prototype the intended network-facing split: keep only keyed
        // fingerprints and exact occupancy resident. Unknown hashes then probe
        // anonymous memory exclusively; mapped id/full-hash state is touched
        // only after a keyed fingerprint match.
        std::string front_error;
        const auto front_load_start{SteadyClock::now()};
        if (m_compact_block_lookup->LoadResidentProbeFront(front_error)) {
            LogPrintf("Compact block index: resident lookup front bytes=%u load=%d ms\n",
                      m_compact_block_lookup->ResidentProbeFrontBytes(),
                      Ticks<std::chrono::milliseconds>(SteadyClock::now() - front_load_start));

            uint64_t resident_hits{0};
            uint64_t resident_positive_probes{0};
            uint32_t resident_positive_max{0};
            const auto resident_positive_start{SteadyClock::now()};
            for (uint64_t sample = 0; sample < samples; ++sample) {
                const BlockIndexId id{static_cast<BlockIndexId>((sample * count) / samples)};
                const CompactBlockIndexEntry* entry{m_compact_block_index->Get(id)};
                uint32_t probes{0};
                const auto found{m_compact_block_lookup->FindResident(
                    entry->hash, *m_compact_block_index, &probes)};
                resident_hits += found && *found == id;
                resident_positive_probes += probes;
                resident_positive_max = std::max(resident_positive_max, probes);
            }
            const auto resident_positive_elapsed{
                SteadyClock::now() - resident_positive_start};

            uint64_t resident_misses{0};
            uint64_t resident_negative_probes{0};
            uint64_t resident_negative_backing_touches{0};
            uint32_t resident_negative_max{0};
            const auto resident_negative_start{SteadyClock::now()};
            for (const uint256& hash : negative_hashes) {
                uint32_t probes{0};
                bool touched_backing{false};
                const auto found{m_compact_block_lookup->FindResident(
                    hash, *m_compact_block_index, &probes, &touched_backing)};
                resident_misses += !found;
                resident_negative_probes += probes;
                resident_negative_backing_touches += touched_backing;
                resident_negative_max = std::max(resident_negative_max, probes);
            }
            const auto resident_negative_elapsed{
                SteadyClock::now() - resident_negative_start};

            LogPrintf("Compact block index: resident-front benchmark positive samples=%u hits=%u time=%d ms probes_avg=%.3f probes_max=%u\n",
                      samples,
                      resident_hits,
                      Ticks<std::chrono::milliseconds>(resident_positive_elapsed),
                      static_cast<double>(resident_positive_probes) / samples,
                      resident_positive_max);
            LogPrintf("Compact block index: resident-front benchmark negative samples=%u misses=%u time=%d ms probes_avg=%.3f probes_max=%u backing_touches=%u\n",
                      samples,
                      resident_misses,
                      Ticks<std::chrono::milliseconds>(resident_negative_elapsed),
                      static_cast<double>(resident_negative_probes) / samples,
                      resident_negative_max,
                      resident_negative_backing_touches);
        } else {
            LogPrintf("Compact block index: resident lookup front unavailable: %s\n",
                      front_error);
        }
    }

    return true;
}

bool BlockManager::LoadCompactBlockIndexPayload(
    const CBlockIndex& index,
    BlockIndexResidentPayload& payload)
{
    AssertLockHeld(cs_main);

    const BlockIndexId id{index.m_compact_id};
    if (id == INVALID_BLOCK_INDEX_ID) return false;

    const CompactBlockIndexEntry* entry{nullptr};
    const auto overlay{m_compact_block_delta_overlay.find(id)};
    if (overlay != m_compact_block_delta_overlay.end()) {
        entry = &overlay->second;
    } else if (m_compact_block_index &&
               static_cast<uint64_t>(id) < m_compact_block_index->EntryCount()) {
        entry = m_compact_block_index->Get(id);
    } else if (m_compact_block_delta && m_compact_block_delta->IsOpen()) {
        entry = m_compact_block_delta->Get(id);
    }

    if (!entry || entry->hash != index.GetBlockHash()) return false;

    payload.nFile = entry->record.file;
    payload.nDataPos = entry->record.data_pos;
    payload.nUndoPos = entry->record.undo_pos;
    payload.hashMerkleRoot = entry->record.merkle_root;
    payload.nTimeMax = entry->record.time_max;
    payload.algo_history = nullptr;
    return true;
}

bool BlockManager::ActivateCompactBlockIndexIdentityStore(
    std::vector<CBlockIndex*>& startup_view)
{
    AssertLockHeld(cs_main);

    if (m_block_index.GetMode() == BlockIndexResidencyMode::FULL) return true;
    if (m_block_index.CompactIdentityActive()) return true;

    if (!m_compact_block_index || !m_compact_block_index->IsOpen()) {
        LogPrintf("Block-index identity: compact base unavailable; retaining legacy unordered_map ownership\n");
        return true;
    }

    // Once the legacy identity map is released, hash lookup becomes part of
    // the correctness path. Require the compact lookup to have been explicitly
    // built/verified by the configured lifecycle mode instead of silently
    // trusting an unverified leftover file when -blockindexcompactlookup=off.
    if (!m_compact_block_lookup || !m_compact_block_lookup->IsOpen()) {
        LogPrintf("Block-index identity: verified compact lookup unavailable; retaining legacy unordered_map ownership (set -blockindexcompactlookup=verify or build)\n");
        return true;
    }

    if (!m_compact_block_lookup->HasResidentProbeFront()) {
        std::string error;
        const auto front_start{SteadyClock::now()};
        if (!m_compact_block_lookup->LoadResidentProbeFront(error)) {
            LogPrintf("Block-index identity: resident lookup front unavailable (%s); retaining legacy unordered_map ownership\n",
                      error);
            return true;
        }
        LogPrintf("Block-index identity: loaded resident lookup front bytes=%u in %d ms\n",
                  m_compact_block_lookup->ResidentProbeFrontBytes(),
                  Ticks<std::chrono::milliseconds>(SteadyClock::now() - front_start));
    }

    const auto start{SteadyClock::now()};
    std::vector<CBlockIndex*> by_id{GetAllBlockIndicesByCompactId()};
    const size_t immutable_count{
        static_cast<size_t>(m_compact_block_index->EntryCount())};

    if (!m_block_index.PrepareCompactIdentityCutover(
            by_id,
            immutable_count,
            [this](BlockIndexId id) -> const uint256* {
                const CompactBlockIndexEntry* entry{m_compact_block_index->Get(id)};
                return entry ? &entry->hash : nullptr;
            })) {
        LogPrintf("Block-index identity: dense-shell preparation failed; retaining legacy unordered_map ownership\n");
        return true;
    }

    for (CBlockIndex*& index : startup_view) {
        index = m_block_index.RemapPreparedIdentity(index);
    }

    std::multimap<CBlockIndex*, CBlockIndex*> remapped_unlinked;
    for (const auto& [parent, child] : m_blocks_unlinked) {
        remapped_unlinked.emplace(
            m_block_index.RemapPreparedIdentity(parent),
            m_block_index.RemapPreparedIdentity(child));
    }

    std::set<CBlockIndex*> remapped_dirty;
    for (CBlockIndex* index : m_dirty_blockindex) {
        remapped_dirty.insert(m_block_index.RemapPreparedIdentity(index));
    }

    m_blocks_unlinked.swap(remapped_unlinked);
    m_dirty_blockindex.swap(remapped_dirty);
    m_block_index.CommitCompactIdentityCutover();

    LogPrintf("Block-index identity: activated dense compact-id shells=%u immutable=%u live_tail=%u shell_bytes=%u in %d ms\n",
              m_block_index.size(),
              immutable_count,
              m_block_index.size() - immutable_count,
              static_cast<uint64_t>(m_block_index.size()) * sizeof(CBlockIndex),
              Ticks<std::chrono::milliseconds>(SteadyClock::now() - start));
    return true;
}

bool BlockManager::ActivateBlockIndexPayloadCache(CBlockIndex* tip)
{
    AssertLockHeld(cs_main);

    if (m_block_index.GetMode() == BlockIndexResidencyMode::FULL) return true;

    if (!m_compact_block_index ||
        !m_compact_block_delta ||
        !m_compact_block_delta->IsOpen()) {
        LogPrintf("Block-index residency: compact backing unavailable; retaining eager payload residency\n");
        return false;
    }

    const bool activated{m_block_index.ActivatePayloadCache(
        tip,
        [this](const CBlockIndex& index, BlockIndexResidentPayload& payload) {
            AssertLockHeld(cs_main);
            return LoadCompactBlockIndexPayload(index, payload);
        })};

    if (!activated) {
        LogPrintf("Block-index residency: payload-cache activation failed; retaining eager payload residency\n");
        return false;
    }

    LogPrintf("Block-index residency: activated payload cache mode=%s hotdepth=%u budget=%u MiB resident=%u bytes=%u shell=%u payload=%u\n",
              BlockIndexStore::ModeName(m_block_index.GetMode()),
              m_block_index.GetHotDepth(),
              m_block_index.CacheLimitBytes() / (1024 * 1024),
              m_block_index.ResidentPayloads(),
              m_block_index.ResidentPayloadBytes(),
              sizeof(CBlockIndex),
              sizeof(BlockIndexResidentPayload));
    return true;
}

CBlockIndex* BlockManager::LookupBlockIndex(const uint256& hash)
{
    AssertLockHeld(cs_main);

    if (CBlockIndex* resident{m_block_index.Lookup(hash)}) return resident;

    if (m_block_index.CompactIdentityActive() &&
        m_compact_block_lookup && m_compact_block_lookup->IsOpen() &&
        m_compact_block_index && m_compact_block_index->IsOpen()) {
        const auto found{
            m_compact_block_lookup->HasResidentProbeFront()
                ? m_compact_block_lookup->FindResident(hash, *m_compact_block_index)
                : m_compact_block_lookup->Find(hash, *m_compact_block_index)};
        if (found &&
            static_cast<size_t>(*found) < m_block_index.ImmutableIdentityCount()) {
            return m_block_index.ByCompactId(*found);
        }
    }
    return nullptr;
}

const CBlockIndex* BlockManager::LookupBlockIndex(const uint256& hash) const
{
    AssertLockHeld(cs_main);

    if (const CBlockIndex* resident{m_block_index.Lookup(hash)}) return resident;

    if (m_block_index.CompactIdentityActive() &&
        m_compact_block_lookup && m_compact_block_lookup->IsOpen() &&
        m_compact_block_index && m_compact_block_index->IsOpen()) {
        const auto found{
            m_compact_block_lookup->HasResidentProbeFront()
                ? m_compact_block_lookup->FindResident(hash, *m_compact_block_index)
                : m_compact_block_lookup->Find(hash, *m_compact_block_index)};
        if (found &&
            static_cast<size_t>(*found) < m_block_index.ImmutableIdentityCount()) {
            return m_block_index.ByCompactId(*found);
        }
    }
    return nullptr;
}

CBlockIndex* BlockManager::AddToBlockIndex(const CBlockHeader& block, CBlockIndex*& best_header)
{
    AssertLockHeld(cs_main);

    if (CBlockIndex* existing{LookupBlockIndex(block.GetHash())}) {
        return existing;
    }

    auto [pindexNew, inserted] = m_block_index.Insert(block.GetHash(), block);
    Assert(inserted);
    pindexNew->m_compact_id = AllocateCompactId();
    if (m_block_index.CompactIdentityActive()) {
        Assert(m_block_index.ByCompactId(pindexNew->m_compact_id) == pindexNew);
    }

    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;

    if (CBlockIndex* pprev{LookupBlockIndex(block.hashPrevBlock)}) {
        pindexNew->pprev = pprev;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();
    }
    // A newly accepted header is live state. Keep its algorithm-history
    // accelerator resident in every mode so validation/mining never needs I/O.
    m_block_index.EnsureAlgoHistory(*pindexNew);
    pindexNew->TimeMax() = (pindexNew->pprev ? std::max(pindexNew->pprev->TimeMax(), pindexNew->nTime) : pindexNew->nTime);
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);
    pindexNew->RaiseValidity(BLOCK_VALID_TREE);
    if (best_header == nullptr || best_header->nChainWork < pindexNew->nChainWork) {
        best_header = pindexNew;
    }

    if (m_dirty_blockindex.insert(pindexNew).second) {
        m_block_index.PinPayload(*pindexNew);
    }

    return pindexNew;
}

void BlockManager::PruneOneBlockFile(const int fileNumber)
{
    AssertLockHeld(cs_main);
    LOCK(cs_LastBlockFile);

    m_block_index.ForEach([&](CBlockIndex& block_index) {
        CBlockIndex* pindex = &block_index;
        if (!(pindex->nStatus & BLOCK_HAVE_DATA)) return;
        if (pindex->StorageFile() == fileNumber) {
            if (m_dirty_blockindex.insert(pindex).second) {
                m_block_index.PinPayload(*pindex);
            }
            pindex->nStatus &= ~BLOCK_HAVE_DATA;
            pindex->nStatus &= ~BLOCK_HAVE_UNDO;
            pindex->StorageFile() = 0;
            pindex->DataPos() = 0;
            pindex->UndoPos() = 0;

            // Prune from m_blocks_unlinked -- any block we prune would have
            // to be downloaded again in order to consider its chain, at which
            // point it would be considered as a candidate for
            // m_blocks_unlinked or setBlockIndexCandidates.
            auto range = m_blocks_unlinked.equal_range(pindex->pprev);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator _it = range.first;
                range.first++;
                if (_it->second == pindex) {
                    m_blocks_unlinked.erase(_it);
                }
            }
        }
    });

    m_blockfile_info.at(fileNumber) = CBlockFileInfo{};
    m_dirty_fileinfo.insert(fileNumber);
}

void BlockManager::FindFilesToPruneManual(
    std::set<int>& setFilesToPrune,
    int nManualPruneHeight,
    const Chainstate& chain,
    ChainstateManager& chainman)
{
    assert(IsPruneMode() && nManualPruneHeight > 0);

    LOCK2(cs_main, cs_LastBlockFile);
    if (chain.m_chain.Height() < 0) {
        return;
    }

    const auto [min_block_to_prune, last_block_can_prune] = chainman.GetPruneRange(chain, nManualPruneHeight);

    int count = 0;
    for (int fileNumber = 0; fileNumber < this->MaxBlockfileNum(); fileNumber++) {
        const auto& fileinfo = m_blockfile_info[fileNumber];
        if (fileinfo.nSize == 0 || fileinfo.nHeightLast > (unsigned)last_block_can_prune || fileinfo.nHeightFirst < (unsigned)min_block_to_prune) {
            continue;
        }

        PruneOneBlockFile(fileNumber);
        setFilesToPrune.insert(fileNumber);
        count++;
    }
    LogPrintf("[%s] Prune (Manual): prune_height=%d removed %d blk/rev pairs\n",
        chain.GetRole(), last_block_can_prune, count);
}

void BlockManager::FindFilesToPrune(
    std::set<int>& setFilesToPrune,
    int last_prune,
    const Chainstate& chain,
    ChainstateManager& chainman)
{
    LOCK2(cs_main, cs_LastBlockFile);
    // Distribute our -prune budget over all chainstates.
    const auto target = std::max(
        MIN_DISK_SPACE_FOR_BLOCK_FILES, GetPruneTarget() / chainman.GetAll().size());

    if (chain.m_chain.Height() < 0 || target == 0) {
        return;
    }
    if (static_cast<uint64_t>(chain.m_chain.Height()) <= chainman.GetParams().PruneAfterHeight()) {
        return;
    }

    const auto [min_block_to_prune, last_block_can_prune] = chainman.GetPruneRange(chain, last_prune);

    uint64_t nCurrentUsage = CalculateCurrentUsage();
    // We don't check to prune until after we've allocated new space for files
    // So we should leave a buffer under our target to account for another allocation
    // before the next pruning.
    uint64_t nBuffer = BLOCKFILE_CHUNK_SIZE + UNDOFILE_CHUNK_SIZE;
    uint64_t nBytesToPrune;
    int count = 0;

    if (nCurrentUsage + nBuffer >= target) {
        // On a prune event, the chainstate DB is flushed.
        // To avoid excessive prune events negating the benefit of high dbcache
        // values, we should not prune too rapidly.
        // So when pruning in IBD, increase the buffer a bit to avoid a re-prune too soon.
        if (chainman.IsInitialBlockDownload()) {
            // Since this is only relevant during IBD, we use a fixed 10%
            nBuffer += target / 10;
        }

        for (int fileNumber = 0; fileNumber < this->MaxBlockfileNum(); fileNumber++) {
            const auto& fileinfo = m_blockfile_info[fileNumber];
            nBytesToPrune = fileinfo.nSize + fileinfo.nUndoSize;

            if (fileinfo.nSize == 0) {
                continue;
            }

            if (nCurrentUsage + nBuffer < target) { // are we below our target?
                break;
            }

            // don't prune files that could have a block that's not within the allowable
            // prune range for the chain being pruned.
            if (fileinfo.nHeightLast > (unsigned)last_block_can_prune || fileinfo.nHeightFirst < (unsigned)min_block_to_prune) {
                continue;
            }

            PruneOneBlockFile(fileNumber);
            // Queue up the files for removal
            setFilesToPrune.insert(fileNumber);
            nCurrentUsage -= nBytesToPrune;
            count++;
        }
    }

    LogPrint(BCLog::PRUNE, "[%s] target=%dMiB actual=%dMiB diff=%dMiB min_height=%d max_prune_height=%d removed %d blk/rev pairs\n",
             chain.GetRole(), target / 1024 / 1024, nCurrentUsage / 1024 / 1024,
             (int64_t(target) - int64_t(nCurrentUsage)) / 1024 / 1024,
             min_block_to_prune, last_block_can_prune, count);
}

void BlockManager::UpdatePruneLock(const std::string& name, const PruneLockInfo& lock_info) {
    AssertLockHeld(::cs_main);
    m_prune_locks[name] = lock_info;
}

CBlockIndex* BlockManager::InsertBlockIndex(const uint256& hash)
{
    AssertLockHeld(cs_main);

    if (hash.IsNull()) {
        return nullptr;
    }

    if (CBlockIndex* existing{LookupBlockIndex(hash)}) {
        return existing;
    }

    auto [pindex, inserted]{m_block_index.Insert(hash)};
    Assert(inserted);
    return pindex;
}

bool BlockManager::LoadBlockIndex(const std::optional<uint256>& snapshot_blockhash)
{
    LogPrintf("Block-index residency: mode=%s hotdepth=%u cache=%u MiB shell=%u payload=%u\n",
              BlockIndexStore::ModeName(m_block_index.GetMode()),
              m_block_index.GetHotDepth(),
              m_block_index.CacheLimitBytes() / (1024 * 1024),
              sizeof(CBlockIndex),
              sizeof(BlockIndexResidentPayload));

    if (!m_block_tree_db->LoadBlockIndexGuts(
            GetConsensus(), [this](const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main) { return this->InsertBlockIndex(hash); }, m_interrupt)) {
        return false;
    }

    if (snapshot_blockhash) {
        const std::optional<AssumeutxoData> maybe_au_data = GetParams().AssumeutxoForBlockhash(*snapshot_blockhash);
        if (!maybe_au_data) {
            LogPrintf("ERROR: Assumeutxo data not found for blockhash '%s'. This is a DigiByte-specific issue where the snapshot was created with DigiByte blocks but chainparams has Bitcoin assumeutxo data.\n", snapshot_blockhash->ToString());
            m_opts.notifications.fatalError(strprintf("Assumeutxo data not found for the given blockhash '%s'.", snapshot_blockhash->ToString()));
            return false;
        }
        const AssumeutxoData& au_data = *Assert(maybe_au_data);
        m_snapshot_height = au_data.height;
        CBlockIndex* base{LookupBlockIndex(*snapshot_blockhash)};

        // Since nChainTx (responsible for estimated progress) isn't persisted
        // to disk, we must bootstrap the value for assumedvalid chainstates
        // from the hardcoded assumeutxo chainparams.
        base->nChainTx = au_data.nChainTx;
        LogPrintf("[snapshot] set nChainTx=%d for %s\n", au_data.nChainTx, snapshot_blockhash->ToString());
    } else {
        // If this isn't called with a snapshot blockhash, make sure the cached snapshot height
        // is null. This is relevant during snapshot completion, when the blockman may be loaded
        // with a height that then needs to be cleared after the snapshot is fully validated.
        m_snapshot_height.reset();
    }

    Assert(m_snapshot_height.has_value() == snapshot_blockhash.has_value());

    // Calculate nChainWork
    const auto collect_start{SteadyClock::now()};
    LogPrintf("LoadBlockIndex: Getting all block indices...");
    std::vector<CBlockIndex*> vSortedByHeight{GetAllBlockIndices()};
    LogPrintf("Startup timing: first block-index vector: %d entries in %d ms\n",
              vSortedByHeight.size(), Ticks<std::chrono::milliseconds>(SteadyClock::now() - collect_start));

    const auto sort_start{SteadyClock::now()};
    LogPrintf("LoadBlockIndex: Sorting %d block indices by height...", vSortedByHeight.size());
    std::sort(vSortedByHeight.begin(), vSortedByHeight.end(),
              [](const CBlockIndex* a, const CBlockIndex* b) {
                  if (a->nHeight != b->nHeight) return a->nHeight < b->nHeight;
                  return a->GetBlockHash() < b->GetBlockHash();
              });
    LogPrintf("Startup timing: first block-index height sort: %d entries in %d ms\n",
              vSortedByHeight.size(), Ticks<std::chrono::milliseconds>(SteadyClock::now() - sort_start));

    if (vSortedByHeight.size() >= static_cast<size_t>(INVALID_BLOCK_INDEX_ID)) {
        return error("%s: compact block-index id space exhausted", __func__);
    }
    LogPrintf("LoadBlockIndex: Sort complete, processing blocks...");
    const auto process_start{SteadyClock::now()};

    CBlockIndex* previous_index{nullptr};
    SteadyClock::duration reconstruction_algo_time{};
    SteadyClock::duration reconstruction_chainwork_time{};
    SteadyClock::duration reconstruction_timemax_time{};
    SteadyClock::duration reconstruction_linkage_time{};
    SteadyClock::duration chainwork_cache_write_time{};
    static constexpr size_t CHAINWORK_CACHE_MIGRATION_BATCH_SIZE{100000};
    std::vector<const CBlockIndex*> chainwork_cache_migration;
    chainwork_cache_migration.reserve(CHAINWORK_CACHE_MIGRATION_BATCH_SIZE);
    size_t chainwork_cache_hits{0};
    size_t chainwork_cache_misses{0};
    size_t chainwork_cache_migrated{0};
    int nProcessed = 0;
    int nLastPercent = -1;
    int nTotal = vSortedByHeight.size();
    for (CBlockIndex* pindex : vSortedByHeight) {
        // Show progress
        if (nTotal > 0) {
            int nPercent = 100 * nProcessed / nTotal;
            if (nPercent > nLastPercent && nPercent % 10 == 0) {
                LogPrintf("LoadBlockIndex: Processing blocks... %d%%\n", nPercent);
                nLastPercent = nPercent;
            }
        }
        nProcessed++;
        if (m_interrupt) return false;
        if (previous_index && pindex->nHeight > previous_index->nHeight + 1) {
            return error("%s: block index is non-contiguous, index of height %d missing", __func__, previous_index->nHeight + 1);
        }
        previous_index = pindex;
        const auto algo_start{SteadyClock::now()};
        if (m_block_index.GetMode() == BlockIndexResidencyMode::FULL) {
            m_block_index.EnsureAlgoHistory(*pindex);
        }
        const auto algo_end{SteadyClock::now()};
        if (pindex->nChainWork != 0) {
            ++chainwork_cache_hits;
        } else {
            pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
            ++chainwork_cache_misses;
            chainwork_cache_migration.push_back(pindex);
        }
        const auto chainwork_end{SteadyClock::now()};
        pindex->TimeMax() = (pindex->pprev ? std::max(pindex->pprev->TimeMax(), pindex->nTime) : pindex->nTime);
        const auto timemax_end{SteadyClock::now()};

        reconstruction_algo_time += algo_end - algo_start;
        reconstruction_chainwork_time += chainwork_end - algo_end;
        reconstruction_timemax_time += timemax_end - chainwork_end;
        const auto linkage_start{SteadyClock::now()};

        // We can link the chain of blocks for which we've received transactions at some point, or
        // blocks that are assumed-valid on the basis of snapshot load (see
        // PopulateAndValidateSnapshot()).
        // Pruned nodes may have deleted the block.
        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (m_snapshot_height && pindex->nHeight == *m_snapshot_height &&
                        pindex->GetBlockHash() == *snapshot_blockhash) {
                    // Should have been set above; don't disturb it with code below.
                    Assert(pindex->nChainTx > 0);
                } else if (pindex->pprev->nChainTx > 0) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                } else {
                    pindex->nChainTx = 0;
                    m_blocks_unlinked.insert(std::make_pair(pindex->pprev, pindex));
                }
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }
        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev && (pindex->pprev->nStatus & BLOCK_FAILED_MASK)) {
            pindex->nStatus |= BLOCK_FAILED_CHILD;
            m_dirty_blockindex.insert(pindex);
        }
        if (pindex->pprev) {
            pindex->BuildSkip();
        }
        reconstruction_linkage_time += SteadyClock::now() - linkage_start;

        if (chainwork_cache_migration.size() >= CHAINWORK_CACHE_MIGRATION_BATCH_SIZE) {
            const auto cache_write_start{SteadyClock::now()};
            if (!m_block_tree_db->WriteBlockIndexBatch(chainwork_cache_migration, true)) {
                return error("%s: failed to persist reconstructed chain-work cache", __func__);
            }
            chainwork_cache_write_time += SteadyClock::now() - cache_write_start;
            chainwork_cache_migrated += chainwork_cache_migration.size();
            chainwork_cache_migration.clear();
        }
    }

    if (!chainwork_cache_migration.empty()) {
        const auto cache_write_start{SteadyClock::now()};
        if (!m_block_tree_db->WriteBlockIndexBatch(chainwork_cache_migration, true)) {
            return error("%s: failed to persist reconstructed chain-work cache", __func__);
        }
        chainwork_cache_write_time += SteadyClock::now() - cache_write_start;
        chainwork_cache_migrated += chainwork_cache_migration.size();
    }

    LogPrintf("Startup timing: chain-work cache: hits=%d misses=%d migrated=%d write=%d ms\n",
              chainwork_cache_hits,
              chainwork_cache_misses,
              chainwork_cache_migrated,
              Ticks<std::chrono::milliseconds>(chainwork_cache_write_time));
    LogPrintf("Startup timing: block-index reconstruction detail: algo=%d ms chainwork=%d ms timemax=%d ms linkage=%d ms\n",
              Ticks<std::chrono::milliseconds>(reconstruction_algo_time),
              Ticks<std::chrono::milliseconds>(reconstruction_chainwork_time),
              Ticks<std::chrono::milliseconds>(reconstruction_timemax_time),
              Ticks<std::chrono::milliseconds>(reconstruction_linkage_time));
    LogPrintf("Startup timing: block-index reconstruction pass: %d entries in %d ms\n",
              nProcessed, Ticks<std::chrono::milliseconds>(SteadyClock::now() - process_start));

    switch (m_opts.block_index_compact_shadow) {
    case kernel::BlockIndexCompactShadowMode::OFF:
        AssignCompactIdsDeterministic(vSortedByHeight);
        break;
    case kernel::BlockIndexCompactShadowMode::BUILD:
        AssignCompactIdsDeterministic(vSortedByHeight);
        if (!BuildCompactBlockIndexShadow(vSortedByHeight) ||
            !VerifyCompactBlockIndexShadow(vSortedByHeight)) {
            LogPrintf("Compact block index: shadow build/verify failed; continuing with legacy block index\n");
            AssignCompactIdsDeterministic(vSortedByHeight);
            m_compact_block_index.reset();
        }
        break;
    case kernel::BlockIndexCompactShadowMode::VERIFY:
        if (!VerifyCompactBlockIndexShadow(vSortedByHeight)) {
            LogPrintf("Compact block index: shadow unavailable or incompatible; continuing with legacy block index\n");
            AssignCompactIdsDeterministic(vSortedByHeight);
            m_compact_block_index.reset();
        }
        break;
    }

    switch (m_opts.block_index_compact_ids) {
    case kernel::BlockIndexCompactIdsMode::OFF:
        break;
    case kernel::BlockIndexCompactIdsMode::BUILD:
        if (!RestoreCompactIds(vSortedByHeight, /*allow_create=*/true)) {
            LogPrintf("Compact block index: persistent id build/restore failed; continuing with process-local ids\n");
            AssignCompactIdsDeterministic(vSortedByHeight);
            m_compact_block_ids.reset();
        }
        break;
    case kernel::BlockIndexCompactIdsMode::VERIFY:
        if (!RestoreCompactIds(vSortedByHeight, /*allow_create=*/false)) {
            LogPrintf("Compact block index: persistent id verification failed; continuing with process-local ids\n");
            AssignCompactIdsDeterministic(vSortedByHeight);
            m_compact_block_ids.reset();
        }
        break;
    }

    switch (m_opts.block_index_compact_delta) {
    case kernel::BlockIndexCompactDeltaMode::OFF:
        break;
    case kernel::BlockIndexCompactDeltaMode::BUILD:
        if (!BuildCompactBlockIndexDelta(vSortedByHeight) ||
            !VerifyCompactBlockIndexDelta()) {
            LogPrintf("Compact block index: metadata delta build/verify failed; continuing without metadata delta\n");
            m_compact_block_delta.reset();
            m_compact_block_delta_log.reset();
            m_compact_block_delta_state.reset();
        }
        break;
    case kernel::BlockIndexCompactDeltaMode::VERIFY:
        if (!VerifyCompactBlockIndexDelta()) {
            LogPrintf("Compact block index: metadata delta verification failed; continuing without metadata delta\n");
            m_compact_block_delta.reset();
            m_compact_block_delta_log.reset();
            m_compact_block_delta_state.reset();
        }
        break;
    case kernel::BlockIndexCompactDeltaMode::COMPACT:
        if (!VerifyCompactBlockIndexDelta() ||
            !CompactBlockIndexMetadata() ||
            !VerifyCompactBlockIndexDelta()) {
            LogPrintf("Compact block index: metadata compaction/verification failed; continuing without metadata delta\n");
            m_compact_block_delta.reset();
            m_compact_block_delta_log.reset();
            m_compact_block_delta_state.reset();
        }
        break;
    }

    switch (m_opts.block_index_compact_lookup) {
    case kernel::BlockIndexCompactLookupMode::OFF:
        break;
    case kernel::BlockIndexCompactLookupMode::BUILD:
        if (!BuildCompactBlockIndexLookup() ||
            !VerifyCompactBlockIndexLookup()) {
            LogPrintf("Compact block index: lookup build/verify failed; continuing without compact lookup\n");
            m_compact_block_lookup.reset();
        }
        break;
    case kernel::BlockIndexCompactLookupMode::VERIFY:
        if (!VerifyCompactBlockIndexLookup()) {
            LogPrintf("Compact block index: lookup unavailable or incompatible; continuing without compact lookup\n");
            m_compact_block_lookup.reset();
        }
        break;
    }

    // BALANCED/LOWMEM can now release the 24M-node legacy hash map while
    // preserving stable CBlockIndex* shells in dense compact-id order. If the
    // derived compact lookup is unavailable we deliberately retain the legacy
    // representation and continue normally.
    if (!ActivateCompactBlockIndexIdentityStore(vSortedByHeight)) {
        return false;
    }

    // ChainstateManager immediately needs to traverse every loaded index to
    // rebuild candidate and best-header state. Preserve this already-built,
    // height-ordered view across the handoff.
    m_startup_block_index_view = std::move(vSortedByHeight);

    return true;
}

bool BlockManager::WriteBlockIndexDB()
{
    AssertLockHeld(::cs_main);
    std::vector<std::pair<int, const CBlockFileInfo*>> vFiles;
    vFiles.reserve(m_dirty_fileinfo.size());
    for (std::set<int>::iterator it = m_dirty_fileinfo.begin(); it != m_dirty_fileinfo.end();) {
        vFiles.emplace_back(*it, &m_blockfile_info[*it]);
        m_dirty_fileinfo.erase(it++);
    }
    std::vector<const CBlockIndex*> vBlocks;
    vBlocks.reserve(m_dirty_blockindex.size());
    for (std::set<CBlockIndex*>::iterator it = m_dirty_blockindex.begin(); it != m_dirty_blockindex.end();) {
        vBlocks.push_back(*it);
        m_dirty_blockindex.erase(it++);
    }
    int max_blockfile = WITH_LOCK(cs_LastBlockFile, return this->MaxBlockfileNum());
    // Publish compact ids before the corresponding upstream block-index batch.
    // If a crash happens after this fsync but before LevelDB commits, startup
    // recognizes and truncates the unpublished id suffix in BUILD mode.
    if (!PersistCompactIds(vBlocks)) {
        return false;
    }

    // Stage the exact compact metadata batch before committing LevelDB. The
    // pending file is a recovery journal, not authoritative state: after a
    // crash startup compares it against the canonical LevelDB graph and either
    // publishes or discards the complete batch.
    bool metadata_staged{false};
    bool metadata_degraded{false};
    if (m_compact_block_delta_log && m_compact_block_delta_log->IsOpen()) {
        if (!StageCompactBlockIndexDeltaPending(vBlocks)) {
            LogPrintf("Compact block index: disabling future metadata writes after pending-batch failure; retaining verified backing for cache reads\n");
            ClearCompactBlockIndexDeltaPending();
            m_compact_block_delta_log.reset();
            m_block_index.DisablePayloadEviction();
            metadata_degraded = true;
        } else {
            metadata_staged = fs::exists(CompactBlockIndexDeltaPendingPath());
        }
    }

    if (metadata_staged &&
        m_opts.block_index_compact_fault == kernel::BlockIndexCompactFaultMode::AFTER_PENDING) {
        LogPrintf("Compact block index: fault injection after pending metadata batch; terminating before legacy LevelDB commit\n");
        std::_Exit(85);
    }

    if (!m_block_tree_db->WriteBatchSync(vFiles, max_blockfile, vBlocks)) {
        if (metadata_staged && !ClearCompactBlockIndexDeltaPending()) {
            LogPrintf("Compact block index: metadata pending batch remains after failed legacy commit; startup will reconcile it\n");
        }
        return false;
    }

    if (metadata_staged &&
        m_opts.block_index_compact_fault == kernel::BlockIndexCompactFaultMode::AFTER_LEVELDB) {
        LogPrintf("Compact block index: fault injection after legacy LevelDB commit; terminating before metadata log publish\n");
        std::_Exit(86);
    }

    // The ordinary upstream block index is canonical. Only after its atomic
    // batch succeeds do we publish the staged compact records. If publication
    // fails, leave the pending batch intact so startup can finish the commit.
    bool compact_metadata_published{!metadata_staged && !metadata_degraded};
    if (metadata_staged && !PublishCompactBlockIndexDeltaPending()) {
        LogPrintf("Compact block index: disabling future metadata writes after publish failure; retaining verified backing and disabling cache eviction\n");
        m_compact_block_delta_log.reset();
        m_block_index.DisablePayloadEviction();
        compact_metadata_published = false;
    } else if (metadata_staged) {
        compact_metadata_published = true;
    }

    // Do not let mutable payload state become evictable until both the
    // canonical LevelDB batch and, when active, its compact metadata publish
    // have completed. A failed compact publish intentionally keeps these
    // payloads pinned so the running process cannot reload stale backing state.
    if (compact_metadata_published) {
        for (const CBlockIndex* index : vBlocks) {
            m_block_index.ReleasePayloadPin(*const_cast<CBlockIndex*>(index));
        }
    }
    return true;
}

bool BlockManager::LoadBlockIndexDB(const std::optional<uint256>& snapshot_blockhash)
{
    if (!LoadBlockIndex(snapshot_blockhash)) {
        return false;
    }
    int max_blockfile_num{0};

    // Load block file info
    m_block_tree_db->ReadLastBlockFile(max_blockfile_num);
    m_blockfile_info.resize(max_blockfile_num + 1);
    LogPrintf("%s: last block file = %i\n", __func__, max_blockfile_num);
    for (int nFile = 0; nFile <= max_blockfile_num; nFile++) {
        m_block_tree_db->ReadBlockFileInfo(nFile, m_blockfile_info[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, m_blockfile_info[max_blockfile_num].ToString());
    for (int nFile = max_blockfile_num + 1; true; nFile++) {
        CBlockFileInfo info;
        if (m_block_tree_db->ReadBlockFileInfo(nFile, info)) {
            m_blockfile_info.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    std::set<int> setBlkDataFiles;
    m_block_index.ForEach([&](const CBlockIndex& block_index) {
        if (block_index.nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(block_index.StorageFile());
        }
    });
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++) {
        FlatFilePos pos(*it, 0);
        if (OpenBlockFile(pos, true).IsNull()) {
            return false;
        }
    }

    {
        // Initialize the blockfile cursors.
        LOCK(cs_LastBlockFile);
        for (size_t i = 0; i < m_blockfile_info.size(); ++i) {
            const auto last_height_in_file = m_blockfile_info[i].nHeightLast;
            m_blockfile_cursors[BlockfileTypeForHeight(last_height_in_file)] = {static_cast<int>(i), 0};
        }
    }

    // Check whether we have ever pruned block & undo files
    m_block_tree_db->ReadFlag("prunedblockfiles", m_have_pruned);
    if (m_have_pruned) {
        LogPrintf("LoadBlockIndexDB(): Block files have previously been pruned\n");
    }

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    m_block_tree_db->ReadReindexing(fReindexing);
    if (fReindexing) fReindex = true;

    return true;
}

void BlockManager::ScanAndUnlinkAlreadyPrunedFiles()
{
    AssertLockHeld(::cs_main);
    int max_blockfile = WITH_LOCK(cs_LastBlockFile, return this->MaxBlockfileNum());
    if (!m_have_pruned) {
        return;
    }

    std::set<int> block_files_to_prune;
    for (int file_number = 0; file_number < max_blockfile; file_number++) {
        if (m_blockfile_info[file_number].nSize == 0) {
            block_files_to_prune.insert(file_number);
        }
    }

    UnlinkPrunedFiles(block_files_to_prune);
}

const CBlockIndex* BlockManager::GetLastCheckpoint(const CCheckpointData& data)
{
    const MapCheckpoints& checkpoints = data.mapCheckpoints;

    for (const MapCheckpoints::value_type& i : reverse_iterate(checkpoints)) {
        const uint256& hash = i.second;
        const CBlockIndex* pindex = LookupBlockIndex(hash);
        if (pindex) {
            return pindex;
        }
    }
    return nullptr;
}

bool BlockManager::IsBlockPruned(const CBlockIndex* pblockindex)
{
    AssertLockHeld(::cs_main);
    return (m_have_pruned && !(pblockindex->nStatus & BLOCK_HAVE_DATA) && pblockindex->nTx > 0);
}

const CBlockIndex* BlockManager::GetFirstStoredBlock(const CBlockIndex& upper_block, const CBlockIndex* lower_block)
{
    AssertLockHeld(::cs_main);
    const CBlockIndex* last_block = &upper_block;
    assert(last_block->nStatus & BLOCK_HAVE_DATA); // 'upper_block' must have data
    while (last_block->pprev && (last_block->pprev->nStatus & BLOCK_HAVE_DATA)) {
        if (lower_block) {
            // Return if we reached the lower_block
            if (last_block == lower_block) return lower_block;
            // if range was surpassed, means that 'lower_block' is not part of the 'upper_block' chain
            // and so far this is not allowed.
            assert(last_block->nHeight >= lower_block->nHeight);
        }
        last_block = last_block->pprev;
    }
    assert(last_block != nullptr);
    return last_block;
}

bool BlockManager::CheckBlockDataAvailability(const CBlockIndex& upper_block, const CBlockIndex& lower_block)
{
    if (!(upper_block.nStatus & BLOCK_HAVE_DATA)) return false;
    return GetFirstStoredBlock(upper_block, &lower_block) == &lower_block;
}

// If we're using -prune with -reindex, then delete block files that will be ignored by the
// reindex.  Since reindexing works by starting at block file 0 and looping until a blockfile
// is missing, do the same here to delete any later block files after a gap.  Also delete all
// rev files since they'll be rewritten by the reindex anyway.  This ensures that m_blockfile_info
// is in sync with what's actually on disk by the time we start downloading, so that pruning
// works correctly.
void BlockManager::CleanupBlockRevFiles() const
{
    std::map<std::string, fs::path> mapBlockFiles;

    // Glob all blk?????.dat and rev?????.dat files from the blocks directory.
    // Remove the rev files immediately and insert the blk file paths into an
    // ordered map keyed by block file index.
    LogPrintf("Removing unusable blk?????.dat and rev?????.dat files for -reindex with -prune\n");
    for (fs::directory_iterator it(m_opts.blocks_dir); it != fs::directory_iterator(); it++) {
        const std::string path = fs::PathToString(it->path().filename());
        if (fs::is_regular_file(*it) &&
            path.length() == 12 &&
            path.substr(8,4) == ".dat")
        {
            if (path.substr(0, 3) == "blk") {
                mapBlockFiles[path.substr(3, 5)] = it->path();
            } else if (path.substr(0, 3) == "rev") {
                remove(it->path());
            }
        }
    }

    // Remove all block files that aren't part of a contiguous set starting at
    // zero by walking the ordered map (keys are block file indices) by
    // keeping a separate counter.  Once we hit a gap (or if 0 doesn't exist)
    // start removing block files.
    int nContigCounter = 0;
    for (const std::pair<const std::string, fs::path>& item : mapBlockFiles) {
        if (LocaleIndependentAtoi<int>(item.first) == nContigCounter) {
            nContigCounter++;
            continue;
        }
        remove(item.second);
    }
}

CBlockFileInfo* BlockManager::GetBlockFileInfo(size_t n)
{
    LOCK(cs_LastBlockFile);

    return &m_blockfile_info.at(n);
}

bool BlockManager::UndoWriteToDisk(const CBlockUndo& blockundo, FlatFilePos& pos, const uint256& hashBlock) const
{
    // Open history file to append
    CAutoFile fileout{OpenUndoFile(pos)};
    if (fileout.IsNull()) {
        return error("%s: OpenUndoFile failed", __func__);
    }

    // Write index header
    unsigned int nSize = GetSerializeSize(blockundo, CLIENT_VERSION);
    fileout << GetParams().MessageStart() << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0) {
        return error("%s: ftell failed", __func__);
    }
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    // calculate & write checksum
    HashWriter hasher{};
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

bool BlockManager::UndoReadFromDisk(CBlockUndo& blockundo, const CBlockIndex& index) const
{
    const FlatFilePos pos{WITH_LOCK(::cs_main, return index.GetUndoPos())};

    if (pos.IsNull()) {
        return error("%s: no undo data available", __func__);
    }

    // Open history file to read
    CAutoFile filein{OpenUndoFile(pos, true)};
    if (filein.IsNull()) {
        return error("%s: OpenUndoFile failed", __func__);
    }

    // Read block
    uint256 hashChecksum;
    HashVerifier verifier{filein}; // Use HashVerifier as reserializing may lose data, c.f. commit d342424301013ec47dc146a4beb49d5c9319d80a
    try {
        verifier << index.pprev->GetBlockHash();
        verifier >> blockundo;
        filein >> hashChecksum;
    } catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    if (hashChecksum != verifier.GetHash()) {
        return error("%s: Checksum mismatch", __func__);
    }

    return true;
}

bool BlockManager::FlushUndoFile(int block_file, bool finalize)
{
    FlatFilePos undo_pos_old(block_file, m_blockfile_info[block_file].nUndoSize);
    if (!UndoFileSeq().Flush(undo_pos_old, finalize)) {
        m_opts.notifications.flushError("Flushing undo file to disk failed. This is likely the result of an I/O error.");
        return false;
    }
    return true;
}

bool BlockManager::FlushBlockFile(int blockfile_num, bool fFinalize, bool finalize_undo)
{
    bool success = true;
    LOCK(cs_LastBlockFile);

    if (m_blockfile_info.size() < 1) {
        // Return if we haven't loaded any blockfiles yet. This happens during
        // chainstate init, when we call ChainstateManager::MaybeRebalanceCaches() (which
        // then calls FlushStateToDisk()), resulting in a call to this function before we
        // have populated `m_blockfile_info` via LoadBlockIndexDB().
        return true;
    }
    assert(static_cast<int>(m_blockfile_info.size()) > blockfile_num);

    FlatFilePos block_pos_old(blockfile_num, m_blockfile_info[blockfile_num].nSize);
    if (!BlockFileSeq().Flush(block_pos_old, fFinalize)) {
        m_opts.notifications.flushError("Flushing block file to disk failed. This is likely the result of an I/O error.");
        success = false;
    }
    // we do not always flush the undo file, as the chain tip may be lagging behind the incoming blocks,
    // e.g. during IBD or a sync after a node going offline
    if (!fFinalize || finalize_undo) {
        if (!FlushUndoFile(blockfile_num, finalize_undo)) {
            success = false;
        }
    }
    return success;
}

BlockfileType BlockManager::BlockfileTypeForHeight(int height)
{
    if (!m_snapshot_height) {
        return BlockfileType::NORMAL;
    }
    return (height >= *m_snapshot_height) ? BlockfileType::ASSUMED : BlockfileType::NORMAL;
}

bool BlockManager::FlushChainstateBlockFile(int tip_height)
{
    LOCK(cs_LastBlockFile);
    auto& cursor = m_blockfile_cursors[BlockfileTypeForHeight(tip_height)];
    // If the cursor does not exist, it means an assumeutxo snapshot is loaded,
    // but no blocks past the snapshot height have been written yet, so there
    // is no data associated with the chainstate, and it is safe not to flush.
    if (cursor) {
        return FlushBlockFile(cursor->file_num, /*fFinalize=*/false, /*finalize_undo=*/false);
    }
    // No need to log warnings in this case.
    return true;
}

uint64_t BlockManager::CalculateCurrentUsage()
{
    LOCK(cs_LastBlockFile);

    uint64_t retval = 0;
    for (const CBlockFileInfo& file : m_blockfile_info) {
        retval += file.nSize + file.nUndoSize;
    }
    return retval;
}

void BlockManager::UnlinkPrunedFiles(const std::set<int>& setFilesToPrune) const
{
    std::error_code ec;
    for (std::set<int>::iterator it = setFilesToPrune.begin(); it != setFilesToPrune.end(); ++it) {
        FlatFilePos pos(*it, 0);
        const bool removed_blockfile{fs::remove(BlockFileSeq().FileName(pos), ec)};
        const bool removed_undofile{fs::remove(UndoFileSeq().FileName(pos), ec)};
        if (removed_blockfile || removed_undofile) {
            LogPrint(BCLog::BLOCKSTORAGE, "Prune: %s deleted blk/rev (%05u)\n", __func__, *it);
        }
    }
}

FlatFileSeq BlockManager::BlockFileSeq() const
{
    return FlatFileSeq(m_opts.blocks_dir, "blk", m_opts.fast_prune ? 0x4000 /* 16kb */ : BLOCKFILE_CHUNK_SIZE);
}

FlatFileSeq BlockManager::UndoFileSeq() const
{
    return FlatFileSeq(m_opts.blocks_dir, "rev", UNDOFILE_CHUNK_SIZE);
}

CAutoFile BlockManager::OpenBlockFile(const FlatFilePos& pos, bool fReadOnly) const
{
    return CAutoFile{BlockFileSeq().Open(pos, fReadOnly), CLIENT_VERSION};
}

/** Open an undo file (rev?????.dat) */
CAutoFile BlockManager::OpenUndoFile(const FlatFilePos& pos, bool fReadOnly) const
{
    return CAutoFile{UndoFileSeq().Open(pos, fReadOnly), CLIENT_VERSION};
}

fs::path BlockManager::GetBlockPosFilename(const FlatFilePos& pos) const
{
    return BlockFileSeq().FileName(pos);
}

bool BlockManager::FindBlockPos(FlatFilePos& pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown)
{
    LOCK(cs_LastBlockFile);

    const BlockfileType chain_type = BlockfileTypeForHeight(nHeight);

    if (!m_blockfile_cursors[chain_type]) {
        // If a snapshot is loaded during runtime, we may not have initialized this cursor yet.
        assert(chain_type == BlockfileType::ASSUMED);
        const auto new_cursor = BlockfileCursor{this->MaxBlockfileNum() + 1};
        m_blockfile_cursors[chain_type] = new_cursor;
        LogPrint(BCLog::BLOCKSTORAGE, "[%s] initializing blockfile cursor to %s\n", chain_type, new_cursor);
    }
    const int last_blockfile = m_blockfile_cursors[chain_type]->file_num;

    int nFile = fKnown ? pos.nFile : last_blockfile;
    if (static_cast<int>(m_blockfile_info.size()) <= nFile) {
        m_blockfile_info.resize(nFile + 1);
    }

    bool finalize_undo = false;
    if (!fKnown) {
        unsigned int max_blockfile_size{MAX_BLOCKFILE_SIZE};
        // Use smaller blockfiles in test-only -fastprune mode - but avoid
        // the possibility of having a block not fit into the block file.
        if (m_opts.fast_prune) {
            max_blockfile_size = 0x10000; // 64kiB
            if (nAddSize >= max_blockfile_size) {
                // dynamically adjust the blockfile size to be larger than the added size
                max_blockfile_size = nAddSize + 1;
            }
        }
        assert(nAddSize < max_blockfile_size);

        while (m_blockfile_info[nFile].nSize + nAddSize >= max_blockfile_size) {
            // when the undo file is keeping up with the block file, we want to flush it explicitly
            // when it is lagging behind (more blocks arrive than are being connected), we let the
            // undo block write case handle it
            finalize_undo = (static_cast<int>(m_blockfile_info[nFile].nHeightLast) ==
                    Assert(m_blockfile_cursors[chain_type])->undo_height);

            // Try the next unclaimed blockfile number
            nFile = this->MaxBlockfileNum() + 1;
            // Set to increment MaxBlockfileNum() for next iteration
            m_blockfile_cursors[chain_type] = BlockfileCursor{nFile};

            if (static_cast<int>(m_blockfile_info.size()) <= nFile) {
                m_blockfile_info.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = m_blockfile_info[nFile].nSize;
    }

    if (nFile != last_blockfile) {
        if (!fKnown) {
            LogPrint(BCLog::BLOCKSTORAGE, "Leaving block file %i: %s (onto %i) (height %i)\n",
                last_blockfile, m_blockfile_info[last_blockfile].ToString(), nFile, nHeight);
        }

        // Do not propagate the return code. The flush concerns a previous block
        // and undo file that has already been written to. If a flush fails
        // here, and we crash, there is no expected additional block data
        // inconsistency arising from the flush failure here. However, the undo
        // data may be inconsistent after a crash if the flush is called during
        // a reindex. A flush error might also leave some of the data files
        // untrimmed.
        if (!FlushBlockFile(last_blockfile, !fKnown, finalize_undo)) {
            LogPrintLevel(BCLog::BLOCKSTORAGE, BCLog::Level::Warning,
                          "Failed to flush previous block file %05i (finalize=%i, finalize_undo=%i) before opening new block file %05i\n",
                          last_blockfile, !fKnown, finalize_undo, nFile);
        }
        // No undo data yet in the new file, so reset our undo-height tracking.
        m_blockfile_cursors[chain_type] = BlockfileCursor{nFile};
    }

    m_blockfile_info[nFile].AddBlock(nHeight, nTime);
    if (fKnown) {
        m_blockfile_info[nFile].nSize = std::max(pos.nPos + nAddSize, m_blockfile_info[nFile].nSize);
    } else {
        m_blockfile_info[nFile].nSize += nAddSize;
    }

    if (!fKnown) {
        bool out_of_space;
        size_t bytes_allocated = BlockFileSeq().Allocate(pos, nAddSize, out_of_space);
        if (out_of_space) {
            m_opts.notifications.fatalError("Disk space is too low!", _("Disk space is too low!"));
            return false;
        }
        if (bytes_allocated != 0 && IsPruneMode()) {
            m_check_for_pruning = true;
        }
    }

    m_dirty_fileinfo.insert(nFile);
    return true;
}

bool BlockManager::FindUndoPos(BlockValidationState& state, int nFile, FlatFilePos& pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    pos.nPos = m_blockfile_info[nFile].nUndoSize;
    m_blockfile_info[nFile].nUndoSize += nAddSize;
    m_dirty_fileinfo.insert(nFile);

    bool out_of_space;
    size_t bytes_allocated = UndoFileSeq().Allocate(pos, nAddSize, out_of_space);
    if (out_of_space) {
        return FatalError(m_opts.notifications, state, "Disk space is too low!", _("Disk space is too low!"));
    }
    if (bytes_allocated != 0 && IsPruneMode()) {
        m_check_for_pruning = true;
    }

    return true;
}

bool BlockManager::WriteBlockToDisk(const CBlock& block, FlatFilePos& pos) const
{
    // Open history file to append
    CAutoFile fileout{OpenBlockFile(pos)};
    if (fileout.IsNull()) {
        return error("WriteBlockToDisk: OpenBlockFile failed");
    }

    // Write index header
    unsigned int nSize = GetSerializeSize(block, fileout.GetVersion());
    fileout << GetParams().MessageStart() << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0) {
        return error("WriteBlockToDisk: ftell failed");
    }
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool BlockManager::WriteUndoDataForBlock(const CBlockUndo& blockundo, BlockValidationState& state, CBlockIndex& block)
{
    AssertLockHeld(::cs_main);
    const BlockfileType type = BlockfileTypeForHeight(block.nHeight);
    auto& cursor = *Assert(WITH_LOCK(cs_LastBlockFile, return m_blockfile_cursors[type]));

    // Write undo information to disk
    if (block.GetUndoPos().IsNull()) {
        FlatFilePos _pos;
        if (!FindUndoPos(state, block.StorageFile(), _pos, ::GetSerializeSize(blockundo, CLIENT_VERSION) + 40)) {
            return error("ConnectBlock(): FindUndoPos failed");
        }
        if (!UndoWriteToDisk(blockundo, _pos, block.pprev->GetBlockHash())) {
            return FatalError(m_opts.notifications, state, "Failed to write undo data");
        }
        // rev files are written in block height order, whereas blk files are written as blocks come in (often out of order)
        // we want to flush the rev (undo) file once we've written the last block, which is indicated by the last height
        // in the block file info as below; note that this does not catch the case where the undo writes are keeping up
        // with the block writes (usually when a synced up node is getting newly mined blocks) -- this case is caught in
        // the FindBlockPos function
        if (_pos.nFile < cursor.file_num && static_cast<uint32_t>(block.nHeight) == m_blockfile_info[_pos.nFile].nHeightLast) {
            // Do not propagate the return code, a failed flush here should not
            // be an indication for a failed write. If it were propagated here,
            // the caller would assume the undo data not to be written, when in
            // fact it is. Note though, that a failed flush might leave the data
            // file untrimmed.
            if (!FlushUndoFile(_pos.nFile, true)) {
                LogPrintLevel(BCLog::BLOCKSTORAGE, BCLog::Level::Warning, "Failed to flush undo file %05i\n", _pos.nFile);
            }
        } else if (_pos.nFile == cursor.file_num && block.nHeight > cursor.undo_height) {
            cursor.undo_height = block.nHeight;
        }
        // update nUndoPos in block index
        if (m_dirty_blockindex.insert(&block).second) {
            m_block_index.PinPayload(block);
        }
        block.UndoPos() = _pos.nPos;
        block.nStatus |= BLOCK_HAVE_UNDO;
    }

    return true;
}

bool BlockManager::ReadBlockFromDisk(CBlock& block, const FlatFilePos& pos) const
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein{OpenBlockFile(pos, true)};
    if (filein.IsNull()) {
        return error("ReadBlockFromDisk: OpenBlockFile failed for %s", pos.ToString());
    }

    // Read block
    try {
        filein >> block;
    } catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s at %s", __func__, e.what(), pos.ToString());
    }

    // Check the header
    if (!CheckProofOfWork(block.GetPoWAlgoHash(GetConsensus()), block.nBits, GetConsensus())) {
        return error("ReadBlockFromDisk: Errors in block header at %s", pos.ToString());
    }

    // Signet only: check block solution
    if (GetConsensus().signet_blocks && !CheckSignetBlockSolution(block, GetConsensus())) {
        return error("ReadBlockFromDisk: Errors in block solution at %s", pos.ToString());
    }

    return true;
}

bool BlockManager::ReadBlockFromDisk(CBlock& block, const CBlockIndex& index) const
{
    const FlatFilePos block_pos{WITH_LOCK(cs_main, return index.GetBlockPos())};

    if (!ReadBlockFromDisk(block, block_pos)) {
        return false;
    }
    if (block.GetHash() != index.GetBlockHash()) {
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*): GetHash() doesn't match index for %s at %s",
                     index.ToString(), block_pos.ToString());
    }
    return true;
}

bool BlockManager::ReadRawBlockFromDisk(std::vector<uint8_t>& block, const FlatFilePos& pos) const
{
    FlatFilePos hpos = pos;
    hpos.nPos -= 8; // Seek back 8 bytes for meta header
    CAutoFile filein{OpenBlockFile(hpos, true)};
    if (filein.IsNull()) {
        return error("%s: OpenBlockFile failed for %s", __func__, pos.ToString());
    }

    try {
        MessageStartChars blk_start;
        unsigned int blk_size;

        filein >> blk_start >> blk_size;

        if (blk_start != GetParams().MessageStart()) {
            return error("%s: Block magic mismatch for %s: %s versus expected %s", __func__, pos.ToString(),
                         HexStr(blk_start),
                         HexStr(GetParams().MessageStart()));
        }

        if (blk_size > MAX_SIZE) {
            return error("%s: Block data is larger than maximum deserialization size for %s: %s versus %s", __func__, pos.ToString(),
                         blk_size, MAX_SIZE);
        }

        block.resize(blk_size); // Zeroing of memory is intentional here
        filein.read(MakeWritableByteSpan(block));
    } catch (const std::exception& e) {
        return error("%s: Read from block file failed: %s for %s", __func__, e.what(), pos.ToString());
    }

    return true;
}

FlatFilePos BlockManager::SaveBlockToDisk(const CBlock& block, int nHeight, const FlatFilePos* dbp)
{
    unsigned int nBlockSize = ::GetSerializeSize(block, CLIENT_VERSION);
    FlatFilePos blockPos;
    const auto position_known {dbp != nullptr};
    if (position_known) {
        blockPos = *dbp;
    } else {
        // when known, blockPos.nPos points at the offset of the block data in the blk file. that already accounts for
        // the serialization header present in the file (the 4 magic message start bytes + the 4 length bytes = 8 bytes = BLOCK_SERIALIZATION_HEADER_SIZE).
        // we add BLOCK_SERIALIZATION_HEADER_SIZE only for new blocks since they will have the serialization header added when written to disk.
        nBlockSize += static_cast<unsigned int>(BLOCK_SERIALIZATION_HEADER_SIZE);
    }
    if (!FindBlockPos(blockPos, nBlockSize, nHeight, block.GetBlockTime(), position_known)) {
        error("%s: FindBlockPos failed", __func__);
        return FlatFilePos();
    }
    if (!position_known) {
        if (!WriteBlockToDisk(block, blockPos)) {
            m_opts.notifications.fatalError("Failed to write block");
            return FlatFilePos();
        }
    }
    return blockPos;
}

class ImportingNow
{
    std::atomic<bool>& m_importing;

public:
    ImportingNow(std::atomic<bool>& importing) : m_importing{importing}
    {
        assert(m_importing == false);
        m_importing = true;
    }
    ~ImportingNow()
    {
        assert(m_importing == true);
        m_importing = false;
    }
};

void ImportBlocks(ChainstateManager& chainman, std::vector<fs::path> vImportFiles)
{
    ScheduleBatchPriority();

    {
        ImportingNow imp{chainman.m_blockman.m_importing};

        // -reindex
        if (fReindex) {
            int nFile = 0;
            // Map of disk positions for blocks with unknown parent (only used for reindex);
            // parent hash -> child disk position, multiple children can have the same parent.
            std::multimap<uint256, FlatFilePos> blocks_with_unknown_parent;
            while (true) {
                FlatFilePos pos(nFile, 0);
                if (!fs::exists(chainman.m_blockman.GetBlockPosFilename(pos))) {
                    break; // No block files left to reindex
                }
                CAutoFile file{chainman.m_blockman.OpenBlockFile(pos, true)};
                if (file.IsNull()) {
                    break; // This error is logged in OpenBlockFile
                }
                LogPrintf("Reindexing block file blk%05u.dat...\n", (unsigned int)nFile);
                chainman.LoadExternalBlockFile(file, &pos, &blocks_with_unknown_parent);
                if (chainman.m_interrupt) {
                    LogPrintf("Interrupt requested. Exit %s\n", __func__);
                    return;
                }
                nFile++;
            }
            WITH_LOCK(::cs_main, chainman.m_blockman.m_block_tree_db->WriteReindexing(false));
            fReindex = false;
            LogPrintf("Reindexing finished\n");
            // To avoid ending up in a situation without genesis block, re-try initializing (no-op if reindexing worked):
            chainman.ActiveChainstate().LoadGenesisBlock();
        }

        // -loadblock=
        for (const fs::path& path : vImportFiles) {
            CAutoFile file{fsbridge::fopen(path, "rb"), CLIENT_VERSION};
            if (!file.IsNull()) {
                LogPrintf("Importing blocks file %s...\n", fs::PathToString(path));
                chainman.LoadExternalBlockFile(file);
                if (chainman.m_interrupt) {
                    LogPrintf("Interrupt requested. Exit %s\n", __func__);
                    return;
                }
            } else {
                LogPrintf("Warning: Could not open blocks file %s\n", fs::PathToString(path));
            }
        }

        // scan for better chains in the block chain database, that are not yet connected in the active best chain

        // We can't hold cs_main during ActivateBestChain even though we're accessing
        // the chainman unique_ptrs since ABC requires us not to be holding cs_main, so retrieve
        // the relevant pointers before the ABC call.
        for (Chainstate* chainstate : WITH_LOCK(::cs_main, return chainman.GetAll())) {
            BlockValidationState state;
            if (!chainstate->ActivateBestChain(state, nullptr)) {
                chainman.GetNotifications().fatalError(strprintf("Failed to connect best block (%s)", state.ToString()));
                return;
            }
        }
    } // End scope of ImportingNow
}

std::ostream& operator<<(std::ostream& os, const BlockfileType& type) {
    switch(type) {
        case BlockfileType::NORMAL: os << "normal"; break;
        case BlockfileType::ASSUMED: os << "assumed"; break;
        default: os.setstate(std::ios_base::failbit);
    }
    return os;
}

std::ostream& operator<<(std::ostream& os, const BlockfileCursor& cursor) {
    os << strprintf("BlockfileCursor(file_num=%d, undo_height=%d)", cursor.file_num, cursor.undo_height);
    return os;
}
} // namespace node
