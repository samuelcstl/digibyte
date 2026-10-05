// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Copyright (c) 2014-2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef DIGIBYTE_CHAIN_H
#define DIGIBYTE_CHAIN_H

#include <arith_uint256.h>
#include <consensus/params.h>
#include <flatfile.h>
#include <kernel/cs_main.h>
#include <primitives/block.h>
#include <pow.h>
#include <sync.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/time.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

/**
 * Maximum amount of time that a block timestamp is allowed to exceed the
 * current network-adjusted time before the block will be accepted.
 */
static constexpr int64_t MAX_FUTURE_BLOCK_TIME = 2 * 60 * 60;

/**
 * Timestamp window used as a grace period by code that compares external
 * timestamps (such as timestamps passed to RPCs, or wallet key creation times)
 * to block timestamps. This should be set at least as high as
 * MAX_FUTURE_BLOCK_TIME.
 */
static constexpr int64_t TIMESTAMP_WINDOW = MAX_FUTURE_BLOCK_TIME;

/**
 * Maximum gap between node time and block time used
 * for the "Catching up..." mode in GUI.
 *
 * Ref: https://github.com/digibyte/digibyte/pull/1026
 */
static constexpr int64_t MAX_BLOCK_TIME_GAP = 90 * 60;

class CBlockFileInfo
{
public:
    unsigned int nBlocks{};      //!< number of blocks stored in file
    unsigned int nSize{};        //!< number of used bytes of block file
    unsigned int nUndoSize{};    //!< number of used bytes in the undo file
    unsigned int nHeightFirst{}; //!< lowest height of block in file
    unsigned int nHeightLast{};  //!< highest height of block in file
    uint64_t nTimeFirst{};       //!< earliest time of block in file
    uint64_t nTimeLast{};        //!< latest time of block in file

    SERIALIZE_METHODS(CBlockFileInfo, obj)
    {
        READWRITE(VARINT(obj.nBlocks));
        READWRITE(VARINT(obj.nSize));
        READWRITE(VARINT(obj.nUndoSize));
        READWRITE(VARINT(obj.nHeightFirst));
        READWRITE(VARINT(obj.nHeightLast));
        READWRITE(VARINT(obj.nTimeFirst));
        READWRITE(VARINT(obj.nTimeLast));
    }

    CBlockFileInfo() {}

    std::string ToString() const;

    /** update statistics (does not update nSize) */
    void AddBlock(unsigned int nHeightIn, uint64_t nTimeIn)
    {
        if (nBlocks == 0 || nHeightFirst > nHeightIn)
            nHeightFirst = nHeightIn;
        if (nBlocks == 0 || nTimeFirst > nTimeIn)
            nTimeFirst = nTimeIn;
        nBlocks++;
        if (nHeightIn > nHeightLast)
            nHeightLast = nHeightIn;
        if (nTimeIn > nTimeLast)
            nTimeLast = nTimeIn;
    }
};

enum BlockStatus : uint32_t {
    //! Unused.
    BLOCK_VALID_UNKNOWN      =    0,

    //! Reserved (was BLOCK_VALID_HEADER).
    BLOCK_VALID_RESERVED     =    1,

    //! All parent headers found, difficulty matches, timestamp >= median previous, checkpoint. Implies all parents
    //! are also at least TREE.
    BLOCK_VALID_TREE         =    2,

    /**
     * Only first tx is coinbase, 2 <= coinbase input script length <= 100, transactions valid, no duplicate txids,
     * sigops, size, merkle root. Implies all parents are at least TREE but not necessarily TRANSACTIONS. When all
     * parent blocks also have TRANSACTIONS, CBlockIndex::nChainTx will be set.
     */
    BLOCK_VALID_TRANSACTIONS =    3,

    //! Outputs do not overspend inputs, no double spends, coinbase output ok, no immature coinbase spends, BIP30.
    //! Implies all parents are either at least VALID_CHAIN, or are ASSUMED_VALID
    BLOCK_VALID_CHAIN        =    4,

    //! Scripts & signatures ok. Implies all parents are either at least VALID_SCRIPTS, or are ASSUMED_VALID.
    BLOCK_VALID_SCRIPTS      =    5,

    //! All validity bits.
    BLOCK_VALID_MASK         =   BLOCK_VALID_RESERVED | BLOCK_VALID_TREE | BLOCK_VALID_TRANSACTIONS |
                                 BLOCK_VALID_CHAIN | BLOCK_VALID_SCRIPTS,

    BLOCK_HAVE_DATA          =    8, //!< full block available in blk*.dat
    BLOCK_HAVE_UNDO          =   16, //!< undo data available in rev*.dat
    BLOCK_HAVE_MASK          =   BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO,

    BLOCK_FAILED_VALID       =   32, //!< stage after last reached validness failed
    BLOCK_FAILED_CHILD       =   64, //!< descends from failed block
    BLOCK_FAILED_MASK        =   BLOCK_FAILED_VALID | BLOCK_FAILED_CHILD,

    BLOCK_OPT_WITNESS        =   128, //!< block data in blk*.dat was received with a witness-enforcing client

    /**
     * If ASSUMED_VALID is set, it means that this block has not been validated
     * and has validity status less than VALID_SCRIPTS. Also that it may have
     * descendant blocks with VALID_SCRIPTS set, because they can be validated
     * based on an assumeutxo snapshot.
     *
     * When an assumeutxo snapshot is loaded, the ASSUMED_VALID flag is added to
     * unvalidated blocks at the snapshot height and below. Then, as the background
     * validation progresses, and these blocks are validated, the ASSUMED_VALID
     * flags are removed. See `doc/design/assumeutxo.md` for details.
     *
     * This flag is only used to implement checks in CheckBlockIndex() and
     * should not be used elsewhere.
     */
    BLOCK_ASSUMED_VALID      =   256,
};

class CBlockIndex;
struct BlockIndexResidentPayload;

class BlockIndexPayloadProvider
{
public:
    virtual ~BlockIndexPayloadProvider() = default;
    virtual BlockIndexResidentPayload& MaterializeBlockIndexPayload(CBlockIndex& index) = 0;
};

struct BlockIndexAlgoHistory
{
    std::array<CBlockIndex*, NUM_ALGOS_IMPL> last_algo_blocks{};
};

struct BlockIndexResidentPayload
{
    int nFile{0};
    unsigned int nDataPos{0};
    unsigned int nUndoPos{0};
    uint256 hashMerkleRoot{};
    unsigned int nTimeMax{0};
    BlockIndexAlgoHistory* algo_history{nullptr};
};

/** The block chain is a tree shaped structure starting with the
 * genesis block at the root, with each block potentially having multiple
 * candidates to be the next block. A blockindex may have multiple pprev pointing
 * to it, but at most one of them can be part of the currently active branch.
 */
class CBlockIndex
{
public:
    //! pointer to the hash of the block, if any. Memory is owned by this CBlockIndex
    const uint256* phashBlock{nullptr};

    //! pointer to the index of the predecessor of this block
    CBlockIndex* pprev{nullptr};

    //! pointer to the index of some further predecessor of this block
    CBlockIndex* pskip{nullptr};

    //! height of the entry in the chain. The genesis block has height 0
    int nHeight{0};

    //! (memory only) Total amount of work (expected number of hashes) in the chain up to and including this block
    arith_uint256 nChainWork{};

    //! Number of transactions in this block.
    //! Note: in a potential headers-first mode, this number cannot be relied upon
    //! Note: this value is faked during UTXO snapshot load to ensure that
    //! LoadBlockIndex() will load index entries for blocks that we lack data for.
    //! @sa ActivateSnapshot
    unsigned int nTx{0};

    //! (memory only) Number of transactions in the chain up to and including this block.
    //! This value will be non-zero only if and only if transactions for this block and all its parents are available.
    //! Change to 64-bit type before 2024 (assuming worst case of 60 byte transactions).
    //!
    //! Note: this value is faked during use of a UTXO snapshot because we don't
    //! have the underlying block data available during snapshot load.
    //! @sa AssumeutxoData
    //! @sa ActivateSnapshot
    unsigned int nChainTx{0};

    //! Verification status of this block. See enum BlockStatus
    //!
    //! Note: this value is modified to show BLOCK_OPT_WITNESS during UTXO snapshot
    //! load to avoid the block index being spuriously rewound.
    //! @sa NeedsRedownload
    //! @sa ActivateSnapshot
    uint32_t nStatus GUARDED_BY(::cs_main){0};

    //! block header
    int32_t nVersion{0};
    uint32_t nTime{0};
    uint32_t nBits{0};
    uint32_t nNonce{0};

    //! (memory only) Sequential id assigned to distinguish order in which blocks are received.
    int32_t nSequenceId{0};

    /**
     * Generation-2 compact-store id.
     *
     * This is runtime identity metadata, not consensus state. Historical
     * storage/merkle/time-max fields now live behind the residency payload,
     * leaving the stable 64-bit shell at 112 bytes.
     */
    uint32_t m_compact_id{std::numeric_limits<uint32_t>::max()};

    /**
     * Residency-managed payload link.
     *
     * Generation 2 originally carried both a resident-payload pointer and a
     * provider pointer in every CBlockIndex shell. Only one is needed at a
     * time: a hot/resident entry points at its payload, while a cold entry
     * points at the BlockIndexStore that can materialize it. Keep that state in
     * one tagged machine word so the stable CBlockIndex* contract is unchanged
     * while the 64-bit shell drops one pointer.
     *
     * Low two bits are tags. All participating objects have >=4-byte
     * alignment:
     *   00 non-zero: standalone-owned payload
     *   01:          payload provider
     *   10:          store/disk-owned borrowed payload
     *   00 zero:     no payload/provider yet
     */
    static constexpr uintptr_t PAYLOAD_LINK_TAG_MASK{uintptr_t{3}};
    static constexpr uintptr_t PAYLOAD_LINK_PROVIDER{uintptr_t{1}};
    static constexpr uintptr_t PAYLOAD_LINK_BORROWED{uintptr_t{2}};
    mutable uintptr_t m_payload_link{0};

    [[nodiscard]] uintptr_t PayloadLinkTag() const noexcept
    {
        return m_payload_link & PAYLOAD_LINK_TAG_MASK;
    }

    [[nodiscard]] BlockIndexResidentPayload* ResidentPayloadIfPresent() const noexcept
    {
        const uintptr_t tag{PayloadLinkTag()};
        if (m_payload_link == 0 || tag == PAYLOAD_LINK_PROVIDER) return nullptr;
        return reinterpret_cast<BlockIndexResidentPayload*>(
            m_payload_link & ~PAYLOAD_LINK_TAG_MASK);
    }

    [[nodiscard]] BlockIndexPayloadProvider* PayloadProvider() const noexcept
    {
        if (PayloadLinkTag() != PAYLOAD_LINK_PROVIDER) return nullptr;
        return reinterpret_cast<BlockIndexPayloadProvider*>(
            m_payload_link & ~PAYLOAD_LINK_TAG_MASK);
    }

    [[nodiscard]] bool OwnsResidentPayload() const noexcept
    {
        return m_payload_link != 0 && PayloadLinkTag() == 0;
    }

    void SetPayloadProvider(BlockIndexPayloadProvider* provider) noexcept
    {
        assert(provider != nullptr);
        const uintptr_t raw{reinterpret_cast<uintptr_t>(provider)};
        assert((raw & PAYLOAD_LINK_TAG_MASK) == 0);
        // Store-managed callers may replace a borrowed bootstrap/cache payload
        // with its provider when the payload becomes cold. An owned standalone
        // payload must be explicitly cleared/copied first.
        assert(!OwnsResidentPayload());
        m_payload_link = raw | PAYLOAD_LINK_PROVIDER;
    }

    void AttachResidentPayload(BlockIndexResidentPayload* payload) const noexcept
    {
        assert(payload != nullptr);
        const uintptr_t raw{reinterpret_cast<uintptr_t>(payload)};
        assert((raw & PAYLOAD_LINK_TAG_MASK) == 0);
        m_payload_link = raw | PAYLOAD_LINK_BORROWED;
    }

    void AttachOwnedResidentPayload(BlockIndexResidentPayload* payload) const noexcept
    {
        assert(payload != nullptr);
        const uintptr_t raw{reinterpret_cast<uintptr_t>(payload)};
        assert((raw & PAYLOAD_LINK_TAG_MASK) == 0);
        m_payload_link = raw;
    }

    void ClearResidentPayload() const noexcept
    {
        if (OwnsResidentPayload()) {
            delete ResidentPayloadIfPresent();
        }
        m_payload_link = 0;
    }

    [[nodiscard]] BlockIndexResidentPayload& ResidentPayload() const
    {
        if (BlockIndexResidentPayload* payload{ResidentPayloadIfPresent()}) {
            return *payload;
        }
        if (BlockIndexPayloadProvider* provider{PayloadProvider()}) {
            return provider->MaterializeBlockIndexPayload(
                *const_cast<CBlockIndex*>(this));
        }

        // Standalone/transient CBlockIndex objects used outside
        // BlockIndexStore lazily own their payload. Store-managed entries
        // install a tagged provider before first payload access.
        auto* payload{new BlockIndexResidentPayload()};
        AttachOwnedResidentPayload(payload);
        return *payload;
    }

    [[nodiscard]] bool HasResidentPayload() const noexcept
    {
        return ResidentPayloadIfPresent() != nullptr;
    }

    [[nodiscard]] bool HasResidentAlgoHistory() const noexcept
    {
        const BlockIndexResidentPayload* payload{ResidentPayloadIfPresent()};
        return payload && payload->algo_history;
    }

    [[nodiscard]] CBlockIndex* GetResidentLastAlgoBlock(int algo) const noexcept
    {
        const BlockIndexResidentPayload* payload{ResidentPayloadIfPresent()};
        if (!payload || !payload->algo_history || algo < 0 || algo >= NUM_ALGOS_IMPL) return nullptr;
        return payload->algo_history->last_algo_blocks[algo];
    }

    int& StorageFile() { return ResidentPayload().nFile; }
    const int& StorageFile() const { return ResidentPayload().nFile; }
    unsigned int& DataPos() { return ResidentPayload().nDataPos; }
    const unsigned int& DataPos() const { return ResidentPayload().nDataPos; }
    unsigned int& UndoPos() { return ResidentPayload().nUndoPos; }
    const unsigned int& UndoPos() const { return ResidentPayload().nUndoPos; }
    uint256& MerkleRoot() { return ResidentPayload().hashMerkleRoot; }
    const uint256& MerkleRoot() const { return ResidentPayload().hashMerkleRoot; }
    unsigned int& TimeMax() { return ResidentPayload().nTimeMax; }
    const unsigned int& TimeMax() const { return ResidentPayload().nTimeMax; }

    /**
     * Full constructor that copies fields from a block header.
     * (Definition is moved to chain.cpp so we can log from there)
     */
    explicit CBlockIndex(const CBlockHeader& block);

    FlatFilePos GetBlockPos() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        FlatFilePos ret;
        if (nStatus & BLOCK_HAVE_DATA) {
            ret.nFile = StorageFile();
            ret.nPos = DataPos();
        }
        return ret;
    }

    FlatFilePos GetUndoPos() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        FlatFilePos ret;
        if (nStatus & BLOCK_HAVE_UNDO) {
            ret.nFile = StorageFile();
            ret.nPos = UndoPos();
        }
        return ret;
    }

    CBlockHeader GetBlockHeader() const
    {
        CBlockHeader block;
        block.nVersion = nVersion;
        if (pprev)
            block.hashPrevBlock = pprev->GetBlockHash();
        block.hashMerkleRoot = MerkleRoot();
        block.nTime = nTime;
        block.nBits = nBits;
        block.nNonce = nNonce;
        return block;
    }

    uint256 GetBlockHash() const
    {
        assert(phashBlock != nullptr);
        return *phashBlock;
    }

    //! DigiByte: Get block PoW hash based on algorithm
    uint256 GetBlockPoWHash() const
    {
        CBlockHeader block = GetBlockHeader();
        return GetPoWAlgoHash(block);
    }


    /**
     * Check whether this block's and all previous blocks' transactions have been
     * downloaded (and stored to disk) at some point.
     *
     * Does not imply the transactions are consensus-valid (ConnectTip might fail)
     * Does not imply the transactions are still stored on disk. (IsBlockPruned might return true)
     *
     * Note that this will be true for the snapshot base block, if one is loaded (and
     * all subsequent assumed-valid blocks) since its nChainTx value will have been set
     * manually based on the related AssumeutxoData entry.
     */
    bool HaveNumChainTxs() const { return nChainTx != 0; }

    //! DigiByte: Get mining algorithm for this block
    int GetAlgo() const;

    NodeSeconds Time() const
    {
        return NodeSeconds{std::chrono::seconds{nTime}};
    }

    int64_t GetBlockTime() const
    {
        return (int64_t)nTime;
    }

    int64_t GetBlockTimeMax() const
    {
        return (int64_t)TimeMax();
    }

    static constexpr int nMedianTimeSpan = 11;

    int64_t GetMedianTimePast() const
    {
        int64_t pmedian[nMedianTimeSpan];
        int64_t* pbegin = &pmedian[nMedianTimeSpan];
        int64_t* pend = &pmedian[nMedianTimeSpan];

        const CBlockIndex* pindex = this;
        for (int i = 0; i < nMedianTimeSpan && pindex; i++, pindex = pindex->pprev)
            *(--pbegin) = pindex->GetBlockTime();

        std::sort(pbegin, pend);
        return pbegin[(pend - pbegin) / 2];
    }

    std::string ToString() const;

    //! Check whether this block index entry is valid up to the passed validity level.
    bool IsValid(enum BlockStatus nUpTo = BLOCK_VALID_TRANSACTIONS) const
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        assert(!(nUpTo & ~BLOCK_VALID_MASK)); // Only validity flags allowed.
        if (nStatus & BLOCK_FAILED_MASK)
            return false;
        return ((nStatus & BLOCK_VALID_MASK) >= nUpTo);
    }

    //! @returns true if the block is assumed-valid; this means it is queued to be
    //!   validated by a background chainstate.
    bool IsAssumedValid() const EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        return nStatus & BLOCK_ASSUMED_VALID;
    }

    //! Raise the validity level of this block index entry.
    //! Returns true if the validity was changed.
    bool RaiseValidity(enum BlockStatus nUpTo) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        AssertLockHeld(::cs_main);
        assert(!(nUpTo & ~BLOCK_VALID_MASK)); // Only validity flags allowed.
        if (nStatus & BLOCK_FAILED_MASK) return false;

        if ((nStatus & BLOCK_VALID_MASK) < nUpTo) {
            // If this block had been marked assumed-valid and we're raising
            // its validity to a certain point, there is no longer an assumption.
            if (nStatus & BLOCK_ASSUMED_VALID && nUpTo >= BLOCK_VALID_SCRIPTS) {
                nStatus &= ~BLOCK_ASSUMED_VALID;
            }

            nStatus = (nStatus & ~BLOCK_VALID_MASK) | nUpTo;
            return true;
        }
        return false;
    }


    //! Build the skiplist pointer for this entry.
    void BuildSkip();

    //! Efficiently find an ancestor of this block.
    CBlockIndex* GetAncestor(int height);
    const CBlockIndex* GetAncestor(int height) const;

    CBlockIndex();
    ~CBlockIndex();

protected:
    //! CBlockIndex should not allow public copy construction because equality
    //! comparison via pointer is very common throughout the codebase, making
    //! use of copy a footgun. Also, use of copies do not have the benefit
    //! of simplifying lifetime considerations due to attributes like pprev and
    //! pskip, which are at risk of becoming dangling pointers in a copied
    //! instance.
    //!
    //! We declare these protected instead of simply deleting them so that
    //! CDiskBlockIndex can reuse copy construction.
    CBlockIndex(const CBlockIndex&);
    CBlockIndex& operator=(const CBlockIndex&) = delete;
    CBlockIndex(CBlockIndex&&) = delete;
    CBlockIndex& operator=(CBlockIndex&&) = delete;
};

static_assert(sizeof(void*) != 8 || sizeof(CBlockIndex) == 104,
              "64-bit CBlockIndex shell must remain 104 bytes");

arith_uint256 GetBlockProof(const CBlockIndex& block);
arith_uint256 GetBlockProof(const CBlockIndex& block, int algo);

/** Return the time it would take to redo the work difference between from and to, assuming the current hashrate corresponds to the difficulty at tip, in seconds. */
int64_t GetBlockProofEquivalentTime(const CBlockIndex& to, const CBlockIndex& from, const CBlockIndex& tip, const Consensus::Params&);
/** Find the forking point between two chain tips. */
const CBlockIndex* LastCommonAncestor(const CBlockIndex* pa, const CBlockIndex* pb);


/** Used to marshal pointers into hashes for db storage. */
class CDiskBlockIndex : public CBlockIndex
{
    /** Historically CBlockLocator's version field has been written to disk
     * streams as the client version, but the value has never been used.
     *
     * Hard-code to the highest client version ever written.
     * SerParams can be used if the field requires any meaning in the future.
     **/
    static constexpr int DUMMY_VERSION = 259900;
    // Block-index records at or above this format version include cached
    // cumulative chain work after the block header fields.
    static constexpr int CHAINWORK_VERSION = 260000;

    bool m_has_persisted_chainwork{false};
    BlockIndexResidentPayload m_disk_payload{};

public:
    uint256 hashPrev;

    CDiskBlockIndex()
    {
        AttachResidentPayload(&m_disk_payload);
        hashPrev = uint256();
    }

    explicit CDiskBlockIndex(const CBlockIndex* pindex) : CBlockIndex(*pindex), m_has_persisted_chainwork{true}
    {
        if (BlockIndexResidentPayload* payload{ResidentPayloadIfPresent()}) {
            m_disk_payload = *payload;
            m_disk_payload.algo_history = nullptr;
        }
        ClearResidentPayload();
        AttachResidentPayload(&m_disk_payload);
        hashPrev = (pprev ? pprev->GetBlockHash() : uint256());
    }

    ~CDiskBlockIndex()
    {
        ClearResidentPayload();
    }

    bool HasPersistedChainWork() const { return m_has_persisted_chainwork; }

    SERIALIZE_METHODS(CDiskBlockIndex, obj)
    {
        LOCK(::cs_main);
        int _nVersion = CHAINWORK_VERSION;
        READWRITE(VARINT_MODE(_nVersion, VarIntMode::NONNEGATIVE_SIGNED));
        SER_READ(obj, obj.m_has_persisted_chainwork = _nVersion >= CHAINWORK_VERSION);

        READWRITE(VARINT_MODE(obj.nHeight, VarIntMode::NONNEGATIVE_SIGNED));
        READWRITE(VARINT(obj.nStatus));
        READWRITE(VARINT(obj.nTx));
        if (obj.nStatus & (BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO)) READWRITE(VARINT_MODE(obj.StorageFile(), VarIntMode::NONNEGATIVE_SIGNED));
        if (obj.nStatus & BLOCK_HAVE_DATA) READWRITE(VARINT(obj.DataPos()));
        if (obj.nStatus & BLOCK_HAVE_UNDO) READWRITE(VARINT(obj.UndoPos()));

        // block header
        READWRITE(obj.nVersion);
        READWRITE(obj.hashPrev);
        READWRITE(obj.MerkleRoot());
        READWRITE(obj.nTime);
        READWRITE(obj.nBits);
        READWRITE(obj.nNonce);

        // nChainWork is expensive to reconstruct on DigiByte after DigiSpeed.
        // Old records simply end after nNonce and are migrated lazily at startup.
        // arith_uint256 is an arithmetic helper rather than a serializable type,
        // so persist its canonical uint256 representation.
        if (_nVersion >= CHAINWORK_VERSION) {
            SER_WRITE(obj, s << ArithToUint256(obj.nChainWork));
            SER_READ(obj, {
                uint256 chain_work;
                s >> chain_work;
                obj.nChainWork = UintToArith256(chain_work);
            });
        }
    }

    uint256 ConstructBlockHash() const
    {
        CBlockHeader block;
        block.nVersion = nVersion;
        block.hashPrevBlock = hashPrev;
        block.hashMerkleRoot = MerkleRoot();
        block.nTime = nTime;
        block.nBits = nBits;
        block.nNonce = nNonce;
        return block.GetHash();
    }

    uint256 GetBlockHash() = delete;
    std::string ToString() = delete;
};

/** An in-memory indexed chain of blocks. */
class CChain
{
private:
    std::vector<CBlockIndex*> vChain;

public:
    CChain() = default;
    CChain(const CChain&) = delete;
    CChain& operator=(const CChain&) = delete;

    /** Returns the index entry for the genesis block of this chain, or nullptr if none. */
    CBlockIndex* Genesis() const
    {
        return vChain.size() > 0 ? vChain[0] : nullptr;
    }

    /** Returns the index entry for the tip of this chain, or nullptr if none. */
    CBlockIndex* Tip() const
    {
        return vChain.size() > 0 ? vChain[vChain.size() - 1] : nullptr;
    }

    /** Returns the index entry at a particular height in this chain, or nullptr if no such height exists. */
    CBlockIndex* operator[](int nHeight) const
    {
        if (nHeight < 0 || nHeight >= (int)vChain.size())
            return nullptr;
        return vChain[nHeight];
    }

    /** Efficiently check whether a block is present in this chain. */
    bool Contains(const CBlockIndex* pindex) const
    {
        return (*this)[pindex->nHeight] == pindex;
    }

    /** Find the successor of a block in this chain, or nullptr if the given index is not found or is the tip. */
    CBlockIndex* Next(const CBlockIndex* pindex) const
    {
        if (Contains(pindex))
            return (*this)[pindex->nHeight + 1];
        else
            return nullptr;
    }

    /** Return the maximal height in the chain. Is equal to chain.Tip() ? chain.Tip()->nHeight : -1. */
    int Height() const
    {
        return int(vChain.size()) - 1;
    }

    /**
     * Generation-2 active-chain compact-id accessors.
     *
     * The stable upstream-compatible pointer vector remains canonical. Compact
     * ids are already present in each CBlockIndex shell, so keeping a second
     * uint32_t vector for every active-chain height duplicated ~4 bytes/block
     * without adding information.
     */
    uint32_t CompactIdAt(int nHeight) const
    {
        const CBlockIndex* index{(*this)[nHeight]};
        return index ? index->m_compact_id : std::numeric_limits<uint32_t>::max();
    }

    size_t CompactIdCount() const { return vChain.size(); }

    bool CompactIdMirrorMatchesPointers() const
    {
        // Compatibility helper retained for existing callers/tests. There is
        // no separate mirror anymore; the pointer-owned shell is authoritative.
        return std::all_of(
            vChain.begin(),
            vChain.end(),
            [](const CBlockIndex* index) { return index != nullptr; });
    }

    bool CompactIdsComplete() const
    {
        return std::all_of(
            vChain.begin(),
            vChain.end(),
            [](const CBlockIndex* index) {
                return index &&
                       index->m_compact_id != std::numeric_limits<uint32_t>::max();
            });
    }

    /** Set/initialize a chain with a given tip. */
    void SetTip(CBlockIndex& block);

    /** Return a CBlockLocator that refers to the tip in of this chain. */
    CBlockLocator GetLocator() const;

    /** Find the last common block between this chain and a block index entry. */
    const CBlockIndex* FindFork(const CBlockIndex* pindex) const;

    /** Find the earliest block with timestamp equal or greater than the given time and height equal or greater than the given height. */
    CBlockIndex* FindEarliestAtLeast(int64_t nTime, int height) const;
};

/** Get a locator for a block index entry. */
CBlockLocator GetLocator(const CBlockIndex* index);

/** Construct a list of hash entries to put in a locator.  */
std::vector<uint256> LocatorEntries(const CBlockIndex* index);

/** DigiByte: Get mining algorithm for a block using consensus parameters */
int GetAlgoForBlockIndex(const CBlockIndex* blockindex, const Consensus::Params& consensus);


#endif // DIGIBYTE_CHAIN_H
