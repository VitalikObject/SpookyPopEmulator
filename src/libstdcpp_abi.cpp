#include "libstdcpp_abi.h"

#include "emulator.h"

#include <algorithm>

LibStdCppAbi::LibStdCppAbi(Emulator& emulator)
    : emulator_(emulator) {}

u32 LibStdCppAbi::RegisterEmptyRepStorage(const std::string& symbol) {
    if (empty_rep_storage_ == 0) {
        empty_rep_storage_ = emulator_.AllocateData(kRepHeaderSize + 4, 4, symbol);
        emulator_.memory_.Write32(empty_rep_storage_ + kRepLengthOffset, 0);
        emulator_.memory_.Write32(empty_rep_storage_ + kRepCapacityOffset, 0);
        emulator_.memory_.Write32(empty_rep_storage_ + kRepRefcountOffset, kRepSharable);
        emulator_.memory_.Write8(empty_rep_storage_ + kRepHeaderSize, 0);
    }
    return empty_rep_storage_;
}

void LibStdCppAbi::InitializeString(const u32 self, const std::string_view value, const std::string& tag) {
    const u32 data = value.empty() ? EmptyRefData() : AllocateRepForText(value, tag);
    emulator_.memory_.Write32(self, data);
}

void LibStdCppAbi::CopyString(const u32 dst, const u32 src, const std::string& tag) {
    InitializeString(dst, ReadString(src), tag);
}

void LibStdCppAbi::DestroyString(const u32 self) {
    emulator_.memory_.Write32(self, EmptyRefData());
}

std::string LibStdCppAbi::ReadString(const u32 self) const {
    const u32 data_pointer = DataPointerOf(self);
    if (data_pointer == 0) {
        return {};
    }
    const u32 rep = RepPointerFromData(data_pointer);
    const u32 length = emulator_.memory_.Read32(rep + kRepLengthOffset);
    if (length == 0) {
        return {};
    }
    const auto bytes = emulator_.memory_.ReadBuffer(data_pointer, length);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

int LibStdCppAbi::CompareStringWithCString(const u32 self, const u32 cstring) const {
    const std::string lhs = ReadString(self);
    const std::string rhs = emulator_.ReadGuestCString(cstring);
    if (lhs < rhs) {
        return -1;
    }
    if (lhs > rhs) {
        return 1;
    }
    return 0;
}

int LibStdCppAbi::CompareStrings(const u32 lhs, const u32 rhs) const {
    const std::string lhs_value = ReadString(lhs);
    const std::string rhs_value = ReadString(rhs);
    if (lhs_value < rhs_value) {
        return -1;
    }
    if (lhs_value > rhs_value) {
        return 1;
    }
    return 0;
}

void LibStdCppAbi::AssignFromCString(const u32 self, const u32 cstring, const u32 length, const std::string& tag) {
    if (cstring == 0 || length == 0) {
        InitializeString(self, std::string_view{}, tag);
        return;
    }
    const auto bytes = emulator_.memory_.ReadBuffer(cstring, length);
    InitializeString(self, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), tag);
}

void LibStdCppAbi::AssignFromString(const u32 self, const u32 other, const std::string& tag) {
    InitializeString(self, ReadString(other), tag);
}

void LibStdCppAbi::PushBack(const u32 self, const char ch, const std::string& tag) {
    std::string value = ReadString(self);
    value.push_back(ch);
    InitializeString(self, value, tag);
}

void LibStdCppAbi::Substr(const u32 out, const u32 self, const u32 pos, const u32 count, const std::string& tag) {
    const std::string source = ReadString(self);
    const std::size_t start = std::min<std::size_t>(pos, source.size());
    InitializeString(out, source.substr(start, count), tag);
}

u32 LibStdCppAbi::AllocateRepForText(const std::string_view value, const std::string& tag) {
    const u32 rep = emulator_.AllocateData(kRepHeaderSize + static_cast<u32>(value.size()) + 1, 4, tag);
    emulator_.memory_.Write32(rep + kRepLengthOffset, static_cast<u32>(value.size()));
    emulator_.memory_.Write32(rep + kRepCapacityOffset, static_cast<u32>(value.size()));
    emulator_.memory_.Write32(rep + kRepRefcountOffset, kRepSharable);
    emulator_.memory_.WriteBuffer(rep + kRepHeaderSize, std::span<const u8>(reinterpret_cast<const u8*>(value.data()), value.size()));
    emulator_.memory_.Write8(rep + kRepHeaderSize + static_cast<u32>(value.size()), 0);
    return rep + kRepHeaderSize;
}

u32 LibStdCppAbi::EmptyRepStorage() {
    return RegisterEmptyRepStorage("__ZNSs4_Rep20_S_empty_rep_storageE");
}

u32 LibStdCppAbi::EmptyRefData() {
    return EmptyRepStorage() + kRepHeaderSize;
}

u32 LibStdCppAbi::DataPointerOf(const u32 self) const {
    return emulator_.memory_.Read32(self);
}

u32 LibStdCppAbi::RepPointerFromData(const u32 data_pointer) const {
    return data_pointer >= kRepHeaderSize ? data_pointer - kRepHeaderSize : 0;
}
