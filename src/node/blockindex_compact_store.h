// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DIGIBYTE_NODE_BLOCKINDEX_COMPACT_STORE_H
#define DIGIBYTE_NODE_BLOCKINDEX_COMPACT_STORE_H

#include <node/blockindex_compact.h>
#include <uint256.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace boost::interprocess {
class file_mapping;
class mapped_region;
}

namespace node {

/**
 * Read-only file-backed view of the compact historical block index.
 *
 * Mapping the file reserves virtual address space but does not make the whole
 * historical index anonymous/resident. Pages are faulted by the OS as touched
 * and remain reclaimable file-backed cache.
 */
class CompactBlockIndexStore
{
public:
    CompactBlockIndexStore();
    ~CompactBlockIndexStore();

    CompactBlockIndexStore(const CompactBlockIndexStore&) = delete;
    CompactBlockIndexStore& operator=(const CompactBlockIndexStore&) = delete;

    bool Open(const fs::path& path, const uint256& expected_genesis, std::string& error);
    void Close();

    [[nodiscard]] bool IsOpen() const noexcept { return m_entries != nullptr; }
    [[nodiscard]] uint64_t EntryCount() const noexcept
    {
        return m_header ? m_header->entry_count : 0;
    }
    [[nodiscard]] uint64_t SizeBytes() const noexcept { return m_size_bytes; }
    [[nodiscard]] const fs::path& Path() const noexcept { return m_path; }

    [[nodiscard]] const CompactBlockIndexFileHeader* Header() const noexcept
    {
        return m_header;
    }

    [[nodiscard]] const CompactBlockIndexEntry* Get(BlockIndexId id) const noexcept
    {
        if (!m_entries || id == INVALID_BLOCK_INDEX_ID || id >= EntryCount()) return nullptr;
        return &m_entries[id];
    }

private:
    fs::path m_path;
    std::unique_ptr<boost::interprocess::file_mapping> m_mapping;
    std::unique_ptr<boost::interprocess::mapped_region> m_region;
    const CompactBlockIndexFileHeader* m_header{nullptr};
    const CompactBlockIndexEntry* m_entries{nullptr};
    uint64_t m_size_bytes{0};
};

} // namespace node

#endif // DIGIBYTE_NODE_BLOCKINDEX_COMPACT_STORE_H
