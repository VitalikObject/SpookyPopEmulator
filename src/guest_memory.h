#pragma once

#include "common.h"

#include "dynarmic/interface/A32/config.h"

enum GuestPerm : u8 {
    kPermRead = 1 << 0,
    kPermWrite = 1 << 1,
    kPermExec = 1 << 2,
};

class GuestMemory final {
public:
    static constexpr u32 kPageBits = 12;
    static constexpr u32 kPageSize = 1u << kPageBits;
    using PageTable = std::array<u8*, Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;

    GuestMemory();

    bool MapZeroFill(u32 address, u32 size, u8 permissions, const std::string& tag);
    bool MapBytes(u32 address, std::span<const u8> bytes, u32 virtual_size, u8 permissions, const std::string& tag);
    bool Protect(u32 address, u32 size, u8 permissions);
    void SetWriteProtection(bool enabled);

    bool IsMapped(u32 address) const;
    bool IsReadOnly(u32 address) const;
    bool HasExecute(u32 address) const;

    std::optional<u32> ReadCode32(u32 address) const;

    u8 Read8(u32 address) const;
    u16 Read16(u32 address) const;
    u32 Read32(u32 address) const;
    u64 Read64(u32 address) const;

    void Write8(u32 address, u8 value);
    void Write16(u32 address, u16 value);
    void Write32(u32 address, u32 value);
    void Write64(u32 address, u64 value);

    void WriteBuffer(u32 address, std::span<const u8> bytes);
    std::vector<u8> ReadBuffer(u32 address, std::size_t size) const;
    std::string ReadCString(u32 address, std::size_t max_length = 4096) const;

    const PageTable& page_table() const {
        return *page_table_;
    }

    PageTable* page_table_mut() {
        return page_table_.get();
    }

private:
    struct Page {
        std::array<u8, kPageSize> bytes{};
        u8 permissions = 0;
        std::string tag;
    };

    void RequireWritable(u32 address, std::size_t size) const;
    Page& EnsurePage(u32 page_base, u8 permissions, const std::string& tag);
    Page* FindPage(u32 address);
    const Page* FindPage(u32 address) const;

    std::unordered_map<u32, std::unique_ptr<Page>> pages_;
    std::unique_ptr<PageTable> page_table_;
    bool write_protection_enabled_ = false;
};
