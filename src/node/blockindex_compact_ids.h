// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_NODE_BLOCKINDEX_COMPACT_IDS_H
#define DIGIBYTE_NODE_BLOCKINDEX_COMPACT_IDS_H

#include <node/blockindex_compact.h>
#include <uint256.h>
#include <util/fs.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace node {

static constexpr uint32_t COMPACT_BLOCK_INDEX_IDS_VERSION{1};
static constexpr std::array<unsigned char, 8> COMPACT_BLOCK_INDEX_IDS_MAGIC{
    'D', 'G', 'B', 'C', 'I', 'D', '1', '\0'};

struct CompactBlockIndexIdsHeader
{
    std::array<unsigned char, 8> magic{COMPACT_BLOCK_INDEX_IDS_MAGIC};
    uint32_t version{COMPACT_BLOCK_INDEX_IDS_VERSION};
    uint32_t record_size{sizeof(uint256)};
    uint64_t base_generation{0};
    uint64_t base_entry_count{0};
    uint64_t tail_entry_count{0};
    uint256 genesis_hash{};
    uint64_t reserved[7]{};
};

static_assert(sizeof(CompactBlockIndexIdsHeader) == 128);

/**
 * Append-only compact-id identity tail.
 *
 * IDs below base_entry_count are owned by the immutable compact generation.
 * Tail hashes are stored in id order, so the id of record n is
 * base_entry_count + n. The file is a derived sidecar and never replaces the
 * ordinary upstream block-index database.
 *
 * Append publication is data-first: tail bytes are fsynced before the header
 * count is advanced. Therefore a crash may leave an uncommitted byte suffix,
 * but can never publish a count whose record bytes were not already durable.
 */
class CompactBlockIndexIds
{
public:
    static bool Create(
        const fs::path& path,
        uint64_t base_generation,
        uint64_t base_entry_count,
        const uint256& genesis_hash,
        std::string& error);

    bool Open(
        const fs::path& path,
        uint64_t expected_base_generation,
        uint64_t expected_base_entry_count,
        const uint256& expected_genesis_hash,
        std::string& error);

    [[nodiscard]] bool IsOpen() const noexcept { return m_open; }
    [[nodiscard]] uint64_t BaseEntryCount() const noexcept { return m_header.base_entry_count; }
    [[nodiscard]] uint64_t TailEntryCount() const noexcept { return m_header.tail_entry_count; }
    [[nodiscard]] uint64_t NextId() const noexcept
    {
        return m_header.base_entry_count + m_header.tail_entry_count;
    }
    [[nodiscard]] uint64_t SizeBytes() const noexcept
    {
        return sizeof(CompactBlockIndexIdsHeader) +
               m_header.tail_entry_count * sizeof(uint256);
    }

    bool ForEachTail(
        const std::function<bool(BlockIndexId, const uint256&)>& fn,
        std::string& error) const;

    bool Append(const std::vector<uint256>& hashes, std::string& error);
    bool TruncateTail(uint64_t tail_entry_count, std::string& error);

private:
    bool WriteHeader(FILE* file, std::string& error);

    fs::path m_path;
    CompactBlockIndexIdsHeader m_header{};
    bool m_open{false};
};

} // namespace node

#endif // DIGIBYTE_NODE_BLOCKINDEX_COMPACT_IDS_H
