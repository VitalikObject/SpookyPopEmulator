#pragma once

#include "common.h"

#include <unordered_set>

class Emulator;

class ObjcAbi final {
public:
    explicit ObjcAbi(Emulator& emulator);

    void Initialize();

    bool IsGuestClass(u32 address) const;
    bool IsGuestMetaClass(u32 address) const;
    bool IsGuestObject(u32 address) const;
    bool IsClassObject(u32 receiver) const;

    std::optional<u32> FindClassByName(std::string_view name) const;
    std::optional<u32> FindMetaClassByName(std::string_view name) const;
    std::optional<std::string> ClassNameForReceiver(u32 receiver) const;
    std::optional<u32> LookupMethodImp(u32 receiver, std::string_view selector, bool is_super, u32 super_class) const;

    bool RespondsToSelector(u32 receiver, std::string_view selector, bool is_super, u32 super_class) const;
    bool IsKindOfClass(u32 receiver, u32 class_address) const;

    u32 AllocateInstance(u32 class_address, const std::string& tag);
    u32 IsaOf(u32 receiver) const;
    u32 SuperclassOf(u32 class_or_object) const;

private:
    struct MethodInfo {
        u32 selector_ptr = 0;
        std::string selector_name;
        u32 types_ptr = 0;
        std::string types;
        u32 imp = 0;
    };

    struct IvarInfo {
        u32 offset_ptr = 0;
        u32 offset = 0;
        std::string name;
        std::string type;
        u32 alignment = 0;
        u32 size = 0;
    };

    struct PropertyInfo {
        std::string name;
        std::string attributes;
    };

    struct ProtocolInfo {
        u32 address = 0;
        std::string name;
        std::vector<u32> protocols;
        std::vector<MethodInfo> instance_methods;
        std::vector<MethodInfo> class_methods;
        std::vector<MethodInfo> optional_instance_methods;
        std::vector<MethodInfo> optional_class_methods;
        std::vector<PropertyInfo> properties;
    };

    struct ClassInfo {
        u32 address = 0;
        u32 isa = 0;
        u32 superclass = 0;
        u32 cache = 0;
        u32 vtable = 0;
        u32 data_bits = 0;
        u32 data_ptr = 0;
        std::string name;
        bool is_meta = false;
        u32 instance_start = 0;
        u32 instance_size = 0;
        std::vector<MethodInfo> methods;
        std::vector<MethodInfo> category_methods;
        std::vector<IvarInfo> ivars;
        std::vector<PropertyInfo> properties;
        std::vector<u32> protocols;
    };

    struct CategoryInfo {
        u32 address = 0;
        std::string name;
        u32 target_class = 0;
        std::vector<MethodInfo> instance_methods;
        std::vector<MethodInfo> class_methods;
        std::vector<u32> protocols;
        std::vector<PropertyInfo> properties;
    };

    bool IsImageAddress(u32 address) const;
    std::string ReadCString(u32 address) const;
    u32 ReadPointer(u32 address) const;

    void ParseClassList();
    void ParseCategoryList();
    void ParseProtocolList();
    void EnsureClassParsed(u32 class_address, bool is_meta_hint);
    void AnnotateKnownClasses();

    std::vector<MethodInfo> ParseMethodList(u32 list_address) const;
    std::vector<IvarInfo> ParseIvarList(u32 list_address) const;
    std::vector<PropertyInfo> ParsePropertyList(u32 list_address) const;
    std::vector<u32> ParseProtocolRefList(u32 list_address) const;

    u32 ResolveDispatchClass(u32 receiver, bool is_super, u32 super_class) const;
    const MethodInfo* FindMethod(u32 dispatch_class, std::string_view selector) const;

    Emulator& emulator_;
    std::unordered_map<u32, ClassInfo> class_infos_;
    std::unordered_map<std::string, u32> class_name_to_address_;
    std::unordered_map<std::string, u32> meta_name_to_address_;
    std::unordered_map<u32, CategoryInfo> categories_;
    std::unordered_map<u32, ProtocolInfo> protocol_infos_;
    std::unordered_set<u32> class_list_roots_;
};
