// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockindex_compact_delta.h>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <util/fs_helpers.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <unordered_map>

namespace node {

CompactBlockIndexDelta::CompactBlockIndexDelta() = default;

CompactBlockIndexDelta::~CompactBlockIndexDelta()
{
    Close();
}

bool CompactBlockIndexDelta::Build(
    const fs::path& path,
    uint64_t base_generation,
    uint64_t base_entry_count,
    const uint256& genesis_hash,
    const std::vector<CompactBlockIndexEntry>& entries,
    std::string& error)
{
    if (base_entry_count + entries.size() >=
        static_cast<uint64_t>(INVALID_BLOCK_INDEX_ID)) {
        error = "compact metadata delta exceeds 32-bit id space";
        return false;
    }

    fs::path tmp{path};
    tmp += ".tmp";

    CompactBlockIndexDeltaHeader header;
    header.base_generation = base_generation;
    header.base_entry_count = base_entry_count;
    header.tail_entry_count = entries.size();
    header.genesis_hash = genesis_hash;

    try {
        FILE* file{fsbridge::fopen(tmp, "wb")};
        if (!file) {
            error = "cannot create compact metadata delta temp file";
            return false;
        }

        bool ok{std::fwrite(&header, sizeof(header), 1, file) == 1};
        if (ok && !entries.empty()) {
            ok = std::fwrite(
                     entries.data(),
                     sizeof(CompactBlockIndexEntry),
                     entries.size(),
                     file) == entries.size();
        }
        if (ok) ok = FileCommit(file);

        const bool closed{std::fclose(file) == 0};
        if (!ok || !closed) {
            fs::remove(tmp);
            error = "failed to write compact metadata delta";
            return false;
        }

        if (!RenameOver(tmp, path)) {
            fs::remove(tmp);
            error = "failed to publish compact metadata delta";
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

bool CompactBlockIndexDelta::Open(
    const fs::path& path,
    uint64_t expected_base_generation,
    uint64_t expected_base_entry_count,
    const uint256& expected_genesis_hash,
    std::string& error)
{
    Close();

    try {
        const std::string path_string{fs::PathToString(path)};
        m_mapping = std::make_unique<boost::interprocess::file_mapping>(
            path_string.c_str(), boost::interprocess::read_only);
        m_region = std::make_unique<boost::interprocess::mapped_region>(
            *m_mapping, boost::interprocess::read_only);

        m_size_bytes = m_region->get_size();
        if (m_size_bytes < sizeof(CompactBlockIndexDeltaHeader)) {
            error = "compact metadata delta is smaller than its header";
            Close();
            return false;
        }

        const auto* base{
            static_cast<const unsigned char*>(m_region->get_address())};
        m_header = reinterpret_cast<const CompactBlockIndexDeltaHeader*>(base);

        if (m_header->magic != COMPACT_BLOCK_INDEX_DELTA_MAGIC ||
            m_header->version != COMPACT_BLOCK_INDEX_DELTA_VERSION ||
            m_header->entry_size != sizeof(CompactBlockIndexEntry)) {
            error = "compact metadata delta format mismatch";
            Close();
            return false;
        }

        if (m_header->base_generation != expected_base_generation ||
            m_header->base_entry_count != expected_base_entry_count ||
            m_header->genesis_hash != expected_genesis_hash) {
            error = "compact metadata delta base-generation mismatch";
            Close();
            return false;
        }

        if (m_header->base_entry_count + m_header->tail_entry_count >=
            static_cast<uint64_t>(INVALID_BLOCK_INDEX_ID)) {
            error = "compact metadata delta exceeds 32-bit id space";
            Close();
            return false;
        }

        const uint64_t expected_size{
            sizeof(CompactBlockIndexDeltaHeader) +
            m_header->tail_entry_count *
                static_cast<uint64_t>(sizeof(CompactBlockIndexEntry))};
        if (expected_size != m_size_bytes) {
            error = "compact metadata delta file-size mismatch";
            Close();
            return false;
        }

        m_entries = reinterpret_cast<const CompactBlockIndexEntry*>(
            base + sizeof(CompactBlockIndexDeltaHeader));
        m_path = path;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        Close();
        return false;
    }
}

bool CompactBlockIndexDelta::PlanCompaction(
    const CompactBlockIndexDelta& snapshot,
    const CompactBlockIndexDeltaLog& log,
    uint64_t expected_next_id,
    CompactBlockIndexDeltaCompaction& result,
    std::string& error)
{
    result = {};

    if (!snapshot.IsOpen() || !log.IsOpen()) {
        error = "compact metadata compaction source is not open";
        return false;
    }

    if (snapshot.BaseGeneration() != log.BaseGeneration() ||
        snapshot.BaseEntryCount() != log.BaseEntryCount() ||
        snapshot.TailEntryCount() != log.SnapshotTailEntryCount() ||
        snapshot.GenesisHash() != log.GenesisHash()) {
        error = "compact metadata compaction source binding mismatch";
        return false;
    }

    const uint64_t base_count{snapshot.BaseEntryCount()};
    const uint64_t snapshot_next{base_count + snapshot.TailEntryCount()};
    if (expected_next_id < snapshot_next ||
        expected_next_id >= static_cast<uint64_t>(INVALID_BLOCK_INDEX_ID)) {
        error = "compact metadata compaction next id is outside valid range";
        return false;
    }

    std::unordered_map<BlockIndexId, CompactBlockIndexDeltaLogRecord> latest;
    latest.reserve(static_cast<size_t>(log.RecordCount()));

    if (!log.ForEach(
            [&](const CompactBlockIndexDeltaLogRecord& record) {
                if (static_cast<uint64_t>(record.id) >= expected_next_id) {
                    error = "compact metadata compaction log references unpublished id";
                    return false;
                }
                latest[record.id] = record;
                return true;
            },
            error)) {
        if (error.empty()) {
            error = "compact metadata compaction log replay failed";
        }
        return false;
    }

    result.tail_entries.reserve(static_cast<size_t>(expected_next_id - base_count));
    for (uint64_t raw_id = base_count; raw_id < expected_next_id; ++raw_id) {
        const BlockIndexId id{static_cast<BlockIndexId>(raw_id)};
        const auto update{latest.find(id)};
        if (update != latest.end()) {
            result.tail_entries.push_back(update->second.entry);
            continue;
        }

        const CompactBlockIndexEntry* snapshot_entry{snapshot.Get(id)};
        if (!snapshot_entry) {
            error = "compact metadata compaction is missing a tail extension";
            result = {};
            return false;
        }
        result.tail_entries.push_back(*snapshot_entry);
    }

    for (const auto& [id, record] : latest) {
        if (static_cast<uint64_t>(id) < base_count) {
            result.base_updates.push_back(record);
        }
    }
    std::sort(
        result.base_updates.begin(),
        result.base_updates.end(),
        [](const CompactBlockIndexDeltaLogRecord& a,
           const CompactBlockIndexDeltaLogRecord& b) {
            return a.id < b.id;
        });

    return true;
}

void CompactBlockIndexDelta::Close()
{
    m_entries = nullptr;
    m_header = nullptr;
    m_size_bytes = 0;
    m_region.reset();
    m_mapping.reset();
    m_path.clear();
}

} // namespace node
