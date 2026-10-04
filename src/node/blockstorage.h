// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_NODE_BLOCKSTORAGE_H
#define DIGIBYTE_NODE_BLOCKSTORAGE_H

#include <attributes.h>
#include <chain.h>
#include <dbwrapper.h>
#include <kernel/blockmanager_opts.h>
#include <node/blockindex_compact_delta.h>
#include <node/blockindex_compact_delta_log.h>
#include <node/blockindex_compact_ids.h>
#include <node/blockindex_compact_lookup.h>
#include <node/blockindex_compact_store.h>
#include <kernel/chain.h>
#include <kernel/chainparams.h>
#include <kernel/cs_main.h>
#include <kernel/messagestartchars.h>
#include <sync.h>
#include <util/fs.h>
#include <util/hasher.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class BlockValidationState;
class CAutoFile;
class CBlock;
class CBlockUndo;
class CChainParams;
class Chainstate;
class ChainstateManager;
struct CCheckpointData;
struct FlatFilePos;
namespace Consensus {
struct Params;
}
namespace util {
class SignalInterrupt;
} // namespace util

namespace kernel {
/** Access to the block database (blocks/index/) */
class BlockTreeDB : public CDBWrapper
{
public:
    using CDBWrapper::CDBWrapper;
    bool WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*>>& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo);
    bool WriteBlockIndexBatch(const std::vector<const CBlockIndex*>& blockinfo, bool sync);
    bool ReadBlockFileInfo(int nFile, CBlockFileInfo& info);
    bool ReadLastBlockFile(int& nFile);
    bool WriteReindexing(bool fReindexing);
    void ReadReindexing(bool& fReindexing);
    bool WriteFlag(const std::string& name, bool fValue);
    bool ReadFlag(const std::string& name, bool& fValue);
    using BlockIndexLoader = std::function<CBlockIndex*(const CDiskBlockIndex&)>;
    bool LoadBlockIndexGuts(const Consensus::Params& consensusParams, BlockIndexLoader loadBlockIndex, const util::SignalInterrupt& interrupt)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
};
} // namespace kernel

namespace node {
using kernel::BlockTreeDB;

/** The pre-allocation chunk size for blk?????.dat files (since 0.8) */
static const unsigned int BLOCKFILE_CHUNK_SIZE = 0x1000000; // 16 MiB
/** The pre-allocation chunk size for rev?????.dat files (since 0.8) */
static const unsigned int UNDOFILE_CHUNK_SIZE = 0x100000; // 1 MiB
/** The maximum size of a blk?????.dat file (since 0.8) */
static const unsigned int MAX_BLOCKFILE_SIZE = 0x8000000; // 128 MiB

/** Size of header written by WriteBlockToDisk before a serialized CBlock */
static constexpr size_t BLOCK_SERIALIZATION_HEADER_SIZE = std::tuple_size_v<MessageStartChars> + sizeof(unsigned int);

extern std::atomic_bool fReindex;

// Because validation code takes pointers to the map's CBlockIndex objects, if
// we ever switch to another associative container, we need to either use a
// container that has stable addressing (true of all std associative
// containers), or make the key a `std::unique_ptr<CBlockIndex>`
using BlockMap = std::unordered_map<uint256, CBlockIndex, BlockHasher>;

using kernel::BlockIndexResidencyMode;

struct BlockIndexResidencyStats {
    uint64_t lookups{0};
    uint64_t lookup_hits{0};
    uint64_t lookup_misses{0};
    uint64_t insertions{0};
    uint64_t no_io_scopes{0};
    uint64_t backing_reads{0};
    uint64_t no_io_violations{0};
    uint64_t payload_cache_hits{0};
    uint64_t payload_cache_misses{0};
    uint64_t payload_cache_evictions{0};
    uint64_t payloads_created{0};
    uint64_t algo_payloads_created{0};
    uint64_t algo_prewarm_blocks{0};
};

/**
 * Stable owner for block-index identity objects.
 *
 * The first residency implementation deliberately keeps every CBlockIndex
 * object resident, preserving the existing pointer/lifetime contract. Future
 * modes can move selected payload domains behind this store without changing
 * callers that only need stable CBlockIndex identity.
 *
 * All access is currently protected by cs_main through BlockManager.
 */
class BlockIndexStore : public BlockIndexPayloadProvider
{
    template <typename T, size_t CHUNK_ELEMENTS = 65536>
    class StableArena
    {
    public:
        T& emplace_back()
        {
            if (m_size % CHUNK_ELEMENTS == 0) {
                m_chunks.push_back(std::make_unique<T[]>(CHUNK_ELEMENTS));
            }
            T& value{m_chunks.back()[m_size % CHUNK_ELEMENTS]};
            ++m_size;
            return value;
        }

        T& back()
        {
            assert(m_size > 0);
            return m_chunks.back()[(m_size - 1) % CHUNK_ELEMENTS];
        }

        T& at(size_t index)
        {
            assert(index < m_size);
            return m_chunks[index / CHUNK_ELEMENTS][index % CHUNK_ELEMENTS];
        }

        const T& at(size_t index) const
        {
            assert(index < m_size);
            return m_chunks[index / CHUNK_ELEMENTS][index % CHUNK_ELEMENTS];
        }

        [[nodiscard]] size_t size() const noexcept { return m_size; }

        void clear()
        {
            m_chunks.clear();
            m_size = 0;
        }

    private:
        std::vector<std::unique_ptr<T[]>> m_chunks;
        size_t m_size{0};
    };

public:
    using PayloadLoader = std::function<bool(const CBlockIndex&, BlockIndexResidentPayload&)>;

    explicit BlockIndexStore(
        BlockIndexResidencyMode mode = BlockIndexResidencyMode::FULL,
        size_t hot_depth = kernel::DEFAULT_BLOCK_INDEX_HOT_DEPTH,
        size_t cache_bytes = kernel::DEFAULT_BLOCK_INDEX_CACHE_MIB_BALANCED * 1024 * 1024)
        : m_mode{mode}, m_hot_depth{hot_depth}, m_cache_limit_bytes{cache_bytes}
    {
    }

    using iterator = BlockMap::iterator;
    using const_iterator = BlockMap::const_iterator;
    using size_type = BlockMap::size_type;

    class PayloadPinGuard
    {
    public:
        PayloadPinGuard(BlockIndexStore& store, CBlockIndex& index)
            : m_store{&store}, m_index{&index}
        {
            m_store->PinPayload(index);
        }
        PayloadPinGuard(const PayloadPinGuard&) = delete;
        PayloadPinGuard& operator=(const PayloadPinGuard&) = delete;
        PayloadPinGuard(PayloadPinGuard&& other) noexcept
            : m_store{std::exchange(other.m_store, nullptr)},
              m_index{std::exchange(other.m_index, nullptr)}
        {
        }
        PayloadPinGuard& operator=(PayloadPinGuard&&) = delete;
        ~PayloadPinGuard()
        {
            if (m_store && m_index) m_store->ReleasePayloadPin(*m_index);
        }
    private:
        BlockIndexStore* m_store;
        CBlockIndex* m_index;
    };

    class NoIOGuard
    {
    public:
        explicit NoIOGuard(BlockIndexStore& store) : m_store{&store}
        {
            ++m_store->m_no_io_depth;
            ++m_store->m_stats.no_io_scopes;
        }

        NoIOGuard(const NoIOGuard&) = delete;
        NoIOGuard& operator=(const NoIOGuard&) = delete;

        NoIOGuard(NoIOGuard&& other) noexcept : m_store{std::exchange(other.m_store, nullptr)} {}
        NoIOGuard& operator=(NoIOGuard&&) = delete;

        ~NoIOGuard()
        {
            if (!m_store) return;
            assert(m_store->m_no_io_depth > 0);
            --m_store->m_no_io_depth;
        }

    private:
        BlockIndexStore* m_store;
    };

    iterator begin() noexcept { return m_entries.begin(); }
    const_iterator begin() const noexcept { return m_entries.begin(); }
    const_iterator cbegin() const noexcept { return m_entries.cbegin(); }
    iterator end() noexcept { return m_entries.end(); }
    const_iterator end() const noexcept { return m_entries.end(); }
    const_iterator cend() const noexcept { return m_entries.cend(); }

    [[nodiscard]] bool empty() const noexcept
    {
        return m_compact_identity_active ? m_compact_identities.size() == 0 : m_entries.empty();
    }

    [[nodiscard]] size_type size() const noexcept
    {
        return m_compact_identity_active ? m_compact_identities.size() : m_entries.size();
    }

    iterator find(const uint256& hash)
    {
        auto it{m_entries.find(hash)};
        NoteLookup(it != m_entries.end());
        return it;
    }

    const_iterator find(const uint256& hash) const
    {
        auto it{m_entries.find(hash)};
        NoteLookup(it != m_entries.end());
        return it;
    }

    size_type count(const uint256& hash) const
    {
        const bool found{m_entries.find(hash) != m_entries.end()};
        NoteLookup(found);
        return found ? 1 : 0;
    }

    std::pair<iterator, bool> try_emplace(const uint256& hash)
    {
        auto result{m_entries.try_emplace(hash)};
        if (result.second) {
            ++m_stats.insertions;
            InitializeStoreEntry(result.first->second);
        }
        return result;
    }

    std::pair<iterator, bool> try_emplace(const uint256& hash, const CBlockHeader& block)
    {
        auto result{m_entries.try_emplace(hash)};
        if (result.second) {
            ++m_stats.insertions;
            CBlockIndex& index{result.first->second};
            InitializeStoreEntry(index);
            index.nVersion = block.nVersion;
            index.nTime = block.nTime;
            index.nBits = block.nBits;
            index.nNonce = block.nNonce;
            MaterializeBlockIndexPayload(index).hashMerkleRoot = block.hashMerkleRoot;
        }
        return result;
    }

    CBlockIndex& operator[](const uint256& hash)
    {
        auto [it, inserted]{m_entries.try_emplace(hash)};
        if (inserted) {
            ++m_stats.insertions;
            NoteLookup(false);
            InitializeStoreEntry(it->second);
        } else {
            NoteLookup(true);
        }
        return it->second;
    }

    // Pointer-oriented identity API used by production code. Keeping callers
    // off unordered_map iterators allows BALANCED/LOWMEM to change identity
    // ownership without changing validation semantics.
    CBlockIndex* Lookup(const uint256& hash)
    {
        if (m_compact_identity_active) {
            if (auto it{m_live_identity_index.find(hash)}; it != m_live_identity_index.end()) {
                NoteLookup(true);
                return it->second;
            }
            if (auto it{m_hot_identity_index.find(hash)}; it != m_hot_identity_index.end()) {
                NoteLookup(true);
                return it->second;
            }
            NoteLookup(false);
            return nullptr;
        }

        auto it{find(hash)};
        return it == end() ? nullptr : &it->second;
    }

    const CBlockIndex* Lookup(const uint256& hash) const
    {
        if (m_compact_identity_active) {
            if (auto it{m_live_identity_index.find(hash)}; it != m_live_identity_index.end()) {
                NoteLookup(true);
                return it->second;
            }
            if (auto it{m_hot_identity_index.find(hash)}; it != m_hot_identity_index.end()) {
                NoteLookup(true);
                return it->second;
            }
            NoteLookup(false);
            return nullptr;
        }

        auto it{find(hash)};
        return it == end() ? nullptr : &it->second;
    }

    [[nodiscard]] bool Contains(const uint256& hash) const
    {
        return Lookup(hash) != nullptr;
    }

    std::pair<CBlockIndex*, bool> Insert(const uint256& hash)
    {
        if (!m_compact_identity_active) {
            auto [it, inserted]{try_emplace(hash)};
            if (inserted) it->second.phashBlock = &it->first;
            return {&it->second, inserted};
        }

        if (CBlockIndex* existing{Lookup(hash)}) return {existing, false};

        CBlockIndex& index{m_compact_identities.emplace_back()};
        InitializeStoreEntry(index);
        auto [it, inserted]{m_live_identity_index.emplace(hash, &index)};
        assert(inserted);
        index.phashBlock = &it->first;
        ++m_stats.insertions;
        return {&index, true};
    }

    std::pair<CBlockIndex*, bool> Insert(const uint256& hash, const CBlockHeader& block)
    {
        auto [index, inserted]{Insert(hash)};
        if (inserted) {
            index->nVersion = block.nVersion;
            index->nTime = block.nTime;
            index->nBits = block.nBits;
            index->nNonce = block.nNonce;
            MaterializeBlockIndexPayload(*index).hashMerkleRoot = block.hashMerkleRoot;
        }
        return {index, inserted};
    }

    template <typename Fn>
    void ForEach(Fn&& fn)
    {
        if (m_compact_identity_active) {
            for (size_t i = 0; i < m_compact_identities.size(); ++i) {
                fn(m_compact_identities.at(i));
            }
            return;
        }
        for (auto& [_, index] : m_entries) fn(index);
    }

    template <typename Fn>
    void ForEach(Fn&& fn) const
    {
        if (m_compact_identity_active) {
            for (size_t i = 0; i < m_compact_identities.size(); ++i) {
                fn(m_compact_identities.at(i));
            }
            return;
        }
        for (const auto& [_, index] : m_entries) fn(index);
    }

    using IdentityHashResolver = std::function<const uint256*(BlockIndexId)>;
    using DirectTailIdentity = std::pair<BlockIndexId, uint256>;

    /**
     * Initialize BALANCED/LOWMEM identity storage directly in compact-id order.
     *
     * Unlike PrepareCompactIdentityCutover(), this path starts from an empty
     * legacy map and deliberately creates no bootstrap payload objects. The
     * canonical LevelDB stream fills the shell fields afterward.
     */
    bool PrepareDirectCompactIdentity(
        size_t total_count,
        size_t immutable_count,
        const IdentityHashResolver& immutable_hash,
        const std::vector<DirectTailIdentity>& tail)
    {
        if (m_mode == BlockIndexResidencyMode::FULL ||
            m_compact_identity_active ||
            m_compact_identity_prepared ||
            !m_entries.empty() ||
            !immutable_hash ||
            immutable_count > total_count ||
            tail.size() != total_count - immutable_count) {
            return false;
        }

        m_compact_identities.clear();
        m_live_identity_index.clear();
        m_hot_identity_index.clear();
        m_live_identity_index.reserve(std::max<size_t>(16, tail.size() * 2));

        const auto abort = [this]() {
            m_compact_identities.clear();
            m_live_identity_index.clear();
            m_hot_identity_index.clear();
            m_immutable_identity_count = 0;
            m_direct_identity_bootstrap = false;
            m_compact_identity_active = false;
        };

        for (size_t raw_id = 0; raw_id < total_count; ++raw_id) {
            const auto id{static_cast<BlockIndexId>(raw_id)};
            CBlockIndex& index{m_compact_identities.emplace_back()};
            index.SetPayloadProvider(this);
            index.m_compact_id = id;

            // -1 is an impossible loaded height and doubles as the canonical
            // LevelDB "seen" marker during direct bootstrap.
            index.nHeight = -1;

            if (raw_id < immutable_count) {
                const uint256* hash{immutable_hash(id)};
                if (!hash) {
                    abort();
                    return false;
                }
                index.phashBlock = hash;
            } else {
                const DirectTailIdentity& identity{tail[raw_id - immutable_count]};
                if (identity.first != id) {
                    abort();
                    return false;
                }
                auto [it, inserted]{
                    m_live_identity_index.emplace(identity.second, &index)};
                if (!inserted) {
                    abort();
                    return false;
                }
                index.phashBlock = &it->first;
            }
        }

        m_immutable_identity_count = immutable_count;
        m_direct_identity_bootstrap = true;
        m_compact_identity_active = true;
        m_stats.insertions += total_count;
        return true;
    }

    void ResetDirectCompactIdentity()
    {
        if (!m_direct_identity_bootstrap) return;
        m_compact_identities.clear();
        m_live_identity_index.clear();
        m_hot_identity_index.clear();
        m_immutable_identity_count = 0;
        m_direct_identity_bootstrap = false;
        m_compact_identity_active = false;
        m_payload_loader = {};
    }

    [[nodiscard]] bool DirectCompactBootstrapActive() const noexcept
    {
        return m_direct_identity_bootstrap;
    }

    bool PrepareCompactIdentityCutover(
        const std::vector<CBlockIndex*>& by_id,
        size_t immutable_count,
        const IdentityHashResolver& immutable_hash)
    {
        if (m_mode == BlockIndexResidencyMode::FULL ||
            m_compact_identity_active ||
            m_compact_identity_prepared ||
            !immutable_hash ||
            by_id.size() != m_entries.size() ||
            immutable_count > by_id.size()) {
            return false;
        }

        // BALANCED/LOWMEM bootstrap deliberately has no historical algo-history
        // accelerators yet. Those are rebuilt only for the hot window after the
        // payload cache cut-over, so no old CBlockIndex* may be hidden inside a
        // payload while identity pointers are being remapped.
        for (size_t id = 0; id < by_id.size(); ++id) {
            const CBlockIndex* source{by_id[id]};
            if (!source ||
                source->m_compact_id != id ||
                source->HasResidentAlgoHistory()) {
                return false;
            }

            if (source->pprev) {
                const BlockIndexId parent_id{source->pprev->m_compact_id};
                if (parent_id == INVALID_BLOCK_INDEX_ID ||
                    static_cast<size_t>(parent_id) >= by_id.size() ||
                    by_id[parent_id] != source->pprev) {
                    return false;
                }
            }
            if (source->pskip) {
                const BlockIndexId skip_id{source->pskip->m_compact_id};
                if (skip_id == INVALID_BLOCK_INDEX_ID ||
                    static_cast<size_t>(skip_id) >= by_id.size() ||
                    by_id[skip_id] != source->pskip) {
                    return false;
                }
            }
        }

        m_compact_identities.clear();
        m_live_identity_index.clear();
        m_hot_identity_index.clear();
        m_live_identity_index.reserve(
            std::max<size_t>(16, (by_id.size() - immutable_count) * 2));

        const auto abort_preparation = [this]() {
            m_compact_identities.clear();
            m_live_identity_index.clear();
            m_hot_identity_index.clear();
            m_immutable_identity_count = 0;
            m_compact_identity_prepared = false;
        };

        for (size_t raw_id = 0; raw_id < by_id.size(); ++raw_id) {
            const auto id{static_cast<BlockIndexId>(raw_id)};
            const CBlockIndex& source{*by_id[raw_id]};
            CBlockIndex& dest{m_compact_identities.emplace_back()};

            dest.nHeight = source.nHeight;
            dest.nChainWork = source.nChainWork;
            dest.nTx = source.nTx;
            dest.nChainTx = source.nChainTx;
            dest.nStatus = source.nStatus;
            dest.nVersion = source.nVersion;
            dest.nTime = source.nTime;
            dest.nBits = source.nBits;
            dest.nNonce = source.nNonce;
            dest.nSequenceId = source.nSequenceId;
            dest.m_compact_id = source.m_compact_id;
            dest.SetPayloadProvider(this);
            if (source.m_resident_payload) {
                dest.AttachResidentPayload(source.m_resident_payload);
            }

            if (raw_id < immutable_count) {
                const uint256* hash{immutable_hash(id)};
                if (!hash || *hash != source.GetBlockHash()) {
                    abort_preparation();
                    return false;
                }
                dest.phashBlock = hash;
            } else {
                auto [it, inserted]{
                    m_live_identity_index.emplace(source.GetBlockHash(), &dest)};
                if (!inserted) {
                    abort_preparation();
                    return false;
                }
                dest.phashBlock = &it->first;
            }
        }

        for (size_t raw_id = 0; raw_id < by_id.size(); ++raw_id) {
            const CBlockIndex& source{*by_id[raw_id]};
            CBlockIndex& dest{m_compact_identities.at(raw_id)};

            if (source.pprev) {
                const BlockIndexId parent_id{source.pprev->m_compact_id};
                assert(parent_id != INVALID_BLOCK_INDEX_ID);
                assert(static_cast<size_t>(parent_id) < m_compact_identities.size());
                dest.pprev = &m_compact_identities.at(parent_id);
            }
            if (source.pskip) {
                const BlockIndexId skip_id{source.pskip->m_compact_id};
                assert(skip_id != INVALID_BLOCK_INDEX_ID);
                assert(static_cast<size_t>(skip_id) < m_compact_identities.size());
                dest.pskip = &m_compact_identities.at(skip_id);
            }
        }

        m_immutable_identity_count = immutable_count;
        m_direct_identity_bootstrap = false;
        m_compact_identity_prepared = true;
        return true;
    }

    CBlockIndex* RemapPreparedIdentity(const CBlockIndex* index)
    {
        if (!index) return nullptr;
        assert(m_compact_identity_prepared);
        const BlockIndexId id{index->m_compact_id};
        assert(id != INVALID_BLOCK_INDEX_ID);
        assert(static_cast<size_t>(id) < m_compact_identities.size());
        return &m_compact_identities.at(id);
    }

    void CommitCompactIdentityCutover()
    {
        assert(m_compact_identity_prepared);
        BlockMap empty;
        m_entries.swap(empty);
        m_compact_identity_active = true;
        m_compact_identity_prepared = false;
    }

    [[nodiscard]] bool CompactIdentityActive() const noexcept
    {
        return m_compact_identity_active;
    }

    [[nodiscard]] size_t ImmutableIdentityCount() const noexcept
    {
        return m_immutable_identity_count;
    }

    CBlockIndex* ByCompactId(BlockIndexId id)
    {
        if (!m_compact_identity_active ||
            static_cast<size_t>(id) >= m_compact_identities.size()) {
            return nullptr;
        }
        return &m_compact_identities.at(id);
    }

    const CBlockIndex* ByCompactId(BlockIndexId id) const
    {
        if (!m_compact_identity_active ||
            static_cast<size_t>(id) >= m_compact_identities.size()) {
            return nullptr;
        }
        return &m_compact_identities.at(id);
    }

    BlockMap& RawMap() noexcept { return m_entries; }
    const BlockMap& RawMap() const noexcept { return m_entries; }

    void SetPayloadLoader(PayloadLoader loader)
    {
        m_payload_loader = std::move(loader);
    }

    [[nodiscard]] BlockIndexResidencyMode GetMode() const noexcept { return m_mode; }
    [[nodiscard]] size_t GetHotDepth() const noexcept { return m_hot_depth; }
    [[nodiscard]] BlockIndexResidencyStats GetResidencyStats() const noexcept { return m_stats; }
    [[nodiscard]] size_t ResidentPayloads() const noexcept
    {
        return m_bootstrap_payloads.size() + m_payload_cache_lru.size();
    }
    [[nodiscard]] size_t ResidentAlgoPayloads() const noexcept
    {
        return m_full_algo_payloads.size() + m_cached_algo_payloads.size();
    }
    [[nodiscard]] uint64_t ResidentPayloadBytes() const noexcept
    {
        return static_cast<uint64_t>(ResidentPayloads()) * sizeof(BlockIndexResidentPayload) +
               static_cast<uint64_t>(ResidentAlgoPayloads()) * sizeof(BlockIndexAlgoHistory);
    }
    [[nodiscard]] size_t CacheLimitBytes() const noexcept { return m_cache_limit_bytes; }

    static const char* ModeName(BlockIndexResidencyMode mode) noexcept
    {
        switch (mode) {
        case BlockIndexResidencyMode::FULL: return "full";
        case BlockIndexResidencyMode::BALANCED: return "balanced";
        case BlockIndexResidencyMode::LOWMEM: return "lowmem";
        }
        return "unknown";
    }

    BlockIndexResidentPayload& MaterializeBlockIndexPayload(CBlockIndex& index) override
    {
        if (index.m_resident_payload) {
            const auto cached{m_payload_cache_index.find(&index)};
            if (cached != m_payload_cache_index.end()) {
                m_payload_cache_lru.splice(
                    m_payload_cache_lru.begin(),
                    m_payload_cache_lru,
                    cached->second);
            }
            ++m_stats.payload_cache_hits;
            return *index.m_resident_payload;
        }

        ++m_stats.payload_cache_misses;

        // If a standalone/transient CBlockIndex is first materialized through
        // this store, make the ownership boundary explicit before attaching a
        // store-owned payload. Store-managed entries already point back here.
        index.SetPayloadProvider(this);

        if (!m_cache_active) {
            BlockIndexResidentPayload& payload{CreateEmptyPayload(index)};
            if (m_direct_identity_bootstrap) {
                if (!m_payload_loader) {
                    throw std::runtime_error("direct block-index bootstrap payload loader unavailable");
                }
                RecordBackingRead();
                if (!m_payload_loader(index, payload)) {
                    throw std::runtime_error("direct block-index bootstrap payload materialization failed");
                }
            }
            return payload;
        }

        if (!m_payload_loader) {
            throw std::runtime_error("block-index payload loader unavailable");
        }
        RecordBackingRead();

        BlockIndexResidentPayload payload;
        if (!m_payload_loader(index, payload)) {
            throw std::runtime_error("block-index payload materialization failed");
        }
        return InsertCachedPayload(index, std::move(payload));
    }

    BlockIndexResidentPayload& EnsureAlgoHistory(CBlockIndex& index)
    {
        auto& payload{MaterializeBlockIndexPayload(index)};
        if (payload.algo_history) return payload;

        BlockIndexAlgoHistory* history{nullptr};
        if (m_mode == BlockIndexResidencyMode::FULL) {
            m_full_algo_payloads.emplace_back();
            history = &m_full_algo_payloads.back();
        } else {
            auto [it, inserted]{m_cached_algo_payloads.try_emplace(&index)};
            assert(inserted);
            history = &it->second;
        }

        if (index.pprev && index.pprev->HasResidentAlgoHistory()) {
            history->last_algo_blocks =
                index.pprev->m_resident_payload->algo_history->last_algo_blocks;
        } else {
            history->last_algo_blocks.fill(nullptr);
        }
        const int algo{index.GetAlgo()};
        if (algo >= 0 && algo < NUM_ALGOS_IMPL) {
            history->last_algo_blocks[algo] = &index;
        }
        payload.algo_history = history;
        ++m_stats.algo_payloads_created;
        return payload;
    }

    void PinPayload(CBlockIndex& index)
    {
        MaterializeBlockIndexPayload(index);
        if (m_cache_active) ++m_payload_pins[&index];
    }

    void ReleasePayloadPin(CBlockIndex& index)
    {
        const auto it{m_payload_pins.find(&index)};
        if (it == m_payload_pins.end()) return;
        assert(it->second > 0);
        if (--it->second == 0) m_payload_pins.erase(it);
    }

    PayloadPinGuard PinPayloadScoped(CBlockIndex& index)
    {
        return PayloadPinGuard{*this, index};
    }

    void DisablePayloadEviction() noexcept
    {
        m_eviction_enabled = false;
    }

    [[nodiscard]] bool PayloadEvictionEnabled() const noexcept
    {
        return m_eviction_enabled;
    }

    void UpdateHotWindow(CBlockIndex* tip)
    {
        if (m_mode == BlockIndexResidencyMode::FULL || !m_cache_active) return;

        if (m_hot_depth == 0 || !tip) {
            for (CBlockIndex* index : m_hot_window) ReleasePayloadPin(*index);
            m_hot_window.clear();
            m_hot_identity_index.clear();
            return;
        }
        if (!m_hot_window.empty() && m_hot_window.back() == tip) {
            if (m_compact_identity_active && m_hot_identity_index.empty()) {
                RebuildHotIdentityIndex();
            }
            return;
        }

        if (!m_hot_window.empty() && tip->pprev == m_hot_window.back()) {
            PinPayload(*tip);
            m_hot_window.push_back(tip);
            if (m_compact_identity_active) {
                m_hot_identity_index[tip->GetBlockHash()] = tip;
            }
            while (m_hot_window.size() > m_hot_depth) {
                CBlockIndex* old{m_hot_window.front()};
                m_hot_window.pop_front();
                if (m_compact_identity_active) {
                    m_hot_identity_index.erase(old->GetBlockHash());
                }
                ReleasePayloadPin(*old);
            }
            return;
        }

        for (CBlockIndex* index : m_hot_window) ReleasePayloadPin(*index);
        m_hot_window.clear();

        std::vector<CBlockIndex*> desired;
        desired.reserve(std::min<size_t>(m_hot_depth, static_cast<size_t>(tip->nHeight) + 1));
        for (CBlockIndex* index{tip}; index && desired.size() < m_hot_depth; index = index->pprev) {
            desired.push_back(index);
        }
        for (auto it = desired.rbegin(); it != desired.rend(); ++it) {
            PinPayload(**it);
            m_hot_window.push_back(*it);
        }
        RebuildHotIdentityIndex();

        // A cold historical branch can become active after its cached algo
        // accelerators have been reclaimed. Rebuild only on this uncommon
        // reorg/jump path, oldest to newest, so normal one-block growth stays
        // constant-time.
        for (CBlockIndex* index : m_hot_window) {
            if (!index->HasResidentAlgoHistory()) {
                EnsureAlgoHistory(*index);
            }
        }
    }

    bool ActivatePayloadCache(CBlockIndex* tip, PayloadLoader loader)
    {
        if (m_mode == BlockIndexResidencyMode::FULL) {
            m_payload_loader = std::move(loader);
            return true;
        }
        if (!tip || !loader) return false;
        if (m_cache_active) return true;

        m_payload_loader = std::move(loader);

        std::vector<std::pair<CBlockIndex*, BlockIndexResidentPayload>> hot;
        hot.reserve(std::min<size_t>(m_hot_depth, static_cast<size_t>(tip->nHeight) + 1));
        for (CBlockIndex* index{tip}; index && hot.size() < m_hot_depth; index = index->pprev) {
            BlockIndexResidentPayload payload;
            if (index->m_resident_payload) {
                payload = *index->m_resident_payload;
            } else if (!m_payload_loader(*index, payload)) {
                return false;
            }
            payload.algo_history = nullptr;
            hot.emplace_back(index, std::move(payload));
        }

        ForEach([](CBlockIndex& index) { index.ClearResidentPayload(); });
        m_bootstrap_payloads.clear();
        m_payload_cache_lru.clear();
        m_payload_cache_index.clear();
        m_cached_algo_payloads.clear();
        m_payload_pins.clear();
        m_hot_window.clear();
        m_cache_active = true;
        m_eviction_enabled = true;

        for (auto it = hot.rbegin(); it != hot.rend(); ++it) {
            InsertCachedPayload(*it->first, std::move(it->second));
            ++m_payload_pins[it->first];
            m_hot_window.push_back(it->first);
        }
        RebuildHotIdentityIndex();
        return true;
    }

    size_t PrewarmAlgoHistory(CBlockIndex* tip)
    {
        if (m_mode == BlockIndexResidencyMode::FULL || !tip || m_hot_depth == 0) return 0;

        std::vector<CBlockIndex*> warm;
        warm.reserve(std::min<size_t>(m_hot_depth, static_cast<size_t>(tip->nHeight) + 1));
        for (CBlockIndex* ancestor{tip}; ancestor && warm.size() < m_hot_depth; ancestor = ancestor->pprev) {
            warm.push_back(ancestor);
        }

        size_t warmed{0};
        for (auto it = warm.rbegin(); it != warm.rend(); ++it) {
            if (!(*it)->HasResidentAlgoHistory()) {
                EnsureAlgoHistory(**it);
                ++warmed;
            }
        }
        m_stats.algo_prewarm_blocks += warmed;
        return warmed;
    }

    /**
     * Mark a scope in which block-index backing-store I/O is forbidden.
     *
     * Future lazy payload accessors must call RecordBackingRead() before
     * performing synchronous storage I/O. Debug builds assert immediately if
     * that happens inside a no-I/O scope; release builds retain a violation
     * counter for diagnostics.
     */
    NoIOGuard EnterNoIO() { return NoIOGuard{*this}; }

    [[nodiscard]] bool BackingReadAllowed() const noexcept { return m_no_io_depth == 0; }

    void RecordBackingRead()
    {
        ++m_stats.backing_reads;
        if (m_no_io_depth > 0) {
            ++m_stats.no_io_violations;
            assert(m_no_io_depth == 0 && "block-index backing read in no-I/O scope");
        }
    }

private:
    struct CacheEntry {
        CBlockIndex* index{nullptr};
        BlockIndexResidentPayload payload{};
    };

    void RebuildHotIdentityIndex()
    {
        if (!m_compact_identity_active) return;
        m_hot_identity_index.clear();
        m_hot_identity_index.reserve(m_hot_window.size() * 2 + 1);
        for (CBlockIndex* index : m_hot_window) {
            m_hot_identity_index.emplace(index->GetBlockHash(), index);
        }
    }

    size_t MaxCachedPayloads() const noexcept
    {
        return std::max<size_t>(1, m_cache_limit_bytes / sizeof(BlockIndexResidentPayload));
    }

    void EvictCachedPayloadIfNeeded()
    {
        if (!m_eviction_enabled) return;
        const size_t max_cached{MaxCachedPayloads()};
        while (m_payload_cache_lru.size() >= max_cached) {
            auto victim{m_payload_cache_lru.end()};
            for (auto it = m_payload_cache_lru.end(); it != m_payload_cache_lru.begin();) {
                --it;
                if (m_payload_pins.count(it->index) == 0) {
                    victim = it;
                    break;
                }
            }
            // Dirty/new payloads remain pinned until the ordinary canonical
            // block-index batch commits. If every candidate is pinned, allow
            // temporary budget overflow rather than dropping unflushed state.
            if (victim == m_payload_cache_lru.end()) break;

            m_cached_algo_payloads.erase(victim->index);
            victim->index->ClearResidentPayload();
            m_payload_cache_index.erase(victim->index);
            m_payload_cache_lru.erase(victim);
            ++m_stats.payload_cache_evictions;
        }
    }

    BlockIndexResidentPayload& InsertCachedPayload(
        CBlockIndex& index,
        BlockIndexResidentPayload payload)
    {
        EvictCachedPayloadIfNeeded();
        m_payload_cache_lru.push_front(CacheEntry{&index, std::move(payload)});
        auto inserted{
            m_payload_cache_index.emplace(&index, m_payload_cache_lru.begin())};
        assert(inserted.second);
        index.AttachResidentPayload(&m_payload_cache_lru.begin()->payload);
        ++m_stats.payloads_created;
        return m_payload_cache_lru.begin()->payload;
    }

    BlockIndexResidentPayload& CreateEmptyPayload(CBlockIndex& index)
    {
        if (!m_cache_active) {
            m_bootstrap_payloads.emplace_back();
            auto& payload{m_bootstrap_payloads.back()};
            index.AttachResidentPayload(&payload);
            ++m_stats.payloads_created;
            return payload;
        }
        return InsertCachedPayload(index, BlockIndexResidentPayload{});
    }

    void InitializeStoreEntry(CBlockIndex& index)
    {
        index.SetPayloadProvider(this);
        // A newly inserted entry has an empty resident payload. The first
        // insertion into m_dirty_blockindex owns its dirty pin, so repeated
        // mutations cannot leak pin references.
        CreateEmptyPayload(index);
    }

    void NoteLookup(bool hit) const noexcept
    {
        ++m_stats.lookups;
        if (hit) {
            ++m_stats.lookup_hits;
        } else {
            ++m_stats.lookup_misses;
        }
    }

    BlockMap m_entries;

    // BALANCED/LOWMEM identity ownership after startup migration. The arena is
    // dense in BlockIndexId order. Immutable-base hashes remain owned by the
    // mapped compact store; only the small mutable/live tail is duplicated in
    // an in-memory exact index so delta checkpoint swaps cannot invalidate
    // phashBlock pointers.
    StableArena<CBlockIndex> m_compact_identities;
    std::unordered_map<uint256, CBlockIndex*, BlockHasher> m_live_identity_index;
    std::unordered_map<uint256, CBlockIndex*, BlockHasher> m_hot_identity_index;
    size_t m_immutable_identity_count{0};
    bool m_direct_identity_bootstrap{false};
    bool m_compact_identity_prepared{false};
    bool m_compact_identity_active{false};

    BlockIndexResidencyMode m_mode{BlockIndexResidencyMode::FULL};
    size_t m_hot_depth{kernel::DEFAULT_BLOCK_INDEX_HOT_DEPTH};
    size_t m_cache_limit_bytes{kernel::DEFAULT_BLOCK_INDEX_CACHE_MIB_BALANCED * 1024 * 1024};

    StableArena<BlockIndexResidentPayload> m_bootstrap_payloads;
    StableArena<BlockIndexAlgoHistory> m_full_algo_payloads;
    std::unordered_map<CBlockIndex*, BlockIndexAlgoHistory> m_cached_algo_payloads;
    std::list<CacheEntry> m_payload_cache_lru;
    std::unordered_map<CBlockIndex*, std::list<CacheEntry>::iterator> m_payload_cache_index;
    std::unordered_map<CBlockIndex*, uint32_t> m_payload_pins;
    std::deque<CBlockIndex*> m_hot_window;
    PayloadLoader m_payload_loader;
    bool m_cache_active{false};
    bool m_eviction_enabled{true};

    mutable BlockIndexResidencyStats m_stats;
    uint32_t m_no_io_depth{0};
};

struct CBlockIndexWorkComparator {
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const;
};

struct CBlockIndexHeightOnlyComparator {
    /* Only compares the height of two block indices, doesn't try to tie-break */
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const;
};

struct PruneLockInfo {
    int height_first{std::numeric_limits<int>::max()}; //! Height of earliest block that should be kept and not pruned
};

enum BlockfileType {
    // Values used as array indexes - do not change carelessly.
    NORMAL = 0,
    ASSUMED = 1,
    NUM_TYPES = 2,
};

std::ostream& operator<<(std::ostream& os, const BlockfileType& type);

struct BlockfileCursor {
    // The latest blockfile number.
    int file_num{0};

    // Track the height of the highest block in file_num whose undo
    // data has been written. Block data is written to block files in download
    // order, but is written to undo files in validation order, which is
    // usually in order by height. To avoid wasting disk space, undo files will
    // be trimmed whenever the corresponding block file is finalized and
    // the height of the highest block written to the block file equals the
    // height of the highest block written to the undo file. This is a
    // heuristic and can sometimes preemptively trim undo files that will write
    // more data later, and sometimes fail to trim undo files that can't have
    // more data written later.
    int undo_height{0};
};

std::ostream& operator<<(std::ostream& os, const BlockfileCursor& cursor);


/**
 * Maintains a tree of blocks (stored in `m_block_index`) which is consulted
 * to determine where the most-work tip is.
 *
 * This data is used mostly in `Chainstate` - information about, e.g.,
 * candidate tips is not maintained here.
 */
class BlockManager
{
    friend Chainstate;
    friend ChainstateManager;

private:
    const CChainParams& GetParams() const { return m_opts.chainparams; }
    const Consensus::Params& GetConsensus() const { return m_opts.chainparams.GetConsensus(); }
    /**
     * Load the blocktree off disk and into memory. Populate certain metadata
     * per index entry (nStatus, nChainWork, nTimeMax, etc.) as well as peripheral
     * collections like m_dirty_blockindex.
     */
    bool LoadBlockIndex(const std::optional<uint256>& snapshot_blockhash)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    fs::path CompactBlockIndexShadowPath() const;
    fs::path CompactBlockIndexLookupPath() const;
    fs::path CompactBlockIndexIdsPath() const;
    fs::path CompactBlockIndexDeltaPath() const;
    fs::path CompactBlockIndexDeltaLogPath() const;
    fs::path CompactBlockIndexDeltaStatePath() const;
    fs::path CompactBlockIndexDeltaPendingPath() const;
    bool OpenCompactBlockIndexDeltaState(bool migrate_legacy)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    void AssignCompactIdsDeterministic(const std::vector<CBlockIndex*>& sorted)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool OpenCompactBlockIndexMapped()
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool BuildCompactBlockIndexShadow(const std::vector<CBlockIndex*>& sorted)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool VerifyCompactBlockIndexShadow(const std::vector<CBlockIndex*>& sorted)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool BuildCompactBlockIndexLookup()
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool VerifyCompactBlockIndexLookup()
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool RestoreCompactIds(const std::vector<CBlockIndex*>& sorted, bool allow_create)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool PersistCompactIds(const std::vector<const CBlockIndex*>& blockinfo)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool BuildCompactBlockIndexDelta(const std::vector<CBlockIndex*>& sorted)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool CompactBlockIndexMetadata()
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool VerifyCompactBlockIndexDelta()
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool OpenCompactBlockIndexDeltaLog(bool create)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool BuildCompactBlockIndexDeltaLogRecords(
        const std::vector<const CBlockIndex*>& blockinfo,
        std::vector<CompactBlockIndexDeltaLogRecord>& updates)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool StageCompactBlockIndexDeltaPending(const std::vector<const CBlockIndex*>& blockinfo)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool PublishCompactBlockIndexDeltaPending()
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool ReconcileCompactBlockIndexDeltaPending()
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool ClearCompactBlockIndexDeltaPending()
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    void InvalidateCompactBlockIndexDeltaOverlay()
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    BlockIndexId AllocateCompactId()
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    bool LoadCompactBlockIndexPayload(const CBlockIndex& index, BlockIndexResidentPayload& payload)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Return false if block file or undo file flushing fails. */
    [[nodiscard]] bool FlushBlockFile(int blockfile_num, bool fFinalize, bool finalize_undo);

    /** Return false if undo file flushing fails. */
    [[nodiscard]] bool FlushUndoFile(int block_file, bool finalize = false);

    [[nodiscard]] bool FindBlockPos(FlatFilePos& pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown);
    [[nodiscard]] bool FlushChainstateBlockFile(int tip_height);
    bool FindUndoPos(BlockValidationState& state, int nFile, FlatFilePos& pos, unsigned int nAddSize);

    FlatFileSeq BlockFileSeq() const;
    FlatFileSeq UndoFileSeq() const;

    CAutoFile OpenUndoFile(const FlatFilePos& pos, bool fReadOnly = false) const;

    bool WriteBlockToDisk(const CBlock& block, FlatFilePos& pos) const;
    bool UndoWriteToDisk(const CBlockUndo& blockundo, FlatFilePos& pos, const uint256& hashBlock) const;

    /* Calculate the block/rev files to delete based on height specified by user with RPC command pruneblockchain */
    void FindFilesToPruneManual(
        std::set<int>& setFilesToPrune,
        int nManualPruneHeight,
        const Chainstate& chain,
        ChainstateManager& chainman);

    /**
     * Prune block and undo files (blk???.dat and rev???.dat) so that the disk space used is less than a user-defined target.
     * The user sets the target (in MB) on the command line or in config file.  This will be run on startup and whenever new
     * space is allocated in a block or undo file, staying below the target. Changing back to unpruned requires a reindex
     * (which in this case means the blockchain must be re-downloaded.)
     *
     * Pruning functions are called from FlushStateToDisk when the m_check_for_pruning flag has been set.
     * Block and undo files are deleted in lock-step (when blk00003.dat is deleted, so is rev00003.dat.)
     * Pruning cannot take place until the longest chain is at least a certain length (CChainParams::nPruneAfterHeight).
     * Pruning will never delete a block within a defined distance (currently 288) from the active chain's tip.
     * The block index is updated by unsetting HAVE_DATA and HAVE_UNDO for any blocks that were stored in the deleted files.
     * A db flag records the fact that at least some block files have been pruned.
     *
     * @param[out]   setFilesToPrune   The set of file indices that can be unlinked will be returned
     * @param        last_prune        The last height we're able to prune, according to the prune locks
     */
    void FindFilesToPrune(
        std::set<int>& setFilesToPrune,
        int last_prune,
        const Chainstate& chain,
        ChainstateManager& chainman);

    RecursiveMutex cs_LastBlockFile;
    std::vector<CBlockFileInfo> m_blockfile_info;

    //! Since assumedvalid chainstates may be syncing a range of the chain that is very
    //! far away from the normal/background validation process, we should segment blockfiles
    //! for assumed chainstates. Otherwise, we might have wildly different height ranges
    //! mixed into the same block files, which would impair our ability to prune
    //! effectively.
    //!
    //! This data structure maintains separate blockfile number cursors for each
    //! BlockfileType. The ASSUMED state is initialized, when necessary, in FindBlockPos().
    //!
    //! The first element is the NORMAL cursor, second is ASSUMED.
    std::array<std::optional<BlockfileCursor>, BlockfileType::NUM_TYPES>
        m_blockfile_cursors GUARDED_BY(cs_LastBlockFile) = {
            BlockfileCursor{},
            std::nullopt,
    };
    int MaxBlockfileNum() const EXCLUSIVE_LOCKS_REQUIRED(cs_LastBlockFile)
    {
        static const BlockfileCursor empty_cursor;
        const auto& normal = m_blockfile_cursors[BlockfileType::NORMAL].value_or(empty_cursor);
        const auto& assumed = m_blockfile_cursors[BlockfileType::ASSUMED].value_or(empty_cursor);
        return std::max(normal.file_num, assumed.file_num);
    }

    /** Global flag to indicate we should check to see if there are
     *  block/undo files that should be deleted.  Set on startup
     *  or if we allocate more file space when we're in prune mode
     */
    bool m_check_for_pruning = false;

    const bool m_prune_mode;

    /** Dirty block index entries. */
    std::set<CBlockIndex*> m_dirty_blockindex;

    /** Dirty block file entries. */
    std::set<int> m_dirty_fileinfo;

    /**
     * Map from external index name to oldest block that must not be pruned.
     *
     * @note Internally, only blocks at height (height_first - PRUNE_LOCK_BUFFER - 1) and
     * below will be pruned, but callers should avoid assuming any particular buffer size.
     */
    std::unordered_map<std::string, PruneLockInfo> m_prune_locks GUARDED_BY(::cs_main);

    BlockfileType BlockfileTypeForHeight(int height);

    const kernel::BlockManagerOpts m_opts;

public:
    using Options = kernel::BlockManagerOpts;

    explicit BlockManager(const util::SignalInterrupt& interrupt, Options opts)
        : m_prune_mode{opts.prune_target > 0},
          m_opts{std::move(opts)},
          m_interrupt{interrupt},
          m_block_index{m_opts.block_index_mode, m_opts.block_index_hot_depth, m_opts.block_index_cache_bytes} {};

    const util::SignalInterrupt& m_interrupt;
    std::atomic<bool> m_importing{false};

    BlockIndexStore m_block_index GUARDED_BY(cs_main);
    std::unique_ptr<CompactBlockIndexStore> m_compact_block_index GUARDED_BY(cs_main);
    std::unique_ptr<CompactBlockIndexLookup> m_compact_block_lookup GUARDED_BY(cs_main);
    std::unique_ptr<CompactBlockIndexIds> m_compact_block_ids GUARDED_BY(cs_main);
    std::unique_ptr<CompactBlockIndexDelta> m_compact_block_delta GUARDED_BY(cs_main);
    std::unique_ptr<CompactBlockIndexDeltaLog> m_compact_block_delta_log GUARDED_BY(cs_main);
    std::unique_ptr<CompactBlockIndexDeltaState> m_compact_block_delta_state GUARDED_BY(cs_main);
    std::unordered_map<BlockIndexId, CompactBlockIndexEntry> m_compact_block_delta_overlay GUARDED_BY(cs_main);

    // Next process-local compact id. Startup restores/assigns the historical
    // namespace, while clean IBD/reindex naturally starts at zero.
    uint64_t m_next_compact_id GUARDED_BY(cs_main){0};

    // Height-ordered startup view built once during BlockManager loading and
    // handed to ChainstateManager for candidate/header initialization. Keeping
    // it briefly avoids a second full traversal of the 24M-entry legacy map.
    std::vector<CBlockIndex*> m_startup_block_index_view GUARDED_BY(cs_main);

    /**
     * The height of the base block of an assumeutxo snapshot, if one is in use.
     *
     * This controls how blockfiles are segmented by chainstate type to avoid
     * comingling different height regions of the chain when an assumedvalid chainstate
     * is in use. If heights are drastically different in the same blockfile, pruning
     * suffers.
     *
     * This is set during ActivateSnapshot() or upon LoadBlockIndex() if a snapshot
     * had been previously loaded. After the snapshot is validated, this is unset to
     * restore normal LoadBlockIndex behavior.
     */
    std::optional<int> m_snapshot_height;

    std::vector<CBlockIndex*> GetAllBlockIndices() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    std::vector<CBlockIndex*> GetAllBlockIndicesByCompactId() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    std::vector<CBlockIndex*> TakeStartupBlockIndexView() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    const CompactBlockIndexEntry* CompactEntryForId(BlockIndexId id) const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool PrepareDirectCompactMetadataBacking() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool PrepareDirectCompactBootstrap() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    CBlockIndex* LoadDirectCompactBlockIndexRecord(const CDiskBlockIndex& diskindex) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool DirectCompactBootstrapComplete() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    void ResetDirectCompactBootstrap() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool ActivateCompactBlockIndexIdentityStore(std::vector<CBlockIndex*>& startup_view) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool ActivateBlockIndexPayloadCache(CBlockIndex* tip) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    void AdviseCompactBlockIndexBackingCold() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * All pairs A->B, where A (or one of its ancestors) misses transactions, but B has transactions.
     * Pruned nodes may have entries where B is missing data.
     */
    std::multimap<CBlockIndex*, CBlockIndex*> m_blocks_unlinked;

    std::unique_ptr<BlockTreeDB> m_block_tree_db GUARDED_BY(::cs_main);

    bool WriteBlockIndexDB() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);
    bool LoadBlockIndexDB(const std::optional<uint256>& snapshot_blockhash)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /**
     * Remove any pruned block & undo files that are still on disk.
     * This could happen on some systems if the file was still being read while unlinked,
     * or if we crash before unlinking.
     */
    void ScanAndUnlinkAlreadyPrunedFiles() EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    CBlockIndex* AddToBlockIndex(const CBlockHeader& block, CBlockIndex*& best_header) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    /** Create a new block index entry for a given block hash */
    CBlockIndex* InsertBlockIndex(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Mark one block file as pruned (modify associated database entries)
    void PruneOneBlockFile(const int fileNumber) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    CBlockIndex* LookupBlockIndex(const uint256& hash) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
    const CBlockIndex* LookupBlockIndex(const uint256& hash) const EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /** Get block file info entry for one block file */
    CBlockFileInfo* GetBlockFileInfo(size_t n);

    bool WriteUndoDataForBlock(const CBlockUndo& blockundo, BlockValidationState& state, CBlockIndex& block)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Store block on disk. If dbp is not nullptr, then it provides the known position of the block within a block file on disk. */
    FlatFilePos SaveBlockToDisk(const CBlock& block, int nHeight, const FlatFilePos* dbp);

    /** Whether running in -prune mode. */
    [[nodiscard]] bool IsPruneMode() const { return m_prune_mode; }

    /** Attempt to stay below this number of bytes of block files. */
    [[nodiscard]] uint64_t GetPruneTarget() const { return m_opts.prune_target; }
    static constexpr auto PRUNE_TARGET_MANUAL{std::numeric_limits<uint64_t>::max()};

    [[nodiscard]] bool LoadingBlocks() const { return m_importing || fReindex; }

    /** Calculate the amount of disk space the block & undo files currently use */
    uint64_t CalculateCurrentUsage();

    //! Returns last CBlockIndex* that is a checkpoint
    const CBlockIndex* GetLastCheckpoint(const CCheckpointData& data) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    //! Check if all blocks in the [upper_block, lower_block] range have data available.
    //! The caller is responsible for ensuring that lower_block is an ancestor of upper_block
    //! (part of the same chain).
    bool CheckBlockDataAvailability(const CBlockIndex& upper_block LIFETIMEBOUND, const CBlockIndex& lower_block LIFETIMEBOUND) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Find the first stored ancestor of start_block immediately after the last
    //! pruned ancestor. Return value will never be null. Caller is responsible
    //! for ensuring that start_block has data is not pruned.
    const CBlockIndex* GetFirstStoredBlock(const CBlockIndex& start_block LIFETIMEBOUND, const CBlockIndex* lower_block=nullptr) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** True if any block files have ever been pruned. */
    bool m_have_pruned = false;

    //! Check whether the block associated with this index entry is pruned or not.
    bool IsBlockPruned(const CBlockIndex* pblockindex) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    //! Create or update a prune lock identified by its name
    void UpdatePruneLock(const std::string& name, const PruneLockInfo& lock_info) EXCLUSIVE_LOCKS_REQUIRED(::cs_main);

    /** Open a block file (blk?????.dat) */
    CAutoFile OpenBlockFile(const FlatFilePos& pos, bool fReadOnly = false) const;

    /** Translation to a filesystem path */
    fs::path GetBlockPosFilename(const FlatFilePos& pos) const;

    /**
     *  Actually unlink the specified files
     */
    void UnlinkPrunedFiles(const std::set<int>& setFilesToPrune) const;

    /** Functions for disk access for blocks */
    bool ReadBlockFromDisk(CBlock& block, const FlatFilePos& pos) const;
    bool ReadBlockFromDisk(CBlock& block, const CBlockIndex& index) const;
    bool ReadRawBlockFromDisk(std::vector<uint8_t>& block, const FlatFilePos& pos) const;

    bool UndoReadFromDisk(CBlockUndo& blockundo, const CBlockIndex& index) const;

    void CleanupBlockRevFiles() const;
};

void ImportBlocks(ChainstateManager& chainman, std::vector<fs::path> vImportFiles);
} // namespace node

#endif // DIGIBYTE_NODE_BLOCKSTORAGE_H
