// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockindex_compact_ids.h>

#include <util/fs_helpers.h>

#include <cstdio>
#include <exception>

namespace node {
namespace {

uint64_t ExpectedSize(const CompactBlockIndexIdsHeader& header)
{
    return sizeof(CompactBlockIndexIdsHeader) +
           header.tail_entry_count * static_cast<uint64_t>(sizeof(uint256));
}

} // namespace

bool CompactBlockIndexIds::Create(
    const fs::path& path,
    uint64_t base_generation,
    uint64_t base_entry_count,
    const uint256& genesis_hash,
    std::string& error)
{
    fs::path tmp{path};
    tmp += ".tmp";

    CompactBlockIndexIdsHeader header;
    header.base_generation = base_generation;
    header.base_entry_count = base_entry_count;
    header.tail_entry_count = 0;
    header.genesis_hash = genesis_hash;

    try {
        FILE* file{fsbridge::fopen(tmp, "wb")};
        if (!file) {
            error = "cannot create compact-id tail temp file";
            return false;
        }

        const bool wrote{std::fwrite(&header, sizeof(header), 1, file) == 1};
        const bool committed{wrote && FileCommit(file)};
        const bool closed{std::fclose(file) == 0};

        if (!wrote || !committed || !closed) {
            fs::remove(tmp);
            error = "failed to write compact-id tail header";
            return false;
        }

        if (!RenameOver(tmp, path)) {
            fs::remove(tmp);
            error = "failed to publish compact-id tail";
            return false;
        }

        DirectoryCommit(path.parent_path());
        return true;
    } catch (const std::exception& e) {
        fs::remove(tmp);
        error = e.what();
        return false;
    }
}

bool CompactBlockIndexIds::Open(
    const fs::path& path,
    uint64_t expected_base_generation,
    uint64_t expected_base_entry_count,
    const uint256& expected_genesis_hash,
    std::string& error)
{
    m_open = false;
    m_path.clear();
    m_header = {};

    try {
        FILE* file{fsbridge::fopen(path, "rb")};
        if (!file) {
            error = "cannot open compact-id tail";
            return false;
        }

        CompactBlockIndexIdsHeader header;
        const bool read{std::fread(&header, sizeof(header), 1, file) == 1};
        const bool closed{std::fclose(file) == 0};
        if (!read || !closed) {
            error = "failed to read compact-id tail header";
            return false;
        }

        if (header.magic != COMPACT_BLOCK_INDEX_IDS_MAGIC ||
            header.version != COMPACT_BLOCK_INDEX_IDS_VERSION ||
            header.record_size != sizeof(uint256)) {
            error = "compact-id tail format mismatch";
            return false;
        }

        if (header.base_generation != expected_base_generation ||
            header.base_entry_count != expected_base_entry_count ||
            header.genesis_hash != expected_genesis_hash) {
            error = "compact-id tail base-generation mismatch";
            return false;
        }

        if (header.base_entry_count + header.tail_entry_count >=
            static_cast<uint64_t>(INVALID_BLOCK_INDEX_ID)) {
            error = "compact-id tail exceeds 32-bit id space";
            return false;
        }

        const uint64_t expected_size{ExpectedSize(header)};
        const uint64_t actual_size{fs::file_size(path)};
        if (actual_size < expected_size) {
            error = "compact-id tail is truncated";
            return false;
        }

        // A crash between the data fsync and header-count publication can
        // leave an uncommitted byte suffix. The header is authoritative.
        if (actual_size > expected_size) {
            fs::resize_file(path, expected_size);
        }

        m_path = path;
        m_header = header;
        m_open = true;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool CompactBlockIndexIds::ForEachTail(
    const std::function<bool(BlockIndexId, const uint256&)>& fn,
    std::string& error) const
{
    if (!m_open) {
        error = "compact-id tail is not open";
        return false;
    }

    FILE* file{fsbridge::fopen(m_path, "rb")};
    if (!file) {
        error = "cannot reopen compact-id tail";
        return false;
    }

    if (std::fseek(file, sizeof(CompactBlockIndexIdsHeader), SEEK_SET) != 0) {
        std::fclose(file);
        error = "cannot seek compact-id tail";
        return false;
    }

    for (uint64_t n = 0; n < m_header.tail_entry_count; ++n) {
        uint256 hash;
        if (std::fread(&hash, sizeof(hash), 1, file) != 1) {
            std::fclose(file);
            error = "failed to read compact-id tail record";
            return false;
        }

        const uint64_t raw_id{m_header.base_entry_count + n};
        if (raw_id >= static_cast<uint64_t>(INVALID_BLOCK_INDEX_ID)) {
            std::fclose(file);
            error = "compact-id tail record exceeds 32-bit id space";
            return false;
        }

        if (!fn(static_cast<BlockIndexId>(raw_id), hash)) {
            std::fclose(file);
            return true;
        }
    }

    if (std::fclose(file) != 0) {
        error = "failed to close compact-id tail";
        return false;
    }
    return true;
}

bool CompactBlockIndexIds::WriteHeader(FILE* file, std::string& error)
{
    if (std::fseek(file, 0, SEEK_SET) != 0 ||
        std::fwrite(&m_header, sizeof(m_header), 1, file) != 1 ||
        !FileCommit(file)) {
        error = "failed to publish compact-id tail header";
        return false;
    }
    return true;
}

bool CompactBlockIndexIds::Append(
    const std::vector<uint256>& hashes,
    std::string& error)
{
    if (hashes.empty()) return true;
    if (!m_open) {
        error = "compact-id tail is not open";
        return false;
    }

    if (NextId() + hashes.size() >= static_cast<uint64_t>(INVALID_BLOCK_INDEX_ID)) {
        error = "compact-id tail append exceeds 32-bit id space";
        return false;
    }

    FILE* file{fsbridge::fopen(m_path, "rb+")};
    if (!file) {
        error = "cannot open compact-id tail for append";
        return false;
    }

    const uint64_t append_offset{ExpectedSize(m_header)};
    if (std::fseek(file, static_cast<long>(append_offset), SEEK_SET) != 0) {
        std::fclose(file);
        error = "cannot seek compact-id tail append position";
        return false;
    }

    for (const uint256& hash : hashes) {
        if (std::fwrite(&hash, sizeof(hash), 1, file) != 1) {
            std::fclose(file);
            error = "failed to append compact-id tail record";
            return false;
        }
    }

    // Data becomes durable before the published record count advances.
    if (!FileCommit(file)) {
        std::fclose(file);
        error = "failed to fsync compact-id tail records";
        return false;
    }

    m_header.tail_entry_count += hashes.size();
    if (!WriteHeader(file, error)) {
        std::fclose(file);
        return false;
    }

    if (std::fclose(file) != 0) {
        error = "failed to close compact-id tail after append";
        return false;
    }
    return true;
}

bool CompactBlockIndexIds::TruncateTail(
    uint64_t tail_entry_count,
    std::string& error)
{
    if (!m_open) {
        error = "compact-id tail is not open";
        return false;
    }
    if (tail_entry_count > m_header.tail_entry_count) {
        error = "cannot grow compact-id tail through truncate";
        return false;
    }

    FILE* file{fsbridge::fopen(m_path, "rb+")};
    if (!file) {
        error = "cannot open compact-id tail for truncate";
        return false;
    }

    m_header.tail_entry_count = tail_entry_count;
    if (!WriteHeader(file, error)) {
        std::fclose(file);
        return false;
    }

    if (std::fclose(file) != 0) {
        error = "failed to close compact-id tail after truncate";
        return false;
    }

    try {
        fs::resize_file(m_path, ExpectedSize(m_header));
        FILE* committed{fsbridge::fopen(m_path, "rb+")};
        if (!committed) {
            error = "cannot reopen truncated compact-id tail";
            return false;
        }
        const bool ok{FileCommit(committed)};
        const bool closed{std::fclose(committed) == 0};
        if (!ok || !closed) {
            error = "failed to fsync truncated compact-id tail";
            return false;
        }
        DirectoryCommit(m_path.parent_path());
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace node
