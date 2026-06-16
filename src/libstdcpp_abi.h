#pragma once

#include "common.h"

class Emulator;

class LibStdCppAbi final {
public:
    explicit LibStdCppAbi(Emulator& emulator);

    u32 RegisterEmptyRepStorage(const std::string& symbol);

    void InitializeString(u32 self, std::string_view value, const std::string& tag);
    void CopyString(u32 dst, u32 src, const std::string& tag);
    void DestroyString(u32 self);

    std::string ReadString(u32 self) const;
    int CompareStringWithCString(u32 self, u32 cstring) const;
    int CompareStrings(u32 lhs, u32 rhs) const;
    void AssignFromCString(u32 self, u32 cstring, u32 length, const std::string& tag);
    void AssignFromString(u32 self, u32 other, const std::string& tag);
    void PushBack(u32 self, char ch, const std::string& tag);
    void Substr(u32 out, u32 self, u32 pos, u32 count, const std::string& tag);

private:
    static constexpr u32 kStringObjectSize = 4;
    static constexpr u32 kRepLengthOffset = 0;
    static constexpr u32 kRepCapacityOffset = 4;
    static constexpr u32 kRepRefcountOffset = 8;
    static constexpr u32 kRepHeaderSize = 12;
    static constexpr s32 kRepSharable = 0;

    u32 AllocateRepForText(std::string_view value, const std::string& tag);
    u32 EmptyRepStorage();
    u32 EmptyRefData();
    u32 DataPointerOf(u32 self) const;
    u32 RepPointerFromData(u32 data_pointer) const;

    Emulator& emulator_;
    u32 empty_rep_storage_ = 0;
};
