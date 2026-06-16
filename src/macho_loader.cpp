#include "macho_loader.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace {

#pragma pack(push, 1)
struct mach_header {
    u32 magic;
    s32 cputype;
    s32 cpusubtype;
    u32 filetype;
    u32 ncmds;
    u32 sizeofcmds;
    u32 flags;
};

struct load_command {
    u32 cmd;
    u32 cmdsize;
};

struct segment_command {
    u32 cmd;
    u32 cmdsize;
    char segname[16];
    u32 vmaddr;
    u32 vmsize;
    u32 fileoff;
    u32 filesize;
    s32 maxprot;
    s32 initprot;
    u32 nsects;
    u32 flags;
};

struct section {
    char sectname[16];
    char segname[16];
    u32 addr;
    u32 size;
    u32 offset;
    u32 align;
    u32 reloff;
    u32 nreloc;
    u32 flags;
    u32 reserved1;
    u32 reserved2;
};

struct dyld_info_command {
    u32 cmd;
    u32 cmdsize;
    u32 rebase_off;
    u32 rebase_size;
    u32 bind_off;
    u32 bind_size;
    u32 weak_bind_off;
    u32 weak_bind_size;
    u32 lazy_bind_off;
    u32 lazy_bind_size;
    u32 export_off;
    u32 export_size;
};

struct symtab_command {
    u32 cmd;
    u32 cmdsize;
    u32 symoff;
    u32 nsyms;
    u32 stroff;
    u32 strsize;
};

struct dysymtab_command {
    u32 cmd;
    u32 cmdsize;
    u32 ilocalsym;
    u32 nlocalsym;
    u32 iextdefsym;
    u32 nextdefsym;
    u32 iundefsym;
    u32 nundefsym;
    u32 tocoff;
    u32 ntoc;
    u32 modtaboff;
    u32 nmodtab;
    u32 extrefsymoff;
    u32 nextrefsyms;
    u32 indirectsymoff;
    u32 nindirectsyms;
    u32 extreloff;
    u32 nextrel;
    u32 locreloff;
    u32 nlocrel;
};

struct thread_command {
    u32 cmd;
    u32 cmdsize;
};

struct entry_point_command {
    u32 cmd;
    u32 cmdsize;
    u64 entryoff;
    u64 stacksize;
};

struct nlist {
    union {
        u32 n_strx;
    } n_un;
    u8 n_type;
    u8 n_sect;
    u16 n_desc;
    u32 n_value;
};
#pragma pack(pop)

constexpr u32 MH_MAGIC = 0xFEEDFACE;
constexpr u32 FAT_MAGIC = 0xBEBAFECA;  // 0xCAFEBABE in little-endian storage
constexpr u32 FAT_CIGAM = 0xCAFEBABE;  // big-endian on disk
constexpr u32 CPU_TYPE_ARM = 12;

constexpr u32 LC_SEGMENT = 0x1;
constexpr u32 LC_SYMTAB = 0x2;
constexpr u32 LC_DYSYMTAB = 0xB;
constexpr u32 LC_UNIXTHREAD = 0x5;
constexpr u32 LC_MAIN = 0x80000028;
constexpr u32 LC_DYLD_INFO_ONLY = 0x80000022;

// Big-endian byte swap for FAT header fields
inline u32 SwapU32BE(const u32 value) {
    return ((value & 0xFF000000u) >> 24)
         | ((value & 0x00FF0000u) >> 8)
         | ((value & 0x0000FF00u) << 8)
         | ((value & 0x000000FFu) << 24);
}

#pragma pack(push, 1)
struct fat_header {
    u32 magic;
    u32 nfat_arch;
};
struct fat_arch {
    u32 cputype;
    u32 cpusubtype;
    u32 offset;
    u32 size;
    u32 align;
};
#pragma pack(pop)

constexpr u8 REBASE_OPCODE_MASK = 0xF0;
constexpr u8 REBASE_IMMEDIATE_MASK = 0x0F;
constexpr u8 REBASE_OPCODE_DONE = 0x00;
constexpr u8 REBASE_OPCODE_SET_TYPE_IMM = 0x10;
constexpr u8 REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x20;
constexpr u8 REBASE_OPCODE_ADD_ADDR_ULEB = 0x30;
constexpr u8 REBASE_OPCODE_ADD_ADDR_IMM_SCALED = 0x40;
constexpr u8 REBASE_OPCODE_DO_REBASE_IMM_TIMES = 0x50;
constexpr u8 REBASE_OPCODE_DO_REBASE_ULEB_TIMES = 0x60;
constexpr u8 REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB = 0x70;
constexpr u8 REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB = 0x80;

constexpr u8 BIND_OPCODE_MASK = 0xF0;
constexpr u8 BIND_IMMEDIATE_MASK = 0x0F;
constexpr u8 BIND_OPCODE_DONE = 0x00;
constexpr u8 BIND_OPCODE_SET_DYLIB_ORDINAL_IMM = 0x10;
constexpr u8 BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB = 0x20;
constexpr u8 BIND_OPCODE_SET_DYLIB_SPECIAL_IMM = 0x30;
constexpr u8 BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40;
constexpr u8 BIND_OPCODE_SET_TYPE_IMM = 0x50;
constexpr u8 BIND_OPCODE_SET_ADDEND_SLEB = 0x60;
constexpr u8 BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70;
constexpr u8 BIND_OPCODE_ADD_ADDR_ULEB = 0x80;
constexpr u8 BIND_OPCODE_DO_BIND = 0x90;
constexpr u8 BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB = 0xA0;
constexpr u8 BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED = 0xB0;
constexpr u8 BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB = 0xC0;

constexpr u8 BIND_SYMBOL_FLAGS_WEAK_IMPORT = 0x1;
constexpr u32 ARM_THREAD_STATE = 1;
constexpr u32 ARM_THREAD_STATE_COUNT = 17;
constexpr u32 POINTER_SIZE = 4;

std::string TrimName(const char raw[16]) {
    std::size_t length = 0;
    while (length < 16 && raw[length] != '\0') {
        ++length;
    }
    return std::string(raw, raw + length);
}

u8 ProtToPerms(const s32 initprot) {
    u8 permissions = 0;
    if ((initprot & 0x1) != 0) {
        permissions |= kPermRead;
    }
    if ((initprot & 0x2) != 0) {
        permissions |= kPermWrite;
    }
    if ((initprot & 0x4) != 0) {
        permissions |= kPermExec;
    }
    return permissions;
}

}  // namespace

MachOImage::MachOImage(const std::filesystem::path& path)
    : path_(path) {
    std::ifstream stream(path_, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("failed to open Mach-O: " + path_.string());
    }

    stream.seekg(0, std::ios::end);
    const std::streamsize size = stream.tellg();
    stream.seekg(0, std::ios::beg);

    file_bytes_.resize(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(file_bytes_.data()), size)) {
        throw std::runtime_error("failed to read Mach-O: " + path_.string());
    }

    Parse();
    ParseInitFunctions();
}

MachOImage::MachOImage(const std::filesystem::path& path, std::vector<u8> bytes)
    : path_(path)
    , file_bytes_(std::move(bytes)) {
    if (file_bytes_.empty()) {
        throw std::runtime_error("empty Mach-O image: " + path_.string());
    }

    Parse();
    ParseInitFunctions();
}

void MachOImage::LoadIntoMemory(GuestMemory& memory) const {
    for (const SegmentInfo& segment : segments_) {
        if (segment.vmsize == 0) {
            continue;
        }

        const u8 permissions = ProtToPerms(segment.initprot);
        const u32 bytes_to_map = std::min(segment.filesize, segment.vmsize);
        const std::span<const u8> segment_bytes(file_bytes_.data() + segment.fileoff, bytes_to_map);
        memory.MapBytes(segment.vmaddr, segment_bytes, segment.vmsize, permissions, segment.name);
    }
}

void MachOImage::ApplyRebases(GuestMemory& memory, const u32 slide) const {
    if (slide == 0 || dyld_info_.rebase_size == 0) {
        return;
    }

    const std::span<const u8> bytes(file_bytes_.data() + dyld_info_.rebase_off, dyld_info_.rebase_size);
    std::size_t offset = 0;
    u32 segment_index = 0;
    u32 segment_offset = 0;
    u32 rebase_type = 0;

    const auto do_rebase = [&]() {
        if (rebase_type != 1) {
            throw std::runtime_error("unsupported rebase type");
        }
        const u32 address = segments_.at(segment_index).vmaddr + segment_offset;
        memory.Write32(address, memory.Read32(address) + slide);
        segment_offset += POINTER_SIZE;
    };

    while (offset < bytes.size()) {
        const u8 op = bytes[offset++];
        const u8 opcode = op & REBASE_OPCODE_MASK;
        const u8 imm = op & REBASE_IMMEDIATE_MASK;

        switch (opcode) {
        case REBASE_OPCODE_DONE:
            return;
        case REBASE_OPCODE_SET_TYPE_IMM:
            rebase_type = imm;
            break;
        case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            segment_index = imm;
            segment_offset = static_cast<u32>(ReadUleb128(bytes, offset));
            break;
        case REBASE_OPCODE_ADD_ADDR_ULEB:
            segment_offset += static_cast<u32>(ReadUleb128(bytes, offset));
            break;
        case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
            segment_offset += imm * POINTER_SIZE;
            break;
        case REBASE_OPCODE_DO_REBASE_IMM_TIMES:
            for (u8 i = 0; i < imm; ++i) {
                do_rebase();
            }
            break;
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: {
            const u64 count = ReadUleb128(bytes, offset);
            for (u64 i = 0; i < count; ++i) {
                do_rebase();
            }
            break;
        }
        case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: {
            do_rebase();
            segment_offset += static_cast<u32>(ReadUleb128(bytes, offset));
            break;
        }
        case REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: {
            const u64 count = ReadUleb128(bytes, offset);
            const u32 skip = static_cast<u32>(ReadUleb128(bytes, offset));
            for (u64 i = 0; i < count; ++i) {
                do_rebase();
                segment_offset += skip;
            }
            break;
        }
        default:
            throw std::runtime_error("unknown rebase opcode");
        }
    }
}

void MachOImage::ApplyBinds(GuestMemory& memory, const ResolveImportFn& resolver) const {
    if (dyld_info_.bind_size != 0) {
        const std::span<const u8> bytes(file_bytes_.data() + dyld_info_.bind_off, dyld_info_.bind_size);
        ParseBindStream(memory, bytes, false, resolver, [](const u32, const u32) {});
    }
    if (dyld_info_.weak_bind_size != 0) {
        const std::span<const u8> bytes(file_bytes_.data() + dyld_info_.weak_bind_off, dyld_info_.weak_bind_size);
        ParseBindStream(memory, bytes, false, resolver, [](const u32, const u32) {});
    }
    if (dyld_info_.lazy_bind_size != 0) {
        const std::span<const u8> bytes(file_bytes_.data() + dyld_info_.lazy_bind_off, dyld_info_.lazy_bind_size);
        ParseBindStream(memory, bytes, true, resolver, [](const u32, const u32) {});
    }
}

void MachOImage::Parse() {
    if (file_bytes_.size() < sizeof(mach_header)) {
        throw std::runtime_error("file too small for mach_header");
    }

    // Handle FAT (universal) binaries: extract the ARM slice
    {
        const auto* fh = reinterpret_cast<const fat_header*>(file_bytes_.data());
        if (fh->magic == FAT_MAGIC || fh->magic == FAT_CIGAM) {
            // FAT headers are always big-endian on disk.
            // On little-endian host: magic reads as 0xBEBAFECA (FAT_MAGIC) → must swap
            // On big-endian host: magic reads as 0xCAFEBABE (FAT_CIGAM) → no swap needed
            const bool needs_swap = (fh->magic == FAT_MAGIC);
            const u32 nfat = needs_swap ? SwapU32BE(fh->nfat_arch) : fh->nfat_arch;
            if (sizeof(fat_header) + nfat * sizeof(fat_arch) > file_bytes_.size()) {
                throw std::runtime_error("FAT header extends beyond file");
            }
            const auto* archs = reinterpret_cast<const fat_arch*>(file_bytes_.data() + sizeof(fat_header));
            bool found = false;
            for (u32 i = 0; i < nfat; ++i) {
                const u32 cputype = needs_swap ? SwapU32BE(archs[i].cputype) : archs[i].cputype;
                const u32 offset  = needs_swap ? SwapU32BE(archs[i].offset)  : archs[i].offset;
                const u32 size    = needs_swap ? SwapU32BE(archs[i].size)    : archs[i].size;
                if (cputype == CPU_TYPE_ARM) {
                    if (offset + size > file_bytes_.size()) {
                        throw std::runtime_error("FAT ARM slice extends beyond file");
                    }
                    std::vector<u8> slice(file_bytes_.begin() + offset, file_bytes_.begin() + offset + size);
                    file_bytes_ = std::move(slice);
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw std::runtime_error("FAT binary has no ARM slice");
            }
        }
    }

    const auto* header = reinterpret_cast<const mach_header*>(file_bytes_.data());
    if (header->magic != MH_MAGIC) {
        throw std::runtime_error("unsupported Mach-O magic");
    }

    std::size_t command_offset = sizeof(mach_header);
    for (u32 i = 0; i < header->ncmds; ++i) {
        if (command_offset + sizeof(load_command) > file_bytes_.size()) {
            throw std::runtime_error("load command extends beyond EOF");
        }

        const auto* command = reinterpret_cast<const load_command*>(file_bytes_.data() + command_offset);
        if (command_offset + command->cmdsize > file_bytes_.size() || command->cmdsize < sizeof(load_command)) {
            throw std::runtime_error("invalid load command size");
        }

        switch (command->cmd) {
        case LC_SEGMENT: {
            const auto* segment = reinterpret_cast<const segment_command*>(command);
            SegmentInfo info;
            info.name = TrimName(segment->segname);
            info.vmaddr = segment->vmaddr;
            info.vmsize = segment->vmsize;
            info.fileoff = segment->fileoff;
            info.filesize = segment->filesize;
            info.maxprot = segment->maxprot;
            info.initprot = segment->initprot;
            info.index = static_cast<u32>(segments_.size());
            segments_.push_back(info);

            const auto* sections = reinterpret_cast<const section*>(reinterpret_cast<const u8*>(segment) + sizeof(segment_command));
            for (u32 section_index = 0; section_index < segment->nsects; ++section_index) {
                SectionInfo sec;
                sec.sectname = TrimName(sections[section_index].sectname);
                sec.segname = TrimName(sections[section_index].segname);
                sec.addr = sections[section_index].addr;
                sec.size = sections[section_index].size;
                sec.offset = sections[section_index].offset;
                sec.flags = sections[section_index].flags;
                sec.reserved1 = sections[section_index].reserved1;
                sec.reserved2 = sections[section_index].reserved2;
                sections_.push_back(sec);
            }
            break;
        }
        case LC_DYLD_INFO_ONLY: {
            const auto* info = reinterpret_cast<const dyld_info_command*>(command);
            dyld_info_.rebase_off = info->rebase_off;
            dyld_info_.rebase_size = info->rebase_size;
            dyld_info_.bind_off = info->bind_off;
            dyld_info_.bind_size = info->bind_size;
            dyld_info_.weak_bind_off = info->weak_bind_off;
            dyld_info_.weak_bind_size = info->weak_bind_size;
            dyld_info_.lazy_bind_off = info->lazy_bind_off;
            dyld_info_.lazy_bind_size = info->lazy_bind_size;
            break;
        }
        case LC_UNIXTHREAD: {
            const auto* thread = reinterpret_cast<const thread_command*>(command);
            const u8* thread_data = reinterpret_cast<const u8*>(thread) + sizeof(thread_command);
            const u32 flavor = *reinterpret_cast<const u32*>(thread_data);
            const u32 count = *reinterpret_cast<const u32*>(thread_data + 4);
            if (flavor == ARM_THREAD_STATE && count == ARM_THREAD_STATE_COUNT) {
                const u32* registers = reinterpret_cast<const u32*>(thread_data + 8);
                entry_pc_ = registers[15];
                entry_cpsr_ = registers[16];
            }
            break;
        }
        case LC_MAIN: {
            const auto* entry = reinterpret_cast<const entry_point_command*>(command);
            const u64 entryoff = entry->entryoff;
            for (const SegmentInfo& segment : segments_) {
                const u64 segment_end = static_cast<u64>(segment.fileoff) + static_cast<u64>(segment.filesize);
                if (entryoff < segment.fileoff || entryoff >= segment_end) {
                    continue;
                }
                const u32 entry_pc = segment.vmaddr + static_cast<u32>(entryoff - segment.fileoff);
                entry_pc_ = entry_pc & ~1u;
                entry_cpsr_ = (entry_pc & 1u) != 0 ? 0x30 : 0x10;
                break;
            }
            break;
        }
        case LC_SYMTAB:
        case LC_DYSYMTAB:
            break;
        default:
            break;
        }

        command_offset += command->cmdsize;
    }
}

void MachOImage::ParseInitFunctions() {
    const SectionInfo* init = FindSection("__DATA", "__mod_init_func");
    if (!init || init->size == 0) {
        return;
    }

    for (u32 offset = 0; offset + 4 <= init->size; offset += 4) {
        const u32 file_offset = init->offset + offset;
        if (file_offset + 4 > file_bytes_.size()) {
            break;
        }
        const u32 address = *reinterpret_cast<const u32*>(file_bytes_.data() + file_offset);
        if (address != 0) {
            init_functions_.push_back(address);
        }
    }
}

const MachOImage::SectionInfo* MachOImage::FindSection(const std::string_view segment_name, const std::string_view section_name) const {
    for (const SectionInfo& section : sections_) {
        if (section.segname == segment_name && section.sectname == section_name) {
            return &section;
        }
    }
    return nullptr;
}

u64 MachOImage::ReadUleb128(const std::span<const u8> bytes, std::size_t& offset) {
    u64 result = 0;
    unsigned shift = 0;
    while (offset < bytes.size()) {
        const u8 byte = bytes[offset++];
        result |= static_cast<u64>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            break;
        }
        shift += 7;
    }
    return result;
}

s64 MachOImage::ReadSleb128(const std::span<const u8> bytes, std::size_t& offset) {
    s64 result = 0;
    unsigned shift = 0;
    u8 byte = 0;

    while (offset < bytes.size()) {
        byte = bytes[offset++];
        result |= static_cast<s64>(byte & 0x7F) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) {
            break;
        }
    }

    if ((shift < 64) && ((byte & 0x40) != 0)) {
        result |= -((s64)1 << shift);
    }
    return result;
}

template<typename DoneCallback>
void MachOImage::ParseBindStream(GuestMemory& memory,
                                 const std::span<const u8> bytes,
                                 const bool lazy_mode,
                                 const ResolveImportFn& resolver,
                                 DoneCallback&& on_bind) const {
    std::size_t offset = 0;
    std::string symbol_name;
    int dylib_ordinal = 0;
    bool weak_import = false;
    u32 segment_index = 0;
    u32 segment_offset = 0;
    u32 bind_type = 1;
    s64 addend = 0;

    const auto reset_lazy_state = [&]() {
        symbol_name.clear();
        dylib_ordinal = 0;
        weak_import = false;
        bind_type = 1;
        addend = 0;
    };

    const auto do_bind = [&]() {
        const u32 address = segments_.at(segment_index).vmaddr + segment_offset;
        const u32 target = resolver(symbol_name, dylib_ordinal, weak_import);
        switch (bind_type) {
        case 1:
            memory.Write32(address, target + static_cast<u32>(addend));
            break;
        case 2:
            memory.Write32(address, target + static_cast<u32>(addend));
            break;
        case 3:
            memory.Write32(address, target + static_cast<u32>(addend) - (address + 4));
            break;
        default:
            throw std::runtime_error("unsupported bind type " + std::to_string(bind_type) + " for " + symbol_name);
        }
        on_bind(address, target);
        segment_offset += POINTER_SIZE;
    };

    while (offset < bytes.size()) {
        const u8 op = bytes[offset++];
        const u8 opcode = op & BIND_OPCODE_MASK;
        const u8 imm = op & BIND_IMMEDIATE_MASK;

        switch (opcode) {
        case BIND_OPCODE_DONE:
            if (!lazy_mode) {
                return;
            }
            reset_lazy_state();
            break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
            dylib_ordinal = imm;
            break;
        case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
            dylib_ordinal = static_cast<int>(ReadUleb128(bytes, offset));
            break;
        case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
            dylib_ordinal = (imm == 0) ? 0 : static_cast<int>(static_cast<std::int8_t>(BIND_OPCODE_MASK | imm));
            break;
        case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
            weak_import = (imm & BIND_SYMBOL_FLAGS_WEAK_IMPORT) != 0;
            symbol_name.clear();
            while (offset < bytes.size() && bytes[offset] != 0) {
                symbol_name.push_back(static_cast<char>(bytes[offset++]));
            }
            ++offset;
            break;
        }
        case BIND_OPCODE_SET_TYPE_IMM:
            bind_type = imm;
            break;
        case BIND_OPCODE_SET_ADDEND_SLEB:
            addend = ReadSleb128(bytes, offset);
            break;
        case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            segment_index = imm;
            segment_offset = static_cast<u32>(ReadUleb128(bytes, offset));
            break;
        case BIND_OPCODE_ADD_ADDR_ULEB:
            segment_offset += static_cast<u32>(ReadUleb128(bytes, offset));
            break;
        case BIND_OPCODE_DO_BIND:
            do_bind();
            break;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
            do_bind();
            segment_offset += static_cast<u32>(ReadUleb128(bytes, offset));
            break;
        case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
            do_bind();
            segment_offset += imm * POINTER_SIZE;
            break;
        case BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: {
            const u64 count = ReadUleb128(bytes, offset);
            const u32 skip = static_cast<u32>(ReadUleb128(bytes, offset));
            for (u64 i = 0; i < count; ++i) {
                do_bind();
                segment_offset += skip;
            }
            break;
        }
        default:
            throw std::runtime_error("unknown bind opcode");
        }
    }
}
