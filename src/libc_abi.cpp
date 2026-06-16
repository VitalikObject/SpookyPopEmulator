#include "libc_abi.h"

#include "emulator.h"

#include <cerrno>
#include <cstdio>

namespace {

bool ModeAllowsRead(const std::string& mode) {
    return mode.find('r') != std::string::npos || mode.find('+') != std::string::npos;
}

bool ModeAllowsWrite(const std::string& mode) {
    return mode.find('w') != std::string::npos || mode.find('a') != std::string::npos || mode.find('+') != std::string::npos;
}

bool ModeAppends(const std::string& mode) {
    return mode.find('a') != std::string::npos;
}

int FileDescriptorFromFile(std::FILE* file) {
#if defined(_WIN32)
    return ::_fileno(file);
#else
    return ::fileno(file);
#endif
}

}  // namespace

LibcAbi::LibcAbi(Emulator& emulator)
    : emulator_(emulator) {
    EnsureMainThread();
}

u32 LibcAbi::RegisterStdStreamCell(const std::string& symbol, const bool stderr_stream) {
    u32& cell = stderr_stream ? stderr_cell_ : stdout_cell_;
    if (cell != 0) {
        return cell;
    }

    const int fd_hint = stderr_stream ? 2 : 1;
    const u16 flags = kGuestFileFlagWrite;
    const u32 guest_file = WrapHostFile(stderr_stream ? stderr : stdout, {}, symbol + ":file", fd_hint, flags);
    cell = emulator_.AllocateData(4, 4, symbol + ":cell");
    emulator_.memory_.Write32(cell, guest_file);
    return cell;
}

u32 LibcAbi::RegisterDispatchMainQueue(const std::string& symbol) {
    if (dispatch_main_queue_ != 0) {
        return dispatch_main_queue_;
    }
    dispatch_main_queue_ = emulator_.AllocateData(kGuestDispatchQueueSize, 8, symbol);
    emulator_.memory_.Write32(dispatch_main_queue_ + kDispatchQueueOffsetMagic, kDispatchQueueMagic);
    return dispatch_main_queue_;
}

u32 LibcAbi::OpenFile(const std::filesystem::path& host_path, const std::string& mode, const std::string& tag) {
    std::FILE* file = std::fopen(host_path.string().c_str(), mode.c_str());
    if (file == nullptr) {
        return 0;
    }
    return WrapHostFile(file, host_path, tag, FileDescriptorFromFile(file), FlagsForMode(mode));
}

u32 LibcAbi::OpenMemoryFile(std::vector<u8> data, const std::filesystem::path& display_path, const std::string& tag) {
    auto bytes = std::make_unique<std::vector<u8>>(std::move(data));
    if (bytes->empty()) {
        bytes->push_back(0);
    }
#if defined(__ANDROID__) || defined(__linux__)
    std::FILE* file = fmemopen(bytes->data(), bytes->size(), "rb");
#else
    std::FILE* file = nullptr;
#endif
    if (file == nullptr) {
        return 0;
    }
    const u32 guest_file = WrapHostFile(file, display_path, tag, NextSyntheticFd(), kGuestFileFlagRead);
    emulator_.file_handles_[guest_file].memory_bytes = std::move(bytes);
    return guest_file;
}

u32 LibcAbi::WrapHostFile(std::FILE* file, const std::filesystem::path& host_path, const std::string& tag, const int fd_hint, const u16 flags) {
    const u32 guest_file = emulator_.AllocateData(kGuestFileSize, 8, tag);
    emulator_.file_handles_[guest_file] = Emulator::FileHandle{
        .file = file,
        .path = host_path,
    };
    emulator_.host_objects_[guest_file] = Emulator::HostObject{
        .kind = Emulator::ObjKind::FileHandle,
        .class_name = "FILE",
    };
    InitializeGuestFileObject(guest_file, fd_hint, flags);
    return guest_file;
}

int LibcAbi::NextSyntheticFd() {
    return next_synthetic_fd_++;
}

std::FILE* LibcAbi::LookupHostFile(const u32 guest_file) const {
    const auto it = emulator_.file_handles_.find(guest_file);
    return it == emulator_.file_handles_.end() ? nullptr : it->second.file;
}

u32 LibcAbi::LookupGuestFileByDescriptor(const int fd) const {
    for (const auto& [guest_file, handle] : emulator_.file_handles_) {
        (void)handle;
        if (Fileno(guest_file) == fd) {
            return guest_file;
        }
    }
    return 0;
}

int LibcAbi::CloseFile(const u32 guest_file, const bool close_host) {
    if (guest_file == 0) {
        return 0;
    }
    const auto it = emulator_.file_handles_.find(guest_file);
    if (it == emulator_.file_handles_.end()) {
        return EOF;
    }
    if (close_host && it->second.file != nullptr) {
        std::fclose(it->second.file);
    }
    emulator_.file_handles_.erase(it);
    UpdateFileFlags(guest_file, 0, 0xFFFFu);
    emulator_.memory_.Write16(guest_file + kFileOffsetFlags, 0);
    emulator_.memory_.Write16(guest_file + kFileOffsetFileNo, static_cast<u16>(-1));
    emulator_.host_objects_.erase(guest_file);
    return 0;
}

int LibcAbi::Fileno(const u32 guest_file) const {
    const auto it = emulator_.file_handles_.find(guest_file);
    if (it == emulator_.file_handles_.end()) {
        return -1;
    }
    return static_cast<int>(static_cast<int16_t>(emulator_.memory_.Read16(guest_file + kFileOffsetFileNo)));
}

void LibcAbi::SyncFileAfterRead(const u32 guest_file, const std::size_t bytes_read, const bool eof_reached) {
    std::FILE* file = LookupHostFile(guest_file);
    if (file == nullptr) {
        return;
    }
    WriteFilePosition(guest_file, static_cast<long>(std::ftell(file)));
    emulator_.memory_.Write32(guest_file + kFileOffsetReadCount, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetWriteCount, 0);
    UpdateFileFlags(guest_file, eof_reached ? kGuestFileFlagEof : 0, eof_reached ? 0 : kGuestFileFlagEof);
    if (bytes_read == 0 && std::ferror(file) != 0) {
        UpdateFileFlags(guest_file, kGuestFileFlagErr, 0);
    }
}

void LibcAbi::SyncFileAfterWrite(const u32 guest_file) {
    std::FILE* file = LookupHostFile(guest_file);
    if (file == nullptr) {
        return;
    }
    WriteFilePosition(guest_file, static_cast<long>(std::ftell(file)));
    emulator_.memory_.Write32(guest_file + kFileOffsetReadCount, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetWriteCount, 0);
    UpdateFileFlags(guest_file, 0, kGuestFileFlagEof);
    if (std::ferror(file) != 0) {
        UpdateFileFlags(guest_file, kGuestFileFlagErr, 0);
    }
}

void LibcAbi::SetFileError(const u32 guest_file) {
    UpdateFileFlags(guest_file, kGuestFileFlagErr, 0);
}

int LibcAbi::PthreadMutexInit(const u32 mutex) {
    if (mutex == 0) {
        return EINVAL;
    }
    std::vector<u8> zeros(kGuestPthreadMutexSize, 0);
    emulator_.memory_.WriteBuffer(mutex, zeros);
    MutexState state;
    mutexes_[mutex] = state;
    WriteMutexMemory(mutex, state);
    return 0;
}

int LibcAbi::PthreadMutexDestroy(const u32 mutex) {
    auto it = mutexes_.find(mutex);
    if (it == mutexes_.end()) {
        EnsureMutexState(mutex);
        it = mutexes_.find(mutex);
    }
    if (it == mutexes_.end()) {
        return EINVAL;
    }
    if (it->second.locked) {
        return EBUSY;
    }
    mutexes_.erase(it);
    emulator_.memory_.Write32(mutex + kMutexOffsetSig, kPthreadMutexSigDead);
    return 0;
}

int LibcAbi::PthreadMutexLock(const u32 mutex) {
    if (mutex == 0) {
        return EINVAL;
    }
    MutexState& state = EnsureMutexState(mutex);
    const u32 owner = EnsureMainThread();
    if (state.locked && state.owner != owner) {
        return EBUSY;
    }
    state.locked = true;
    state.owner = owner;
    state.recursion += 1;
    WriteMutexMemory(mutex, state);
    return 0;
}

int LibcAbi::PthreadMutexUnlock(const u32 mutex) {
    auto it = mutexes_.find(mutex);
    if (it == mutexes_.end()) {
        EnsureMutexState(mutex);
        it = mutexes_.find(mutex);
    }
    if (it == mutexes_.end()) {
        return EINVAL;
    }
    const u32 owner = EnsureMainThread();
    if (!it->second.locked || it->second.owner != owner) {
        return EPERM;
    }
    if (it->second.recursion > 1) {
        it->second.recursion -= 1;
    } else {
        it->second.locked = false;
        it->second.owner = 0;
        it->second.recursion = 0;
    }
    WriteMutexMemory(mutex, it->second);
    return 0;
}

int LibcAbi::PthreadCreate(const u32 out_thread, const u32 /*attr*/, const u32 start_routine, const u32 arg) {
    if (out_thread == 0) {
        return EINVAL;
    }
    const u32 thread = CreateThreadObject(start_routine, arg, false);
    emulator_.memory_.Write32(out_thread, thread);
    return 0;
}

u32 LibcAbi::MainThread() const {
    return main_thread_;
}

u32 LibcAbi::EnsureMainThread() {
    if (main_thread_ != 0) {
        return main_thread_;
    }
    main_thread_ = emulator_.AllocateData(kGuestPthreadSize, 16, "pthread.main");
    emulator_.memory_.Write32(main_thread_ + kThreadOffsetSig, kPthreadThreadSig);
    emulator_.memory_.Write32(main_thread_ + kThreadOffsetCleanupStack, 0);
    emulator_.memory_.Write32(main_thread_ + kThreadOffsetDetached, 0);
    emulator_.memory_.Write32(main_thread_ + kThreadOffsetFinished, 0);
    threads_[main_thread_] = ThreadState{};
    return main_thread_;
}

u32 LibcAbi::CreateThreadObject(const u32 start_routine, const u32 arg, const bool detached) {
    const u32 thread = emulator_.AllocateData(kGuestPthreadSize, 16, "pthread");
    emulator_.memory_.Write32(thread + kThreadOffsetSig, kPthreadThreadSig);
    emulator_.memory_.Write32(thread + kThreadOffsetCleanupStack, 0);
    emulator_.memory_.Write32(thread + kThreadOffsetStartRoutine, start_routine);
    emulator_.memory_.Write32(thread + kThreadOffsetArgument, arg);
    emulator_.memory_.Write32(thread + kThreadOffsetDetached, detached ? 1u : 0u);
    emulator_.memory_.Write32(thread + kThreadOffsetFinished, 1);
    emulator_.memory_.Write32(thread + kThreadOffsetReturnValue, 0);
    emulator_.memory_.Write32(thread + kThreadOffsetErrno, 0);
    threads_[thread] = ThreadState{
        .start_routine = start_routine,
        .argument = arg,
        .detached = detached,
        .finished = true,
        .return_value = 0,
    };
    return thread;
}

LibcAbi::MutexState& LibcAbi::EnsureMutexState(const u32 mutex) {
    const auto [it, inserted] = mutexes_.try_emplace(mutex);
    if (inserted) {
        const u32 sig = emulator_.memory_.IsMapped(mutex) ? emulator_.memory_.Read32(mutex + kMutexOffsetSig) : 0;
        if (sig == kPthreadMutexSigDead) {
            it->second = {};
        }
        WriteMutexMemory(mutex, it->second);
    }
    return it->second;
}

void LibcAbi::WriteMutexMemory(const u32 mutex, const MutexState& state) const {
    emulator_.memory_.Write32(mutex + kMutexOffsetSig, kPthreadMutexSigLive);
    emulator_.memory_.Write32(mutex + kMutexOffsetLocked, state.locked ? 1u : 0u);
    emulator_.memory_.Write32(mutex + kMutexOffsetOwner, state.owner);
    emulator_.memory_.Write32(mutex + kMutexOffsetRecursion, state.recursion);
}

void LibcAbi::InitializeGuestFileObject(const u32 guest_file, const int fd_hint, const u16 flags) const {
    std::vector<u8> zeros(kGuestFileSize, 0);
    emulator_.memory_.WriteBuffer(guest_file, zeros);
    emulator_.memory_.Write32(guest_file + kFileOffsetPtr, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetReadCount, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetWriteCount, 0);
    emulator_.memory_.Write16(guest_file + kFileOffsetFlags, flags);
    emulator_.memory_.Write16(guest_file + kFileOffsetFileNo, static_cast<u16>(fd_hint));
    emulator_.memory_.Write32(guest_file + kFileOffsetBufferBase, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetBufferSize, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetLineBufferSize, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetCookie, guest_file);
    emulator_.memory_.Write32(guest_file + kFileOffsetSeek, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetWriteFn, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetBlockSize, 4096);
    WriteFilePosition(guest_file, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetMutex, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetOwner, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetLockCount, 0);
    emulator_.memory_.Write32(guest_file + kFileOffsetFlags2, 0);
}

void LibcAbi::WriteFilePosition(const u32 guest_file, const long position) const {
    emulator_.memory_.Write32(guest_file + kFileOffsetOffset + 0, static_cast<u32>(position));
    emulator_.memory_.Write32(guest_file + kFileOffsetOffset + 4, position < 0 ? 0xFFFFFFFFu : 0u);
}

void LibcAbi::UpdateFileFlags(const u32 guest_file, const u16 set_mask, const u16 clear_mask) const {
    u16 flags = emulator_.memory_.Read16(guest_file + kFileOffsetFlags);
    flags = static_cast<u16>((flags | set_mask) & ~clear_mask);
    emulator_.memory_.Write16(guest_file + kFileOffsetFlags, flags);
}

u16 LibcAbi::FlagsForMode(const std::string& mode) {
    u16 flags = 0;
    if (ModeAllowsRead(mode) && ModeAllowsWrite(mode)) {
        flags |= kGuestFileFlagReadWrite;
    } else if (ModeAllowsRead(mode)) {
        flags |= kGuestFileFlagRead;
    } else if (ModeAllowsWrite(mode)) {
        flags |= kGuestFileFlagWrite;
    }
    if (ModeAppends(mode)) {
        flags |= kGuestFileFlagAppend;
    }
    return flags;
}
