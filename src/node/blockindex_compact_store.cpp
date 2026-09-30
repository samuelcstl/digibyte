// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockindex_compact_store.h>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <util/fs.h>

#include <exception>

namespace node {

CompactBlockIndexStore::~CompactBlockIndexStore()
{
    Close();
}

bool CompactBlockIndexStore::Open(
    const fs::path& path,
    const uint256& expected_genesis,
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
        if (m_size_bytes < sizeof(CompactBlockIndexFileHeader)) {
            error = "compact block-index file is smaller than its header";
            Close();
            return false;
        }

        const auto* base{static_cast<const unsigned char*>(m_region->get_address())};
        m_header = reinterpret_cast<const CompactBlockIndexFileHeader*>(base);

        if (m_header->magic != COMPACT_BLOCK_INDEX_MAGIC) {
            error = "compact block-index magic mismatch";
            Close();
            return false;
        }
        if (m_header->version != COMPACT_BLOCK_INDEX_FORMAT_VERSION) {
            error = "compact block-index format version mismatch";
            Close();
            return false;
        }
        if (m_header->entry_size != sizeof(CompactBlockIndexEntry)) {
            error = "compact block-index entry-size mismatch";
            Close();
            return false;
        }
        if (m_header->genesis_hash != expected_genesis) {
            error = "compact block-index genesis mismatch";
            Close();
            return false;
        }

        const uint64_t expected_size{
            sizeof(CompactBlockIndexFileHeader) +
            m_header->entry_count * static_cast<uint64_t>(sizeof(CompactBlockIndexEntry))};
        if (expected_size != m_size_bytes) {
            error = "compact block-index file-size mismatch";
            Close();
            return false;
        }

        m_entries = reinterpret_cast<const CompactBlockIndexEntry*>(
            base + sizeof(CompactBlockIndexFileHeader));
        m_path = path;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        Close();
        return false;
    }
}

void CompactBlockIndexStore::Close()
{
    m_entries = nullptr;
    m_header = nullptr;
    m_size_bytes = 0;
    m_region.reset();
    m_mapping.reset();
    m_path.clear();
}

} // namespace node
