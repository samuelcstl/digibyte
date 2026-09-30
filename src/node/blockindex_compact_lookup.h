// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_NODE_BLOCKINDEX_COMPACT_LOOKUP_H
#define DIGIBYTE_NODE_BLOCKINDEX_COMPACT_LOOKUP_H

#include <node/blockindex_compact.h>
#include <uint256.h>
#include <util/fs.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace boost::interprocess {
class file_mapping;
class mapped_region;
}

namespace node {

class CompactBlockIndexStore;

static constexpr uint32_t COMPACT_BLOCK_INDEX_LOOKUP_VERSION{1};
static constexpr std::array<unsigned char, 8> COMPACT_BLOCK_INDEX_LOOKUP_MAGIC{
    'D', 'G', 'B', 'C', 'B', 'L', '1', '\0'};

struct CompactBlockIndexLookupHeader
{
    std::array<unsigned char, 8> magic{COMPACT_BLOCK_INDEX_LOOKUP_MAGIC};
    uint32_t version{COMPACT_BLOCK_INDEX_LOOKUP_VERSION};
    uint32_t slot_size{16};
    uint64_t slot_count{0};
    uint64_t entry_count{0};
    uint64_t source_generation{0};
    uint64_t source_size{0};
    uint64_t k0{0};
    uint64_t k1{0};
    uint256 genesis_hash{};
    uint64_t reserved[4]{};
};

static_assert(sizeof(CompactBlockIndexLookupHeader) == 128);

struct CompactBlockIndexLookupSlot
{
    uint64_t fingerprint{0};
    BlockIndexId id{INVALID_BLOCK_INDEX_ID};
    uint32_t reserved{0};
};

static_assert(sizeof(CompactBlockIndexLookupSlot) == 16);

/**
 * Persistent open-addressing hash -> BlockIndexId table.
 *
 * The table stores a keyed 64-bit SipHash fingerprint and the compact id. A
 * fingerprint match is always verified against the authoritative full 256-bit
 * hash in CompactBlockIndexStore before a result is returned.
 *
 * The lookup file is mmap-backed. It is a derived cache and can always be
 * rebuilt from the compact store.
 */
class CompactBlockIndexLookup
{
public:
    CompactBlockIndexLookup();
    ~CompactBlockIndexLookup();

    CompactBlockIndexLookup(const CompactBlockIndexLookup&) = delete;
    CompactBlockIndexLookup& operator=(const CompactBlockIndexLookup&) = delete;

    static bool Build(
        const fs::path& path,
        const CompactBlockIndexStore& source,
        std::string& error);

    bool Open(
        const fs::path& path,
        const CompactBlockIndexStore& source,
        std::string& error);

    void Close();

    [[nodiscard]] bool IsOpen() const noexcept { return m_slots != nullptr; }
    [[nodiscard]] uint64_t SlotCount() const noexcept
    {
        return m_header ? m_header->slot_count : 0;
    }
    [[nodiscard]] uint64_t EntryCount() const noexcept
    {
        return m_header ? m_header->entry_count : 0;
    }
    [[nodiscard]] uint64_t SizeBytes() const noexcept { return m_size_bytes; }

    [[nodiscard]] std::optional<BlockIndexId> Find(
        const uint256& hash,
        const CompactBlockIndexStore& source,
        uint32_t* probes = nullptr) const noexcept;

    /**
     * Copy the keyed fingerprint/occupancy probe surface into anonymous
     * resident memory. FindResident() can then reject unknown hashes without
     * touching the mmap-backed slot/id table or compact record store.
     */
    bool LoadResidentProbeFront(std::string& error);

    [[nodiscard]] bool HasResidentProbeFront() const noexcept
    {
        return !m_resident_fingerprints.empty();
    }

    [[nodiscard]] uint64_t ResidentProbeFrontBytes() const noexcept
    {
        return static_cast<uint64_t>(m_resident_fingerprints.size()) * sizeof(uint64_t) +
               static_cast<uint64_t>(m_resident_occupancy.size()) * sizeof(uint64_t);
    }

    [[nodiscard]] std::optional<BlockIndexId> FindResident(
        const uint256& hash,
        const CompactBlockIndexStore& source,
        uint32_t* probes = nullptr,
        bool* touched_backing = nullptr) const noexcept;

private:
    fs::path m_path;
    std::unique_ptr<boost::interprocess::file_mapping> m_mapping;
    std::unique_ptr<boost::interprocess::mapped_region> m_region;
    const CompactBlockIndexLookupHeader* m_header{nullptr};
    const CompactBlockIndexLookupSlot* m_slots{nullptr};
    uint64_t m_size_bytes{0};

    // Anonymous resident negative-lookup surface. The exact id and full hash
    // remain file-backed and are touched only after a keyed fingerprint match.
    std::vector<uint64_t> m_resident_fingerprints;
    std::vector<uint64_t> m_resident_occupancy;
    uint64_t m_resident_slot_count{0};
    uint64_t m_resident_mask{0};
    uint64_t m_resident_k0{0};
    uint64_t m_resident_k1{0};
};

} // namespace node

#endif // DIGIBYTE_NODE_BLOCKINDEX_COMPACT_LOOKUP_H
