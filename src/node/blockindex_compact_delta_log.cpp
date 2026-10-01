// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockindex_compact_delta_log.h>

#include <util/fs_helpers.h>

#include <cstdio>
#include <exception>

namespace node {
namespace {

uint64_t ExpectedSize(const CompactBlockIndexDeltaLogHeader& header)
{
    return sizeof(CompactBlockIndexDeltaLogHeader) +
           header.record_count *
               static_cast<uint64_t>(sizeof(CompactBlockIndexDeltaLogRecord));
}

} // namespace

bool CompactBlockIndexDeltaLog::Create(
    const fs::path& path,
    uint64_t base_generation,
    uint64_t base_entry_count,
    uint64_t snapshot_tail_entry_count,
    const uint256& genesis_hash,
    std::string& error)
{
    fs::path tmp{path};
    tmp += ".tmp";

    CompactBlockIndexDeltaLogHeader header;
    header.base_generation = base_generation;
    header.base_entry_count = base_entry_count;
    header.snapshot_tail_entry_count = snapshot_tail_entry_count;
    header.genesis_hash = genesis_hash;

    try {
        FILE* file{fsbridge::fopen(tmp, "wb")};
        if (!file) {
            error = "cannot create compact metadata delta log temp file";
            return false;
        }

        const bool wrote{std::fwrite(&header, sizeof(header), 1, file) == 1};
        const bool committed{wrote && FileCommit(file)};
        const bool closed{std::fclose(file) == 0};
        if (!wrote || !committed || !closed) {
            fs::remove(tmp);
            error = "failed to write compact metadata delta log header";
            return false;
        }

        if (!RenameOver(tmp, path)) {
            fs::remove(tmp);
            error = "failed to publish compact metadata delta log";
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

bool CompactBlockIndexDeltaLog::Open(
    const fs::path& path,
    uint64_t expected_base_generation,
    uint64_t expected_base_entry_count,
    uint64_t expected_snapshot_tail_entry_count,
    const uint256& expected_genesis_hash,
    std::string& error)
{
    m_open = false;
    m_path.clear();
    m_header = {};

    try {
        FILE* file{fsbridge::fopen(path, "rb")};
        if (!file) {
            error = "cannot open compact metadata delta log";
            return false;
        }

        CompactBlockIndexDeltaLogHeader header;
        const bool read{std::fread(&header, sizeof(header), 1, file) == 1};
        const bool closed{std::fclose(file) == 0};
        if (!read || !closed) {
            error = "failed to read compact metadata delta log header";
            return false;
        }

        if (header.magic != COMPACT_BLOCK_INDEX_DELTA_LOG_MAGIC ||
            header.version != COMPACT_BLOCK_INDEX_DELTA_LOG_VERSION ||
            header.record_size != sizeof(CompactBlockIndexDeltaLogRecord)) {
            error = "compact metadata delta log format mismatch";
            return false;
        }

        if (header.base_generation != expected_base_generation ||
            header.base_entry_count != expected_base_entry_count ||
            header.snapshot_tail_entry_count != expected_snapshot_tail_entry_count ||
            header.genesis_hash != expected_genesis_hash) {
            error = "compact metadata delta log base/snapshot mismatch";
            return false;
        }

        const uint64_t expected_size{ExpectedSize(header)};
        const uint64_t actual_size{fs::file_size(path)};
        if (actual_size < expected_size) {
            error = "compact metadata delta log is truncated";
            return false;
        }

        // A crash after fsyncing record bytes but before publishing record_count
        // leaves an uncommitted suffix. The header count is authoritative.
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

bool CompactBlockIndexDeltaLog::ForEach(
    const std::function<bool(const CompactBlockIndexDeltaLogRecord&)>& fn,
    std::string& error) const
{
    if (!m_open) {
        error = "compact metadata delta log is not open";
        return false;
    }

    FILE* file{fsbridge::fopen(m_path, "rb")};
    if (!file) {
        error = "cannot reopen compact metadata delta log";
        return false;
    }

    if (std::fseek(file, sizeof(CompactBlockIndexDeltaLogHeader), SEEK_SET) != 0) {
        std::fclose(file);
        error = "cannot seek compact metadata delta log";
        return false;
    }

    for (uint64_t n = 0; n < m_header.record_count; ++n) {
        CompactBlockIndexDeltaLogRecord record;
        if (std::fread(&record, sizeof(record), 1, file) != 1) {
            std::fclose(file);
            error = "failed to read compact metadata delta log record";
            return false;
        }

        if (record.id == INVALID_BLOCK_INDEX_ID) {
            std::fclose(file);
            error = "compact metadata delta log contains invalid id";
            return false;
        }

        if (!fn(record)) {
            std::fclose(file);
            return true;
        }
    }

    if (std::fclose(file) != 0) {
        error = "failed to close compact metadata delta log";
        return false;
    }
    return true;
}

bool CompactBlockIndexDeltaLog::WriteHeader(FILE* file, std::string& error)
{
    if (std::fseek(file, 0, SEEK_SET) != 0 ||
        std::fwrite(&m_header, sizeof(m_header), 1, file) != 1 ||
        !FileCommit(file)) {
        error = "failed to publish compact metadata delta log header";
        return false;
    }
    return true;
}

bool CompactBlockIndexDeltaLog::Append(
    const std::vector<CompactBlockIndexDeltaLogRecord>& records,
    std::string& error)
{
    if (records.empty()) return true;
    if (!m_open) {
        error = "compact metadata delta log is not open";
        return false;
    }

    for (const CompactBlockIndexDeltaLogRecord& record : records) {
        if (record.id == INVALID_BLOCK_INDEX_ID) {
            error = "compact metadata delta log append contains invalid id";
            return false;
        }
    }

    FILE* file{fsbridge::fopen(m_path, "rb+")};
    if (!file) {
        error = "cannot open compact metadata delta log for append";
        return false;
    }

    const uint64_t append_offset{ExpectedSize(m_header)};
    if (std::fseek(file, static_cast<long>(append_offset), SEEK_SET) != 0) {
        std::fclose(file);
        error = "cannot seek compact metadata delta log append position";
        return false;
    }

    if (std::fwrite(
            records.data(),
            sizeof(CompactBlockIndexDeltaLogRecord),
            records.size(),
            file) != records.size()) {
        std::fclose(file);
        error = "failed to append compact metadata delta log records";
        return false;
    }

    if (!FileCommit(file)) {
        std::fclose(file);
        error = "failed to fsync compact metadata delta log records";
        return false;
    }

    m_header.record_count += records.size();
    if (!WriteHeader(file, error)) {
        std::fclose(file);
        return false;
    }

    if (std::fclose(file) != 0) {
        error = "failed to close compact metadata delta log after append";
        return false;
    }
    return true;
}

} // namespace node
