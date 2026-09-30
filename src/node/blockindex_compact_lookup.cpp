// Copyright (c) 2026 The DigiByte Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/blockindex_compact_lookup.h>

#include <crypto/siphash.h>
#include <node/blockindex_compact_store.h>
#include <random.h>
#include <util/fs.h>
#include <util/fs_helpers.h>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

#include <cstring>
#include <exception>
#include <limits>

namespace node {
namespace {

uint64_t NextPowerOfTwo(uint64_t value)
{
    if (value <= 1) return 1;
    --value;
    for (unsigned shift = 1; shift < 64; shift <<= 1) {
        value |= value >> shift;
    }
    return value + 1;
}

uint64_t LookupCapacity(uint64_t entry_count)
{
    // Keep load <= 75%. Current mainnet (~24.3M entries) therefore uses
    // 2^25 slots, or 512 MiB at 16 bytes per slot.
    const uint64_t target{entry_count + (entry_count + 2) / 3};
    return NextPowerOfTwo(target);
}

uint64_t Fingerprint(uint64_t k0, uint64_t k1, const uint256& hash)
{
    return SipHashUint256(k0, k1, hash);
}

} // namespace

CompactBlockIndexLookup::CompactBlockIndexLookup() = default;

CompactBlockIndexLookup::~CompactBlockIndexLookup()
{
    Close();
}

bool CompactBlockIndexLookup::Build(
    const fs::path& path,
    const CompactBlockIndexStore& source,
    std::string& error)
{
    if (!source.IsOpen() || !source.Header()) {
        error = "compact source store is not open";
        return false;
    }
    if (source.EntryCount() >= static_cast<uint64_t>(INVALID_BLOCK_INDEX_ID)) {
        error = "compact source exceeds 32-bit id space";
        return false;
    }

    const uint64_t slot_count{LookupCapacity(source.EntryCount())};
    if (slot_count == 0 || (slot_count & (slot_count - 1)) != 0) {
        error = "invalid lookup slot count";
        return false;
    }

    CompactBlockIndexLookupHeader header;
    header.slot_count = slot_count;
    header.entry_count = source.EntryCount();
    header.source_generation = source.Header()->generation;
    header.source_size = source.SizeBytes();
    header.genesis_hash = source.Header()->genesis_hash;

    FastRandomContext rng;
    header.k0 = rng.rand64();
    header.k1 = rng.rand64();

    fs::path tmp{path};
    tmp += ".tmp";

    try {
        {
            FILE* file{fsbridge::fopen(tmp, "wb")};
            if (!file) {
                error = "cannot create compact lookup temp file";
                return false;
            }
            const bool wrote_header{
                std::fwrite(&header, sizeof(header), 1, file) == 1};
            const bool closed{std::fclose(file) == 0};
            if (!wrote_header || !closed) {
                fs::remove(tmp);
                error = "failed to write compact lookup header";
                return false;
            }
        }

        const uint64_t slots_bytes{
            slot_count * static_cast<uint64_t>(sizeof(CompactBlockIndexLookupSlot))};
        const uint64_t total_bytes{
            sizeof(CompactBlockIndexLookupHeader) + slots_bytes};

        fs::resize_file(tmp, total_bytes);

        {
            const std::string tmp_string{fs::PathToString(tmp)};
            boost::interprocess::file_mapping mapping(
                tmp_string.c_str(), boost::interprocess::read_write);
            boost::interprocess::mapped_region region(
                mapping, boost::interprocess::read_write);

            auto* base{static_cast<unsigned char*>(region.get_address())};
            auto* mapped_header{
                reinterpret_cast<CompactBlockIndexLookupHeader*>(base)};
            *mapped_header = header;

            auto* slots{reinterpret_cast<CompactBlockIndexLookupSlot*>(
                base + sizeof(CompactBlockIndexLookupHeader))};

            std::memset(slots, 0xff, slots_bytes);

            const uint64_t mask{slot_count - 1};
            for (uint64_t raw_id = 0; raw_id < source.EntryCount(); ++raw_id) {
                const BlockIndexId id{static_cast<BlockIndexId>(raw_id)};
                const CompactBlockIndexEntry* entry{source.Get(id)};
                if (!entry) {
                    error = "compact source entry missing while building lookup";
                    fs::remove(tmp);
                    return false;
                }

                const uint64_t fp{Fingerprint(header.k0, header.k1, entry->hash)};
                uint64_t pos{fp & mask};

                bool inserted{false};
                for (uint64_t probe = 0; probe < slot_count; ++probe) {
                    CompactBlockIndexLookupSlot& slot{slots[pos]};
                    if (slot.id == INVALID_BLOCK_INDEX_ID) {
                        slot.fingerprint = fp;
                        slot.id = id;
                        slot.reserved = 0;
                        inserted = true;
                        break;
                    }
                    pos = (pos + 1) & mask;
                }

                if (!inserted) {
                    error = "compact lookup table unexpectedly full";
                    fs::remove(tmp);
                    return false;
                }
            }

            if (!region.flush(0, 0, false)) {
                error = "failed to flush compact lookup mapping";
                fs::remove(tmp);
                return false;
            }
        }

        {
            FILE* file{fsbridge::fopen(tmp, "rb+")};
            if (!file) {
                error = "cannot reopen compact lookup temp file for fsync";
                fs::remove(tmp);
                return false;
            }
            if (!FileCommit(file)) {
                std::fclose(file);
                fs::remove(tmp);
                error = "failed to fsync compact lookup temp file";
                return false;
            }
            if (std::fclose(file) != 0) {
                fs::remove(tmp);
                error = "failed to close compact lookup temp file";
                return false;
            }
        }

        if (!RenameOver(tmp, path)) {
            fs::remove(tmp);
            error = "failed to publish compact lookup file";
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

bool CompactBlockIndexLookup::Open(
    const fs::path& path,
    const CompactBlockIndexStore& source,
    std::string& error)
{
    Close();

    if (!source.IsOpen() || !source.Header()) {
        error = "compact source store is not open";
        return false;
    }

    try {
        const std::string path_string{fs::PathToString(path)};
        m_mapping = std::make_unique<boost::interprocess::file_mapping>(
            path_string.c_str(), boost::interprocess::read_only);
        m_region = std::make_unique<boost::interprocess::mapped_region>(
            *m_mapping, boost::interprocess::read_only);

        m_size_bytes = m_region->get_size();
        if (m_size_bytes < sizeof(CompactBlockIndexLookupHeader)) {
            error = "compact lookup file is smaller than its header";
            Close();
            return false;
        }

        const auto* base{static_cast<const unsigned char*>(m_region->get_address())};
        m_header = reinterpret_cast<const CompactBlockIndexLookupHeader*>(base);

        if (m_header->magic != COMPACT_BLOCK_INDEX_LOOKUP_MAGIC ||
            m_header->version != COMPACT_BLOCK_INDEX_LOOKUP_VERSION ||
            m_header->slot_size != sizeof(CompactBlockIndexLookupSlot)) {
            error = "compact lookup format mismatch";
            Close();
            return false;
        }
        if (m_header->entry_count != source.EntryCount() ||
            m_header->source_generation != source.Header()->generation ||
            m_header->source_size != source.SizeBytes() ||
            m_header->genesis_hash != source.Header()->genesis_hash) {
            error = "compact lookup source-generation mismatch";
            Close();
            return false;
        }
        if (m_header->slot_count == 0 ||
            (m_header->slot_count & (m_header->slot_count - 1)) != 0) {
            error = "compact lookup slot count is not a power of two";
            Close();
            return false;
        }

        const uint64_t expected_size{
            sizeof(CompactBlockIndexLookupHeader) +
            m_header->slot_count *
                static_cast<uint64_t>(sizeof(CompactBlockIndexLookupSlot))};
        if (expected_size != m_size_bytes) {
            error = "compact lookup file-size mismatch";
            Close();
            return false;
        }

        m_slots = reinterpret_cast<const CompactBlockIndexLookupSlot*>(
            base + sizeof(CompactBlockIndexLookupHeader));
        m_path = path;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        Close();
        return false;
    }
}

std::optional<BlockIndexId> CompactBlockIndexLookup::Find(
    const uint256& hash,
    const CompactBlockIndexStore& source,
    uint32_t* probes) const noexcept
{
    if (probes) *probes = 0;
    if (!m_header || !m_slots || !source.IsOpen()) return std::nullopt;

    const uint64_t fp{Fingerprint(m_header->k0, m_header->k1, hash)};
    const uint64_t mask{m_header->slot_count - 1};
    uint64_t pos{fp & mask};

    for (uint64_t probe = 0; probe < m_header->slot_count; ++probe) {
        if (probes) *probes = static_cast<uint32_t>(probe + 1);

        const CompactBlockIndexLookupSlot& slot{m_slots[pos]};
        if (slot.id == INVALID_BLOCK_INDEX_ID) {
            return std::nullopt;
        }

        if (slot.fingerprint == fp) {
            const CompactBlockIndexEntry* entry{source.Get(slot.id)};
            if (entry && entry->hash == hash) {
                return slot.id;
            }
        }

        pos = (pos + 1) & mask;
    }

    return std::nullopt;
}

bool CompactBlockIndexLookup::LoadResidentProbeFront(std::string& error)
{
    if (!m_header || !m_slots) {
        error = "compact lookup is not open";
        return false;
    }

    try {
        m_resident_slot_count = m_header->slot_count;
        m_resident_mask = m_resident_slot_count - 1;
        m_resident_k0 = m_header->k0;
        m_resident_k1 = m_header->k1;

        m_resident_fingerprints.assign(m_resident_slot_count, 0);
        m_resident_occupancy.assign((m_resident_slot_count + 63) / 64, 0);

        for (uint64_t pos = 0; pos < m_resident_slot_count; ++pos) {
            const CompactBlockIndexLookupSlot& slot{m_slots[pos]};
            if (slot.id == INVALID_BLOCK_INDEX_ID) continue;

            m_resident_fingerprints[pos] = slot.fingerprint;
            m_resident_occupancy[pos >> 6] |= uint64_t{1} << (pos & 63);
        }

        return true;
    } catch (const std::exception& e) {
        m_resident_fingerprints.clear();
        m_resident_occupancy.clear();
        m_resident_slot_count = 0;
        m_resident_mask = 0;
        m_resident_k0 = 0;
        m_resident_k1 = 0;
        error = e.what();
        return false;
    }
}

std::optional<BlockIndexId> CompactBlockIndexLookup::FindResident(
    const uint256& hash,
    const CompactBlockIndexStore& source,
    uint32_t* probes,
    bool* touched_backing) const noexcept
{
    if (probes) *probes = 0;
    if (touched_backing) *touched_backing = false;
    if (!m_slots || m_resident_slot_count == 0 ||
        m_resident_fingerprints.size() != m_resident_slot_count) {
        return std::nullopt;
    }

    const uint64_t fp{
        Fingerprint(m_resident_k0, m_resident_k1, hash)};
    uint64_t pos{fp & m_resident_mask};

    for (uint64_t probe = 0; probe < m_resident_slot_count; ++probe) {
        if (probes) *probes = static_cast<uint32_t>(probe + 1);

        const bool occupied{
            (m_resident_occupancy[pos >> 6] >> (pos & 63)) & uint64_t{1}};
        if (!occupied) {
            return std::nullopt;
        }

        if (m_resident_fingerprints[pos] == fp) {
            // The id/full-hash backing is intentionally touched only after a
            // keyed fingerprint match. For arbitrary remote misses this keeps
            // the entire probe walk on the resident front.
            if (touched_backing) *touched_backing = true;
            const CompactBlockIndexLookupSlot& slot{m_slots[pos]};
            const CompactBlockIndexEntry* entry{source.Get(slot.id)};
            if (entry && entry->hash == hash) {
                return slot.id;
            }
        }

        pos = (pos + 1) & m_resident_mask;
    }

    return std::nullopt;
}

void CompactBlockIndexLookup::Close()
{
    m_resident_fingerprints.clear();
    m_resident_occupancy.clear();
    m_resident_slot_count = 0;
    m_resident_mask = 0;
    m_resident_k0 = 0;
    m_resident_k1 = 0;
    m_slots = nullptr;
    m_header = nullptr;
    m_size_bytes = 0;
    m_region.reset();
    m_mapping.reset();
    m_path.clear();
}

} // namespace node
