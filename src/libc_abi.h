#pragma once

#include "common.h"

class Emulator;

class LibcAbi final {
public:
    explicit LibcAbi(Emulator& emulator);

    u32 RegisterStdStreamCell(const std::string& symbol, bool stderr_stream);
    u32 RegisterDispatchMainQueue(const std::string& symbol);

    u32 OpenFile(const std::filesystem::path& host_path, const std::string& mode, const std::string& tag);
    u32 OpenMemoryFile(std::vector<u8> data, const std::filesystem::path& display_path, const std::string& tag);
    u32 WrapHostFile(std::FILE* file, const std::filesystem::path& host_path, const std::string& tag, int fd_hint, u16 flags);
    std::FILE* LookupHostFile(u32 guest_file) const;
    u32 LookupGuestFileByDescriptor(int fd) const;
    int CloseFile(u32 guest_file, bool close_host = true);
    int Fileno(u32 guest_file) const;
    void SyncFileAfterRead(u32 guest_file, std::size_t bytes_read, bool eof_reached);
    void SyncFileAfterWrite(u32 guest_file);
    void SetFileError(u32 guest_file);

    int PthreadMutexInit(u32 mutex);
    int PthreadMutexDestroy(u32 mutex);
    int PthreadMutexLock(u32 mutex);
    int PthreadMutexUnlock(u32 mutex);
    int PthreadCreate(u32 out_thread, u32 attr, u32 start_routine, u32 arg);
    u32 MainThread() const;

private:
    struct MutexState {
        bool locked = false;
        u32 owner = 0;
        u32 recursion = 0;
    };

    struct ThreadState {
        u32 start_routine = 0;
        u32 argument = 0;
        bool detached = false;
        bool finished = false;
        u32 return_value = 0;
    };

    static constexpr u32 kGuestFileSize = 0xA0;
    static constexpr u32 kGuestPthreadSize = 4096;
    static constexpr u32 kGuestPthreadMutexSize = 44;
    static constexpr u32 kGuestDispatchQueueSize = 0x40;

    static constexpr u32 kFileOffsetPtr = 0;
    static constexpr u32 kFileOffsetReadCount = 4;
    static constexpr u32 kFileOffsetWriteCount = 8;
    static constexpr u32 kFileOffsetFlags = 12;
    static constexpr u32 kFileOffsetFileNo = 14;
    static constexpr u32 kFileOffsetBufferBase = 16;
    static constexpr u32 kFileOffsetBufferSize = 20;
    static constexpr u32 kFileOffsetLineBufferSize = 24;
    static constexpr u32 kFileOffsetCookie = 28;
    static constexpr u32 kFileOffsetSeek = 40;
    static constexpr u32 kFileOffsetWriteFn = 44;
    static constexpr u32 kFileOffsetBlockSize = 76;
    static constexpr u32 kFileOffsetOffset = 80;
    static constexpr u32 kFileOffsetMutex = 88;
    static constexpr u32 kFileOffsetOwner = 92;
    static constexpr u32 kFileOffsetLockCount = 96;
    static constexpr u32 kFileOffsetFlags2 = 112;

    static constexpr u32 kThreadOffsetSig = 0;
    static constexpr u32 kThreadOffsetCleanupStack = 4;
    static constexpr u32 kThreadOffsetStartRoutine = 8;
    static constexpr u32 kThreadOffsetArgument = 12;
    static constexpr u32 kThreadOffsetDetached = 16;
    static constexpr u32 kThreadOffsetFinished = 20;
    static constexpr u32 kThreadOffsetReturnValue = 24;
    static constexpr u32 kThreadOffsetErrno = 28;

    static constexpr u32 kMutexOffsetSig = 0;
    static constexpr u32 kMutexOffsetLocked = 4;
    static constexpr u32 kMutexOffsetOwner = 8;
    static constexpr u32 kMutexOffsetRecursion = 12;

    static constexpr u32 kDispatchQueueOffsetMagic = 0;

    static constexpr u32 kPthreadMutexSigInit = 0x32AAABA7u;
    static constexpr u32 kPthreadMutexSigLive = 0x4D555458u;
    static constexpr u32 kPthreadMutexSigDead = 0x4D555444u;
    static constexpr u32 kPthreadThreadSig = 0x54485244u;
    static constexpr u32 kDispatchQueueMagic = 0x4453514Du;

    static constexpr u16 kGuestFileFlagRead = 0x0004;
    static constexpr u16 kGuestFileFlagWrite = 0x0008;
    static constexpr u16 kGuestFileFlagReadWrite = 0x0010;
    static constexpr u16 kGuestFileFlagEof = 0x0020;
    static constexpr u16 kGuestFileFlagErr = 0x0040;
    static constexpr u16 kGuestFileFlagAppend = 0x0100;

    u32 EnsureMainThread();
    u32 CreateThreadObject(u32 start_routine, u32 arg, bool detached);
    MutexState& EnsureMutexState(u32 mutex);
    void WriteMutexMemory(u32 mutex, const MutexState& state) const;
    void InitializeGuestFileObject(u32 guest_file, int fd_hint, u16 flags) const;
    void WriteFilePosition(u32 guest_file, long position) const;
    void UpdateFileFlags(u32 guest_file, u16 set_mask, u16 clear_mask) const;
    static u16 FlagsForMode(const std::string& mode);
    int NextSyntheticFd();

    Emulator& emulator_;
    u32 main_thread_ = 0;
    u32 stdout_cell_ = 0;
    u32 stderr_cell_ = 0;
    u32 dispatch_main_queue_ = 0;
    int next_synthetic_fd_ = 10000;
    std::unordered_map<u32, MutexState> mutexes_;
    std::unordered_map<u32, ThreadState> threads_;
};
