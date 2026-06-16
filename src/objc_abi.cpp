#include "objc_abi.h"

#include "emulator.h"

namespace {

constexpr u32 kObjcClassDataMask = 0xFFFFFFFCu;
constexpr u32 kObjcMethodListEntsizeMask = 0xFFFFFFFCu;
constexpr u32 kObjcClassRoMeta = 1u << 0;

#pragma pack(push, 1)
struct ObjcClass32 {
    u32 isa;
    u32 superclass;
    u32 cache;
    u32 vtable;
    u32 data;
};

struct ObjcClassRo32 {
    u32 flags;
    u32 instance_start;
    u32 instance_size;
    u32 ivar_layout;
    u32 name;
    u32 base_methods;
    u32 base_protocols;
    u32 ivars;
    u32 weak_ivar_layout;
    u32 base_properties;
};

struct ObjcMethodList32 {
    u32 entsize_and_flags;
    u32 count;
};

struct ObjcMethod32 {
    u32 name;
    u32 types;
    u32 imp;
};

struct ObjcIvarList32 {
    u32 entsize;
    u32 count;
};

struct ObjcIvar32 {
    u32 offset_ptr;
    u32 name;
    u32 type;
    u32 alignment;
    u32 size;
};

struct ObjcPropertyList32 {
    u32 entsize;
    u32 count;
};

struct ObjcProperty32 {
    u32 name;
    u32 attributes;
};

struct ObjcProtocolList32 {
    u32 count;
};

struct ObjcProtocol32 {
    u32 isa;
    u32 name;
    u32 protocols;
    u32 instance_methods;
    u32 class_methods;
    u32 optional_instance_methods;
    u32 optional_class_methods;
    u32 instance_properties;
};

struct ObjcCategory32 {
    u32 name;
    u32 cls;
    u32 instance_methods;
    u32 class_methods;
    u32 protocols;
    u32 instance_properties;
};
#pragma pack(pop)

}  // namespace

ObjcAbi::ObjcAbi(Emulator& emulator)
    : emulator_(emulator) {}

void ObjcAbi::Initialize() {
    ParseClassList();
    ParseCategoryList();
    ParseProtocolList();
    AnnotateKnownClasses();
}

bool ObjcAbi::IsGuestClass(const u32 address) const {
    const auto it = class_infos_.find(address);
    return it != class_infos_.end() && !it->second.is_meta;
}

bool ObjcAbi::IsGuestMetaClass(const u32 address) const {
    const auto it = class_infos_.find(address);
    return it != class_infos_.end() && it->second.is_meta;
}

bool ObjcAbi::IsGuestObject(const u32 address) const {
    if (IsGuestClass(address) || IsGuestMetaClass(address) || !emulator_.memory_.IsMapped(address)) {
        return false;
    }
    return IsGuestClass(IsaOf(address));
}

bool ObjcAbi::IsClassObject(const u32 receiver) const {
    return IsGuestClass(receiver) || IsGuestMetaClass(receiver);
}

std::optional<u32> ObjcAbi::FindClassByName(const std::string_view name) const {
    if (const auto it = class_name_to_address_.find(std::string(name)); it != class_name_to_address_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<u32> ObjcAbi::FindMetaClassByName(const std::string_view name) const {
    if (const auto it = meta_name_to_address_.find(std::string(name)); it != meta_name_to_address_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string> ObjcAbi::ClassNameForReceiver(const u32 receiver) const {
    if (const auto it = class_infos_.find(receiver); it != class_infos_.end()) {
        return it->second.name;
    }
    const u32 isa = IsaOf(receiver);
    if (const auto it = class_infos_.find(isa); it != class_infos_.end()) {
        return it->second.name;
    }
    return std::nullopt;
}

std::optional<u32> ObjcAbi::LookupMethodImp(const u32 receiver, const std::string_view selector, const bool is_super, const u32 super_class) const {
    const u32 dispatch_class = ResolveDispatchClass(receiver, is_super, super_class);
    if (dispatch_class == 0) {
        return std::nullopt;
    }
    if (const MethodInfo* method = FindMethod(dispatch_class, selector)) {
        return method->imp;
    }
    return std::nullopt;
}

bool ObjcAbi::RespondsToSelector(const u32 receiver, const std::string_view selector, const bool is_super, const u32 super_class) const {
    return LookupMethodImp(receiver, selector, is_super, super_class).has_value();
}

bool ObjcAbi::IsKindOfClass(const u32 receiver, const u32 class_address) const {
    if (class_address == 0) {
        return false;
    }
    u32 current = IsClassObject(receiver) ? receiver : IsaOf(receiver);
    while (current != 0) {
        if (current == class_address) {
            return true;
        }
        const auto it = class_infos_.find(current);
        if (it == class_infos_.end()) {
            break;
        }
        current = it->second.superclass;
    }
    return false;
}

u32 ObjcAbi::AllocateInstance(const u32 class_address, const std::string& tag) {
    const auto class_it = class_infos_.find(class_address);
    const u32 size = class_it == class_infos_.end() ? 0x40u : std::max<u32>(class_it->second.instance_size, 4u);
    const u32 object = emulator_.AllocateData(size, 4, tag);
    emulator_.memory_.Write32(object, class_address);
    emulator_.host_objects_[object] = Emulator::HostObject{
        .kind = Emulator::ObjKind::Generic,
        .class_name = class_it == class_infos_.end() ? "NSObject" : class_it->second.name,
        .isa = class_address
    };
    return object;
}

u32 ObjcAbi::IsaOf(const u32 receiver) const {
    if (!emulator_.memory_.IsMapped(receiver)) {
        return 0;
    }
    return emulator_.memory_.Read32(receiver);
}

u32 ObjcAbi::SuperclassOf(const u32 class_or_object) const {
    if (const auto it = class_infos_.find(class_or_object); it != class_infos_.end()) {
        return it->second.superclass;
    }
    const u32 isa = IsaOf(class_or_object);
    if (const auto it = class_infos_.find(isa); it != class_infos_.end()) {
        return it->second.superclass;
    }
    return 0;
}

bool ObjcAbi::IsImageAddress(const u32 address) const {
    if (address == 0) {
        return false;
    }
    for (const auto& segment : emulator_.image_.segments()) {
        if (address >= segment.vmaddr && address < segment.vmaddr + segment.vmsize) {
            return true;
        }
    }
    return false;
}

std::string ObjcAbi::ReadCString(const u32 address) const {
    if (!IsImageAddress(address) && !emulator_.memory_.IsMapped(address)) {
        return {};
    }
    return emulator_.ReadGuestCString(address);
}

u32 ObjcAbi::ReadPointer(const u32 address) const {
    if (!emulator_.memory_.IsMapped(address)) {
        return 0;
    }
    return emulator_.memory_.Read32(address);
}

void ObjcAbi::ParseClassList() {
    for (const auto& section : emulator_.image_.sections()) {
        if (section.sectname != "__objc_classlist" && section.sectname != "__objc_nlclslist") {
            continue;
        }
        for (u32 offset = 0; offset + 4 <= section.size; offset += 4) {
            const u32 class_address = ReadPointer(section.addr + offset);
            if (class_address == 0) {
                continue;
            }
            class_list_roots_.insert(class_address);
            EnsureClassParsed(class_address, false);
        }
    }
}

void ObjcAbi::ParseCategoryList() {
    for (const auto& section : emulator_.image_.sections()) {
        if (section.sectname != "__objc_catlist" && section.sectname != "__objc_nlcatlist") {
            continue;
        }
        for (u32 offset = 0; offset + 4 <= section.size; offset += 4) {
            const u32 category_address = ReadPointer(section.addr + offset);
            if (category_address == 0 || !IsImageAddress(category_address)) {
                continue;
            }

            const ObjcCategory32 category{
                .name = ReadPointer(category_address + 0),
                .cls = ReadPointer(category_address + 4),
                .instance_methods = ReadPointer(category_address + 8),
                .class_methods = ReadPointer(category_address + 12),
                .protocols = ReadPointer(category_address + 16),
                .instance_properties = ReadPointer(category_address + 20),
            };

            EnsureClassParsed(category.cls, false);
            const auto target_it = class_infos_.find(category.cls);
            if (target_it == class_infos_.end()) {
                continue;
            }

            CategoryInfo info;
            info.address = category_address;
            info.name = ReadCString(category.name);
            info.target_class = category.cls;
            info.instance_methods = ParseMethodList(category.instance_methods);
            info.class_methods = ParseMethodList(category.class_methods);
            info.protocols = ParseProtocolRefList(category.protocols);
            info.properties = ParsePropertyList(category.instance_properties);

            target_it->second.category_methods.insert(
                target_it->second.category_methods.end(),
                info.instance_methods.begin(),
                info.instance_methods.end());

            if (const auto meta_it = class_infos_.find(target_it->second.isa); meta_it != class_infos_.end()) {
                auto& meta_methods = const_cast<ClassInfo&>(meta_it->second).category_methods;
                meta_methods.insert(meta_methods.end(), info.class_methods.begin(), info.class_methods.end());
            }

            categories_[category_address] = std::move(info);
        }
    }
}

void ObjcAbi::ParseProtocolList() {
    for (const auto& section : emulator_.image_.sections()) {
        if (section.sectname != "__objc_protolist") {
            continue;
        }
        for (u32 offset = 0; offset + 4 <= section.size; offset += 4) {
            const u32 protocol_address = ReadPointer(section.addr + offset);
            if (protocol_address == 0 || !IsImageAddress(protocol_address) || protocol_infos_.count(protocol_address) != 0) {
                continue;
            }
            const ObjcProtocol32 protocol{
                .isa = ReadPointer(protocol_address + 0),
                .name = ReadPointer(protocol_address + 4),
                .protocols = ReadPointer(protocol_address + 8),
                .instance_methods = ReadPointer(protocol_address + 12),
                .class_methods = ReadPointer(protocol_address + 16),
                .optional_instance_methods = ReadPointer(protocol_address + 20),
                .optional_class_methods = ReadPointer(protocol_address + 24),
                .instance_properties = ReadPointer(protocol_address + 28),
            };

            ProtocolInfo info;
            info.address = protocol_address;
            info.name = ReadCString(protocol.name);
            info.protocols = ParseProtocolRefList(protocol.protocols);
            info.instance_methods = ParseMethodList(protocol.instance_methods);
            info.class_methods = ParseMethodList(protocol.class_methods);
            info.optional_instance_methods = ParseMethodList(protocol.optional_instance_methods);
            info.optional_class_methods = ParseMethodList(protocol.optional_class_methods);
            info.properties = ParsePropertyList(protocol.instance_properties);
            protocol_infos_[protocol_address] = std::move(info);
        }
    }
}

void ObjcAbi::EnsureClassParsed(const u32 class_address, const bool is_meta_hint) {
    if (class_address == 0 || !IsImageAddress(class_address) || class_infos_.count(class_address) != 0) {
        return;
    }

    const ObjcClass32 class_record{
        .isa = ReadPointer(class_address + 0),
        .superclass = ReadPointer(class_address + 4),
        .cache = ReadPointer(class_address + 8),
        .vtable = ReadPointer(class_address + 12),
        .data = ReadPointer(class_address + 16),
    };

    ClassInfo info;
    info.address = class_address;
    info.isa = class_record.isa;
    info.superclass = class_record.superclass;
    info.cache = class_record.cache;
    info.vtable = class_record.vtable;
    info.data_bits = class_record.data;
    info.data_ptr = class_record.data & kObjcClassDataMask;
    info.is_meta = is_meta_hint;

    if (info.data_ptr != 0 && IsImageAddress(info.data_ptr)) {
        const ObjcClassRo32 ro{
            .flags = ReadPointer(info.data_ptr + 0),
            .instance_start = ReadPointer(info.data_ptr + 4),
            .instance_size = ReadPointer(info.data_ptr + 8),
            .ivar_layout = ReadPointer(info.data_ptr + 12),
            .name = ReadPointer(info.data_ptr + 16),
            .base_methods = ReadPointer(info.data_ptr + 20),
            .base_protocols = ReadPointer(info.data_ptr + 24),
            .ivars = ReadPointer(info.data_ptr + 28),
            .weak_ivar_layout = ReadPointer(info.data_ptr + 32),
            .base_properties = ReadPointer(info.data_ptr + 36),
        };

        info.is_meta = (ro.flags & kObjcClassRoMeta) != 0;
        info.name = ReadCString(ro.name);
        info.instance_start = ro.instance_start;
        info.instance_size = ro.instance_size;
        info.methods = ParseMethodList(ro.base_methods);
        info.protocols = ParseProtocolRefList(ro.base_protocols);
        info.ivars = ParseIvarList(ro.ivars);
        info.properties = ParsePropertyList(ro.base_properties);
    }

    class_infos_[class_address] = info;

    if (!info.is_meta && !info.name.empty()) {
        class_name_to_address_[info.name] = class_address;
        meta_name_to_address_[info.name] = info.isa;
    }

    if (info.superclass != 0) {
        EnsureClassParsed(info.superclass, info.is_meta);
    }
    if (info.isa != 0 && info.isa != class_address) {
        EnsureClassParsed(info.isa, true);
    }
}

void ObjcAbi::AnnotateKnownClasses() {
    for (const auto& [address, info] : class_infos_) {
        emulator_.host_objects_[address] = Emulator::HostObject{
            .kind = info.is_meta ? Emulator::ObjKind::MetaClass : Emulator::ObjKind::Class,
            .class_name = info.name,
            .isa = info.isa,
            .meta = info.superclass,
        };
    }
}

std::vector<ObjcAbi::MethodInfo> ObjcAbi::ParseMethodList(const u32 list_address) const {
    std::vector<MethodInfo> methods;
    if (list_address == 0 || !IsImageAddress(list_address)) {
        return methods;
    }

    const ObjcMethodList32 header{
        .entsize_and_flags = ReadPointer(list_address + 0),
        .count = ReadPointer(list_address + 4),
    };
    const u32 entry_size = std::max<u32>(header.entsize_and_flags & kObjcMethodListEntsizeMask, sizeof(ObjcMethod32));
    for (u32 index = 0; index < header.count; ++index) {
        const u32 entry = list_address + 8 + index * entry_size;
        MethodInfo method;
        method.selector_ptr = ReadPointer(entry + 0);
        method.types_ptr = ReadPointer(entry + 4);
        method.imp = ReadPointer(entry + 8);
        method.selector_name = ReadCString(method.selector_ptr);
        method.types = ReadCString(method.types_ptr);
        methods.push_back(std::move(method));
    }
    return methods;
}

std::vector<ObjcAbi::IvarInfo> ObjcAbi::ParseIvarList(const u32 list_address) const {
    std::vector<IvarInfo> ivars;
    if (list_address == 0 || !IsImageAddress(list_address)) {
        return ivars;
    }

    const ObjcIvarList32 header{
        .entsize = ReadPointer(list_address + 0),
        .count = ReadPointer(list_address + 4),
    };
    const u32 entry_size = std::max<u32>(header.entsize, sizeof(ObjcIvar32));
    for (u32 index = 0; index < header.count; ++index) {
        const u32 entry = list_address + 8 + index * entry_size;
        IvarInfo ivar;
        ivar.offset_ptr = ReadPointer(entry + 0);
        ivar.offset = ivar.offset_ptr == 0 ? 0 : ReadPointer(ivar.offset_ptr);
        ivar.name = ReadCString(ReadPointer(entry + 4));
        ivar.type = ReadCString(ReadPointer(entry + 8));
        ivar.alignment = ReadPointer(entry + 12);
        ivar.size = ReadPointer(entry + 16);
        ivars.push_back(std::move(ivar));
    }
    return ivars;
}

std::vector<ObjcAbi::PropertyInfo> ObjcAbi::ParsePropertyList(const u32 list_address) const {
    std::vector<PropertyInfo> properties;
    if (list_address == 0 || !IsImageAddress(list_address)) {
        return properties;
    }

    const ObjcPropertyList32 header{
        .entsize = ReadPointer(list_address + 0),
        .count = ReadPointer(list_address + 4),
    };
    const u32 entry_size = std::max<u32>(header.entsize, sizeof(ObjcProperty32));
    for (u32 index = 0; index < header.count; ++index) {
        const u32 entry = list_address + 8 + index * entry_size;
        PropertyInfo property;
        property.name = ReadCString(ReadPointer(entry + 0));
        property.attributes = ReadCString(ReadPointer(entry + 4));
        properties.push_back(std::move(property));
    }
    return properties;
}

std::vector<u32> ObjcAbi::ParseProtocolRefList(const u32 list_address) const {
    std::vector<u32> protocols;
    if (list_address == 0 || !IsImageAddress(list_address)) {
        return protocols;
    }

    const ObjcProtocolList32 header{
        .count = ReadPointer(list_address + 0),
    };
    for (u32 index = 0; index < header.count; ++index) {
        protocols.push_back(ReadPointer(list_address + 4 + index * 4));
    }
    return protocols;
}

u32 ObjcAbi::ResolveDispatchClass(const u32 receiver, const bool is_super, const u32 super_class) const {
    if (receiver == 0) {
        return 0;
    }

    if (is_super) {
        const auto current_it = class_infos_.find(super_class);
        if (current_it == class_infos_.end()) {
            return 0;
        }
        u32 lookup_class = current_it->second.superclass;
        if (lookup_class == 0) {
            return 0;
        }
        if (IsClassObject(receiver) && !current_it->second.is_meta) {
            const auto super_it = class_infos_.find(lookup_class);
            return super_it == class_infos_.end() ? 0 : super_it->second.isa;
        }
        return lookup_class;
    }

    if (IsClassObject(receiver)) {
        const auto class_it = class_infos_.find(receiver);
        return class_it == class_infos_.end() ? 0 : class_it->second.isa;
    }

    const u32 isa = IsaOf(receiver);
    return class_infos_.count(isa) != 0 ? isa : 0;
}

const ObjcAbi::MethodInfo* ObjcAbi::FindMethod(const u32 dispatch_class, const std::string_view selector) const {
    u32 current = dispatch_class;
    while (current != 0) {
        const auto class_it = class_infos_.find(current);
        if (class_it == class_infos_.end()) {
            return nullptr;
        }
        for (const MethodInfo& method : class_it->second.methods) {
            if (method.selector_name == selector) {
                return &method;
            }
        }
        for (const MethodInfo& method : class_it->second.category_methods) {
            if (method.selector_name == selector) {
                return &method;
            }
        }
        current = class_it->second.superclass;
    }
    return nullptr;
}
