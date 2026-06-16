#include "guest_memory.h"

#include <cstring>
#include <stdexcept>

#if !defined(__ANDROID__)
#include <execinfo.h>
#endif

GuestMemory::GuestMemory()
    : page_table_(std::make_unique<PageTable>()) {
    page_table_->fill(nullptr);
}

bool GuestMemory::MapZeroFill(const u32 address, const u32 size, const u8 permissions, const std::string& tag) {
    if (size == 0) {
        return true;
    }

    const u32 begin = AlignDown(address, kPageSize);
    const u32 end = AlignUp(address + size, kPageSize);
    for (u32 page = begin; page < end; page += kPageSize) {
        EnsurePage(page, permissions, tag);
    }
    return true;
}

bool GuestMemory::MapBytes(const u32 address,
                           const std::span<const u8> bytes,
                           const u32 virtual_size,
                           const u8 permissions,
                           const std::string& tag) {
    MapZeroFill(address, virtual_size, permissions, tag);
    WriteBuffer(address, bytes);
    return true;
}

bool GuestMemory::Protect(const u32 address, const u32 size, const u8 permissions) {
    if (size == 0) {
        return true;
    }

    const u32 begin = AlignDown(address, kPageSize);
    const u32 end = AlignUp(address + size, kPageSize);
    for (u32 page = begin; page < end; page += kPageSize) {
        if (Page* existing = FindPage(page)) {
            existing->permissions = permissions;
        }
    }
    return true;
}

void GuestMemory::SetWriteProtection(const bool enabled) {
    write_protection_enabled_ = enabled;
}

bool GuestMemory::IsMapped(const u32 address) const {
    return FindPage(address) != nullptr;
}

bool GuestMemory::IsReadOnly(const u32 address) const {
    const Page* page = FindPage(address);
    return page != nullptr && (page->permissions & kPermWrite) == 0;
}

bool GuestMemory::HasExecute(const u32 address) const {
    const Page* page = FindPage(address);
    return page != nullptr && (page->permissions & kPermExec) != 0;
}

std::optional<u32> GuestMemory::ReadCode32(const u32 address) const {
    if (!HasExecute(address)) {
        return std::nullopt;
    }
    return Read32(address);
}

u8 GuestMemory::Read8(const u32 address) const {
    const Page* page = FindPage(address);
    if (!page) {
        return 0;
    }
    return page->bytes[address & (kPageSize - 1)];
}

u16 GuestMemory::Read16(const u32 address) const {
    return static_cast<u16>(Read8(address))
        | (static_cast<u16>(Read8(address + 1)) << 8);
}

u32 GuestMemory::Read32(const u32 address) const {
    return static_cast<u32>(Read16(address))
        | (static_cast<u32>(Read16(address + 2)) << 16);
}

u64 GuestMemory::Read64(const u32 address) const {
    return static_cast<u64>(Read32(address))
        | (static_cast<u64>(Read32(address + 4)) << 32);
}

void GuestMemory::Write8(const u32 address, const u8 value) {
    RequireWritable(address, 1);
    if (Page* page = FindPage(address)) {
        page->bytes[address & (kPageSize - 1)] = value;
    }
}

void GuestMemory::Write16(const u32 address, const u16 value) {
    Write8(address, static_cast<u8>(value));
    Write8(address + 1, static_cast<u8>(value >> 8));
}

void GuestMemory::Write32(const u32 address, const u32 value) {
    Write16(address, static_cast<u16>(value));
    Write16(address + 2, static_cast<u16>(value >> 16));
}

void GuestMemory::Write64(const u32 address, const u64 value) {
    Write32(address, static_cast<u32>(value));
    Write32(address + 4, static_cast<u32>(value >> 32));
}

void GuestMemory::WriteBuffer(const u32 address, const std::span<const u8> bytes) {
    RequireWritable(address, bytes.size());
    u32 current = address;
    std::size_t offset = 0;
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        u8* const page = page_table_->at(current >> kPageBits);
        const std::size_t page_offset = current & (kPageSize - 1);
        const std::size_t chunk = remaining < (kPageSize - page_offset)
            ? remaining
            : (kPageSize - page_offset);
        if (page != nullptr) {
            std::memcpy(page + page_offset, bytes.data() + offset, chunk);
        }
        current += static_cast<u32>(chunk);
        offset += chunk;
        remaining -= chunk;
    }
}

void GuestMemory::RequireWritable(const u32 address, const std::size_t size) const {
    if (!write_protection_enabled_ || size == 0) {
        return;
    }

    const u32 begin = AlignDown(address, kPageSize);
    const u32 end = AlignUp(address + static_cast<u32>(size), kPageSize);
    for (u32 page_base = begin; page_base < end; page_base += kPageSize) {
        const Page* page = FindPage(page_base);
        if (page == nullptr) {
            continue;
        }
        if ((page->permissions & kPermWrite) != 0) {
            continue;
        }
        std::string message = "guest write to read-only page at "
            + Hex32(address) + " size=" + std::to_string(size)
            + " page=" + Hex32(page_base)
            + " tag=" + page->tag;
#if !defined(__ANDROID__)
        void* frames[16]{};
        const int frame_count = ::backtrace(frames, static_cast<int>(std::size(frames)));
        if (frame_count > 0) {
            if (char** symbols = ::backtrace_symbols(frames, frame_count); symbols != nullptr) {
                message += " backtrace=";
                for (int index = 0; index < frame_count; ++index) {
                    if (index != 0) {
                        message += " | ";
                    }
                    message += symbols[index];
                }
                std::free(symbols);
            }
        }
#endif
        throw std::runtime_error(message);
    }
}

std::vector<u8> GuestMemory::ReadBuffer(const u32 address, const std::size_t size) const {
    std::vector<u8> out(size);
    u32 current = address;
    std::size_t offset = 0;
    std::size_t remaining = size;
    while (remaining > 0) {
        u8* const page = page_table_->at(current >> kPageBits);
        const std::size_t page_offset = current & (kPageSize - 1);
        const std::size_t chunk = remaining < (kPageSize - page_offset)
            ? remaining
            : (kPageSize - page_offset);
        if (page != nullptr) {
            std::memcpy(out.data() + offset, page + page_offset, chunk);
        }
        current += static_cast<u32>(chunk);
        offset += chunk;
        remaining -= chunk;
    }
    return out;
}

std::string GuestMemory::ReadCString(const u32 address, const std::size_t max_length) const {
    std::string out;
    out.reserve(32);
    for (std::size_t i = 0; i < max_length; ++i) {
        const char c = static_cast<char>(Read8(address + static_cast<u32>(i)));
        if (c == '\0') {
            break;
        }
        out.push_back(c);
    }
    return out;
}

GuestMemory::Page& GuestMemory::EnsurePage(const u32 page_base, const u8 permissions, const std::string& tag) {
    auto it = pages_.find(page_base);
    if (it == pages_.end()) {
        auto page = std::make_unique<Page>();
        page->permissions = permissions;
        page->tag = tag;
        page_table_->at(page_base >> kPageBits) = page->bytes.data();
        it = pages_.emplace(page_base, std::move(page)).first;
    } else {
        it->second->permissions |= permissions;
        if (it->second->tag.empty()) {
            it->second->tag = tag;
        }
    }
    return *it->second;
}

GuestMemory::Page* GuestMemory::FindPage(const u32 address) {
    const auto it = pages_.find(AlignDown(address, kPageSize));
    return it == pages_.end() ? nullptr : it->second.get();
}

const GuestMemory::Page* GuestMemory::FindPage(const u32 address) const {
    const auto it = pages_.find(AlignDown(address, kPageSize));
    return it == pages_.end() ? nullptr : it->second.get();
}
