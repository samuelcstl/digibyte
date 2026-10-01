// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_NODE_BLOCKINDEX_COMPACT_DELTA_H
#define DIGIBYTE_NODE_BLOCKINDEX_COMPACT_DELTA_H

#include <node/blockindex_compact.h>
#include <uint256.h>
#include <util/fs.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace boost::interprocess {
class file_mapping;
class mapped_region;
}

namespace node {

static constexpr uint32_t COMPACT_BLOCK_INDEX_DELTA_VERSION{1};
static constexpr std::array<unsigned char, 8> COMPACT_BLOCK_INDEX_DELTA_MAGIC{
    'D', 'G', 'B', 'C', 'D', 'L', '1', '\0'};

struct CompactBlockIndexDeltaHeader
{
    std::array<unsigned char, 8> magic{COMPACT_BLOCK_INDEX_DELTA_MAGIC};
    uint32_t version{COMPACT_BLOCK_INDEX_DELTA_VERSION};
    uint32_t entry_size{sizeof(CompactBlockIndexEntry)};
    uint64_t base_generation{0};
    uint64_t base_entry_count{0};
    uint64_t tail_entry_count{0};
    uint256 genesis_hash{};
    uint64_t reserved[7]{};
};

static_assert(sizeof(CompactBlockIndexDeltaHeader) == 128);

/**
 * Full-metadata compact tail for ids after an immutable compact generation.
 *
 * Record position defines identity:
 *   id = base_entry_count + tail_offset
 *
 * This first slice is intentionally an atomic snapshot/read-only mapping. Live
 * update semantics are added separately so their crash ordering can be tested
 * without coupling them to the file-format proof.
 */
class CompactBlockIndexDelta
{
public:
    CompactBlockIndexDelta();
    ~CompactBlockIndexDelta();

    CompactBlockIndexDelta(const CompactBlockIndexDelta&) = delete;
    CompactBlockIndexDelta& operator=(const CompactBlockIndexDelta&) = delete;

    static bool Build(
        const fs::path& path,
        uint64_t base_generation,
        uint64_t base_entry_count,
        const uint256& genesis_hash,
        const std::vector<CompactBlockIndexEntry>& entries,
        std::string& error);

    bool Open(
        const fs::path& path,
        uint64_t expected_base_generation,
        uint64_t expected_base_entry_count,
        const uint256& expected_genesis_hash,
        std::string& error);

    void Close();

    [[nodiscard]] bool IsOpen() const noexcept { return m_entries != nullptr; }
    [[nodiscard]] uint64_t BaseEntryCount() const noexcept
    {
        return m_header ? m_header->base_entry_count : 0;
    }
    [[nodiscard]] uint64_t TailEntryCount() const noexcept
    {
        return m_header ? m_header->tail_entry_count : 0;
    }
    [[nodiscard]] uint64_t SizeBytes() const noexcept { return m_size_bytes; }

    [[nodiscard]] const CompactBlockIndexEntry* Get(BlockIndexId id) const noexcept
    {
        if (!m_header || !m_entries) return nullptr;
        if (id < m_header->base_entry_count) return nullptr;

        const uint64_t offset{
            static_cast<uint64_t>(id) - m_header->base_entry_count};
        if (offset >= m_header->tail_entry_count) return nullptr;
        return &m_entries[offset];
    }

private:
    fs::path m_path;
    std::unique_ptr<boost::interprocess::file_mapping> m_mapping;
    std::unique_ptr<boost::interprocess::mapped_region> m_region;
    const CompactBlockIndexDeltaHeader* m_header{nullptr};
    const CompactBlockIndexEntry* m_entries{nullptr};
    uint64_t m_size_bytes{0};
};

} // namespace node

#endif // DIGIBYTE_NODE_BLOCKINDEX_COMPACT_DELTA_H
