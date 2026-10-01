// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_NODE_BLOCKINDEX_COMPACT_DELTA_LOG_H
#define DIGIBYTE_NODE_BLOCKINDEX_COMPACT_DELTA_LOG_H

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

static constexpr uint32_t COMPACT_BLOCK_INDEX_DELTA_LOG_VERSION{1};
static constexpr std::array<unsigned char, 8> COMPACT_BLOCK_INDEX_DELTA_LOG_MAGIC{
    'D', 'G', 'B', 'C', 'D', 'J', '1', '\0'};

struct CompactBlockIndexDeltaLogHeader
{
    std::array<unsigned char, 8> magic{COMPACT_BLOCK_INDEX_DELTA_LOG_MAGIC};
    uint32_t version{COMPACT_BLOCK_INDEX_DELTA_LOG_VERSION};
    uint32_t record_size{168};
    uint64_t base_generation{0};
    uint64_t base_entry_count{0};
    uint64_t snapshot_tail_entry_count{0};
    uint64_t record_count{0};
    uint256 genesis_hash{};
    uint64_t reserved[6]{};
};

struct CompactBlockIndexDeltaLogRecord
{
    BlockIndexId id{INVALID_BLOCK_INDEX_ID};
    uint32_t reserved{0};
    CompactBlockIndexEntry entry{};
};

static_assert(sizeof(CompactBlockIndexDeltaLogHeader) == 128);
static_assert(sizeof(CompactBlockIndexDeltaLogRecord) == 168);

/**
 * Append-only full-record updates layered over CompactBlockIndexDelta.
 *
 * Multiple records for the same id are allowed; replay order is authoritative
 * and the last published record wins. A record may also introduce the next
 * tail id after the snapshot, allowing normal tip growth without rewriting the
 * existing delta snapshot.
 *
 * Publication is data-first: update records are fsynced before record_count is
 * advanced and fsynced in the header. A crash can therefore leave only an
 * uncommitted byte suffix, which Open() discards.
 */
class CompactBlockIndexDeltaLog
{
public:
    static bool Create(
        const fs::path& path,
        uint64_t base_generation,
        uint64_t base_entry_count,
        uint64_t snapshot_tail_entry_count,
        const uint256& genesis_hash,
        std::string& error);

    bool Open(
        const fs::path& path,
        uint64_t expected_base_generation,
        uint64_t expected_base_entry_count,
        uint64_t expected_snapshot_tail_entry_count,
        const uint256& expected_genesis_hash,
        std::string& error);

    [[nodiscard]] bool IsOpen() const noexcept { return m_open; }
    [[nodiscard]] uint64_t RecordCount() const noexcept { return m_header.record_count; }
    [[nodiscard]] uint64_t SizeBytes() const noexcept
    {
        return sizeof(CompactBlockIndexDeltaLogHeader) +
               m_header.record_count * sizeof(CompactBlockIndexDeltaLogRecord);
    }

    bool ForEach(
        const std::function<bool(const CompactBlockIndexDeltaLogRecord&)>& fn,
        std::string& error) const;

    bool Append(
        const std::vector<CompactBlockIndexDeltaLogRecord>& records,
        std::string& error);

private:
    bool WriteHeader(FILE* file, std::string& error);

    fs::path m_path;
    CompactBlockIndexDeltaLogHeader m_header{};
    bool m_open{false};
};

} // namespace node

#endif // DIGIBYTE_NODE_BLOCKINDEX_COMPACT_DELTA_LOG_H
