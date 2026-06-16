#pragma once

#include "common.h"
#include "guest_memory.h"

class MachOImage final {
public:
    struct SegmentInfo {
        std::string name;
        u32 vmaddr = 0;
        u32 vmsize = 0;
        u32 fileoff = 0;
        u32 filesize = 0;
        s32 maxprot = 0;
        s32 initprot = 0;
        u32 index = 0;
    };

    struct SectionInfo {
        std::string segname;
        std::string sectname;
        u32 addr = 0;
        u32 size = 0;
        u32 offset = 0;
        u32 flags = 0;
        u32 reserved1 = 0;
        u32 reserved2 = 0;
    };

    using ResolveImportFn = std::function<u32(std::string_view symbol_name, int dylib_ordinal, bool weak_import)>;

    explicit MachOImage(const std::filesystem::path& path);
    MachOImage(const std::filesystem::path& path, std::vector<u8> bytes);

    void LoadIntoMemory(GuestMemory& memory) const;
    void ApplyRebases(GuestMemory& memory, u32 slide = 0) const;
    void ApplyBinds(GuestMemory& memory, const ResolveImportFn& resolver) const;

    u32 entry_pc() const {
        return entry_pc_;
    }

    u32 entry_cpsr() const {
        return entry_cpsr_;
    }

    const std::vector<u32>& init_functions() const {
        return init_functions_;
    }

    const std::filesystem::path& path() const {
        return path_;
    }

    const std::vector<SegmentInfo>& segments() const {
        return segments_;
    }

    const std::vector<SectionInfo>& sections() const {
        return sections_;
    }

private:
    struct DyldInfo {
        u32 rebase_off = 0;
        u32 rebase_size = 0;
        u32 bind_off = 0;
        u32 bind_size = 0;
        u32 weak_bind_off = 0;
        u32 weak_bind_size = 0;
        u32 lazy_bind_off = 0;
        u32 lazy_bind_size = 0;
    };

    void Parse();
    void ParseInitFunctions();
    const SectionInfo* FindSection(std::string_view segment_name, std::string_view section_name) const;

    static u64 ReadUleb128(std::span<const u8> bytes, std::size_t& offset);
    static s64 ReadSleb128(std::span<const u8> bytes, std::size_t& offset);

    template<typename DoneCallback>
    void ParseBindStream(GuestMemory& memory,
                         std::span<const u8> bytes,
                         bool lazy_mode,
                         const ResolveImportFn& resolver,
                         DoneCallback&& on_bind) const;

    std::filesystem::path path_;
    std::vector<u8> file_bytes_;
    std::vector<SegmentInfo> segments_;
    std::vector<SectionInfo> sections_;
    DyldInfo dyld_info_;
    u32 entry_pc_ = 0;
    u32 entry_cpsr_ = 0;
    std::vector<u32> init_functions_;
};
