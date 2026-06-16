#include "emulator.h"
#include "crypto_compat.h"
#include "graphics/host_gl_backend.h"
#include "libc_abi.h"
#include "libstdcpp_abi.h"
#include "objc_abi.h"
#include "shims/shim_registry.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#if defined(__ANDROID__)
#include <android/log.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#endif

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <zlib.h>
#include <sqlite3.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb/stb_truetype.h"

namespace {

constexpr u32 kArmSvcBase = 0xEF000000;
constexpr u32 kArmBxLr = 0xE12FFF1E;
constexpr u32 kArmBxIp = 0xE12FFF1C;
constexpr u16 kThumbSvcBase = 0xDF00;
constexpr u16 kThumbBxLr = 0x4770;
constexpr u16 kThumbMovsR0Zero = 0x2000;
constexpr u32 kObjcObjectSize = 0x40;
constexpr u32 kMaxGuestCString = 1024 * 1024;
constexpr u64 kGuestRunSliceTicks = 5'000'000;
constexpr u32 kGuestStringCodepointLengthOffset = 0x4;
constexpr u32 kGuestStringByteLengthOffset = 0x8;
constexpr u32 kGuestStringByteOffsetCacheOffset = 0xC;
constexpr u32 kGuestStringCodepointIndexCacheOffset = 0x10;
constexpr u32 kGuestStringStorageOffset = 0x14;
constexpr u32 kGuestStringInlineCapacity = 7;
constexpr u32 kGuestLogicVersionIsDev = 0x000780EC;
constexpr u32 kGuestLogicVersionIsProd = 0x000780F0;
constexpr u32 kGuestLogicDefinesIsPlatformAndroid = 0x000EEB6C;
constexpr u32 kGuestLogicDefinesIsPlatformIOS = 0x000EEB68;
constexpr u32 kGameSettingsIsMusicEnabled = 0x0001FAC4;
constexpr u32 kGuestGameMainIsIapWarningNeeded = 0x00028C9C;
constexpr u32 kGuestResourceManagerLazyLoadingEnabled = 0x002AF6C0;
constexpr bool kDisableHostTextRenderingProbe = true;
constexpr bool kEnableHostCoreTextRendering = true;
constexpr bool kUseRasterCoreTextMetrics = false;

struct BitmapContentStats {
    u32 sampled = 0;
    u32 alpha_nonzero = 0;
    u32 rgb_nonzero = 0;
    u32 visible_rgb_nonzero = 0;
    u32 opaque_dark = 0;
};

BitmapContentStats AnalyzeBitmapContent(
    const s32 width,
    const s32 height,
    const s32 bytes_per_row,
    const std::vector<u8>& pixels) {
    BitmapContentStats stats;
    if (width <= 0 || height <= 0 || pixels.empty()) {
        return stats;
    }
    const std::size_t stride = static_cast<std::size_t>(
        bytes_per_row > 0 ? bytes_per_row : width * 4);
    for (s32 y = 0; y < height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * stride;
        if (row >= pixels.size()) {
            break;
        }
        for (s32 x = 0; x < width; ++x) {
            const std::size_t offset = row + static_cast<std::size_t>(x) * 4;
            if (offset + 3 >= pixels.size()) {
                break;
            }
            const u8 r = pixels[offset + 0];
            const u8 g = pixels[offset + 1];
            const u8 b = pixels[offset + 2];
            const u8 a = pixels[offset + 3];
            ++stats.sampled;
            if (a != 0) {
                ++stats.alpha_nonzero;
            }
            if (r != 0 || g != 0 || b != 0) {
                ++stats.rgb_nonzero;
            }
            if (a != 0 && (r > 3 || g > 3 || b > 3)) {
                ++stats.visible_rgb_nonzero;
            }
            if (a >= 240 && r <= 4 && g <= 4 && b <= 4) {
                ++stats.opaque_dark;
            }
        }
    }
    return stats;
}

bool ShouldSkipGeneratedBitmapImage(const BitmapContentStats& stats) {
    if (stats.sampled == 0 || stats.alpha_nonzero == 0) {
        return true;
    }
    const double sampled = static_cast<double>(stats.sampled);
    const double alpha_ratio = static_cast<double>(stats.alpha_nonzero) / sampled;
    const double visible_ratio = static_cast<double>(stats.visible_rgb_nonzero) / sampled;
    const double dark_ratio = static_cast<double>(stats.opaque_dark) / sampled;
    return (alpha_ratio > 0.92 && visible_ratio < 0.02)
        || (dark_ratio > 0.80 && visible_ratio < 0.05);
}

std::mutex g_host_touch_mutex;
std::deque<HostTouchEvent> g_host_touch_events;
std::mutex g_host_key_mutex;
std::deque<HostKeyEvent> g_host_key_events;
std::mutex g_host_popup_mutex;
std::deque<HostPopupResult> g_host_popup_results;
std::mutex g_host_keyboard_callback_mutex;
HostKeyboardVisibilityCallback g_host_keyboard_visibility_callback = nullptr;
std::mutex g_host_popup_callback_mutex;
HostPopupRequestCallback g_host_popup_request_callback = nullptr;
HostPopupDismissCallback g_host_popup_dismiss_callback = nullptr;

void RequestHostKeyboardVisibility(const bool visible) {
    HostKeyboardVisibilityCallback callback = nullptr;
    {
        std::lock_guard lock(g_host_keyboard_callback_mutex);
        callback = g_host_keyboard_visibility_callback;
    }
    if (callback != nullptr) {
        callback(visible);
    }
}

bool RequestHostPopup(const HostPopupRequest& request) {
    HostPopupRequestCallback callback = nullptr;
    {
        std::lock_guard lock(g_host_popup_callback_mutex);
        callback = g_host_popup_request_callback;
    }
    if (callback == nullptr) {
        return false;
    }
    callback(request);
    return true;
}

void RequestHostPopupDismiss(const u32 token) {
    HostPopupDismissCallback callback = nullptr;
    {
        std::lock_guard lock(g_host_popup_callback_mutex);
        callback = g_host_popup_dismiss_callback;
    }
    if (callback != nullptr) {
        callback(token);
    }
}

bool StartsWith(const std::string_view text, const std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool EndsWith(const std::string_view text, const std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::string NormalizeGuestResourceKey(std::string path, const std::string_view guest_home) {
    const bool under_guest_home = StartsWith(path, guest_home);
    if (under_guest_home) {
        path.erase(0, guest_home.size());
    }

    std::filesystem::path normalized = std::filesystem::path(path).lexically_normal();
    path = normalized.generic_string();

    if (under_guest_home || (!path.empty() && path.front() != '/')) {
        while (!path.empty() && path.front() == '/') {
            path.erase(path.begin());
        }
    }

    if (path == ".") {
        path.clear();
    }
    return path;
}

bool IsFontPath(const std::string_view path) {
    return path.find("font/") != std::string_view::npos
        || EndsWith(path, ".otf")
        || EndsWith(path, ".ttf")
        || EndsWith(path, ".fnt");
}

std::string GuestOpenModeString(const u32 guest_flags) {
    constexpr u32 kGuestOpenAccessMask = 0x3;
    constexpr u32 kGuestOpenWriteOnly = 0x1;
    constexpr u32 kGuestOpenReadWrite = 0x2;
    constexpr u32 kGuestOpenAppend = 0x8;
    constexpr u32 kGuestOpenCreate = 0x200;
    constexpr u32 kGuestOpenTruncate = 0x400;

    const u32 access = guest_flags & kGuestOpenAccessMask;
    const bool append = (guest_flags & kGuestOpenAppend) != 0;
    const bool create = (guest_flags & kGuestOpenCreate) != 0;
    const bool truncate = (guest_flags & kGuestOpenTruncate) != 0;

    if (access == kGuestOpenWriteOnly) {
        return append ? "ab" : (create || truncate ? "wb" : "rb");
    }
    if (access == kGuestOpenReadWrite) {
        return append ? "a+b" : (create || truncate ? "w+b" : "r+b");
    }
    return "rb";
}

bool GuestOpenIsReadOnly(const u32 guest_flags) {
    constexpr u32 kGuestOpenAccessMask = 0x3;
    constexpr u32 kGuestOpenAppend = 0x8;
    constexpr u32 kGuestOpenCreate = 0x200;
    constexpr u32 kGuestOpenTruncate = 0x400;
    return (guest_flags & kGuestOpenAccessMask) == 0
        && (guest_flags & (kGuestOpenAppend | kGuestOpenCreate | kGuestOpenTruncate)) == 0;
}

float BitsToFloat(const u32 value) {
    return std::bit_cast<float>(value);
}

u32 FloatToBits(const float value) {
    return std::bit_cast<u32>(value);
}

u64 Pack64(const u32 lo, const u32 hi) {
    return static_cast<u64>(lo) | (static_cast<u64>(hi) << 32);
}

int FileDescriptorFromFile(std::FILE* file) {
#if defined(_WIN32)
    return ::_fileno(file);
#else
    return ::fileno(file);
#endif
}

u16 ReadLe16(const std::vector<u8>& bytes, const std::size_t offset) {
    if (offset + 2 > bytes.size()) {
        return 0;
    }
    return static_cast<u16>(bytes[offset] | (bytes[offset + 1] << 8));
}

u32 ReadLe32(const std::vector<u8>& bytes, const std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return static_cast<u32>(bytes[offset])
        | (static_cast<u32>(bytes[offset + 1]) << 8)
        | (static_cast<u32>(bytes[offset + 2]) << 16)
        | (static_cast<u32>(bytes[offset + 3]) << 24);
}

u64 ReadLe64(const std::vector<u8>& bytes, const std::size_t offset) {
    if (offset + 8 > bytes.size()) {
        return 0;
    }
    return static_cast<u64>(bytes[offset])
        | (static_cast<u64>(bytes[offset + 1]) << 8)
        | (static_cast<u64>(bytes[offset + 2]) << 16)
        | (static_cast<u64>(bytes[offset + 3]) << 24)
        | (static_cast<u64>(bytes[offset + 4]) << 32)
        | (static_cast<u64>(bytes[offset + 5]) << 40)
        | (static_cast<u64>(bytes[offset + 6]) << 48)
        | (static_cast<u64>(bytes[offset + 7]) << 56);
}

u16 ReadBe16(const std::vector<u8>& bytes, const std::size_t offset) {
    if (offset + 2 > bytes.size()) {
        return 0;
    }
    return (static_cast<u16>(bytes[offset]) << 8) | static_cast<u16>(bytes[offset + 1]);
}

u32 ReadBe32(const std::vector<u8>& bytes, const std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return (static_cast<u32>(bytes[offset]) << 24)
        | (static_cast<u32>(bytes[offset + 1]) << 16)
        | (static_cast<u32>(bytes[offset + 2]) << 8)
        | static_cast<u32>(bytes[offset + 3]);
}

u64 ReadBe64(const std::vector<u8>& bytes, const std::size_t offset) {
    if (offset + 8 > bytes.size()) {
        return 0;
    }
    return (static_cast<u64>(bytes[offset]) << 56)
        | (static_cast<u64>(bytes[offset + 1]) << 48)
        | (static_cast<u64>(bytes[offset + 2]) << 40)
        | (static_cast<u64>(bytes[offset + 3]) << 32)
        | (static_cast<u64>(bytes[offset + 4]) << 24)
        | (static_cast<u64>(bytes[offset + 5]) << 16)
        | (static_cast<u64>(bytes[offset + 6]) << 8)
        | static_cast<u64>(bytes[offset + 7]);
}

void AppendLe16(std::vector<u8>& bytes, const u16 value) {
    bytes.push_back(static_cast<u8>(value & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 8) & 0xFFu));
}

void AppendLe32(std::vector<u8>& bytes, const u32 value) {
    bytes.push_back(static_cast<u8>(value & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 8) & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 16) & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 24) & 0xFFu));
}

void AppendLe64(std::vector<u8>& bytes, const u64 value) {
    bytes.push_back(static_cast<u8>(value & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 8) & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 16) & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 24) & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 32) & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 40) & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 48) & 0xFFu));
    bytes.push_back(static_cast<u8>((value >> 56) & 0xFFu));
}

constexpr u32 MakeFourCC(const char a, const char b, const char c, const char d) {
    return (static_cast<u32>(static_cast<u8>(a)) << 24)
        | (static_cast<u32>(static_cast<u8>(b)) << 16)
        | (static_cast<u32>(static_cast<u8>(c)) << 8)
        | static_cast<u32>(static_cast<u8>(d));
}

struct AudioStreamBasicDescriptionData {
    double sample_rate = 0.0;
    u32 format_id = 0;
    u32 format_flags = 0;
    u32 bytes_per_packet = 0;
    u32 frames_per_packet = 0;
    u32 bytes_per_frame = 0;
    u32 channels_per_frame = 0;
    u32 bits_per_channel = 0;
    u32 reserved = 0;
};

struct HostPcmAudio {
    std::vector<u8> pcm;
    u32 format = 0;
    u32 frequency = 0;
    u32 channels = 0;
    u32 bits_per_sample = 0;
    u32 bytes_per_packet = 0;
    u32 frames_per_packet = 1;
    u32 file_format = 0;
};

std::optional<AudioStreamBasicDescriptionData> DecodeAudioStreamBasicDescription(const std::vector<u8>& bytes, const std::size_t offset = 0) {
    constexpr std::size_t kSize = 40;
    if (offset + kSize > bytes.size()) {
        return std::nullopt;
    }

    AudioStreamBasicDescriptionData description;
    description.sample_rate = std::bit_cast<double>(ReadLe64(bytes, offset + 0));
    description.format_id = ReadLe32(bytes, offset + 8);
    description.format_flags = ReadLe32(bytes, offset + 12);
    description.bytes_per_packet = ReadLe32(bytes, offset + 16);
    description.frames_per_packet = ReadLe32(bytes, offset + 20);
    description.bytes_per_frame = ReadLe32(bytes, offset + 24);
    description.channels_per_frame = ReadLe32(bytes, offset + 28);
    description.bits_per_channel = ReadLe32(bytes, offset + 32);
    description.reserved = ReadLe32(bytes, offset + 36);
    return description;
}

std::vector<u8> EncodeAudioStreamBasicDescription(const AudioStreamBasicDescriptionData& description) {
    std::vector<u8> bytes;
    bytes.reserve(40);
    AppendLe64(bytes, std::bit_cast<u64>(description.sample_rate));
    AppendLe32(bytes, description.format_id);
    AppendLe32(bytes, description.format_flags);
    AppendLe32(bytes, description.bytes_per_packet);
    AppendLe32(bytes, description.frames_per_packet);
    AppendLe32(bytes, description.bytes_per_frame);
    AppendLe32(bytes, description.channels_per_frame);
    AppendLe32(bytes, description.bits_per_channel);
    AppendLe32(bytes, description.reserved);
    return bytes;
}

std::optional<HostPcmAudio> DecodeLinearPcmAudio(
    const std::vector<u8>& bytes,
    const std::size_t data_offset,
    std::size_t data_size,
    const double sample_rate,
    const u32 channels,
    const u32 bits_per_sample,
    const bool little_endian,
    const u32 file_format) {
    if (sample_rate == 0.0 || channels == 0 || (bits_per_sample != 8 && bits_per_sample != 16)) {
        return std::nullopt;
    }
    const u32 bytes_per_packet = std::max<u32>(1, channels * bits_per_sample / 8);
    if (data_offset >= bytes.size()) {
        return std::nullopt;
    }
    data_size = std::min<std::size_t>(data_size, bytes.size() - data_offset);
    data_size -= data_size % bytes_per_packet;
    if (data_size == 0) {
        return std::nullopt;
    }

    HostPcmAudio audio;
    audio.format = 0;
    if (channels == 1 && bits_per_sample == 8) {
        audio.format = 0x1100;
    } else if (channels == 1 && bits_per_sample == 16) {
        audio.format = 0x1101;
    } else if (channels == 2 && bits_per_sample == 8) {
        audio.format = 0x1102;
    } else if (channels == 2 && bits_per_sample == 16) {
        audio.format = 0x1103;
    } else {
        return std::nullopt;
    }

    audio.frequency = static_cast<u32>(sample_rate);
    audio.channels = channels;
    audio.bits_per_sample = bits_per_sample;
    audio.bytes_per_packet = bytes_per_packet;
    audio.frames_per_packet = 1;
    audio.file_format = file_format;
    audio.pcm.assign(bytes.begin() + static_cast<std::ptrdiff_t>(data_offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(data_offset + data_size));
    if (!little_endian && bits_per_sample == 16) {
        for (std::size_t index = 0; index + 1 < audio.pcm.size(); index += 2) {
            std::swap(audio.pcm[index], audio.pcm[index + 1]);
        }
    }
    return audio;
}

std::optional<HostPcmAudio> DecodeWavPcmBytes(const std::vector<u8>& bytes) {
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        return std::nullopt;
    }

    u16 audio_format = 0;
    u16 channels = 0;
    u32 sample_rate = 0;
    u16 bits_per_sample = 0;
    std::size_t data_offset = 0;
    u32 data_size = 0;

    std::size_t offset = 8;
    while (offset + 8 <= bytes.size()) {
        const char* chunk_id = reinterpret_cast<const char*>(bytes.data() + offset);
        const u32 chunk_size = ReadLe32(bytes, offset + 4);
        const std::size_t payload = offset + 8;
        if (payload + chunk_size > bytes.size()) {
            break;
        }
        if (std::memcmp(chunk_id, "fmt ", 4) == 0 && chunk_size >= 16) {
            audio_format = ReadLe16(bytes, payload + 0);
            channels = ReadLe16(bytes, payload + 2);
            sample_rate = ReadLe32(bytes, payload + 4);
            bits_per_sample = ReadLe16(bytes, payload + 14);
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            data_offset = payload;
            data_size = chunk_size;
        }
        offset = payload + chunk_size + (chunk_size & 1u);
    }

    if (audio_format != 1 || sample_rate == 0 || data_offset == 0 || data_size == 0) {
        return std::nullopt;
    }

    return DecodeLinearPcmAudio(
        bytes,
        data_offset,
        data_size,
        static_cast<double>(sample_rate),
        channels,
        bits_per_sample,
        true,
        MakeFourCC('W', 'A', 'V', 'E'));
}

std::optional<HostPcmAudio> DecodeCafePcmBytes(const std::vector<u8>& bytes) {
    if (bytes.size() < 8 || std::memcmp(bytes.data(), "caff", 4) != 0) {
        return std::nullopt;
    }

    u32 channels = 0;
    u32 bits_per_sample = 0;
    double sample_rate = 0.0;
    u32 format_id = 0;
    u32 format_flags = 0;
    u32 bytes_per_packet = 0;
    u32 frames_per_packet = 0;
    std::size_t data_offset = 0;
    std::size_t data_size = 0;

    // CAF has an 8-byte file header ("caff" + version/flags) before the first chunk.
    std::size_t offset = 8;
    while (offset + 12 <= bytes.size()) {
        const u32 chunk_type = ReadBe32(bytes, offset + 0);
        const std::int64_t signed_chunk_size = std::bit_cast<std::int64_t>(ReadBe64(bytes, offset + 4));
        std::size_t chunk_size = signed_chunk_size < 0
            ? bytes.size() - (offset + 12)
            : static_cast<std::size_t>(signed_chunk_size);
        const std::size_t payload = offset + 12;
        if (payload > bytes.size()) {
            break;
        }
        chunk_size = std::min(chunk_size, bytes.size() - payload);
        if (chunk_type == MakeFourCC('d', 'e', 's', 'c') && chunk_size >= 32) {
            sample_rate = std::bit_cast<double>(ReadBe64(bytes, payload + 0));
            format_id = ReadBe32(bytes, payload + 8);
            format_flags = ReadBe32(bytes, payload + 12);
            bytes_per_packet = ReadBe32(bytes, payload + 16);
            frames_per_packet = ReadBe32(bytes, payload + 20);
            channels = ReadBe32(bytes, payload + 24);
            bits_per_sample = ReadBe32(bytes, payload + 28);
        } else if (chunk_type == MakeFourCC('d', 'a', 't', 'a') && chunk_size >= 4) {
            data_offset = payload + 4;
            data_size = chunk_size - 4;
        }
        offset = payload + chunk_size;
    }

    if (format_id != MakeFourCC('l', 'p', 'c', 'm') || sample_rate == 0.0 || channels == 0 || bits_per_sample == 0 || data_offset == 0 || data_size == 0) {
        return std::nullopt;
    }

    const bool little_endian = (format_flags & 0x2u) != 0;
    auto audio = DecodeLinearPcmAudio(
        bytes,
        data_offset,
        data_size,
        sample_rate,
        channels,
        bits_per_sample,
        little_endian,
        MakeFourCC('c', 'a', 'f', 'f'));
    if (audio) {
        audio->bytes_per_packet = bytes_per_packet == 0 ? audio->bytes_per_packet : bytes_per_packet;
        audio->frames_per_packet = frames_per_packet == 0 ? audio->frames_per_packet : frames_per_packet;
    }
    return audio;
}

std::optional<AudioStreamBasicDescriptionData> SniffMp3StreamDescription(const std::vector<u8>& bytes) {
    auto find_frame = [&bytes](std::size_t& offset, u8& header0, u8& header1, u8& header2, u8& header3) -> bool {
        const std::size_t limit = bytes.size() < 4096 ? bytes.size() : 4096;
        for (std::size_t index = 0; index + 4 <= limit; ++index) {
            if (bytes[index] != 0xFFu || (bytes[index + 1] & 0xE0u) != 0xE0u) {
                continue;
            }
            header0 = bytes[index + 0];
            header1 = bytes[index + 1];
            header2 = bytes[index + 2];
            header3 = bytes[index + 3];
            offset = index;
            return true;
        }
        return false;
    };

    std::size_t offset = 0;
    u8 header0 = 0;
    u8 header1 = 0;
    u8 header2 = 0;
    u8 header3 = 0;
    if (!find_frame(offset, header0, header1, header2, header3)) {
        return std::nullopt;
    }

    const u32 version_bits = (static_cast<u32>(header1) >> 3) & 0x3u;
    const u32 layer_bits = (static_cast<u32>(header1) >> 1) & 0x3u;
    const u32 bitrate_index = (static_cast<u32>(header2) >> 4) & 0xFu;
    const u32 sample_rate_index = (static_cast<u32>(header2) >> 2) & 0x3u;
    const u32 channel_mode = (static_cast<u32>(header3) >> 6) & 0x3u;
    if (layer_bits != 0x1u || bitrate_index == 0x0u || bitrate_index == 0xFu || sample_rate_index == 0x3u) {
        return std::nullopt;
    }

    static constexpr u32 kMpeg1Bitrates[15] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320
    };
    static constexpr u32 kMpeg2Bitrates[15] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160
    };
    static constexpr u32 kMpeg1SampleRates[3] = {44100, 48000, 32000};
    static constexpr u32 kMpeg2SampleRates[3] = {22050, 24000, 16000};
    static constexpr u32 kMpeg25SampleRates[3] = {11025, 12000, 8000};

    u32 sample_rate = 0;
    u32 bitrate_kbps = 0;
    u32 frames_per_packet = 0;
    switch (version_bits) {
    case 0x3u:
        sample_rate = kMpeg1SampleRates[sample_rate_index];
        bitrate_kbps = kMpeg1Bitrates[bitrate_index];
        frames_per_packet = 1152;
        break;
    case 0x2u:
        sample_rate = kMpeg2SampleRates[sample_rate_index];
        bitrate_kbps = kMpeg2Bitrates[bitrate_index];
        frames_per_packet = 576;
        break;
    case 0x0u:
        sample_rate = kMpeg25SampleRates[sample_rate_index];
        bitrate_kbps = kMpeg2Bitrates[bitrate_index];
        frames_per_packet = 576;
        break;
    default:
        return std::nullopt;
    }
    if (sample_rate == 0 || bitrate_kbps == 0) {
        return std::nullopt;
    }

    const u32 channels = channel_mode == 3 ? 1u : 2u;
    const u32 frame_size = version_bits == 0x3u
        ? ((144000u * bitrate_kbps) / sample_rate) + ((static_cast<u32>(header2) >> 1) & 0x1u)
        : ((72000u * bitrate_kbps) / sample_rate) + ((static_cast<u32>(header2) >> 1) & 0x1u);

    AudioStreamBasicDescriptionData description;
    description.sample_rate = static_cast<double>(sample_rate);
    description.format_id = MakeFourCC('.', 'm', 'p', '3');
    description.format_flags = 0;
    description.bytes_per_packet = 0;
    description.frames_per_packet = frames_per_packet;
    description.bytes_per_frame = 0;
    description.channels_per_frame = channels;
    description.bits_per_channel = 0;
    description.reserved = 0;
    (void)frame_size;
    return description;
}

std::optional<HostPcmAudio> DecodeAudioBytes(const std::vector<u8>& bytes) {
    if (auto wav = DecodeWavPcmBytes(bytes)) {
        return wav;
    }
    if (auto caf = DecodeCafePcmBytes(bytes)) {
        return caf;
    }
    return std::nullopt;
}

constexpr u32 kAudioFormatLinearPcm = MakeFourCC('l', 'p', 'c', 'm');
constexpr u32 kAudioFormatFlagIsBigEndian = 1u << 1;
constexpr u32 kAudioFormatFlagIsSignedInteger = 1u << 2;
constexpr u32 kAudioFormatFlagIsPacked = 1u << 3;
constexpr u32 kAudioFormatFlagIsNonInterleaved = 1u << 5;

constexpr u32 kAudioFilePropertyFileFormat = MakeFourCC('f', 'f', 'm', 't');
constexpr u32 kAudioFilePropertyDataFormat = MakeFourCC('d', 'f', 'm', 't');
constexpr u32 kAudioFilePropertyAudioDataByteCount = MakeFourCC('b', 'c', 'n', 't');
constexpr u32 kAudioFilePropertyAudioDataPacketCount = MakeFourCC('p', 'c', 'n', 't');
constexpr u32 kAudioFilePropertyMaximumPacketSize = MakeFourCC('p', 's', 'z', 'e');

constexpr u32 kExtAudioFilePropertyFileDataFormat = MakeFourCC('f', 'f', 'm', 't');
constexpr u32 kExtAudioFilePropertyClientDataFormat = MakeFourCC('c', 'f', 'm', 't');
constexpr u32 kExtAudioFilePropertyAudioFile = MakeFourCC('a', 'f', 'i', 'l');
constexpr u32 kExtAudioFilePropertyFileMaxPacketSize = MakeFourCC('f', 'm', 'p', 's');
constexpr u32 kExtAudioFilePropertyClientMaxPacketSize = MakeFourCC('c', 'm', 'p', 's');
constexpr u32 kExtAudioFilePropertyFileLengthFrames = MakeFourCC('#', 'f', 'r', 'm');
constexpr u32 kExtAudioFileErrorNonPcmClientFormat = static_cast<u32>(-66563);
constexpr u32 kExtAudioFileErrorInvalidDataFormat = static_cast<u32>(-66566);

AudioStreamBasicDescriptionData MakeLinearPcmDescription(
    const double sample_rate,
    const u32 channels,
    const u32 bits_per_channel,
    const u32 format_flags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked) {
    AudioStreamBasicDescriptionData description;
    description.sample_rate = sample_rate;
    description.format_id = kAudioFormatLinearPcm;
    description.format_flags = format_flags;
    description.bytes_per_packet = channels * bits_per_channel / 8;
    description.frames_per_packet = 1;
    description.bytes_per_frame = description.bytes_per_packet;
    description.channels_per_frame = channels;
    description.bits_per_channel = bits_per_channel;
    description.reserved = 0;
    return description;
}

AudioStreamBasicDescriptionData MakeFileDataDescription(const HostPcmAudio& audio) {
    return MakeLinearPcmDescription(
        static_cast<double>(audio.frequency),
        audio.channels,
        audio.bits_per_sample,
        (audio.bits_per_sample == 8 ? 0u : kAudioFormatFlagIsSignedInteger) | kAudioFormatFlagIsPacked);
}

u64 AudioFrameCount(const std::vector<u8>& bytes, const AudioStreamBasicDescriptionData& description) {
    if (description.bytes_per_frame == 0) {
        return 0;
    }
    return static_cast<u64>(bytes.size() / description.bytes_per_frame);
}

u64 AudioFrameCount(const HostPcmAudio& audio) {
    const u32 bytes_per_frame = std::max<u32>(1, audio.channels * audio.bits_per_sample / 8);
    return static_cast<u64>(audio.pcm.size() / bytes_per_frame);
}

bool IsSupportedLinearPcmDescription(const AudioStreamBasicDescriptionData& description) {
    return description.format_id == kAudioFormatLinearPcm
        && description.sample_rate > 0.0
        && (description.format_flags & kAudioFormatFlagIsNonInterleaved) == 0
        && description.channels_per_frame >= 1
        && description.channels_per_frame <= 2
        && (description.bits_per_channel == 8 || description.bits_per_channel == 16)
        && description.bytes_per_frame == description.channels_per_frame * description.bits_per_channel / 8
        && description.bytes_per_packet == description.bytes_per_frame
        && description.frames_per_packet == 1;
}

double ReadPcmSampleNormalized(const HostPcmAudio& source, const u64 frame_index, const u32 channel_index) {
    if (source.channels == 0 || source.bits_per_sample == 0) {
        return 0.0;
    }
    const u32 source_channel = std::min<u32>(channel_index, source.channels - 1);
    const u32 bytes_per_sample = source.bits_per_sample / 8;
    const u32 bytes_per_frame = std::max<u32>(1, source.channels * bytes_per_sample);
    const std::size_t offset = static_cast<std::size_t>(frame_index) * bytes_per_frame + source_channel * bytes_per_sample;
    if (offset + bytes_per_sample > source.pcm.size()) {
        return 0.0;
    }

    if (source.bits_per_sample == 16) {
        const std::int16_t sample = static_cast<std::int16_t>(
            static_cast<u16>(source.pcm[offset])
            | (static_cast<u16>(source.pcm[offset + 1]) << 8));
        return std::clamp(static_cast<double>(sample) / 32768.0, -1.0, 1.0);
    }

    const int sample = static_cast<int>(source.pcm[offset]) - 128;
    return std::clamp(static_cast<double>(sample) / 128.0, -1.0, 1.0);
}

void WritePcmSampleNormalized(
    std::vector<u8>& output,
    const std::size_t offset,
    const AudioStreamBasicDescriptionData& description,
    const double normalized) {
    const double clamped = std::clamp(normalized, -1.0, 1.0);
    if (description.bits_per_channel == 16) {
        const s32 sample = static_cast<s32>(std::lrint(clamped * 32767.0));
        const u16 encoded = static_cast<u16>(static_cast<std::int16_t>(std::clamp<s32>(sample, -32768, 32767)));
        if ((description.format_flags & kAudioFormatFlagIsBigEndian) != 0) {
            output[offset + 0] = static_cast<u8>((encoded >> 8) & 0xFFu);
            output[offset + 1] = static_cast<u8>(encoded & 0xFFu);
        } else {
            output[offset + 0] = static_cast<u8>(encoded & 0xFFu);
            output[offset + 1] = static_cast<u8>((encoded >> 8) & 0xFFu);
        }
        return;
    }

    if ((description.format_flags & kAudioFormatFlagIsSignedInteger) != 0) {
        const s32 sample = static_cast<s32>(std::lrint(clamped * 127.0));
        output[offset] = static_cast<u8>(static_cast<std::int8_t>(std::clamp<s32>(sample, -128, 127)));
        return;
    }

    const s32 sample = static_cast<s32>(std::lrint((clamped * 127.0) + 128.0));
    output[offset] = static_cast<u8>(std::clamp<s32>(sample, 0, 255));
}

std::optional<std::vector<u8>> ConvertPcmForClientFormat(
    const HostPcmAudio& source,
    const AudioStreamBasicDescriptionData& client_format) {
    if (!IsSupportedLinearPcmDescription(client_format)
        || source.channels < 1
        || source.channels > 2
        || (source.bits_per_sample != 8 && source.bits_per_sample != 16)
        || source.frequency == 0
        || source.pcm.empty()) {
        return std::nullopt;
    }

    const u64 source_frames = AudioFrameCount(source);
    if (source_frames == 0) {
        return std::vector<u8>{};
    }

    const double sample_rate_ratio = client_format.sample_rate / static_cast<double>(source.frequency);
    const u64 target_frames = std::max<u64>(1, static_cast<u64>(std::llround(static_cast<double>(source_frames) * sample_rate_ratio)));
    const u32 target_bytes_per_frame = client_format.bytes_per_frame;
    std::vector<u8> converted(static_cast<std::size_t>(target_frames) * target_bytes_per_frame);

    for (u64 target_frame = 0; target_frame < target_frames; ++target_frame) {
        const double source_position = static_cast<double>(target_frame) * static_cast<double>(source.frequency) / client_format.sample_rate;
        const u64 source_frame = std::min<u64>(source_frames - 1, static_cast<u64>(source_position));
        double left = ReadPcmSampleNormalized(source, source_frame, 0);
        double right = source.channels > 1 ? ReadPcmSampleNormalized(source, source_frame, 1) : left;

        for (u32 channel = 0; channel < client_format.channels_per_frame; ++channel) {
            double sample = 0.0;
            if (client_format.channels_per_frame == 1) {
                sample = source.channels > 1 ? (left + right) * 0.5 : left;
            } else {
                sample = channel == 0 ? left : right;
            }
            const std::size_t offset = static_cast<std::size_t>(target_frame) * target_bytes_per_frame
                + channel * (client_format.bits_per_channel / 8);
            WritePcmSampleNormalized(converted, offset, client_format, sample);
        }
    }

    return converted;
}

std::optional<std::vector<u8>> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::vector<u8>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

std::optional<HostPcmAudio> DecodeWavPcm(const std::filesystem::path& path) {
    if (const auto bytes = ReadFileBytes(path)) {
        return DecodeWavPcmBytes(*bytes);
    }
    return std::nullopt;
}

struct HostAudioFormat {
    u16 channels = 0;
    u16 bits_per_sample = 0;
};

std::optional<HostAudioFormat> DecodeAlFormat(const u32 format) {
    switch (format) {
    case 0x1100:
        return HostAudioFormat{1, 8};
    case 0x1101:
        return HostAudioFormat{1, 16};
    case 0x1102:
        return HostAudioFormat{2, 8};
    case 0x1103:
        return HostAudioFormat{2, 16};
    default:
        return std::nullopt;
    }
}

u64 HostAudioDurationMs(const std::size_t byte_size, const HostAudioFormat& format, const u32 frequency) {
    const u32 bytes_per_frame = std::max<u32>(1, format.channels * format.bits_per_sample / 8);
    const u64 frames = byte_size / bytes_per_frame;
    return frames * 1000ull / std::max<u32>(frequency, 1);
}

#if defined(__ANDROID__)
class AndroidAudioPlayer final {
public:
    static AndroidAudioPlayer& Instance() {
        static AndroidAudioPlayer player;
        return player;
    }

    void Play(std::vector<u8> pcm, const u32 format, const u32 frequency) {
        const auto decoded = DecodeAlFormat(format);
        if (!decoded || pcm.empty() || frequency == 0 || !EnsureEngine()) {
            return;
        }

        std::lock_guard lock(mutex_);
        SLDataLocator_AndroidSimpleBufferQueue queue_locator{SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1};
        SLDataFormat_PCM pcm_format{
            SL_DATAFORMAT_PCM,
            decoded->channels,
            frequency * 1000,
            decoded->bits_per_sample,
            decoded->bits_per_sample,
            decoded->channels == 1 ? SL_SPEAKER_FRONT_CENTER : static_cast<SLuint32>(SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
            SL_BYTEORDER_LITTLEENDIAN,
        };
        SLDataSource source{&queue_locator, &pcm_format};
        SLDataLocator_OutputMix output_locator{SL_DATALOCATOR_OUTPUTMIX, output_mix_};
        SLDataSink sink{&output_locator, nullptr};

        const SLInterfaceID ids[] = {SL_IID_BUFFERQUEUE};
        const SLboolean required[] = {SL_BOOLEAN_TRUE};
        SLObjectItf player_object = nullptr;
        if ((*engine_)->CreateAudioPlayer(engine_, &player_object, &source, &sink, 1, ids, required) != SL_RESULT_SUCCESS || player_object == nullptr) {
            return;
        }
        if ((*player_object)->Realize(player_object, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
            (*player_object)->Destroy(player_object);
            return;
        }

        SLPlayItf play = nullptr;
        SLAndroidSimpleBufferQueueItf queue = nullptr;
        if ((*player_object)->GetInterface(player_object, SL_IID_PLAY, &play) != SL_RESULT_SUCCESS
            || (*player_object)->GetInterface(player_object, SL_IID_BUFFERQUEUE, &queue) != SL_RESULT_SUCCESS) {
            (*player_object)->Destroy(player_object);
            return;
        }

        auto clip = std::make_shared<Clip>();
        clip->pcm = std::move(pcm);
        clip->object = player_object;
        clip->duration_ms = DurationMs(clip->pcm.size(), *decoded, frequency);
        (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
        if ((*queue)->Enqueue(queue, clip->pcm.data(), clip->pcm.size()) != SL_RESULT_SUCCESS) {
            (*player_object)->Destroy(player_object);
            return;
        }
        std::thread([clip] {
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max<u64>(clip->duration_ms + 100, 250)));
            (*clip->object)->Destroy(clip->object);
        }).detach();
    }

    bool PlayFile(const std::filesystem::path& path) {
        if (path.empty() || !EnsureEngine()) {
            return false;
        }

        std::lock_guard lock(mutex_);
        StopMusicLocked();

        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return false;
        }
        const off_t length = ::lseek(fd, 0, SEEK_END);
        if (length <= 0 || ::lseek(fd, 0, SEEK_SET) < 0) {
            ::close(fd);
            return false;
        }

        auto clip = std::make_shared<FileClip>();
        clip->fd = fd;

        SLDataLocator_AndroidFD fd_locator{SL_DATALOCATOR_ANDROIDFD, fd, 0, static_cast<SLAint64>(length)};
        SLDataFormat_MIME mime_format{SL_DATAFORMAT_MIME, nullptr, SL_CONTAINERTYPE_MP3};
        SLDataSource source{&fd_locator, &mime_format};
        SLDataLocator_OutputMix output_locator{SL_DATALOCATOR_OUTPUTMIX, output_mix_};
        SLDataSink sink{&output_locator, nullptr};

        const SLInterfaceID ids[] = {SL_IID_PLAY, SL_IID_SEEK};
        const SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
        SLObjectItf player_object = nullptr;
        const SLresult create_result = (*engine_)->CreateAudioPlayer(engine_, &player_object, &source, &sink, 2, ids, required);
        if (create_result != SL_RESULT_SUCCESS || player_object == nullptr) {
            ::close(fd);
            return false;
        }
        if ((*player_object)->Realize(player_object, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
            (*player_object)->Destroy(player_object);
            ::close(fd);
            return false;
        }

        SLPlayItf play = nullptr;
        if ((*player_object)->GetInterface(player_object, SL_IID_PLAY, &play) != SL_RESULT_SUCCESS || play == nullptr) {
            (*player_object)->Destroy(player_object);
            ::close(fd);
            return false;
        }

        SLSeekItf seek = nullptr;
        if ((*player_object)->GetInterface(player_object, SL_IID_SEEK, &seek) == SL_RESULT_SUCCESS && seek != nullptr) {
            (*seek)->SetLoop(seek, SL_BOOLEAN_TRUE, 0, SL_TIME_UNKNOWN);
        }
        clip->object = player_object;
        music_clip_ = clip;
        (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);
        return true;
    }

    void StopMusic() {
        std::lock_guard lock(mutex_);
        StopMusicLocked();
    }

private:
    struct Clip {
        std::vector<u8> pcm;
        SLObjectItf object = nullptr;
        u64 duration_ms = 0;
    };

    struct FileClip {
        SLObjectItf object = nullptr;
        int fd = -1;
    };

    bool EnsureEngine() {
        std::lock_guard lock(mutex_);
        if (engine_ != nullptr && output_mix_ != nullptr) {
            return true;
        }
        SLObjectItf engine_object = nullptr;
        if (slCreateEngine(&engine_object, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS || engine_object == nullptr) {
            return false;
        }
        if ((*engine_object)->Realize(engine_object, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
            (*engine_object)->Destroy(engine_object);
            return false;
        }
        SLEngineItf engine = nullptr;
        if ((*engine_object)->GetInterface(engine_object, SL_IID_ENGINE, &engine) != SL_RESULT_SUCCESS || engine == nullptr) {
            (*engine_object)->Destroy(engine_object);
            return false;
        }
        SLObjectItf output_mix = nullptr;
        if ((*engine)->CreateOutputMix(engine, &output_mix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS || output_mix == nullptr) {
            (*engine_object)->Destroy(engine_object);
            return false;
        }
        if ((*output_mix)->Realize(output_mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
            (*output_mix)->Destroy(output_mix);
            (*engine_object)->Destroy(engine_object);
            return false;
        }
        engine_object_ = engine_object;
        engine_ = engine;
        output_mix_ = output_mix;
        return true;
    }

    static u64 DurationMs(const std::size_t byte_size, const HostAudioFormat& format, const u32 frequency) {
        return HostAudioDurationMs(byte_size, format, frequency);
    }

    void StopMusicLocked() {
        if (!music_clip_) {
            return;
        }
        if (music_clip_->object != nullptr) {
            (*music_clip_->object)->Destroy(music_clip_->object);
        }
        if (music_clip_->fd >= 0) {
            ::close(music_clip_->fd);
        }
        music_clip_.reset();
    }

    std::mutex mutex_;
    SLObjectItf engine_object_ = nullptr;
    SLEngineItf engine_ = nullptr;
    SLObjectItf output_mix_ = nullptr;
    std::shared_ptr<FileClip> music_clip_;
};
#endif

void PlayHostAudio(std::vector<u8> pcm, const u32 format, const u32 frequency) {
#if defined(__ANDROID__)
    AndroidAudioPlayer::Instance().Play(std::move(pcm), format, frequency);
#else
    (void)pcm;
    (void)format;
    (void)frequency;
#endif
}

bool PlayHostAudioFile(const std::filesystem::path& path) {
#if defined(__ANDROID__)
    return AndroidAudioPlayer::Instance().PlayFile(path);
#else
    (void)path;
    return false;
#endif
}

void StopHostAudioFile() {
#if defined(__ANDROID__)
    AndroidAudioPlayer::Instance().StopMusic();
#endif
}

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string GenerateHexToken(const std::size_t bytes) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static constexpr char kHexDigits[] = "0123456789abcdef";

    std::string result;
    result.reserve(bytes * 2);
    for (std::size_t i = 0; i < bytes; ++i) {
        const u8 value = static_cast<u8>(rng() & 0xFFu);
        result.push_back(kHexDigits[(value >> 4) & 0x0Fu]);
        result.push_back(kHexDigits[value & 0x0Fu]);
    }
    return result;
}

std::string GenerateUuidString() {
    std::string token = GenerateHexToken(16);
    token[12] = '4';
    token[16] = "89ab"[std::random_device{}() & 0x3u];
    return token.substr(0, 8) + "-" + token.substr(8, 4) + "-" + token.substr(12, 4)
        + "-" + token.substr(16, 4) + "-" + token.substr(20, 12);
}

std::string DescribeDouble(const double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    std::string text = out.str();
    while (text.size() > 2 && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text;
}

std::string DescribeHaltReason(const Dynarmic::HaltReason halt) {
    std::vector<std::string> names;
    const auto add = [&](const Dynarmic::HaltReason bit, const std::string& name) {
        if (Dynarmic::Has(halt, bit)) {
            names.push_back(name);
        }
    };
    add(Dynarmic::HaltReason::Step, "Step");
    add(Dynarmic::HaltReason::CacheInvalidation, "CacheInvalidation");
    add(Dynarmic::HaltReason::MemoryAbort, "MemoryAbort");
    add(Dynarmic::HaltReason::UserDefined1, "UserDefined1");
    add(Dynarmic::HaltReason::UserDefined2, "UserDefined2");
    add(Dynarmic::HaltReason::UserDefined3, "UserDefined3");
    add(Dynarmic::HaltReason::UserDefined4, "UserDefined4");
    add(Dynarmic::HaltReason::UserDefined5, "UserDefined5");
    add(Dynarmic::HaltReason::UserDefined6, "UserDefined6");
    add(Dynarmic::HaltReason::UserDefined7, "UserDefined7");
    add(Dynarmic::HaltReason::UserDefined8, "UserDefined8");
    if (names.empty()) {
        return Hex32(static_cast<u32>(halt));
    }
    std::string result = names.front();
    for (std::size_t i = 1; i < names.size(); ++i) {
        result += "|" + names[i];
    }
    return result;
}

u32 GuestCStringLength(const GuestMemory& memory, const u32 address, const u32 limit = kMaxGuestCString) {
    u32 length = 0;
    while (length < limit && memory.Read8(address + length) != 0) {
        ++length;
    }
    return length;
}

int CompareGuestCStrings(const GuestMemory& memory, const u32 lhs, const u32 rhs, const bool ignore_case, const u32 limit = kMaxGuestCString) {
    for (u32 index = 0; index < limit; ++index) {
        u8 left = memory.Read8(lhs + index);
        u8 right = memory.Read8(rhs + index);
        if (ignore_case) {
            left = static_cast<u8>(std::tolower(left));
            right = static_cast<u8>(std::tolower(right));
        }
        if (left != right) {
            return static_cast<int>(left) - static_cast<int>(right);
        }
        if (left == 0) {
            return 0;
        }
    }
    return 0;
}

u32 CopyGuestCString(GuestMemory& memory, const u32 destination, const u32 source, const u32 limit) {
    if (limit == 0) {
        return destination;
    }
    for (u32 index = 0; index < limit; ++index) {
        const u8 value = memory.Read8(source + index);
        memory.Write8(destination + index, value);
        if (value == 0) {
            return destination;
        }
    }
    memory.Write8(destination + limit - 1, 0);
    return destination;
}

u32 ParseGuestAtoi(const GuestMemory& memory, const u32 address) {
    u32 cursor = address;
    while (std::isspace(memory.Read8(cursor))) {
        ++cursor;
    }

    bool negative = false;
    const u8 sign = memory.Read8(cursor);
    if (sign == '+' || sign == '-') {
        negative = sign == '-';
        ++cursor;
    }

    s32 value = 0;
    while (true) {
        const u8 c = memory.Read8(cursor);
        if (c < '0' || c > '9') {
            break;
        }
        value = value * 10 + static_cast<s32>(c - '0');
        ++cursor;
    }

    return static_cast<u32>(negative ? -value : value);
}

struct DecodedUtf8String {
    std::vector<u32> codepoints;
    std::vector<u32> byte_offsets;
};

DecodedUtf8String DecodeUtf8String(const std::string_view bytes) {
    DecodedUtf8String decoded;
    decoded.codepoints.reserve(bytes.size());
    decoded.byte_offsets.reserve(bytes.size());

    std::size_t index = 0;
    while (index < bytes.size()) {
        decoded.byte_offsets.push_back(static_cast<u32>(index));

        const u8 lead = static_cast<u8>(bytes[index]);
        u32 codepoint = 0xFFFDu;
        std::size_t advance = 1;

        if (lead < 0x80u) {
            codepoint = lead;
        } else if ((lead & 0xE0u) == 0xC0u
            && index + 1 < bytes.size()
            && (static_cast<u8>(bytes[index + 1]) & 0xC0u) == 0x80u) {
            codepoint = (static_cast<u32>(lead & 0x1Fu) << 6)
                | static_cast<u32>(static_cast<u8>(bytes[index + 1]) & 0x3Fu);
            advance = 2;
        } else if ((lead & 0xF0u) == 0xE0u
            && index + 2 < bytes.size()
            && (static_cast<u8>(bytes[index + 1]) & 0xC0u) == 0x80u
            && (static_cast<u8>(bytes[index + 2]) & 0xC0u) == 0x80u) {
            codepoint = (static_cast<u32>(lead & 0x0Fu) << 12)
                | (static_cast<u32>(static_cast<u8>(bytes[index + 1]) & 0x3Fu) << 6)
                | static_cast<u32>(static_cast<u8>(bytes[index + 2]) & 0x3Fu);
            advance = 3;
        } else if ((lead & 0xF8u) == 0xF0u
            && index + 3 < bytes.size()
            && (static_cast<u8>(bytes[index + 1]) & 0xC0u) == 0x80u
            && (static_cast<u8>(bytes[index + 2]) & 0xC0u) == 0x80u
            && (static_cast<u8>(bytes[index + 3]) & 0xC0u) == 0x80u) {
            codepoint = (static_cast<u32>(lead & 0x07u) << 18)
                | (static_cast<u32>(static_cast<u8>(bytes[index + 1]) & 0x3Fu) << 12)
                | (static_cast<u32>(static_cast<u8>(bytes[index + 2]) & 0x3Fu) << 6)
                | static_cast<u32>(static_cast<u8>(bytes[index + 3]) & 0x3Fu);
            advance = 4;
        }

        decoded.codepoints.push_back(codepoint);
        index += advance;
    }

    return decoded;
}

std::filesystem::path ResolveBinaryPath(std::filesystem::path binary_path, const std::filesystem::path& external_root) {
    if (!binary_path.empty() && (binary_path.is_absolute() || std::filesystem::exists(binary_path))) {
        return binary_path;
    }
    if (!external_root.empty()) {
        const std::filesystem::path candidate = external_root / binary_path;
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return binary_path;
}

std::string TranslateGlesShaderSource(std::string source) {
#if defined(__ANDROID__)
    return source;
#else
    static const std::regex precision_statement(R"(\bprecision\s+(?:lowp|mediump|highp)\s+(?:float|int)\s*;\s*)");
    static const std::regex qualifier(R"(\b(lowp|mediump|highp)\b\s*)");

    source = std::regex_replace(source, precision_statement, "");
    source = std::regex_replace(source, qualifier, "");
    return source;
#endif
}

std::string OneLineForLog(std::string text, const std::size_t max_length = 180) {
    for (char& ch : text) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        }
    }
    if (text.size() > max_length) {
        text.resize(max_length);
        text += "...";
    }
    return text;
}

std::string HexBytesForLog(std::span<const u8> bytes, const std::size_t max_bytes = 16) {
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    std::string out;
    const std::size_t count = std::min(bytes.size(), max_bytes);
    out.reserve(count * 3 + (bytes.size() > count ? 4 : 0));
    for (std::size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        const u8 value = bytes[i];
        out.push_back(kHexDigits[value >> 4]);
        out.push_back(kHexDigits[value & 0x0F]);
    }
    if (bytes.size() > count) {
        out += " ...";
    }
    return out;
}

std::string DescribePiranhaPacket(std::span<const u8> bytes) {
    if (bytes.size() < 7) {
        return "short bytes=" + HexBytesForLog(bytes);
    }
    const u32 type = (static_cast<u32>(bytes[0]) << 8) | bytes[1];
    const u32 length = (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 8) | bytes[4];
    const u32 version = (static_cast<u32>(bytes[5]) << 8) | bytes[6];
    return "type=" + std::to_string(type)
        + " len=" + std::to_string(length)
        + " ver=" + std::to_string(version)
        + " bytes=" + HexBytesForLog(bytes);
}

std::size_t GlTypeSize(const u32 type) {
    switch (type) {
    case 0x1400:  // GL_BYTE
    case 0x1401:  // GL_UNSIGNED_BYTE
        return 1;
    case 0x1402:  // GL_SHORT
    case 0x1403:  // GL_UNSIGNED_SHORT
        return 2;
    case 0x1404:  // GL_INT
    case 0x1405:  // GL_UNSIGNED_INT
    case 0x1406:  // GL_FLOAT
    case 0x140C:  // GL_FIXED
        return 4;
    default:
        return 0;
    }
}

using HostGLenum = unsigned int;
using HostGLuint = unsigned int;
using HostGLint = int;
using HostGLsizei = int;
using HostGLboolean = unsigned char;
using HostGLubyte = unsigned char;
using HostGLfloat = float;
using HostGLsizeiptr = std::ptrdiff_t;

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNativeSocket = INVALID_SOCKET;

bool EnsureSocketSubsystem() {
    static bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}

int CloseNativeSocket(const NativeSocket socket) {
    return closesocket(socket);
}

int HostSocketErrno() {
    return WSAGetLastError();
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidNativeSocket = -1;

bool EnsureSocketSubsystem() {
    return true;
}

int CloseNativeSocket(const NativeSocket socket) {
    return close(socket);
}

int HostSocketErrno() {
    return errno;
}
#endif

template<typename Fn>
Fn LookupHostGLProc(HostGLBackend* backend, const char* primary_name, const char* fallback_name = nullptr) {
    if (backend == nullptr) {
        return nullptr;
    }
    if (void* proc = backend->GetProcAddress(primary_name); proc != nullptr) {
        return reinterpret_cast<Fn>(proc);
    }
    if (fallback_name != nullptr) {
        if (void* proc = backend->GetProcAddress(fallback_name); proc != nullptr) {
            return reinterpret_cast<Fn>(proc);
        }
    }
    return nullptr;
}

}  // namespace

void EnqueueHostTouchEvent(const HostTouchEvent& event) {
    std::lock_guard lock(g_host_touch_mutex);
    g_host_touch_events.push_back(event);
}

void EnqueueHostKeyEvent(const HostKeyEvent& event) {
    std::lock_guard lock(g_host_key_mutex);
    g_host_key_events.push_back(event);
}

void EnqueueHostPopupResult(const HostPopupResult& result) {
    std::lock_guard lock(g_host_popup_mutex);
    g_host_popup_results.push_back(result);
}

void SetHostKeyboardVisibilityCallback(const HostKeyboardVisibilityCallback callback) {
    std::lock_guard lock(g_host_keyboard_callback_mutex);
    g_host_keyboard_visibility_callback = callback;
}

void SetHostPopupCallbacks(const HostPopupRequestCallback request_callback, const HostPopupDismissCallback dismiss_callback) {
    std::lock_guard lock(g_host_popup_callback_mutex);
    g_host_popup_request_callback = request_callback;
    g_host_popup_dismiss_callback = dismiss_callback;
}

struct RuntimeState {
    struct BitVector {
        std::vector<u8> bits;
    };

    struct ReadStream {
        std::string guest_path;
        std::filesystem::path path;
        std::vector<u8> data;
        u32 cursor = 0;
        bool open = false;
    };

    struct Reachability {
        std::string target;
        std::vector<u8> address;
        bool scheduled = false;
        u32 callback = 0;
        u32 context = 0;
    };

    struct SecurityObject {
        std::string kind;
        std::vector<u8> bytes;
        std::vector<u32> refs;
        std::unordered_map<std::string, u32> fields;
    };

    struct KeychainItem {
        std::unordered_map<std::string, u32> attrs;
        std::vector<u8> data;
    };

    struct GlShader {
        u32 type = 0;
        u32 host_id = 0;
        std::string source;
        bool compiled = false;
        std::string info_log;
    };

    struct GlProgram {
        u32 host_id = 0;
        std::vector<u32> shaders;
        std::unordered_map<u32, std::string> attributes;
        std::unordered_map<std::string, s32> uniform_locations;
        bool linked = false;
        std::string info_log;
        s32 next_uniform_location = 0;
    };

    struct GlBuffer {
        u32 host_id = 0;
        u32 target = 0;
        std::vector<u8> data;
    };

    struct GlTexture {
        u32 host_id = 0;
        u32 target = 0;
        s32 width = 0;
        s32 height = 0;
        u32 format = 0;
        u32 type = 0;
        u32 source_data = 0;
        bool mipmaps_generated = false;
        std::vector<u8> data;
        std::unordered_map<u32, s32> iparams;
        std::unordered_map<u32, float> fparams;
    };

    struct GlFramebuffer {
        u32 host_id = 0;
        std::unordered_map<u32, u32> attachments;
    };

    struct GlRenderbuffer {
        u32 host_id = 0;
        s32 width = 0;
        s32 height = 0;
        s32 samples = 0;
        u32 format = 0;
    };

    struct GlVertexAttrib {
        s32 size = 4;
        u32 type = 0x1406;
        bool normalized = false;
        s32 stride = 0;
        u32 pointer = 0;
        u32 buffer = 0;
        bool enabled = false;
        std::vector<u8> scratch;
    };

    struct GlState {
        u32 active_texture = 0x84C0;
        u32 current_program = 0;
        std::unordered_map<u32, u32> bound_buffers;
        std::unordered_map<u64, u32> bound_textures;
        std::unordered_map<u32, GlVertexAttrib> vertex_attribs;
        u32 framebuffer = 0;
        u32 renderbuffer = 0;
        std::array<s32, 4> viewport{0, 0, 0, 0};
        std::array<s32, 4> scissor{0, 0, 0, 0};
        std::array<float, 4> clear_color{0.0f, 0.0f, 0.0f, 0.0f};
        std::set<u32> enabled_caps;
        s32 unpack_alignment = 4;
        s32 pack_alignment = 4;
        u32 debug_active_texture_logs = 0;
        u32 debug_uniform1i_logs = 0;
        u32 debug_uniform2f_logs = 0;
        u32 debug_uniform4f_logs = 0;
        u32 debug_get_uniform_logs = 0;
        u32 debug_shader_logs = 0;
        u32 debug_program_logs = 0;
        u32 debug_large_draw_logs = 0;
        u32 debug_quad_draw_logs = 0;
        u32 debug_draw_call_logs = 0;
        u32 debug_present_logs = 0;
        u32 debug_viewport_logs = 0;
        u32 debug_gen_logs = 0;
        u32 debug_draw_error_logs = 0;
        u32 debug_present_error_logs = 0;
        u32 debug_stale_error_logs = 0;
        u32 debug_pixel_store_logs = 0;
        u32 debug_texture_upload_logs = 0;
        u32 debug_fbo_probe_logs = 0;
        u32 debug_cap_logs = 0;
        u32 debug_texture_compat_logs = 0;
        u32 last_error = 0;
        std::set<std::string> debug_quad_signatures;
        std::set<std::string> debug_fbo_probe_signatures;
        std::set<std::string> debug_program_signatures;
    };

    struct AlDevice {
        std::string name;
    };

    struct AlContext {
        u32 device = 0;
        bool current = false;
    };

    struct AlSource {
        std::unordered_map<u32, s32> ints;
        std::unordered_map<u32, float> floats;
        bool playing = false;
        bool has_stop_time = false;
        std::chrono::steady_clock::time_point stop_time{};
    };

    struct AlBuffer {
        std::vector<u8> data;
        u32 format = 0;
        u32 frequency = 0;
        std::filesystem::path path;
    };

    struct AudioFile {
        std::filesystem::path path;
        std::string guest_path;
        std::vector<u8> raw_bytes;
        std::vector<u8> data;
        std::optional<HostPcmAudio> decoded_audio;
        AudioStreamBasicDescriptionData file_format;
        AudioStreamBasicDescriptionData client_format;
        u32 file_type = 0;
        u32 max_packet_size = 0;
        u64 packet_count = 0;
        u64 frame_count = 0;
        bool has_client_format = false;
        bool is_ext_audio = false;
        u32 cursor = 0;
    };

    struct AudioQueueBuffer {
        u32 capacity = 0;
        u32 data = 0;
        u32 data_size = 0;
    };

    struct AudioQueue {
        bool running = false;
        std::vector<u32> buffers;
        std::unordered_map<u32, float> parameters;
        std::unordered_map<u32, std::vector<u8>> properties;
        u32 callback = 0;
        u32 callback_user_data = 0;
    };

    struct AudioSession {
        bool initialized = false;
        bool active = false;
        std::unordered_map<u32, std::vector<u8>> properties;
    };

    struct RegexEntry {
        std::string pattern;
        std::optional<std::regex> compiled;
        std::string last_error;
        int cflags = 0;
    };

    struct GraphicsImage {
        s32 width = 0;
        s32 height = 0;
        s32 bytes_per_row = 0;
        bool skip_draw = false;
        std::vector<float> components;
        std::vector<u8> pixels;
    };

    struct GraphicsGradient {
        std::vector<u32> colors;
    };

    struct GraphicsStateSnapshot {
        std::array<float, 4> fill_color{0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 4> stroke_color{0.0f, 0.0f, 0.0f, 1.0f};
        float line_width = 1.0f;
        std::array<float, 6> transform{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        std::array<float, 6> text_matrix{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        std::array<float, 2> text_position{0.0f, 0.0f};
    };

    struct GraphicsContext {
        u32 data = 0;
        s32 width = 0;
        s32 height = 0;
        s32 bytes_per_row = 0;
        u32 bits_per_component = 8;
        u32 color_space = 0;
        u32 bitmap_info = 0;
        std::array<float, 4> fill_color{0.0f, 0.0f, 0.0f, 1.0f};
        std::array<float, 4> stroke_color{0.0f, 0.0f, 0.0f, 1.0f};
        float line_width = 1.0f;
        std::array<float, 6> transform{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        std::array<float, 6> text_matrix{1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
        std::array<float, 2> text_position{0.0f, 0.0f};
        std::vector<GraphicsStateSnapshot> saved_states;
    };

    struct RasterFont {
        std::vector<u8> bytes;
        stbtt_fontinfo info{};
        bool initialized = false;
        int ascent = 0;
        int descent = 0;
        int line_gap = 0;
    };

    struct CTGlyphRun {
        u32 font_handle = 0;
        std::vector<u16> glyphs;
        std::vector<std::array<float, 2>> positions;
        float width = 0.0f;
        float ascent = 0.0f;
        float descent = 0.0f;
        float leading = 0.0f;
    };

    struct CTLineData {
        u32 attributed_string = 0;
        u32 font_handle = 0;
        std::string text;
        float font_size = 16.0f;
        std::vector<CTGlyphRun> runs;
        float total_width = 0.0f;
        float ascent = 0.0f;
        float descent = 0.0f;
        float leading = 0.0f;
    };

    struct CTFrameData {
        u32 framesetter = 0;
        std::array<float, 4> frame_rect{0, 0, 320, 480};
        std::vector<u32> line_handles;  // guest handles to CTLine objects
    };

    struct SocketHandle {
        NativeSocket socket = kInvalidNativeSocket;
        int family = 0;
        int type = 0;
        int protocol = 0;
    };

    struct DisplayLink {
        u32 object = 0;
        u32 target = 0;
        u32 selector = 0;
        u32 run_loop = 0;
        u32 mode = 0;
        int frame_interval = 1;
        int frame_counter = 0;
        bool active = false;
        bool paused = false;
    };

    struct TouchState {
        u32 object = 0;
        float x = 0.0f;
        float y = 0.0f;
        float previous_x = 0.0f;
        float previous_y = 0.0f;
        u32 phase = 0;
        double timestamp = 0.0;
    };

    std::unique_ptr<HostGLBackend> host_gl;
    u32 current_eagl_context = 0;
    u32 main_screen = 0;
    u32 main_application = 0;
    u32 main_delegate = 0;
    u32 main_window = 0;
    u32 main_view_controller = 0;
    u32 main_gl_view = 0;
    u32 main_layer = 0;
    u32 main_bundle = 0;
    u32 notification_center = 0;
    u32 standard_user_defaults = 0;
    u32 default_file_manager = 0;
    u32 shared_cookie_storage = 0;
    u32 current_device = 0;
    u32 process_info = 0;
    u32 current_run_loop = 0;
    u32 current_thread = 0;
    u32 drawable_framebuffer = 0;
    u32 drawable_renderbuffer = 0;
    int screen_width_points = 320;
    int screen_height_points = 480;
    float screen_scale = 2.0f;
    std::unordered_map<u32, BitVector> bit_vectors;
    std::unordered_map<u32, ReadStream> read_streams;
    std::unordered_map<u32, Reachability> reachability;
    std::unordered_map<u32, SecurityObject> security_objects;
    std::vector<KeychainItem> keychain_items;
    std::unordered_map<u32, GlShader> gl_shaders;
    std::unordered_map<u32, GlProgram> gl_programs;
    std::unordered_map<u32, GlBuffer> gl_buffers;
    std::unordered_map<u32, GlTexture> gl_textures;
    std::unordered_map<u32, GlFramebuffer> gl_framebuffers;
    std::unordered_map<u32, GlRenderbuffer> gl_renderbuffers;
    GlState gl_state;
    std::unordered_map<u32, AlDevice> al_devices;
    std::unordered_map<u32, AlContext> al_contexts;
    std::unordered_map<u32, AlSource> al_sources;
    std::unordered_map<u32, AlBuffer> al_buffers;
    u32 al_current_context = 0;
    std::unordered_map<u32, AudioFile> audio_files;
    std::unordered_map<u32, AudioQueueBuffer> audio_queue_buffers;
    std::unordered_map<u32, AudioQueue> audio_queues;
    std::filesystem::path last_music_path;
    bool music_autoplay_started = false;
    AudioSession audio_session;
    std::unordered_map<u32, RegexEntry> regex_entries;
    std::unordered_map<u32, GraphicsImage> graphics_images;
    std::unordered_map<u32, GraphicsGradient> graphics_gradients;
    std::unordered_map<u32, GraphicsContext> graphics_contexts;
    std::vector<u32> graphics_context_stack;
    std::unordered_map<u32, RasterFont> raster_fonts;
    std::unordered_map<u32, CTLineData> ct_lines;
    std::unordered_map<u32, CTFrameData> ct_frames;
    std::unordered_map<u32, SocketHandle> sockets;
    std::unordered_map<u32, DisplayLink> display_links;
    std::unordered_map<int, TouchState> active_touches;
    u32 keyboard_responder = 0;
    bool keyboard_visible = false;
    bool keyboard_touch_armed = false;
    std::unordered_map<std::string, u32> named_pasteboards;
    u32 next_socket_fd = 64;
    u32 debug_net_logs = 0;
    u32 debug_mode_logs = 0;
    u32 debug_sfx_logs = 0;
    u32 debug_present_count = 0;
    u32 debug_present_probe_logs = 0;
    u32 debug_display_link_logs = 0;
    u32 debug_uigraphics_logs = 0;
    u32 debug_uigraphics_image_logs = 0;
    u32 debug_bitmap_context_logs = 0;
    u32 debug_text_draw_logs = 0;
    u32 debug_image_draw_logs = 0;
    u32 debug_fill_rect_logs = 0;
    std::string debug_last_mode_signature;
    std::string debug_last_audio_path;
    std::vector<std::pair<u32, u32>> atexit_callbacks;

    // SQLite3 real handles
    std::unordered_map<u32, sqlite3*> sqlite_dbs;
    std::unordered_map<u32, sqlite3_stmt*> sqlite_stmts;
    std::unordered_map<u32, u32> sqlite_stmt_to_db;  // stmt guest → db guest

    ~RuntimeState() {
        for (auto& [_, stmt] : sqlite_stmts) {
            if (stmt != nullptr) {
                sqlite3_finalize(stmt);
            }
        }
        for (auto& [_, db] : sqlite_dbs) {
            if (db != nullptr) {
                sqlite3_close(db);
            }
        }
    }
};

Emulator::Emulator(EmulatorOptions options)
    : binary_path_(ResolveBinaryPath(options.binary_path, options.external_root))
    , external_root_(std::move(options.external_root))
    , sandbox_root_(options.sandbox_root.empty() ? std::filesystem::current_path() / "sandbox" : std::move(options.sandbox_root))
    , asset_exists_(std::move(options.asset_exists))
    , read_asset_(std::move(options.read_asset))
    , guest_home_("/var/mobile/Applications/com.supercell.phoenix")
    , guest_tmp_(JoinPathForGuest(guest_home_, "tmp"))
    , trace_shims_(options.trace_shims)
    , image_(options.binary_bytes
          ? MachOImage(binary_path_, std::move(*options.binary_bytes))
          : MachOImage(binary_path_)) {
    std::filesystem::create_directories(sandbox_root_ / "Documents");
    std::filesystem::create_directories(sandbox_root_ / "Library");
    std::filesystem::create_directories(sandbox_root_ / "Library" / "Preferences");
    std::filesystem::create_directories(sandbox_root_ / "tmp");
    restore_persisted_iap_warning_suppression_ = HasPersistedIapWarningSuppression();

    errno_address_ = AllocateData(4, 4, "errno");
    memory_.Write32(errno_address_, 0);
    runtime_ = std::make_unique<RuntimeState>();
    runtime_->host_gl = CreateHostGLBackend();
    libc_abi_ = std::make_unique<LibcAbi>(*this);
    libstdcpp_abi_ = std::make_unique<LibStdCppAbi>(*this);
    shim_registry_ = std::make_unique<ShimRegistry>(*this);

    BuildImage();
    objc_abi_ = std::make_unique<ObjcAbi>(*this);
    objc_abi_->Initialize();
    CreateBuiltins();
    BuildCpu();
    InstallGuestFastPaths();
    memory_.SetWriteProtection(true);
    ApplyGuestLogicVersionPatches();
    //ApplyGuestLogicDefinesPatches();
    BuildProcessState();
}

Emulator::~Emulator() = default;

int Emulator::Run(const bool run_initializers, const bool run_main, const std::uint64_t tick_budget) {
    Log("[build] android-zlib-fastpath-2026-05-13b");
    if (restore_persisted_iap_warning_suppression_) {
        if (ApplyGuestIapWarningSuppression("from persisted accept")) {
            Log("[iap] restored persisted warning suppression from "
                + IapWarningStatePath().string());
        }
        restore_persisted_iap_warning_suppression_ = false;
    }
    ticks_left_ = 0;
    total_ticks_left_ = tick_budget;
    returned_from_guest_ = false;
    saw_exit_ = false;
    last_guest_error_.clear();
    suppress_next_unwind_resume_ = false;
    active_objc_callback_.reset();
    queued_objc_callbacks_.clear();
    hot_pc_samples_.clear();

    if (run_initializers) {
        for (const u32 init : image_.init_functions()) {
            returned_from_guest_ = false;
            cpu_->Regs().fill(0);
            cpu_->Regs()[13] = stack_pointer_;
            cpu_->Regs()[14] = return_stub_;
            cpu_->Regs()[15] = init & ~1u;
            cpu_->SetCpsr((init & 1u) != 0 ? 0x30 : 0x10);
            while (true) {
                if (!BeginTickSlice()) {
                    Log("tick budget exhausted during initializer at " + Hex32(cpu_->Regs()[15]));
                    DumpHotPcSamples();
                    return exit_code_;
                }
                const Dynarmic::HaltReason halt = cpu_->Run();
                if (returned_from_guest_ || Dynarmic::Has(halt, Dynarmic::HaltReason::UserDefined1)) {
                    break;
                }
                if (ticks_left_ == 0) {
                    RecordHotPcSample();
                    if (total_ticks_left_ == 0) {
                        Log("tick budget exhausted during initializer at " + Hex32(cpu_->Regs()[15])
                            + " halt=" + DescribeHaltReason(halt));
                        DumpHotPcSamples();
                        return exit_code_;
                    }
                    continue;
                }
                if (Dynarmic::Has(halt, Dynarmic::HaltReason::UserDefined2)) {
                    break;
                }
                throw std::runtime_error("initializer halted unexpectedly at " + Hex32(cpu_->Regs()[15])
                    + " halt=" + DescribeHaltReason(halt));
            }
        }
    }

    if (run_main) {
        returned_from_guest_ = false;
        cpu_->Regs().fill(0);
        cpu_->Regs()[13] = stack_pointer_;
        cpu_->Regs()[14] = return_stub_;
        cpu_->Regs()[15] = image_.entry_pc();
        cpu_->SetCpsr(image_.entry_cpsr() == 0 ? 0x10 : image_.entry_cpsr());

        while (!saw_exit_) {
            if (!BeginTickSlice()) {
                Log("tick budget exhausted at " + Hex32(cpu_->Regs()[15]));
                DumpHotPcSamples();
                break;
            }

            const Dynarmic::HaltReason halt = cpu_->Run();
            if (Dynarmic::Has(halt, Dynarmic::HaltReason::UserDefined5)) {
                RunUIApplicationLoop();
                break;
            }
            if (returned_from_guest_ || Dynarmic::Has(halt, Dynarmic::HaltReason::UserDefined1)
                || Dynarmic::Has(halt, Dynarmic::HaltReason::UserDefined2)) {
                break;
            }
            if (ticks_left_ == 0) {
                RecordHotPcSample();
                if (total_ticks_left_ == 0) {
                    Log("tick budget exhausted at " + Hex32(cpu_->Regs()[15])
                        + " halt=" + DescribeHaltReason(halt));
                    DumpHotPcSamples();
                    break;
                }
                continue;
            }
            throw std::runtime_error("guest execution halted unexpectedly at " + Hex32(cpu_->Regs()[15])
                + " halt=" + DescribeHaltReason(halt));
        }
    }

    return exit_code_;
}

std::optional<std::uint32_t> Emulator::MemoryReadCode(const Dynarmic::A32::VAddr vaddr) {
    return memory_.ReadCode32(vaddr);
}

std::uint8_t Emulator::MemoryRead8(const Dynarmic::A32::VAddr vaddr) {
    return memory_.Read8(vaddr);
}

std::uint16_t Emulator::MemoryRead16(const Dynarmic::A32::VAddr vaddr) {
    return memory_.Read16(vaddr);
}

std::uint32_t Emulator::MemoryRead32(const Dynarmic::A32::VAddr vaddr) {
    return memory_.Read32(vaddr);
}

std::uint64_t Emulator::MemoryRead64(const Dynarmic::A32::VAddr vaddr) {
    return memory_.Read64(vaddr);
}

void Emulator::MemoryWrite8(const Dynarmic::A32::VAddr vaddr, const std::uint8_t value) {
    try {
        memory_.Write8(vaddr, value);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string(ex.what())
            + " pc=" + Hex32(cpu_->Regs()[15])
            + " lr=" + Hex32(cpu_->Regs()[14])
            + " sp=" + Hex32(cpu_->Regs()[13])
            + " r0=" + Hex32(cpu_->Regs()[0])
            + " r1=" + Hex32(cpu_->Regs()[1]));
    }
    if (memory_.HasExecute(vaddr)) {
        cpu_->InvalidateCacheRange(vaddr, 1);
    }
}

void Emulator::MemoryWrite16(const Dynarmic::A32::VAddr vaddr, const std::uint16_t value) {
    try {
        memory_.Write16(vaddr, value);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string(ex.what())
            + " pc=" + Hex32(cpu_->Regs()[15])
            + " lr=" + Hex32(cpu_->Regs()[14])
            + " sp=" + Hex32(cpu_->Regs()[13])
            + " r0=" + Hex32(cpu_->Regs()[0])
            + " r1=" + Hex32(cpu_->Regs()[1]));
    }
    if (memory_.HasExecute(vaddr)) {
        cpu_->InvalidateCacheRange(vaddr, 2);
    }
}

void Emulator::MemoryWrite32(const Dynarmic::A32::VAddr vaddr, const std::uint32_t value) {
    try {
        memory_.Write32(vaddr, value);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string(ex.what())
            + " pc=" + Hex32(cpu_->Regs()[15])
            + " lr=" + Hex32(cpu_->Regs()[14])
            + " sp=" + Hex32(cpu_->Regs()[13])
            + " r0=" + Hex32(cpu_->Regs()[0])
            + " r1=" + Hex32(cpu_->Regs()[1]));
    }
    if (memory_.HasExecute(vaddr)) {
        cpu_->InvalidateCacheRange(vaddr, 4);
    }
}

void Emulator::MemoryWrite64(const Dynarmic::A32::VAddr vaddr, const std::uint64_t value) {
    try {
        memory_.Write64(vaddr, value);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string(ex.what())
            + " pc=" + Hex32(cpu_->Regs()[15])
            + " lr=" + Hex32(cpu_->Regs()[14])
            + " sp=" + Hex32(cpu_->Regs()[13])
            + " r0=" + Hex32(cpu_->Regs()[0])
            + " r1=" + Hex32(cpu_->Regs()[1]));
    }
    if (memory_.HasExecute(vaddr)) {
        cpu_->InvalidateCacheRange(vaddr, 8);
    }
}

bool Emulator::MemoryWriteExclusive8(const Dynarmic::A32::VAddr vaddr, const std::uint8_t value, const std::uint8_t expected) {
    if (memory_.Read8(vaddr) != expected) {
        return false;
    }
    MemoryWrite8(vaddr, value);
    return true;
}

bool Emulator::MemoryWriteExclusive16(const Dynarmic::A32::VAddr vaddr, const std::uint16_t value, const std::uint16_t expected) {
    if (memory_.Read16(vaddr) != expected) {
        return false;
    }
    MemoryWrite16(vaddr, value);
    return true;
}

bool Emulator::MemoryWriteExclusive32(const Dynarmic::A32::VAddr vaddr, const std::uint32_t value, const std::uint32_t expected) {
    if (memory_.Read32(vaddr) != expected) {
        return false;
    }
    MemoryWrite32(vaddr, value);
    return true;
}

bool Emulator::MemoryWriteExclusive64(const Dynarmic::A32::VAddr vaddr, const std::uint64_t value, const std::uint64_t expected) {
    if (memory_.Read64(vaddr) != expected) {
        return false;
    }
    MemoryWrite64(vaddr, value);
    return true;
}

bool Emulator::IsReadOnlyMemory(const Dynarmic::A32::VAddr vaddr) {
    return memory_.IsReadOnly(vaddr);
}

void Emulator::InterpreterFallback(const Dynarmic::A32::VAddr pc, const size_t num_instructions) {
    throw std::runtime_error("interpreter fallback at " + Hex32(pc) + " for " + std::to_string(num_instructions) + " instructions");
}

void Emulator::CallSVC(const std::uint32_t swi) {
    const auto it = svc_handlers_.find(swi);
    if (it == svc_handlers_.end()) {
        throw std::runtime_error("unhandled SVC " + std::to_string(swi));
    }

    std::string svc_name = "<unknown>";
    if (trace_shims_) {
        const auto name_it = svc_names_.find(swi);
        if (name_it != svc_names_.end()) {
            svc_name = name_it->second;
            Log("svc " + std::to_string(swi) + " -> " + name_it->second);
        }
    } else if (const auto name_it = svc_names_.find(swi); name_it != svc_names_.end()) {
        svc_name = name_it->second;
    }
    try {
        it->second();
    } catch (const std::exception& ex) {
        throw std::runtime_error("svc " + svc_name + " failed: " + ex.what()
            + " pc=" + Hex32(cpu_->Regs()[15])
            + " lr=" + Hex32(cpu_->Regs()[14])
            + " sp=" + Hex32(cpu_->Regs()[13])
            + " r0=" + Hex32(cpu_->Regs()[0])
            + " r1=" + Hex32(cpu_->Regs()[1])
            + " r2=" + Hex32(cpu_->Regs()[2])
            + " r3=" + Hex32(cpu_->Regs()[3]));
    }
}

void Emulator::ExceptionRaised(const Dynarmic::A32::VAddr pc, const Dynarmic::A32::Exception exception) {
    // Guest jumped to NULL — this commonly happens when ObjC messages nil,
    // which returns 0/NULL in real iOS. Simulate that: set r0=0 and return to LR.
    if (pc == 0) {
        Log("[null-call] pc=0x0 lr=" + Hex32(cpu_->Regs()[14])
            + " r0=" + Hex32(cpu_->Regs()[0])
            + " r1=" + Hex32(cpu_->Regs()[1]));
        cpu_->Regs()[0] = 0;
        cpu_->Regs()[1] = 0;
        cpu_->SetCpsr(cpu_->Cpsr() & ~(1u << 5));  // ensure ARM mode
        cpu_->Regs()[15] = cpu_->Regs()[14] & ~1u;  // return to LR
        return;
    }
    throw std::runtime_error(
        "guest exception " + std::to_string(static_cast<int>(exception))
        + " at " + Hex32(pc)
        + " lr=" + Hex32(cpu_->Regs()[14])
        + " sp=" + Hex32(cpu_->Regs()[13])
        + " r0=" + Hex32(cpu_->Regs()[0])
        + " r1=" + Hex32(cpu_->Regs()[1]));
}

void Emulator::AddTicks(const std::uint64_t ticks) {
    if (ticks >= ticks_left_) {
        if (ticks >= total_ticks_left_) {
            total_ticks_left_ = 0;
        } else {
            total_ticks_left_ -= ticks;
        }
        ticks_left_ = 0;
        return;
    }
    ticks_left_ -= ticks;
    total_ticks_left_ -= ticks;
}

std::uint64_t Emulator::GetTicksRemaining() {
    return ticks_left_ == 0 ? 1 : ticks_left_;
}

bool Emulator::BeginTickSlice() {
    if (total_ticks_left_ == 0) {
        ticks_left_ = 0;
        return false;
    }
    ticks_left_ = std::min(total_ticks_left_, kGuestRunSliceTicks);
    return true;
}

void Emulator::RecordHotPcSample() {
    hot_pc_samples_[cpu_->Regs()[15] & ~1u] += 1;
}

void Emulator::DumpHotPcSamples(const std::size_t limit) const {
    if (hot_pc_samples_.empty()) {
        return;
    }

    std::vector<std::pair<u32, u64>> samples(hot_pc_samples_.begin(), hot_pc_samples_.end());
    std::sort(samples.begin(), samples.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.second > rhs.second;
    });

    Log("hot guest PCs:");
    for (std::size_t i = 0; i < std::min(limit, samples.size()); ++i) {
        Log("  " + Hex32(samples[i].first) + " hits=" + std::to_string(samples[i].second));
    }
}

void Emulator::BuildImage() {
    image_.LoadIntoMemory(memory_);
    image_.ApplyBinds(memory_, [this](const std::string_view symbol_name, const int dylib_ordinal, const bool weak_import) {
        return ResolveImport(symbol_name, dylib_ordinal, weak_import);
    });

    // Force LogicDefines::OFFLINE_MODE on so we can bypass the server path for now.
    memory_.Write8(0x002AF73D, 1);
    memory_.Write8(kGuestResourceManagerLazyLoadingEnabled, 1);
}

void Emulator::BuildCpu() {
    Dynarmic::A32::UserConfig config;
    config.callbacks = this;
    exclusive_monitor_ = std::make_unique<Dynarmic::ExclusiveMonitor>(1);
    config.processor_id = 0;
    config.global_monitor = exclusive_monitor_.get();
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.always_little_endian = true;
    config.page_table = memory_.page_table_mut();
    config.detect_misaligned_access_via_page_table = 8 | 16 | 32 | 64;
    config.enable_cycle_counting = true;
    cpu_ = std::make_unique<Dynarmic::A32::Jit>(config);
}

void Emulator::BuildProcessState() {
    const u32 stack_bottom = stack_base_ - stack_size_;
    memory_.MapZeroFill(stack_bottom, stack_size_, kPermRead | kPermWrite, "stack");

    const u32 argv0 = AllocateCString(JoinPathForGuest(guest_home_, binary_path_.filename().string()), "argv0");
    u32 sp = stack_base_;
    auto push = [&](const u32 value) {
        sp -= 4;
        memory_.Write32(sp, value);
    };

    push(0);
    push(0);
    push(argv0);
    push(1);
    stack_pointer_ = AlignDown(sp, 8);
}

void Emulator::CreateBuiltins() {
    return_stub_ = RegisterFunctionShim("__guest_return", [this] {
        returned_from_guest_ = true;
        Stop(Dynarmic::HaltReason::UserDefined1);
    });
    uiapplicationmain_loop_stub_ = RegisterFunctionShim("__uiapplicationmain_loop", [this] {
        Stop(Dynarmic::HaltReason::UserDefined5);
    });
}

void Emulator::InstallGuestFastPaths() {
    auto allocate_thumb_svc = [this]() -> u32 {
        for (u32 svc_id = 1; svc_id < 0x100; ++svc_id) {
            if (!svc_handlers_.contains(svc_id)) {
                return svc_id;
            }
        }
        throw std::runtime_error("no free thumb svc ids for guest fast paths");
    };

    auto read_guest_string_bytes = [this](const u32 self) {
        const u32 byte_length = memory_.Read32(self + kGuestStringByteLengthOffset);
        u32 storage = self + kGuestStringStorageOffset;
        if (byte_length + 1 > kGuestStringInlineCapacity + 1) {
            storage = memory_.Read32(storage);
        }

        std::string bytes;
        bytes.resize(byte_length);
        if (byte_length != 0) {
            const auto raw = memory_.ReadBuffer(storage, byte_length);
            std::memcpy(bytes.data(), raw.data(), raw.size());
        }
        return bytes;
    };

    auto install_thumb_fast_path = [this, &allocate_thumb_svc](const u32 address, const std::string& name, std::function<void()> handler) {
        const u32 svc_id = allocate_thumb_svc();
        svc_handlers_[svc_id] = std::move(handler);
        svc_names_[svc_id] = name;
        memory_.Write16(address, static_cast<u16>(kThumbSvcBase | (svc_id & 0xFFu)));
        memory_.Write16(address + 2, kThumbBxLr);
        cpu_->InvalidateCacheRange(address, 4);
    };

    install_thumb_fast_path(0xCAC50, "__guest_fast_String_updateLength", [this, read_guest_string_bytes]() {
        const u32 self = Arg(0);
        const std::string bytes = read_guest_string_bytes(self);
        const DecodedUtf8String decoded = DecodeUtf8String(bytes);
        memory_.Write32(self + kGuestStringCodepointLengthOffset, static_cast<u32>(decoded.codepoints.size()));
        SetReturnU32(self);
    });

    install_thumb_fast_path(0xCB54C, "__guest_fast_String_offsetAt", [this, read_guest_string_bytes]() {
        const u32 self = Arg(0);
        const s32 requested_index = static_cast<s32>(Arg(1));
        const std::string bytes = read_guest_string_bytes(self);
        const DecodedUtf8String decoded = DecodeUtf8String(bytes);
        const u32 codepoint_count = static_cast<u32>(decoded.codepoints.size());
        memory_.Write32(self + kGuestStringCodepointLengthOffset, codepoint_count);

        u32 byte_offset = 0;
        u32 cached_index = 0;
        if (requested_index > 0) {
            cached_index = static_cast<u32>(std::min<s32>(requested_index, static_cast<s32>(codepoint_count)));
            if (cached_index >= codepoint_count) {
                byte_offset = static_cast<u32>(bytes.size());
            } else {
                byte_offset = decoded.byte_offsets[cached_index];
            }
        }

        memory_.Write32(self + kGuestStringByteOffsetCacheOffset, byte_offset);
        memory_.Write32(self + kGuestStringCodepointIndexCacheOffset, cached_index);
        SetReturnU32(byte_offset);
    });

    install_thumb_fast_path(0xC194C, "__guest_fast_stbi_zlib_decode_malloc_guesssize_headerflag", [this]() {
        const u32 input_ptr = Arg(0);
        const u32 input_size = Arg(1);
        const u32 initial_size = std::max<u32>(Arg(2), 1);
        const u32 out_len_ptr = Arg(3);
        const u32 parse_header = memory_.IsMapped(cpu_->Regs()[13]) && memory_.IsMapped(cpu_->Regs()[13] + 3)
            ? memory_.Read32(cpu_->Regs()[13])
            : 1;

        if (input_ptr == 0 || input_size == 0) {
            SetReturnU32(0);
            return;
        }

        const auto input = memory_.ReadBuffer(input_ptr, input_size);
        z_stream stream{};
        stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
        stream.avail_in = input_size;

        const int window_bits = parse_header != 0 ? MAX_WBITS : -MAX_WBITS;
        int rc = inflateInit2(&stream, window_bits);
        if (rc != Z_OK) {
            SetReturnU32(0);
            return;
        }

        std::vector<u8> output(std::max<u32>(initial_size, 16 * 1024));
        while (true) {
            if (stream.total_out >= output.size()) {
                if (output.size() >= 128u * 1024u * 1024u) {
                    rc = Z_MEM_ERROR;
                    break;
                }
                output.resize(output.size() * 2);
            }
            stream.next_out = reinterpret_cast<Bytef*>(output.data() + stream.total_out);
            stream.avail_out = static_cast<uInt>(std::min<std::size_t>(output.size() - stream.total_out, std::numeric_limits<uInt>::max()));
            rc = inflate(&stream, Z_NO_FLUSH);
            if (rc == Z_STREAM_END) {
                break;
            }
            if (rc != Z_OK) {
                break;
            }
        }
        const uLong total_out = stream.total_out;
        inflateEnd(&stream);

        if (rc != Z_STREAM_END) {
            Log("stbi zlib fastpath failed rc=" + std::to_string(rc)
                + " input=" + std::to_string(input_size)
                + " header=" + std::to_string(parse_header));
            SetReturnU32(0);
            return;
        }

        output.resize(static_cast<std::size_t>(total_out));
        const u32 guest_output = AllocateGuest(heap_cursor_, std::max<u32>(static_cast<u32>(output.size()), 1), 16, kPermRead | kPermWrite, "heap.stbi_zlib");
        heap_alloc_sizes_[guest_output] = std::max<u32>(static_cast<u32>(output.size()), 1);
        if (!output.empty()) {
            memory_.WriteBuffer(guest_output, output);
        }
        if (out_len_ptr != 0) {
            memory_.Write32(out_len_ptr, static_cast<u32>(output.size()));
        }
        SetReturnU32(guest_output);
    });

    install_thumb_fast_path(0xBF4F4, "__guest_fast_ResourceListener_addFile", [this]() {
        constexpr u32 kVectorBeginOffset = 0x4;
        constexpr u32 kVectorEndOffset = 0x8;
        constexpr u32 kVectorCapacityOffset = 0xC;
        constexpr u32 kElementSize = 0xC;

        const u32 self = Arg(0);
        const u32 arg2 = Arg(2);
        const u32 arg3 = Arg(3);
        const std::string guest_path = ReadGuestCString(Arg(1));
        const std::string normalized_path = NormalizeGuestResourceKey(guest_path, guest_home_);

        u32 begin = memory_.Read32(self + kVectorBeginOffset);
        u32 end = memory_.Read32(self + kVectorEndOffset);
        u32 capacity_end = memory_.Read32(self + kVectorCapacityOffset);

        const u32 used_bytes = (begin != 0 && end >= begin) ? (end - begin) : 0;
        const u32 used_count = used_bytes / kElementSize;
        const u32 capacity_count = (begin != 0 && capacity_end >= begin)
            ? ((capacity_end - begin) / kElementSize)
            : 0;

        if (used_count >= capacity_count) {
            const u32 new_capacity_count = std::max<u32>(capacity_count == 0 ? 4 : capacity_count * 2, used_count + 1);
            const u32 new_begin = AllocateData(new_capacity_count * kElementSize, 4, "ResourceListener.files");
            if (used_bytes != 0) {
                memory_.WriteBuffer(new_begin, memory_.ReadBuffer(begin, used_bytes));
            }
            begin = new_begin;
            end = begin + used_bytes;
            capacity_end = begin + new_capacity_count * kElementSize;
            memory_.Write32(self + kVectorBeginOffset, begin);
            memory_.Write32(self + kVectorEndOffset, end);
            memory_.Write32(self + kVectorCapacityOffset, capacity_end);
        }

        libstdcpp_abi_->InitializeString(end, normalized_path, "ResourceListener.addFile.path");
        memory_.Write32(end + 0x4, arg2);
        memory_.Write32(end + 0x8, arg3);
        memory_.Write32(self + kVectorEndOffset, end + kElementSize);

        if (guest_path.find("Button_click_13") != std::string::npos
            || normalized_path.find("Button_click_13") != std::string::npos) {
            Log("[sound-debug] ResourceListener::addFile guest=" + guest_path
                + " normalized=" + normalized_path
                + " arg2=" + std::to_string(arg2)
                + " arg3=" + std::to_string(arg3));
        }
        SetReturnU32(self);
    });

    install_thumb_fast_path(0xC3838, "__guest_fast_ResourceManager_getSound", [this]() {
        constexpr u32 kSoundSystemSingleton = 0x002AF704;
        constexpr u32 kSoundSystemImplOffset = 0x0;
        constexpr u32 kSoundSystemSlotCountOffset = 0x4;
        constexpr u32 kSoundSystemSlotArrayOffset = 0x1430;
        constexpr u32 kSoundFxSlotSize = 0x110;
        constexpr u32 kSoundFxActiveOffset = 0x0;
        constexpr u32 kSoundFxPathOffset = 0x10;

        const std::string query_path = ReadGuestCString(Arg(0));
        const std::string normalized_query = NormalizeGuestResourceKey(query_path, guest_home_);
        const std::string absolute_query = StartsWith(query_path, guest_home_)
            ? query_path
            : (normalized_query.empty() ? query_path : JoinPathForGuest(guest_home_, normalized_query));

        const u32 sound_system = memory_.IsMapped(kSoundSystemSingleton)
            ? memory_.Read32(kSoundSystemSingleton)
            : 0;
        if (sound_system != 0 && memory_.IsMapped(sound_system + kSoundSystemSlotCountOffset)) {
            const u32 impl = memory_.Read32(sound_system + kSoundSystemImplOffset);
            const u32 slot_count = std::min<u32>(memory_.Read32(sound_system + kSoundSystemSlotCountOffset), 256);
            const u32 slot_array = (impl != 0 && memory_.IsMapped(impl + kSoundSystemSlotArrayOffset + 3))
                ? memory_.Read32(impl + kSoundSystemSlotArrayOffset)
                : 0;
            if (slot_array != 0) {
                for (u32 slot_index = 0; slot_index < slot_count; ++slot_index) {
                    const u32 slot = slot_array + slot_index * kSoundFxSlotSize;
                    if (!memory_.IsMapped(slot + kSoundFxPathOffset + 3) || memory_.Read32(slot + kSoundFxActiveOffset) == 0) {
                        continue;
                    }
                    const std::string slot_path = ReadGuestCString(slot + kSoundFxPathOffset);
                    const std::string normalized_slot = NormalizeGuestResourceKey(slot_path, guest_home_);
                    if (slot_path == query_path
                        || slot_path == absolute_query
                        || normalized_slot == normalized_query) {
                        const u32 sound_id = slot_index + 1;
                        if (query_path.find("Button_click_13") != std::string::npos
                            || slot_path.find("Button_click_13") != std::string::npos) {
                            Log("[sound-debug] ResourceManager::getSound fallback query=" + query_path
                                + " slot_path=" + slot_path
                                + " sound_id=" + std::to_string(sound_id));
                        }
                        SetReturnU32(sound_id);
                        return;
                    }
                }
            }
        }

        if (query_path.find("Button_click_13") != std::string::npos) {
            Log("[sound-debug] ResourceManager::getSound miss query=" + query_path
                + " normalized=" + normalized_query
                + " absolute=" + absolute_query);
        }
        Log("[warning] Cant find resource: " + query_path);
        SetReturnU32(0);
    });

    install_thumb_fast_path(0xC31C4, "__guest_fast_ResourceManager_doesFileExist", [this]() {
        const std::string query_path = ReadGuestCString(Arg(0));
        const std::string normalized_query = NormalizeGuestResourceKey(query_path, guest_home_);
        const std::string absolute_query = StartsWith(query_path, guest_home_)
            ? query_path
            : (normalized_query.empty() ? query_path : JoinPathForGuest(guest_home_, normalized_query));

        bool exists = GuestPathExists(query_path);
        if (!exists && normalized_query != query_path) {
            exists = GuestPathExists(normalized_query);
        }
        if (!exists && absolute_query != query_path && absolute_query != normalized_query) {
            exists = GuestPathExists(absolute_query);
        }

        if (query_path.find("sc/ui.sc") != std::string::npos
            || normalized_query.find("sc/ui.sc") != std::string::npos) {
            Log("[resource-debug] ResourceManager::doesFileExist query=" + query_path
                + " normalized=" + normalized_query
                + " absolute=" + absolute_query
                + " exists=" + std::to_string(exists ? 1 : 0));
        }

        SetReturnU32(exists ? 1u : 0u);
    });

}

u32 Emulator::ResolveImport(const std::string_view name, const int dylib_ordinal, const bool weak_import) {
    return shim_registry_->ResolveImport(name, dylib_ordinal, weak_import);
}

u32 Emulator::RegisterFunctionShim(const std::string& name, std::function<void()> handler) {
    const bool tail_call = name == "_objc_msgSend"
        || name == "_objc_msgSendSuper2"
        || name == "_objc_msgSend_stret"
        || name == "_UIApplicationMain";
    return RegisterFunctionShim(name, std::move(handler), tail_call);
}

u32 Emulator::RegisterFunctionShim(const std::string& name, std::function<void()> handler, const bool tail_call) {
    if (const auto it = import_cache_.find(name); it != import_cache_.end()) {
        return it->second;
    }

    const u32 stub = AllocateExecutable(8, 4, "svc:" + name);
    if (stub == 0x30001418u || (stub & ~0xFFFu) == 0x30001000u) {
        Log("[shim] " + name + " -> " + Hex32(stub));
    }
    const u32 svc_id = next_svc_++;
    memory_.Protect(stub, 8, kPermRead | kPermWrite | kPermExec);
    memory_.Write32(stub, kArmSvcBase | (svc_id & 0x00FFFFFFu));
    memory_.Write32(stub + 4, tail_call ? kArmBxIp : kArmBxLr);
    memory_.Protect(stub, 8, kPermRead | kPermExec);
    svc_handlers_[svc_id] = std::move(handler);
    svc_names_[svc_id] = name;
    import_cache_[name] = stub;
    import_name_by_stub_[stub] = name;
    return stub;
}

u32 Emulator::RegisterDataShim(const std::string& name, const std::span<const u8> bytes) {
    if (const auto it = import_cache_.find(name); it != import_cache_.end()) {
        return it->second;
    }

    const u32 address = AllocateData(static_cast<u32>(bytes.size()), 4, name);
    memory_.WriteBuffer(address, bytes);
    import_cache_[name] = address;
    return address;
}

u32 Emulator::RegisterStringConstant(const std::string& name, const std::string& value) {
    if (const auto it = import_cache_.find(name); it != import_cache_.end()) {
        return it->second;
    }
    const u32 address = EnsureNSString(value);
    import_cache_[name] = address;
    return address;
}

u32 Emulator::AllocateGuest(u32& cursor, const u32 size, const u32 alignment, const u8 permissions, const std::string& tag) {
    const u32 address = AlignUp(cursor, alignment);
    cursor = AlignUp(address + std::max<u32>(size, 1), alignment);
    memory_.MapZeroFill(address, std::max<u32>(size, 1), permissions, tag);
    return address;
}

u32 Emulator::AllocateData(const u32 size, const u32 alignment, const std::string& tag) {
    return AllocateGuest(object_cursor_, size, alignment, kPermRead | kPermWrite, tag);
}

u32 Emulator::AllocateExecutable(const u32 size, const u32 alignment, const std::string& tag) {
    return AllocateGuest(executable_cursor_, size, alignment, kPermRead | kPermExec, tag);
}

u32 Emulator::AllocateCString(const std::string& text, const std::string& tag) {
    const u32 address = AllocateData(static_cast<u32>(text.size() + 1), 1, tag);
    memory_.WriteBuffer(address, std::span<const u8>(reinterpret_cast<const u8*>(text.c_str()), text.size() + 1));
    return address;
}

u32 Emulator::EnsureClass(const std::string& class_name) {
    if (objc_abi_ != nullptr) {
        if (const auto guest_class = objc_abi_->FindClassByName(class_name)) {
            return *guest_class;
        }
    }
    if (const auto it = class_cache_.find(class_name); it != class_cache_.end()) {
        return it->second.first;
    }

    const u32 class_address = AllocateData(kObjcObjectSize, 4, "objc_class:" + class_name);
    const u32 meta_address = AllocateData(kObjcObjectSize, 4, "objc_metaclass:" + class_name);
    class_cache_[class_name] = {class_address, meta_address};

    const u32 superclass = class_name == "NSObject" ? 0 : EnsureClass("NSObject");
    const u32 super_meta = class_name == "NSObject" ? meta_address : EnsureMetaClass("NSObject");

    host_objects_[class_address] = HostObject{
        .kind = ObjKind::Class,
        .class_name = class_name,
        .isa = meta_address,
        .meta = superclass
    };
    host_objects_[meta_address] = HostObject{
        .kind = ObjKind::MetaClass,
        .class_name = class_name,
        .isa = meta_address,
        .meta = super_meta
    };

    memory_.Write32(class_address, meta_address);
    memory_.Write32(class_address + 4, superclass);
    memory_.Write32(meta_address, meta_address);
    memory_.Write32(meta_address + 4, super_meta);
    return class_address;
}

u32 Emulator::EnsureMetaClass(const std::string& class_name) {
    if (objc_abi_ != nullptr) {
        if (const auto guest_meta_class = objc_abi_->FindMetaClassByName(class_name)) {
            return *guest_meta_class;
        }
    }
    EnsureClass(class_name);
    return class_cache_.at(class_name).second;
}

u32 Emulator::EnsureNSString(const std::string& value) {
    const u32 object = AllocateData(kObjcObjectSize, 4, "NSString");
    const u32 backing = AllocateCString(value, "NSString.bytes");
    host_objects_[object] = HostObject{
        .kind = ObjKind::String,
        .class_name = "NSString",
        .string_value = value,
        .isa = EnsureClass("NSString"),
        .backing_store = backing
    };
    memory_.Write32(object, EnsureClass("NSString"));
    memory_.Write32(object + 4, backing);
    memory_.Write32(object + 8, backing);
    memory_.Write32(object + 12, static_cast<u32>(value.size()));
    return object;
}

u32 Emulator::EnsureNSData(const std::vector<u8>& data) {
    const u32 object = AllocateData(kObjcObjectSize, 4, "NSData");
    const u32 backing = AllocateData(static_cast<u32>(data.size() == 0 ? 1 : data.size()), 4, "NSData.bytes");
    if (!data.empty()) {
        memory_.WriteBuffer(backing, data);
    }
    host_objects_[object] = HostObject{
        .kind = ObjKind::Data,
        .class_name = "NSData",
        .bytes = data,
        .isa = EnsureClass("NSData"),
        .backing_store = backing
    };
    memory_.Write32(object, EnsureClass("NSData"));
    memory_.Write32(object + 4, backing);
    memory_.Write32(object + 8, static_cast<u32>(data.size()));
    return object;
}

u32 Emulator::EnsureArray(const std::vector<u32>& values) {
    const u32 object = AllocateData(kObjcObjectSize, 4, "NSArray");
    host_objects_[object] = HostObject{
        .kind = ObjKind::Array,
        .class_name = "NSArray",
        .items = values,
        .isa = EnsureClass("NSArray")
    };
    memory_.Write32(object, EnsureClass("NSArray"));
    memory_.Write32(object + 8, static_cast<u32>(values.size()));
    return object;
}

u32 Emulator::EnsureDictionary(const std::unordered_map<std::string, u32>& values) {
    return EnsureDictionaryOfClass(values, "NSDictionary");
}

u32 Emulator::EnsureDictionaryOfClass(const std::unordered_map<std::string, u32>& values, const std::string& class_name) {
    const u32 object = AllocateData(kObjcObjectSize, 4, "NSDictionary");
    host_objects_[object] = HostObject{
        .kind = ObjKind::Dictionary,
        .class_name = class_name,
        .dict = values,
        .isa = EnsureClass(class_name)
    };
    memory_.Write32(object, EnsureClass(class_name));
    memory_.Write32(object + 8, static_cast<u32>(values.size()));
    return object;
}

u32 Emulator::EnsureNumber(const double value) {
    const u32 object = AllocateData(kObjcObjectSize, 4, "NSNumber");
    host_objects_[object] = HostObject{
        .kind = ObjKind::Number,
        .class_name = "NSNumber",
        .number_value = value,
        .isa = EnsureClass("NSNumber")
    };
    memory_.Write32(object, EnsureClass("NSNumber"));
    return object;
}

u32 Emulator::EnsureBoolean(const bool value) {
    const u32 object = AllocateData(kObjcObjectSize, 4, value ? "CFBooleanTrue" : "CFBooleanFalse");
    host_objects_[object] = HostObject{
        .kind = ObjKind::Boolean,
        .class_name = "NSNumber",
        .number_value = value ? 1.0 : 0.0,
        .boolean_value = value,
        .isa = EnsureClass("NSNumber")
    };
    memory_.Write32(object, EnsureClass("NSNumber"));
    return object;
}

u32 Emulator::EnsureDate(const double unix_seconds) {
    const u32 object = AllocateData(kObjcObjectSize, 4, "NSDate");
    host_objects_[object] = HostObject{
        .kind = ObjKind::Generic,
        .class_name = "NSDate",
        .string_value = "NSDate(" + DescribeDouble(unix_seconds) + ")",
        .number_value = unix_seconds,
        .isa = EnsureClass("NSDate")
    };
    memory_.Write32(object, EnsureClass("NSDate"));
    return object;
}

u32 Emulator::EnsureNSError(const std::string& domain, const s32 code, const u32 user_info) {
    std::string description = domain + " (" + std::to_string(code) + ")";
    if (const auto user_info_it = host_objects_.find(user_info); user_info_it != host_objects_.end() && user_info_it->second.kind == ObjKind::Dictionary) {
        if (const auto desc_it = user_info_it->second.dict.find("description"); desc_it != user_info_it->second.dict.end()) {
            description = DescribeNSObject(desc_it->second);
        }
    }

    const u32 object = AllocateData(kObjcObjectSize, 4, "NSError");
    host_objects_[object] = HostObject{
        .kind = ObjKind::Generic,
        .class_name = "NSError",
        .string_value = description,
        .number_value = static_cast<double>(code),
        .isa = EnsureClass("NSError")
    };
    HostObject& error = host_objects_[object];
    error.dict["domain"] = EnsureNSString(domain);
    error.dict["code"] = EnsureNumber(static_cast<double>(code));
    error.dict["userInfo"] = user_info;
    error.dict["description"] = EnsureNSString(description);
    memory_.Write32(object, EnsureClass("NSError"));
    return object;
}

std::optional<std::string> Emulator::DecodeNSString(const u32 address) const {
    if (address == 0) {
        return std::nullopt;
    }

    if (const auto it = host_objects_.find(address); it != host_objects_.end()) {
        if (it->second.kind == ObjKind::String) {
            return it->second.string_value;
        }
    }

    const u32 isa = memory_.Read32(address);
    if (const auto class_it = host_objects_.find(isa); class_it != host_objects_.end()) {
        if (class_it->second.kind == ObjKind::Class
            && (class_it->second.class_name == "NSString" || class_it->second.class_name == "NSMutableString")) {
            const u32 chars = memory_.Read32(address + 8);
            const u32 length = memory_.Read32(address + 12);
            return std::string(reinterpret_cast<const char*>(memory_.ReadBuffer(chars, length).data()), length);
        }
    }

    const std::string cstring = ReadGuestCString(address);
    if (!cstring.empty()) {
        return cstring;
    }
    return std::nullopt;
}

std::string Emulator::DescribeNSObject(const u32 address) const {
    if (address == 0) {
        return "(null)";
    }

    if (const auto text = DecodeNSString(address)) {
        return *text;
    }

    const auto it = host_objects_.find(address);
    if (it == host_objects_.end()) {
        if (objc_abi_ != nullptr) {
            if (const auto class_name = objc_abi_->ClassNameForReceiver(address)) {
                return "<" + *class_name + ":" + Hex32(address) + ">";
            }
        }
        return Hex32(address);
    }

    const HostObject& object = it->second;
    switch (object.kind) {
    case ObjKind::String:
        return object.string_value;
    case ObjKind::Number:
        return DescribeDouble(object.number_value);
    case ObjKind::Boolean:
        return object.boolean_value ? "1" : "0";
    case ObjKind::Data:
        return "<NSData:" + std::to_string(object.bytes.size()) + ">";
    case ObjKind::Array: {
        std::string out = "(";
        for (std::size_t index = 0; index < object.items.size(); ++index) {
            if (index != 0) {
                out += ", ";
            }
            out += DescribeNSObject(object.items[index]);
        }
        out += ")";
        return out;
    }
    case ObjKind::Dictionary: {
        std::vector<std::string> keys;
        keys.reserve(object.dict.size());
        for (const auto& [key, _] : object.dict) {
            if (StartsWith(key, "pb:")) {
                continue;
            }
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        std::string out = "{";
        for (std::size_t index = 0; index < keys.size(); ++index) {
            if (index != 0) {
                out += ", ";
            }
            out += keys[index] + " = " + DescribeNSObject(object.dict.at(keys[index]));
        }
        out += "}";
        return out;
    }
    case ObjKind::Class:
    case ObjKind::MetaClass:
        return "<" + object.class_name + " class>";
    default:
        break;
    }

    if (!object.string_value.empty()) {
        return object.string_value;
    }
    if (object.class_name == "NSError") {
        if (const auto desc_it = object.dict.find("description"); desc_it != object.dict.end()) {
            return DescribeNSObject(desc_it->second);
        }
    }
    if (object.class_name == "NSDate") {
        return "NSDate(" + DescribeDouble(object.number_value) + ")";
    }
    return "<" + object.class_name + ":" + Hex32(address) + ">";
}

std::string Emulator::FormatVarArgsString(const std::string& format, const std::size_t first_arg, const std::function<u32(std::size_t)>& read_arg) const {
    auto format_integer = [](const u64 value, const bool negative, const bool upper, const int base, const int width, const bool zero_pad) {
        std::ostringstream out;
        if (negative) {
            out << '-';
        }
        if (base == 16) {
            out << std::hex << (upper ? std::uppercase : std::nouppercase);
        }
        if (width > 0) {
            out << std::setw(width) << std::setfill(zero_pad ? '0' : ' ');
        }
        out << value;
        return out.str();
    };

    std::string rendered;
    std::size_t arg_index = first_arg;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%' || i + 1 >= format.size()) {
            rendered.push_back(format[i]);
            continue;
        }
        if (format[i + 1] == '%') {
            rendered.push_back('%');
            ++i;
            continue;
        }

        std::size_t spec_index = i + 1;
        bool zero_pad = false;
        while (spec_index < format.size() && std::strchr("-+ #0", format[spec_index]) != nullptr) {
            zero_pad = zero_pad || format[spec_index] == '0';
            ++spec_index;
        }

        int width = 0;
        while (spec_index < format.size() && std::isdigit(static_cast<unsigned char>(format[spec_index])) != 0) {
            width = width * 10 + (format[spec_index] - '0');
            ++spec_index;
        }
        if (spec_index < format.size() && format[spec_index] == '.') {
            ++spec_index;
            while (spec_index < format.size() && std::isdigit(static_cast<unsigned char>(format[spec_index])) != 0) {
                ++spec_index;
            }
        }

        bool long_long_value = false;
        if (spec_index < format.size() && (format[spec_index] == 'l' || format[spec_index] == 'h' || format[spec_index] == 'z' || format[spec_index] == 't')) {
            if (format[spec_index] == 'l' && spec_index + 1 < format.size() && format[spec_index + 1] == 'l') {
                long_long_value = true;
                ++spec_index;
            }
            ++spec_index;
        }
        if (spec_index >= format.size()) {
            break;
        }

        const char spec = format[spec_index];
        switch (spec) {
        case '@':
            rendered += DescribeNSObject(read_arg(arg_index++));
            break;
        case 's':
            rendered += ReadGuestCString(read_arg(arg_index++));
            break;
        case 'd':
        case 'i': {
            const s64 raw = long_long_value
                ? static_cast<s64>(Pack64(read_arg(arg_index), read_arg(arg_index + 1)))
                : static_cast<s32>(read_arg(arg_index));
            arg_index += long_long_value ? 2 : 1;
            const u64 magnitude = raw < 0 ? static_cast<u64>(-raw) : static_cast<u64>(raw);
            rendered += format_integer(magnitude, raw < 0, false, 10, width, zero_pad);
            break;
        }
        case 'u': {
            const u64 raw = long_long_value
                ? Pack64(read_arg(arg_index), read_arg(arg_index + 1))
                : read_arg(arg_index);
            arg_index += long_long_value ? 2 : 1;
            rendered += format_integer(raw, false, false, 10, width, zero_pad);
            break;
        }
        case 'x':
        case 'X': {
            const u64 raw = long_long_value
                ? Pack64(read_arg(arg_index), read_arg(arg_index + 1))
                : read_arg(arg_index);
            arg_index += long_long_value ? 2 : 1;
            rendered += format_integer(raw, false, spec == 'X', 16, width, zero_pad);
            break;
        }
        case 'p': {
            std::ostringstream out;
            out << "0x" << std::hex << std::nouppercase << read_arg(arg_index++);
            rendered += out.str();
            break;
        }
        case 'c':
            rendered.push_back(static_cast<char>(read_arg(arg_index++)));
            break;
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'e':
        case 'E': {
            const double value = BitCastFromU64<double>(Pack64(read_arg(arg_index), read_arg(arg_index + 1)));
            arg_index += 2;
            std::ostringstream out;
            if (width > 0) {
                out << std::setw(width) << std::setfill(zero_pad ? '0' : ' ');
            }
            out << value;
            rendered += out.str();
            break;
        }
        default:
            rendered.push_back('%');
            rendered.push_back(spec);
            break;
        }
        i = spec_index;
    }
    return rendered;
}

u32 Emulator::ArchiveObject(const u32 object) {
    auto append_u8 = [](std::vector<u8>& out, const u8 value) {
        out.push_back(value);
    };
    auto append_u32 = [&](std::vector<u8>& out, const u32 value) {
        append_u8(out, static_cast<u8>(value & 0xFFu));
        append_u8(out, static_cast<u8>((value >> 8) & 0xFFu));
        append_u8(out, static_cast<u8>((value >> 16) & 0xFFu));
        append_u8(out, static_cast<u8>((value >> 24) & 0xFFu));
    };
    auto append_u64 = [&](std::vector<u8>& out, const u64 value) {
        append_u32(out, static_cast<u32>(value & 0xFFFFFFFFu));
        append_u32(out, static_cast<u32>(value >> 32));
    };
    auto append_string = [&](std::vector<u8>& out, const std::string& value) {
        append_u32(out, static_cast<u32>(value.size()));
        out.insert(out.end(), value.begin(), value.end());
    };

    std::unordered_set<u32> seen;
    std::function<bool(u32, std::vector<u8>&)> encode = [&](const u32 value, std::vector<u8>& out) -> bool {
        if (value == 0) {
            append_u8(out, 0);
            return true;
        }
        if (const auto text = DecodeNSString(value)) {
            append_u8(out, 1);
            append_string(out, *text);
            return true;
        }

        const auto it = host_objects_.find(value);
        if (it == host_objects_.end()) {
            append_u8(out, 1);
            append_string(out, DescribeNSObject(value));
            return true;
        }

        const HostObject& host = it->second;
        switch (host.kind) {
        case ObjKind::Number:
            append_u8(out, 2);
            append_u64(out, BitCastToU64(host.number_value));
            return true;
        case ObjKind::Boolean:
            append_u8(out, 3);
            append_u8(out, host.boolean_value ? 1u : 0u);
            return true;
        case ObjKind::Data:
            append_u8(out, 4);
            append_u32(out, static_cast<u32>(host.bytes.size()));
            out.insert(out.end(), host.bytes.begin(), host.bytes.end());
            return true;
        case ObjKind::Array:
            if (!seen.insert(value).second) {
                append_u8(out, 1);
                append_string(out, "<recursive-array>");
                return true;
            }
            append_u8(out, 5);
            append_u32(out, static_cast<u32>(host.items.size()));
            for (const u32 item : host.items) {
                if (!encode(item, out)) {
                    return false;
                }
            }
            seen.erase(value);
            return true;
        case ObjKind::Dictionary:
            if (!seen.insert(value).second) {
                append_u8(out, 1);
                append_string(out, "<recursive-dict>");
                return true;
            }
            append_u8(out, 6);
            append_u32(out, static_cast<u32>(host.dict.size()));
            for (const auto& [key, dict_value] : host.dict) {
                append_string(out, key);
                if (!encode(dict_value, out)) {
                    return false;
                }
            }
            seen.erase(value);
            return true;
        default:
            if (host.class_name == "NSDate") {
                append_u8(out, 7);
                append_u64(out, BitCastToU64(host.number_value));
                return true;
            }
            append_u8(out, 1);
            append_string(out, DescribeNSObject(value));
            return true;
        }
    };

    std::vector<u8> out{'C', 'D', 'X', '1'};
    if (!encode(object, out)) {
        return EnsureNSData({});
    }
    return EnsureNSData(out);
}

u32 Emulator::UnarchiveObject(const u32 data_object) {
    const auto data_it = host_objects_.find(data_object);
    if (data_it == host_objects_.end() || data_it->second.kind != ObjKind::Data) {
        return 0;
    }

    const std::vector<u8>& bytes = data_it->second.bytes;
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "CDX1", 4) != 0) {
        return 0;
    }

    auto read_u8 = [&](std::size_t& cursor, u8& value) -> bool {
        if (cursor >= bytes.size()) {
            return false;
        }
        value = bytes[cursor++];
        return true;
    };
    auto read_u32 = [&](std::size_t& cursor, u32& value) -> bool {
        if (cursor + 4 > bytes.size()) {
            return false;
        }
        value = static_cast<u32>(bytes[cursor])
            | (static_cast<u32>(bytes[cursor + 1]) << 8)
            | (static_cast<u32>(bytes[cursor + 2]) << 16)
            | (static_cast<u32>(bytes[cursor + 3]) << 24);
        cursor += 4;
        return true;
    };
    auto read_u64 = [&](std::size_t& cursor, u64& value) -> bool {
        u32 lo = 0;
        u32 hi = 0;
        if (!read_u32(cursor, lo) || !read_u32(cursor, hi)) {
            return false;
        }
        value = Pack64(lo, hi);
        return true;
    };
    auto read_string = [&](std::size_t& cursor, std::string& value) -> bool {
        u32 size = 0;
        if (!read_u32(cursor, size) || cursor + size > bytes.size()) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(bytes.data() + cursor), size);
        cursor += size;
        return true;
    };

    std::function<u32(std::size_t&)> decode = [&](std::size_t& cursor) -> u32 {
        u8 tag = 0;
        if (!read_u8(cursor, tag)) {
            return 0;
        }
        switch (tag) {
        case 0:
            return 0;
        case 1: {
            std::string value;
            return read_string(cursor, value) ? EnsureNSString(value) : 0;
        }
        case 2: {
            u64 bits = 0;
            return read_u64(cursor, bits) ? EnsureNumber(BitCastFromU64<double>(bits)) : 0;
        }
        case 3: {
            u8 value = 0;
            return read_u8(cursor, value) ? EnsureBoolean(value != 0) : 0;
        }
        case 4: {
            u32 size = 0;
            if (!read_u32(cursor, size) || cursor + size > bytes.size()) {
                return 0;
            }
            std::vector<u8> data(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                 bytes.begin() + static_cast<std::ptrdiff_t>(cursor + size));
            cursor += size;
            return EnsureNSData(data);
        }
        case 5: {
            u32 count = 0;
            if (!read_u32(cursor, count)) {
                return 0;
            }
            std::vector<u32> items;
            items.reserve(count);
            for (u32 index = 0; index < count; ++index) {
                items.push_back(decode(cursor));
            }
            return EnsureArray(items);
        }
        case 6: {
            u32 count = 0;
            if (!read_u32(cursor, count)) {
                return 0;
            }
            std::unordered_map<std::string, u32> values;
            for (u32 index = 0; index < count; ++index) {
                std::string key;
                if (!read_string(cursor, key)) {
                    return 0;
                }
                values[key] = decode(cursor);
            }
            return EnsureDictionary(values);
        }
        case 7: {
            u64 bits = 0;
            return read_u64(cursor, bits) ? EnsureDate(BitCastFromU64<double>(bits)) : 0;
        }
        default:
            return 0;
        }
    };

    std::size_t cursor = 4;
    return decode(cursor);
}

void Emulator::EnsureGuestAnnotation(const u32 receiver) {
    if (receiver == 0 || host_objects_.count(receiver) != 0 || objc_abi_ == nullptr || !objc_abi_->IsGuestObject(receiver)) {
        return;
    }
    if (const auto class_name = objc_abi_->ClassNameForReceiver(receiver)) {
        host_objects_[receiver] = HostObject{
            .kind = ObjKind::Generic,
            .class_name = *class_name,
            .isa = objc_abi_->IsaOf(receiver)
        };
    }
}

bool Emulator::TryHandleObjcOverride(const u32 receiver, const std::string& selector_name, u32* result) {
    const std::string class_name = objc_abi_ != nullptr
        ? objc_abi_->ClassNameForReceiver(receiver).value_or("NSObject")
        : DescribeNSObject(receiver);

    if (const auto object_it = host_objects_.find(receiver); object_it != host_objects_.end()) {
        HostObject& object = object_it->second;
        if (object.kind == ObjKind::String) {
            if (selector_name == "UTF8String" || selector_name == "cString" || selector_name == "cStringUsingEncoding:") {
                if (object.backing_store == 0) {
                    object.backing_store = AllocateCString(object.string_value, "NSString.bytes");
                    memory_.Write32(receiver + 4, object.backing_store);
                    memory_.Write32(receiver + 8, object.backing_store);
                    memory_.Write32(receiver + 12, static_cast<u32>(object.string_value.size()));
                }
                if (result != nullptr) {
                    *result = object.backing_store;
                }
                cpu_->Regs()[0] = object.backing_store;
                return true;
            }
            if (selector_name == "length") {
                const u32 length = static_cast<u32>(object.string_value.size());
                if (result != nullptr) {
                    *result = length;
                }
                cpu_->Regs()[0] = length;
                return true;
            }
        }
    }

    if (selector_name == "sendSessionsToServerForAppCircle"
        || StartsWith(selector_name, "sendSessionsToServer")) {
        if (trace_shims_) {
            Log("objc override [" + class_name + " " + selector_name + "] -> void");
        }
        if (result != nullptr) {
            *result = 0;
        }
        cpu_->Regs()[0] = 0;
        return true;
    }

    return false;
}

bool Emulator::BeginObjcHostDispatch(const u32 receiver, const std::string& selector_name, const std::initializer_list<u32> args, const u32 continuation_stub, u32* sync_result) {
    EnsureGuestAnnotation(receiver);
    ObserveObjcKeyboardMessage(receiver, selector_name);

    if (TryHandleObjcOverride(receiver, selector_name, sync_result)) {
        return false;
    }

    const u32 selector = AllocateCString(selector_name, "selector");
    cpu_->Regs()[0] = receiver;
    cpu_->Regs()[1] = selector;
    std::size_t index = 0;
    for (const u32 arg : args) {
        if (index + 2 < cpu_->Regs().size()) {
            cpu_->Regs()[index + 2] = arg;
        }
        ++index;
    }

    if (objc_abi_ != nullptr) {
        if (const auto imp = objc_abi_->LookupMethodImp(receiver, selector_name, false, 0)) {
            if (trace_shims_) {
                const std::string class_name = objc_abi_->ClassNameForReceiver(receiver).value_or("NSObject");
                Log("objc guest " + Hex32(receiver) + " [" + class_name + " " + selector_name + "] -> " + Hex32(*imp));
            }
            cpu_->Regs()[14] = continuation_stub;
            cpu_->Regs()[12] = *imp;
            if (sync_result != nullptr) {
                *sync_result = receiver;
            }
            return true;
        }
    }

    const u32 result = DispatchObjCMessage(receiver, selector, false, false, 0);
    cpu_->Regs()[0] = result;
    if (sync_result != nullptr) {
        *sync_result = result;
    }
    return false;
}

u32 Emulator::BeginUIApplicationMain(const std::string& principal_class, const std::string& delegate_class) {
    Log("UIApplicationMain principal=" + principal_class + " delegate=" + delegate_class);

    PendingUIApplicationMain state;
    state.original_lr = cpu_->Regs()[14];
    state.principal_class = principal_class;
    state.delegate_class = delegate_class;
    state.continuation_stub = RegisterFunctionShim(
        "__uiapplicationmain_continue_" + std::to_string(next_internal_stub_id_++),
        [this] {
            ContinueUIApplicationMain();
        },
        true);
    state.application = DispatchObjCMessage(EnsureClass(principal_class), AllocateCString("sharedApplication", "selector"), false, false, 0);
    state.delegate = DispatchObjCMessage(EnsureClass(delegate_class), AllocateCString("alloc", "selector"), false, false, 0);
    state.launch_options = EnsureDictionary({});
    Log("UIApplicationMain state app=" + Hex32(state.application)
        + " delegate=" + Hex32(state.delegate)
        + " launchOptions=" + Hex32(state.launch_options)
        + " continue=" + Hex32(state.continuation_stub));
    state.phase = PendingUIApplicationMain::Phase::Init;

    runtime_->main_application = state.application;
    runtime_->main_delegate = state.delegate;
    host_objects_[state.application].dict["setDelegate:"] = state.delegate;
    pending_uiapplication_main_ = std::move(state);

    ContinueUIApplicationMain();
    return cpu_->Regs()[0];
}

void Emulator::ContinueUIApplicationMain() {
    while (pending_uiapplication_main_.has_value()) {
        PendingUIApplicationMain& state = *pending_uiapplication_main_;
        switch (state.phase) {
        case PendingUIApplicationMain::Phase::Init: {
            Log("UIApplicationMain phase Init delegate=" + Hex32(state.delegate)
                + " continue=" + Hex32(state.continuation_stub));
            state.phase = PendingUIApplicationMain::Phase::DidFinishLaunching;
            u32 result = 0;
            if (BeginObjcHostDispatch(state.delegate, "init", {}, state.continuation_stub, &result)) {
                return;
            }
            state.delegate = result != 0 ? result : state.delegate;
            cpu_->Regs()[0] = state.delegate;
            continue;
        }
        case PendingUIApplicationMain::Phase::DidFinishLaunching: {
            state.delegate = cpu_->Regs()[0] != 0 ? cpu_->Regs()[0] : state.delegate;
            Log("UIApplicationMain phase DidFinishLaunching delegate=" + Hex32(state.delegate)
                + " app=" + Hex32(state.application)
                + " launchOptions=" + Hex32(state.launch_options));
            state.phase = PendingUIApplicationMain::Phase::DidBecomeActive;
            u32 result = 0;
            if (BeginObjcHostDispatch(
                    state.delegate,
                    "application:didFinishLaunchingWithOptions:",
                    {state.application, state.launch_options},
                    state.continuation_stub,
                    &result)) {
                return;
            }
            cpu_->Regs()[0] = result;
            continue;
        }
        case PendingUIApplicationMain::Phase::DidBecomeActive: {
            if (cpu_->Regs()[0] == 0) {
                const bool has_initialized_ui = runtime_->main_window != 0 || runtime_->main_gl_view != 0;
                if (!has_initialized_ui) {
                    Log("UIApplicationMain launch failed");
                    cpu_->Regs()[0] = 1;
                    cpu_->Regs()[12] = state.original_lr;
                    pending_uiapplication_main_.reset();
                    return;
                }
                Log("UIApplicationMain guest delegate returned 0 after initializing UI, continuing lifecycle");
            }
            state.phase = PendingUIApplicationMain::Phase::Completed;
            u32 result = 0;
            if (BeginObjcHostDispatch(
                    state.delegate,
                    "applicationDidBecomeActive:",
                    {state.application},
                    state.continuation_stub,
                    &result)) {
                return;
            }
            cpu_->Regs()[0] = result;
            continue;
        }
        case PendingUIApplicationMain::Phase::Completed:
            if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported()) {
                runtime_->host_gl->PumpEvents();
            }
            cpu_->Regs()[0] = 0;
            cpu_->Regs()[12] = uiapplicationmain_loop_stub_;
            pending_uiapplication_main_.reset();
            return;
        }
    }
    cpu_->Regs()[12] = cpu_->Regs()[14];
}

void Emulator::RunUIApplicationLoop() {
    while (!saw_exit_ && total_ticks_left_ > 0) {
        if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported()) {
            runtime_->host_gl->PumpEvents();
        }
        if (active_objc_callback_.has_value()) {
            const ObjcCallbackResult result = ResumeObjcCallback();
            if (result == ObjcCallbackResult::Stopped) {
                break;
            }
            continue;
        }
        if (!queued_objc_callbacks_.empty()) {
            PendingObjcCallback callback = std::move(queued_objc_callbacks_.front());
            queued_objc_callbacks_.pop_front();
            const ObjcCallbackResult result = RunObjcCallback(callback.receiver, callback.selector_name, callback.args);
            if (result == ObjcCallbackResult::Stopped) {
                break;
            }
            continue;
        }
        ProcessHostTouchEvents();
        ProcessHostKeyEvents();
        ProcessHostPopupResults();
        if (!queued_objc_callbacks_.empty()) {
            continue;
        }
        if (!FireDisplayLinks()) {
            Log("[runloop] stopping UIApplication loop ticks_left=" + std::to_string(total_ticks_left_)
                + " queued_callbacks=" + std::to_string(queued_objc_callbacks_.size())
                + " display_links=" + std::to_string(runtime_->display_links.size()));
            break;
        }
    }
}

void Emulator::ProcessHostTouchEvents() {
    std::deque<HostTouchEvent> events;
    {
        std::lock_guard lock(g_host_touch_mutex);
        events.swap(g_host_touch_events);
    }
    if (events.empty() || runtime_->main_gl_view == 0) {
        return;
    }

    auto make_host_array_of_class = [&](const std::vector<u32>& values, const std::string& class_name) {
        const u32 object = AllocateData(kObjcObjectSize, 4, class_name);
        host_objects_[object] = HostObject{
            .kind = ObjKind::Array,
            .class_name = class_name,
            .items = values,
            .isa = EnsureClass(class_name)
        };
        memory_.Write32(object, EnsureClass(class_name));
        memory_.Write32(object + 8, static_cast<u32>(values.size()));
        return object;
    };

    auto make_host_event = [&](const u32 touches) {
        const u32 object = AllocateData(kObjcObjectSize, 4, "UIEvent");
        host_objects_[object] = HostObject{
            .kind = ObjKind::Generic,
            .class_name = "UIEvent",
            .isa = EnsureClass("UIEvent")
        };
        host_objects_[object].dict["touches"] = touches;
        memory_.Write32(object, EnsureClass("UIEvent"));
        return object;
    };

    auto ensure_touch = [&](const HostTouchEvent& event) -> RuntimeState::TouchState& {
        auto& touch = runtime_->active_touches[event.pointer_id];
        if (touch.object == 0) {
            touch.object = AllocateData(kObjcObjectSize, 4, "UITouch");
            host_objects_[touch.object] = HostObject{
                .kind = ObjKind::Generic,
                .class_name = "UITouch",
                .isa = EnsureClass("UITouch")
            };
            memory_.Write32(touch.object, EnsureClass("UITouch"));
            touch.x = event.x_points;
            touch.y = event.y_points;
            touch.previous_x = event.x_points;
            touch.previous_y = event.y_points;
            host_objects_[touch.object].dict["tapCount"] = 1;
            host_objects_[touch.object].dict["view"] = runtime_->main_gl_view;
            host_objects_[touch.object].dict["window"] = runtime_->main_window;
        }
        return touch;
    };

    for (const HostTouchEvent& event : events) {
        RuntimeState::TouchState& touch = ensure_touch(event);
        touch.previous_x = touch.x;
        touch.previous_y = touch.y;
        touch.x = std::clamp(event.x_points, 0.0f, static_cast<float>(runtime_->screen_width_points));
        touch.y = std::clamp(event.y_points, 0.0f, static_cast<float>(runtime_->screen_height_points));
        touch.timestamp = event.timestamp_seconds;

        std::string selector_name;
        switch (event.phase) {
        case HostTouchPhase::Began:
            touch.phase = 0;
            touch.previous_x = touch.x;
            touch.previous_y = touch.y;
            selector_name = "touchesBegan:withEvent:";
            break;
        case HostTouchPhase::Moved:
            touch.phase = 1;
            selector_name = "touchesMoved:withEvent:";
            break;
        case HostTouchPhase::Ended:
            touch.phase = 3;
            selector_name = "touchesEnded:withEvent:";
            break;
        case HostTouchPhase::Cancelled:
            touch.phase = 4;
            selector_name = "touchesCancelled:withEvent:";
            break;
        }

        HostObject& touch_object = host_objects_[touch.object];
        touch_object.number_value = touch.timestamp;
        touch_object.dict["phase"] = touch.phase;
        touch_object.dict["x.bits"] = FloatToBits(touch.x);
        touch_object.dict["y.bits"] = FloatToBits(touch.y);
        touch_object.dict["previous_x.bits"] = FloatToBits(touch.previous_x);
        touch_object.dict["previous_y.bits"] = FloatToBits(touch.previous_y);

        EnsureGuestAnnotation(runtime_->main_gl_view);
        if (event.phase == HostTouchPhase::Began || event.phase == HostTouchPhase::Ended || event.phase == HostTouchPhase::Cancelled) {
            const auto receiver_it = host_objects_.find(runtime_->main_gl_view);
            const std::string receiver_class = receiver_it == host_objects_.end() ? "guest" : receiver_it->second.class_name;
            Log("queue touch " + selector_name
                + " receiver=" + Hex32(runtime_->main_gl_view)
                + " class=" + receiver_class
                + " x=" + std::to_string(touch.x)
                + " y=" + std::to_string(touch.y));
        }

        const u32 touches = make_host_array_of_class({touch.object}, "NSSet");
        const u32 ui_event = make_host_event(touches);
        queued_objc_callbacks_.push_back(PendingObjcCallback{
            .receiver = runtime_->main_gl_view,
            .selector_name = selector_name,
            .args = {touches, ui_event},
        });

        if (event.phase == HostTouchPhase::Ended || event.phase == HostTouchPhase::Cancelled) {
            runtime_->active_touches.erase(event.pointer_id);
        }
    }
}

void Emulator::ProcessHostKeyEvents() {
    std::deque<HostKeyEvent> events;
    {
        std::lock_guard lock(g_host_key_mutex);
        events.swap(g_host_key_events);
    }
    if (events.empty()) {
        return;
    }

    if (runtime_->keyboard_responder == 0) {
        Log("[keyboard] dropping " + std::to_string(events.size()) + " event(s): no active KeyInput responder");
        return;
    }

    const u32 receiver = runtime_->keyboard_responder;
    EnsureGuestAnnotation(receiver);
    const auto receiver_it = host_objects_.find(receiver);
    const std::string receiver_class = receiver_it == host_objects_.end() ? "guest" : receiver_it->second.class_name;

    for (const HostKeyEvent& event : events) {
        std::string selector_name;
        std::vector<u32> args;
        std::string log_value;

        switch (event.phase) {
        case HostKeyPhase::Text:
            if (event.text.empty()) {
                continue;
            }
            for (const unsigned char byte : event.text) {
                if (byte < 0x20 || byte > 0x7E) {
                    continue;
                }
                const std::string value(1, static_cast<char>(byte));
                Log("[keyboard] queue insertText:"
                    + std::string(" receiver=") + Hex32(receiver)
                    + " class=" + receiver_class
                    + " value=" + value);
                queued_objc_callbacks_.push_back(PendingObjcCallback{
                    .receiver = receiver,
                    .selector_name = "insertText:",
                    .args = {EnsureNSString(value)},
                });
            }
            continue;
        case HostKeyPhase::Backspace:
            selector_name = "deleteBackward";
            log_value = "backspace";
            break;
        case HostKeyPhase::Enter:
            selector_name = "resignFirstResponder";
            log_value = "enter";
            break;
        }

        Log("[keyboard] queue " + selector_name
            + " receiver=" + Hex32(receiver)
            + " class=" + receiver_class
            + " value=" + log_value);
        queued_objc_callbacks_.push_back(PendingObjcCallback{
            .receiver = receiver,
            .selector_name = selector_name,
            .args = std::move(args),
        });
    }
}

void Emulator::ProcessHostPopupResults() {
    std::deque<HostPopupResult> results;
    {
        std::lock_guard lock(g_host_popup_mutex);
        results.swap(g_host_popup_results);
    }
    if (results.empty()) {
        return;
    }

    for (const HostPopupResult& result : results) {
        const auto popup_it = active_host_popups_.find(result.token);
        if (popup_it == active_host_popups_.end()) {
            Log("objc popup result ignored token=" + std::to_string(result.token)
                + " button=" + std::to_string(result.button_index));
            continue;
        }

        const u32 popup = popup_it->second;
        active_host_popups_.erase(popup_it);

        const auto object_it = host_objects_.find(popup);
        if (object_it == host_objects_.end()) {
            continue;
        }

        HostObject& object = object_it->second;
        const std::string& class_name = object.class_name;
        const bool is_alert = class_name == "UIAlertView";
        const bool is_action_sheet = class_name == "UIActionSheet";
        if (!is_alert && !is_action_sheet) {
            continue;
        }

        const auto delegate_it = object.dict.find("setDelegate:");
        const u32 delegate = delegate_it == object.dict.end() ? 0 : delegate_it->second;
        const auto cancel_it = object.dict.find("cancelButtonIndex");
        const s32 cancel_button_index = cancel_it == object.dict.end() ? -1 : static_cast<s32>(cancel_it->second);
        s32 button_index = result.button_index;
        if (button_index < 0 || static_cast<std::size_t>(button_index) >= object.items.size()) {
            button_index = cancel_button_index >= 0 ? cancel_button_index : (object.items.empty() ? 0 : 0);
        }

        SuppressGuestIapWarning(object, button_index);

        object.dict["hostPopupPending"] = 0;
        object.dict["hostPopupVisible"] = 0;
        object.dict["hostPopupDismissed"] = 1;
        object.dict["hostPopupToken"] = 0;

        Log("objc host popup result [" + class_name + "] delegate=" + Hex32(delegate)
            + " button=" + std::to_string(button_index));
        if (delegate == 0) {
            continue;
        }

        if (is_alert) {
            queued_objc_callbacks_.push_back(PendingObjcCallback{
                .receiver = delegate,
                .selector_name = "alertView:clickedButtonAtIndex:",
                .args = {popup, static_cast<u32>(button_index)},
            });
            queued_objc_callbacks_.push_back(PendingObjcCallback{
                .receiver = delegate,
                .selector_name = "alertView:willDismissWithButtonIndex:",
                .args = {popup, static_cast<u32>(button_index)},
            });
            queued_objc_callbacks_.push_back(PendingObjcCallback{
                .receiver = delegate,
                .selector_name = "alertView:didDismissWithButtonIndex:",
                .args = {popup, static_cast<u32>(button_index)},
            });
        } else {
            queued_objc_callbacks_.push_back(PendingObjcCallback{
                .receiver = delegate,
                .selector_name = "actionSheet:clickedButtonAtIndex:",
                .args = {popup, static_cast<u32>(button_index)},
            });
            queued_objc_callbacks_.push_back(PendingObjcCallback{
                .receiver = delegate,
                .selector_name = "actionSheet:willDismissWithButtonIndex:",
                .args = {popup, static_cast<u32>(button_index)},
            });
            queued_objc_callbacks_.push_back(PendingObjcCallback{
                .receiver = delegate,
                .selector_name = "actionSheet:didDismissWithButtonIndex:",
                .args = {popup, static_cast<u32>(button_index)},
            });
        }
    }
}

bool Emulator::ApplyGuestForcedBoolReturn(const u32 address, const bool value, const std::string& symbol_name) {
    const u16 desired0 = value ? static_cast<u16>(kThumbMovsR0Zero + 1) : kThumbMovsR0Zero;
    const u16 desired1 = kThumbBxLr;
    const u16 original0 = memory_.Read16(address);
    const u16 original1 = memory_.Read16(address + 2);
    if (original0 == desired0 && original1 == desired1) {
        return true;
    }
    if (original1 != kThumbBxLr) {
        Log("[patch] skipped " + symbol_name
            + " at " + Hex32(address)
            + " first=" + Hex32(original0)
            + " second=" + Hex32(original1));
        return false;
    }

    memory_.SetWriteProtection(false);
    memory_.Write16(address, desired0);
    memory_.Write16(address + 2, desired1);
    memory_.SetWriteProtection(true);
    cpu_->InvalidateCacheRange(address, 4);
    Log("[patch] forced " + symbol_name + " -> " + std::string(value ? "1" : "0")
        + " at " + Hex32(address));
    return true;
}

void Emulator::ApplyGuestLogicVersionPatches() {
    ApplyGuestForcedBoolReturn(kGuestLogicVersionIsDev, true, "LogicVersion::isDev");
    ApplyGuestForcedBoolReturn(kGuestLogicVersionIsProd, false, "LogicVersion::isProd");
}

void Emulator::ApplyGuestLogicDefinesPatches() {
    ApplyGuestForcedBoolReturn(kGuestLogicDefinesIsPlatformAndroid, true, "LogicDefines::isPlatformAndroid");
    ApplyGuestForcedBoolReturn(kGuestLogicDefinesIsPlatformIOS, false, "LogicDefines::isPlatformIOS");
    ApplyGuestForcedBoolReturn(kGameSettingsIsMusicEnabled, false, "GameSettings::isMusicEnabled");
}

std::filesystem::path Emulator::IapWarningStatePath() const {
    return sandbox_root_ / "Library" / "Preferences" / "iap-warning-accepted.flag";
}

bool Emulator::HasPersistedIapWarningSuppression() const {
    const auto path = IapWarningStatePath();
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
}

void Emulator::PersistIapWarningSuppression() const {
    const auto path = IapWarningStatePath();
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        Log("[iap] failed to persist warning suppression marker at " + path.string());
        return;
    }
    output << "accepted\n";
    output.flush();
    if (!output) {
        Log("[iap] failed to flush warning suppression marker at " + path.string());
        return;
    }
    Log("[iap] persisted warning suppression marker at " + path.string());
}

bool Emulator::ApplyGuestIapWarningSuppression(const std::string& reason) {
    const u16 original0 = memory_.Read16(kGuestGameMainIsIapWarningNeeded);
    const u16 original1 = memory_.Read16(kGuestGameMainIsIapWarningNeeded + 2);
    if (original0 == kThumbMovsR0Zero && original1 == kThumbBxLr) {
        iap_warning_suppressed_ = true;
        return true;
    }
    if (original0 != 0xB590) {
        Log("[iap] suppression skipped: unexpected prologue at "
            + Hex32(kGuestGameMainIsIapWarningNeeded)
            + " first=" + Hex32(original0)
            + " second=" + Hex32(original1)
            + " reason=" + reason);
        return false;
    }

    memory_.SetWriteProtection(false);
    memory_.Write16(kGuestGameMainIsIapWarningNeeded, kThumbMovsR0Zero);
    memory_.Write16(kGuestGameMainIsIapWarningNeeded + 2, kThumbBxLr);
    memory_.SetWriteProtection(true);
    cpu_->InvalidateCacheRange(kGuestGameMainIsIapWarningNeeded, 4);
    iap_warning_suppressed_ = true;

    Log("[iap] patched GameMain::IsIAPWarningNeeded " + reason + " at "
        + Hex32(kGuestGameMainIsIapWarningNeeded));
    return true;
}

void Emulator::SuppressGuestIapWarning(const HostObject& popup, const s32 button_index) {
    if (iap_warning_suppressed_ || popup.class_name != "UIAlertView" || button_index < 0) {
        return;
    }

    const auto title_it = popup.dict.find("title");
    const auto message_it = popup.dict.find("message");
    const std::string title = title_it == popup.dict.end() ? "" : DecodeNSString(title_it->second).value_or("");
    const std::string message = message_it == popup.dict.end() ? "" : DecodeNSString(message_it->second).value_or("");
    if (title != "In-App purchase notification" || message != "This application includes in-app purchases.") {
        return;
    }
    if (!ApplyGuestIapWarningSuppression("after popup accept")) {
        return;
    }
    PersistIapWarningSuppression();
}

void Emulator::ObserveObjcKeyboardMessage(const u32 receiver, const std::string& selector_name) {
    if (receiver == 0 || objc_abi_ == nullptr) {
        return;
    }

    std::string class_name = objc_abi_->ClassNameForReceiver(receiver).value_or("");
    if (class_name.empty()) {
        if (const auto it = host_objects_.find(receiver); it != host_objects_.end()) {
            class_name = it->second.class_name;
        }
    }
    if (class_name != "KeyInput") {
        return;
    }

    if (selector_name == "show") {
        runtime_->keyboard_responder = receiver;
        Log("[keyboard] responder receiver=" + Hex32(receiver) + " selector=show");
        return;
    }

    if (selector_name == "becomeFirstResponder") {
        runtime_->keyboard_responder = receiver;
        if (!runtime_->keyboard_visible) {
            runtime_->keyboard_visible = true;
            Log("[keyboard] show receiver=" + Hex32(receiver) + " selector=becomeFirstResponder");
            RequestHostKeyboardVisibility(true);
        }
        return;
    }

    if (selector_name == "resignFirstResponder") {
        if (runtime_->keyboard_responder == receiver) {
            runtime_->keyboard_responder = 0;
        }
        if (runtime_->keyboard_visible) {
            runtime_->keyboard_visible = false;
            Log("[keyboard] hide receiver=" + Hex32(receiver));
            RequestHostKeyboardVisibility(false);
        }
    }
}

bool Emulator::FireDisplayLinks() {
    std::vector<u32> active_links;
    active_links.reserve(runtime_->display_links.size());
    std::size_t paused_links = 0;
    std::size_t inactive_links = 0;
    for (const auto& [object, link] : runtime_->display_links) {
        if (link.active && !link.paused) {
            active_links.push_back(object);
        } else if (link.active && link.paused) {
            ++paused_links;
        } else {
            ++inactive_links;
        }
    }
    if (active_links.empty()) {
        if (runtime_->debug_display_link_logs < 64) {
            ++runtime_->debug_display_link_logs;
            Log("[displaylink] idle no active links total=" + std::to_string(runtime_->display_links.size())
                + " paused=" + std::to_string(paused_links)
                + " inactive=" + std::to_string(inactive_links)
                + " queued_callbacks=" + std::to_string(queued_objc_callbacks_.size()));
        }
        // A UIKit run loop does not terminate when CADisplayLink is temporarily
        // paused/removed. Keep the host loop alive so later events can resume it.
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        return true;
    }

    for (const u32 object : active_links) {
        auto it = runtime_->display_links.find(object);
        if (it == runtime_->display_links.end()) {
            continue;
        }
        RuntimeState::DisplayLink& link = it->second;
        if (!link.active || link.paused) {
            continue;
        }
        if (++link.frame_counter < std::max(link.frame_interval, 1)) {
            continue;
        }
        link.frame_counter = 0;
        const std::string selector_name = ReadGuestCString(link.selector);
        if (selector_name.empty()) {
            continue;
        }
        if (trace_shims_) {
            Log("CADisplayLink fire target=" + Hex32(link.target)
                + " selector=" + selector_name
                + " link=" + Hex32(object));
        }
        const std::array<u32, 1> callback_args{object};
        const ObjcCallbackResult result = RunObjcCallback(link.target, selector_name, callback_args);
        if (result == ObjcCallbackResult::Stopped) {
            return false;
        }
        if (result == ObjcCallbackResult::Yielded) {
            return true;
        }
        if (saw_exit_ || total_ticks_left_ == 0) {
            return false;
        }
    }
    return true;
}

Emulator::ObjcCallbackResult Emulator::ResumeObjcCallback() {
    if (!active_objc_callback_.has_value()) {
        return ObjcCallbackResult::Completed;
    }

    if (!BeginTickSlice()) {
        Log("tick budget exhausted during objc callback " + active_objc_callback_->selector_name
            + " at " + Hex32(cpu_->Regs()[15]));
        DumpHotPcSamples();
        active_objc_callback_.reset();
        return ObjcCallbackResult::Stopped;
    }

    const Dynarmic::HaltReason halt = cpu_->Run();
    if (returned_from_guest_ || Dynarmic::Has(halt, Dynarmic::HaltReason::UserDefined1)) {
        active_objc_callback_.reset();
        return ObjcCallbackResult::Completed;
    }
    if (Dynarmic::Has(halt, Dynarmic::HaltReason::UserDefined2)) {
        active_objc_callback_.reset();
        return ObjcCallbackResult::Stopped;
    }
    if (ticks_left_ == 0) {
        RecordHotPcSample();
        const u32 pc = cpu_->Regs()[15] & ~1u;
        if (pc == active_objc_callback_->last_pc) {
            active_objc_callback_->same_pc_yields += 1;
        } else {
            active_objc_callback_->last_pc = pc;
            active_objc_callback_->same_pc_yields = 1;
        }
        if (total_ticks_left_ == 0) {
            Log("tick budget exhausted during objc callback " + active_objc_callback_->selector_name
                + " at " + Hex32(cpu_->Regs()[15])
                + " halt=" + DescribeHaltReason(halt));
            DumpHotPcSamples();
            active_objc_callback_.reset();
            return ObjcCallbackResult::Stopped;
        }
        active_objc_callback_->yield_count += 1;
        if (active_objc_callback_->yield_count <= 8u
            || (active_objc_callback_->yield_count % 32u) == 0u
            || (active_objc_callback_->same_pc_yields >= 64u && (active_objc_callback_->same_pc_yields % 64u) == 0u)) {
            const u32 cpsr = cpu_->Cpsr();
            const bool thumb = (cpsr & (1u << 5)) != 0;
            auto read_code_unit = [&](const u32 address) -> std::string {
                if (!memory_.IsMapped(address)) {
                    return "??";
                }
                if (thumb) {
                    std::ostringstream out;
                    out << "0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
                        << memory_.Read16(address);
                    return out.str();
                }
                if (!memory_.IsMapped(address + 3)) {
                    return "????????";
                }
                return Hex32(memory_.Read32(address));
            };
            Log("objc callback still running " + active_objc_callback_->selector_name
                + " pc=" + Hex32(cpu_->Regs()[15])
                + " lr=" + Hex32(cpu_->Regs()[14])
                + " sp=" + Hex32(cpu_->Regs()[13])
                + " r0=" + Hex32(cpu_->Regs()[0])
                + " r1=" + Hex32(cpu_->Regs()[1])
                + " cpsr=" + Hex32(cpsr)
                + " thumb=" + std::to_string(thumb ? 1 : 0)
                + " code=[" + read_code_unit(pc)
                + "," + read_code_unit(pc + (thumb ? 2u : 4u))
                + "," + read_code_unit(pc + (thumb ? 4u : 8u))
                + "," + read_code_unit(pc + (thumb ? 6u : 12u)) + "]"
                + " yields=" + std::to_string(active_objc_callback_->yield_count)
                + " same_pc=" + std::to_string(active_objc_callback_->same_pc_yields)
                + " queued_callbacks=" + std::to_string(queued_objc_callbacks_.size())
                + " total_ticks_left=" + std::to_string(total_ticks_left_));
        }
        return ObjcCallbackResult::Yielded;
    }

    std::string import_name_suffix;
    if (const auto it = import_name_by_stub_.find(cpu_->Regs()[15] & ~1u); it != import_name_by_stub_.end()) {
        import_name_suffix = " import=" + it->second;
    }
    throw std::runtime_error("guest callback halted unexpectedly at " + Hex32(cpu_->Regs()[15])
        + " halt=" + DescribeHaltReason(halt)
        + import_name_suffix);
}

Emulator::ObjcCallbackResult Emulator::RunObjcCallback(const u32 receiver, const std::string& selector_name, const std::span<const u32> args) {
    EnsureGuestAnnotation(receiver);
    ObserveObjcKeyboardMessage(receiver, selector_name);

    u32 override_result = 0;
    if (TryHandleObjcOverride(receiver, selector_name, &override_result)) {
        return saw_exit_ ? ObjcCallbackResult::Stopped : ObjcCallbackResult::Completed;
    }

    const u32 selector = AllocateCString(selector_name, "selector");
    cpu_->Regs()[0] = receiver;
    cpu_->Regs()[1] = selector;
    std::size_t index = 0;
    for (const u32 arg : args) {
        if (index + 2 < cpu_->Regs().size()) {
            cpu_->Regs()[index + 2] = arg;
        }
        ++index;
    }

    if (objc_abi_ != nullptr) {
        if (const auto imp = objc_abi_->LookupMethodImp(receiver, selector_name, false, 0)) {
            if (trace_shims_) {
                const std::string class_name = objc_abi_->ClassNameForReceiver(receiver).value_or("NSObject");
                Log("objc guest " + Hex32(receiver) + " [" + class_name + " " + selector_name + "] -> " + Hex32(*imp));
            }
            returned_from_guest_ = false;
            cpu_->Regs()[14] = return_stub_;
            cpu_->Regs()[15] = *imp & ~1u;
            cpu_->SetCpsr((*imp & 1u) != 0 ? 0x30 : 0x10);
            active_objc_callback_ = PendingObjcCallback{receiver, selector_name, std::vector<u32>(args.begin(), args.end())};
            return ResumeObjcCallback();
        }
    }

    DispatchObjCMessage(receiver, selector, false, false, 0);
    return saw_exit_ ? ObjcCallbackResult::Stopped : ObjcCallbackResult::Completed;
}

u32 Emulator::DispatchObjCMessage(u32 self, const u32 selector, const bool is_super, const bool stret, const u32 stret_buffer) {
    if (is_super) {
        self = memory_.Read32(self);
    }

    const std::string selector_name = ReadGuestCString(selector);
    if (selector_name.empty()) {
        return 0;
    }
    ObserveObjcKeyboardMessage(self, selector_name);

    auto object_it = host_objects_.find(self);
    bool has_object = object_it != host_objects_.end();
    const std::optional<std::string> guest_class_name =
        !has_object && objc_abi_ != nullptr ? objc_abi_->ClassNameForReceiver(self) : std::nullopt;
    const bool is_class = has_object && (object_it->second.kind == ObjKind::Class || object_it->second.kind == ObjKind::MetaClass);
    std::string class_name = has_object ? object_it->second.class_name : guest_class_name.value_or("NSObject");
    auto is_eagl_view_class = [](const std::string& name) {
        return name == "EAGLView"
            || name == "EAGLTouchScreenView"
            || (name.find("EAGL") != std::string::npos && name.find("View") != std::string::npos);
    };
    auto host_class_matches = [](const std::string& lhs, const std::string& rhs) {
        if (lhs == rhs) {
            return true;
        }
        if (rhs == "NSObject") {
            return true;
        }
        if ((lhs == "NSMutableDictionary" || lhs == "NSUserDefaults") && rhs == "NSDictionary") {
            return true;
        }
        if (lhs == "NSMutableArray" && rhs == "NSArray") {
            return true;
        }
        if (lhs == "NSMutableString" && rhs == "NSString") {
            return true;
        }
        return false;
    };
    auto is_kind_of_named_class = [&](const std::string& target) {
        if (class_name == target) {
            return true;
        }
        if (has_object) {
            return host_class_matches(class_name, target);
        }
        if (!kDisableHostTextRenderingProbe
            && target == "UILabel"
            && class_name.find("Label") != std::string::npos) {
            return true;
        }
        if (objc_abi_ != nullptr) {
            if (const auto target_class = objc_abi_->FindClassByName(target)) {
                return objc_abi_->IsKindOfClass(self, *target_class);
            }
        }
        return false;
    };

    if (trace_shims_) {
        Log("objc " + Hex32(self) + " [" + class_name + " " + selector_name + "]");
    }

    auto make_instance = [this](const std::string& name, const ObjKind kind) {
        if (kind == ObjKind::Generic && objc_abi_ != nullptr) {
            if (const auto guest_class = objc_abi_->FindClassByName(name)) {
                return objc_abi_->AllocateInstance(*guest_class, name);
            }
        }
        const u32 object = AllocateData(kObjcObjectSize, 4, name);
        host_objects_[object] = HostObject{
            .kind = kind,
            .class_name = name,
            .isa = EnsureClass(name)
        };
        memory_.Write32(object, EnsureClass(name));
        return object;
    };
    auto default_kind_for_class = [](const std::string& name) {
        if (name == "NSArray" || name == "NSMutableArray") {
            return ObjKind::Array;
        }
        if (name == "NSDictionary" || name == "NSMutableDictionary" || name == "NSUserDefaults") {
            return ObjKind::Dictionary;
        }
        if (name == "NSData" || name == "NSMutableData") {
            return ObjKind::Data;
        }
        if (name == "NSString" || name == "NSMutableString") {
            return ObjKind::String;
        }
        if (name == "NSNumber") {
            return ObjKind::Number;
        }
        return ObjKind::Generic;
    };
    auto ensure_singleton = [&](u32& slot, const std::string& name, const ObjKind kind) {
        if (slot != 0 && host_objects_.count(slot) != 0) {
            return slot;
        }
        slot = make_instance(name, kind);
        return slot;
    };
    auto ensure_main_layer = [&]() {
        if (runtime_->main_layer != 0 && host_objects_.count(runtime_->main_layer) != 0) {
            return runtime_->main_layer;
        }
        runtime_->main_layer = make_instance("CAEAGLLayer", ObjKind::Generic);
        host_objects_[runtime_->main_layer].dict["drawable.scale.bits"] = FloatToBits(runtime_->screen_scale);
        host_objects_[runtime_->main_layer].dict["setBounds:.w.bits"] = FloatToBits(static_cast<float>(runtime_->screen_width_points));
        host_objects_[runtime_->main_layer].dict["setBounds:.h.bits"] = FloatToBits(static_cast<float>(runtime_->screen_height_points));
        return runtime_->main_layer;
    };
    auto write_rect = [this](const u32 address, const float x, const float y, const float width, const float height) {
        memory_.Write32(address + 0, FloatToBits(x));
        memory_.Write32(address + 4, FloatToBits(y));
        memory_.Write32(address + 8, FloatToBits(width));
        memory_.Write32(address + 12, FloatToBits(height));
    };
    auto get_rect_component = [](const HostObject& object, const std::string& key, const float fallback) {
        const auto it = object.dict.find(key);
        return it == object.dict.end() ? fallback : BitsToFloat(it->second);
    };
    auto get_rect = [&](const HostObject& object, const std::string& selector_prefix, const float fallback_width, const float fallback_height) {
        return std::array<float, 4>{
            get_rect_component(object, selector_prefix + ".x.bits", 0.0f),
            get_rect_component(object, selector_prefix + ".y.bits", 0.0f),
            get_rect_component(object, selector_prefix + ".w.bits", fallback_width),
            get_rect_component(object, selector_prefix + ".h.bits", fallback_height),
        };
    };
    auto property_key_for_getter = [](const std::string& getter) {
        if (getter.empty()) {
            return std::string("set:");
        }
        std::string key = "set";
        key.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(getter.front()))));
        key.append(getter.substr(1));
        key.push_back(':');
        return key;
    };
    auto ensure_drawable_ids = [&]() {
        if (runtime_->drawable_framebuffer == 0) {
            runtime_->drawable_framebuffer = next_gl_name_++;
            runtime_->gl_framebuffers[runtime_->drawable_framebuffer] = {};
        }
        if (runtime_->drawable_renderbuffer == 0) {
            runtime_->drawable_renderbuffer = next_gl_name_++;
            runtime_->gl_renderbuffers[runtime_->drawable_renderbuffer] = {};
        }
    };
    auto ensure_gl_context = [&]() {
        if (runtime_->current_eagl_context != 0 && host_objects_.count(runtime_->current_eagl_context) != 0) {
            return runtime_->current_eagl_context;
        }
        const u32 context = make_instance("EAGLContext", ObjKind::Generic);
        host_objects_[context].dict["eagl.api"] = 2;
        runtime_->current_eagl_context = context;
        if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported()) {
            runtime_->host_gl->EnsureWindow(
                static_cast<int>(runtime_->screen_width_points),
                static_cast<int>(runtime_->screen_height_points),
                binary_path_.filename().string());
            runtime_->host_gl->MakeCurrent();
        }
        return context;
    };
    auto ensure_main_gl_view = [&]() {
        if (runtime_->main_gl_view != 0) {
            EnsureGuestAnnotation(runtime_->main_gl_view);
            if (host_objects_.count(runtime_->main_gl_view) != 0
                || (objc_abi_ != nullptr && objc_abi_->IsGuestObject(runtime_->main_gl_view))) {
                return runtime_->main_gl_view;
            }
        }
        runtime_->main_gl_view = make_instance("EAGLView", ObjKind::Generic);
        HostObject& gl_view = host_objects_[runtime_->main_gl_view];
        gl_view.dict["setFrame:.w.bits"] = FloatToBits(static_cast<float>(runtime_->screen_width_points));
        gl_view.dict["setFrame:.h.bits"] = FloatToBits(static_cast<float>(runtime_->screen_height_points));
        gl_view.dict["setBounds:.w.bits"] = FloatToBits(static_cast<float>(runtime_->screen_width_points));
        gl_view.dict["setBounds:.h.bits"] = FloatToBits(static_cast<float>(runtime_->screen_height_points));
        gl_view.dict["setAnimationFrameInterval:"] = 1;
        gl_view.dict["setDisplayLinkSupported:"] = 1;
        gl_view.dict["setDisplayLinkInUse:"] = 1;
        gl_view.dict["setAutoresizesSurface:"] = 1;
        gl_view.dict["setContext:"] = ensure_gl_context();
        gl_view.dict["setDelegate:"] = runtime_->main_delegate;
        ensure_drawable_ids();
        if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported()) {
            runtime_->host_gl->EnsureWindow(
                static_cast<int>(runtime_->screen_width_points),
                static_cast<int>(runtime_->screen_height_points),
                binary_path_.filename().string());
            runtime_->host_gl->MakeCurrent();
        }
        return runtime_->main_gl_view;
    };
    auto ensure_main_view_controller = [&]() {
        if (runtime_->main_view_controller != 0 && host_objects_.count(runtime_->main_view_controller) != 0) {
            return runtime_->main_view_controller;
        }
        runtime_->main_view_controller = make_instance("GameViewController", ObjKind::Generic);
        HostObject& controller = host_objects_[runtime_->main_view_controller];
        const u32 gl_view = ensure_main_gl_view();
        controller.dict["setGlView:"] = gl_view;
        controller.dict["setView:"] = gl_view;
        return runtime_->main_view_controller;
    };
    auto ensure_main_window = [&]() {
        if (runtime_->main_window != 0 && host_objects_.count(runtime_->main_window) != 0) {
            return runtime_->main_window;
        }
        runtime_->main_window = make_instance("UIWindow", ObjKind::Generic);
        HostObject& window = host_objects_[runtime_->main_window];
        window.dict["setFrame:.w.bits"] = FloatToBits(static_cast<float>(runtime_->screen_width_points));
        window.dict["setFrame:.h.bits"] = FloatToBits(static_cast<float>(runtime_->screen_height_points));
        window.dict["setBounds:.w.bits"] = FloatToBits(static_cast<float>(runtime_->screen_width_points));
        window.dict["setBounds:.h.bits"] = FloatToBits(static_cast<float>(runtime_->screen_height_points));
        window.dict["setRootViewController:"] = ensure_main_view_controller();
        return runtime_->main_window;
    };
    auto load_guest_file_bytes = [&](const std::string& guest_path) -> std::optional<std::vector<u8>> {
        const auto host_path = ResolveGuestPath(guest_path);
        const bool is_music_path = guest_path.find("music/") != std::string::npos
            || (guest_path.size() >= 4 && guest_path.compare(guest_path.size() - 4, 4, ".mp3") == 0);
        if (is_music_path) {
            Log("objc file load guest=" + guest_path + " host=" + host_path.string());
        }
        auto data = ReadGuestFileBytes(guest_path);
        if (!data) {
            if (is_music_path) {
                Log("objc file load failed guest=" + guest_path + " host=" + host_path.string());
            }
            return std::nullopt;
        }
        if (is_music_path) {
            Log("objc file load bytes=" + std::to_string(data->size()));
        }
        return data;
    };
    auto ensure_cgcolor = [&](const std::array<float, 4>& rgba) {
        const u32 color = make_instance("CGColor", ObjKind::Generic);
        runtime_->graphics_images[color].components.assign(rgba.begin(), rgba.end());
        return color;
    };
    auto ensure_uicolor = [&](const std::array<float, 4>& rgba) {
        const u32 color = make_instance("UIColor", ObjKind::Generic);
        host_objects_[color].dict["CGColor"] = ensure_cgcolor(rgba);
        return color;
    };
    std::function<std::optional<std::array<float, 4>>(u32)> color_components_for_handle =
        [&](const u32 color_handle) -> std::optional<std::array<float, 4>> {
            if (color_handle == 0) {
                return std::nullopt;
            }
            if (const auto image_it = runtime_->graphics_images.find(color_handle);
                image_it != runtime_->graphics_images.end() && image_it->second.components.size() >= 4) {
                return std::array<float, 4>{
                    image_it->second.components[0],
                    image_it->second.components[1],
                    image_it->second.components[2],
                    image_it->second.components[3],
                };
            }
            if (const auto object_it = host_objects_.find(color_handle); object_it != host_objects_.end()) {
                if (const auto cg_it = object_it->second.dict.find("CGColor"); cg_it != object_it->second.dict.end()) {
                    return color_components_for_handle(cg_it->second);
                }
            }
            return std::nullopt;
        };
    auto current_graphics_context = [&]() -> u32 {
        return runtime_->graphics_context_stack.empty() ? 0u : runtime_->graphics_context_stack.back();
    };
    auto blend_pixel = [](u8* dst, const std::array<float, 4>& color, const float alpha_scale) {
        const float src_alpha = std::clamp(color[3], 0.0f, 1.0f) * std::clamp(alpha_scale, 0.0f, 1.0f);
        if (src_alpha <= 0.0f) {
            return;
        }
        const float dst_alpha = static_cast<float>(dst[3]) / 255.0f;
        const float out_alpha = src_alpha + dst_alpha * (1.0f - src_alpha);
        if (out_alpha <= 0.0f) {
            dst[0] = 0;
            dst[1] = 0;
            dst[2] = 0;
            dst[3] = 0;
            return;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            const float src_channel = std::clamp(color[i], 0.0f, 1.0f);
            const float dst_channel = static_cast<float>(dst[i]) / 255.0f;
            const float out_channel =
                (src_channel * src_alpha + dst_channel * dst_alpha * (1.0f - src_alpha)) / out_alpha;
            dst[i] = static_cast<u8>(std::clamp(out_channel, 0.0f, 1.0f) * 255.0f);
        }
        dst[3] = static_cast<u8>(std::clamp(out_alpha, 0.0f, 1.0f) * 255.0f);
    };
    auto font_size_for_handle = [&](const u32 font_handle) {
        const auto it = host_objects_.find(font_handle);
        if (it != host_objects_.end() && it->second.number_value > 0.0) {
            return static_cast<float>(it->second.number_value);
        }
        return 16.0f;
    };
    auto looks_like_font_object = [](const HostObject& object) {
        return object.class_name.find("Font") != std::string::npos
            || object.class_name == "CGDataProvider";
    };
    auto fallback_font_handle = [&]() -> u32 {
        for (const auto& [handle, font] : runtime_->raster_fonts) {
            if (font.initialized) {
                return handle;
            }
        }
        for (const auto& [handle, object] : host_objects_) {
            if (looks_like_font_object(object) && !object.bytes.empty()) {
                return handle;
            }
            if (object.backing_store != 0) {
                const auto backing_it = host_objects_.find(object.backing_store);
                if (backing_it != host_objects_.end()
                    && looks_like_font_object(*&backing_it->second)
                    && !backing_it->second.bytes.empty()) {
                    return object.backing_store;
                }
            }
        }
        return 0;
    };
    auto resolve_font_bytes = [&](const u32 font_handle) -> const std::vector<u8>* {
        const auto try_handle = [&](const u32 handle) -> const std::vector<u8>* {
            const auto it = host_objects_.find(handle);
            if (it == host_objects_.end()) {
                return nullptr;
            }
            if (looks_like_font_object(it->second) && !it->second.bytes.empty()) {
                return &it->second.bytes;
            }
            if (it->second.backing_store != 0) {
                const auto backing_it = host_objects_.find(it->second.backing_store);
                if (backing_it != host_objects_.end()
                    && looks_like_font_object(backing_it->second)
                    && !backing_it->second.bytes.empty()) {
                    return &backing_it->second.bytes;
                }
            }
            return nullptr;
        };
        if (const std::vector<u8>* bytes = try_handle(font_handle)) {
            return bytes;
        }
        if (const u32 fallback = fallback_font_handle()) {
            return try_handle(fallback);
        }
        return nullptr;
    };
    auto ensure_raster_font = [&](const u32 requested_font_handle) -> std::pair<u32, RuntimeState::RasterFont*> {
        const u32 resolved_font_handle = requested_font_handle != 0 ? requested_font_handle : fallback_font_handle();
        if (resolved_font_handle == 0) {
            return {0, nullptr};
        }
        if (auto it = runtime_->raster_fonts.find(resolved_font_handle);
            it != runtime_->raster_fonts.end() && it->second.initialized) {
            return {resolved_font_handle, &it->second};
        }
        const std::vector<u8>* bytes = resolve_font_bytes(resolved_font_handle);
        if (bytes == nullptr || bytes->empty()) {
            const u32 fallback = fallback_font_handle();
            if (fallback == 0 || fallback == resolved_font_handle) {
                return {resolved_font_handle, nullptr};
            }
            if (auto it = runtime_->raster_fonts.find(fallback);
                it != runtime_->raster_fonts.end() && it->second.initialized) {
                return {fallback, &it->second};
            }
            bytes = resolve_font_bytes(fallback);
            if (bytes == nullptr || bytes->empty()) {
                return {fallback, nullptr};
            }
            RuntimeState::RasterFont raster_font;
            raster_font.bytes = *bytes;
            const int font_offset = stbtt_GetFontOffsetForIndex(raster_font.bytes.data(), 0);
            if (font_offset < 0 || stbtt_InitFont(&raster_font.info, raster_font.bytes.data(), font_offset) == 0) {
                return {fallback, nullptr};
            }
            stbtt_GetFontVMetrics(&raster_font.info, &raster_font.ascent, &raster_font.descent, &raster_font.line_gap);
            raster_font.initialized = true;
            auto [it, _] = runtime_->raster_fonts.insert_or_assign(fallback, std::move(raster_font));
            return {fallback, &it->second};
        }
        RuntimeState::RasterFont raster_font;
        raster_font.bytes = *bytes;
        const int font_offset = stbtt_GetFontOffsetForIndex(raster_font.bytes.data(), 0);
        if (font_offset < 0 || stbtt_InitFont(&raster_font.info, raster_font.bytes.data(), font_offset) == 0) {
            return {resolved_font_handle, nullptr};
        }
        stbtt_GetFontVMetrics(&raster_font.info, &raster_font.ascent, &raster_font.descent, &raster_font.line_gap);
        raster_font.initialized = true;
        auto [it, _] = runtime_->raster_fonts.insert_or_assign(resolved_font_handle, std::move(raster_font));
        return {resolved_font_handle, &it->second};
    };
    auto measure_text = [&](const std::string& text, const u32 font_handle, const float max_width) {
        struct Measurement {
            float width = 0.0f;
            float height = 0.0f;
            float ascent = 0.0f;
            float line_height = 0.0f;
        };
        Measurement measurement;
        const float font_size = font_size_for_handle(font_handle);
        const auto [resolved_font_handle, raster_font] = ensure_raster_font(font_handle);
        (void)resolved_font_handle;
        const float scale = raster_font != nullptr ? stbtt_ScaleForPixelHeight(&raster_font->info, font_size) : 0.0f;
        const float ascent = raster_font != nullptr ? raster_font->ascent * scale : font_size * 0.8f;
        const float descent = raster_font != nullptr ? std::abs(raster_font->descent) * scale : font_size * 0.2f;
        const float leading = raster_font != nullptr ? std::max(0.0f, raster_font->line_gap * scale) : font_size * 0.1f;
        const float line_height = std::max(1.0f, ascent + descent + leading);
        measurement.ascent = ascent;
        measurement.line_height = line_height;

        const float width_limit = max_width > 0.0f ? max_width : std::numeric_limits<float>::max();
        float pen_x = 0.0f;
        float max_line_width = 0.0f;
        u32 line_count = 1;
        for (const unsigned char c : text) {
            if (c == '\n') {
                max_line_width = std::max(max_line_width, pen_x);
                pen_x = 0.0f;
                ++line_count;
                continue;
            }
            float advance = font_size * 0.6f;
            if (raster_font != nullptr) {
                const int glyph_index = stbtt_FindGlyphIndex(&raster_font->info, static_cast<int>(c));
                int advance_width = 0;
                int left_side_bearing = 0;
                stbtt_GetGlyphHMetrics(&raster_font->info, glyph_index, &advance_width, &left_side_bearing);
                (void)left_side_bearing;
                advance = advance_width * scale;
            }
            if (pen_x > 0.0f && pen_x + advance > width_limit) {
                max_line_width = std::max(max_line_width, pen_x);
                pen_x = 0.0f;
                ++line_count;
            }
            pen_x += advance;
        }
        measurement.width = std::max(max_line_width, pen_x);
        measurement.height = std::max(1u, line_count) * line_height;
        return measurement;
    };
    auto draw_text_into_context = [&](const u32 context_handle,
                                      const std::string& text,
                                      const u32 font_handle,
                                      const std::array<float, 4>& rect,
                                      const std::optional<std::array<float, 4>>& color_override) {
        if (kDisableHostTextRenderingProbe) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                Log("[text] host UILabel/CoreGraphics text rendering disabled for probe");
            }
            return true;
        }
        auto ctx_it = runtime_->graphics_contexts.find(context_handle);
        if (ctx_it == runtime_->graphics_contexts.end()) {
            return false;
        }
        RuntimeState::GraphicsContext& context = ctx_it->second;
        if (context.data == 0 || context.width <= 0 || context.height <= 0 || context.bytes_per_row <= 0 || text.empty()) {
            return false;
        }
        const float font_size = font_size_for_handle(font_handle);
        const auto [resolved_font_handle, raster_font] = ensure_raster_font(font_handle);
        (void)resolved_font_handle;
        if (raster_font == nullptr) {
            return false;
        }
        auto pixels = memory_.ReadBuffer(
            context.data,
            static_cast<std::size_t>(context.bytes_per_row) * static_cast<std::size_t>(context.height));
        const float scale = stbtt_ScaleForPixelHeight(&raster_font->info, font_size);
        const float ascent = raster_font->ascent * scale;
        const float descent = std::abs(raster_font->descent) * scale;
        const float leading = std::max(0.0f, raster_font->line_gap * scale);
        const float line_height = std::max(1.0f, ascent + descent + leading);
        const float width_limit = rect[2] > 0.0f ? rect[2] : std::numeric_limits<float>::max();
        const float max_y = rect[3] > 0.0f ? rect[1] + rect[3] : std::numeric_limits<float>::max();
        const std::array<float, 4> draw_color = color_override.value_or(context.fill_color);

        float pen_x = rect[0];
        float baseline_y = rect[1] + ascent;
        for (const unsigned char c : text) {
            if (c == '\n') {
                pen_x = rect[0];
                baseline_y += line_height;
                if (baseline_y - ascent > max_y) {
                    break;
                }
                continue;
            }

            const int glyph_index = stbtt_FindGlyphIndex(&raster_font->info, static_cast<int>(c));
            int advance_width = 0;
            int left_side_bearing = 0;
            stbtt_GetGlyphHMetrics(&raster_font->info, glyph_index, &advance_width, &left_side_bearing);
            (void)left_side_bearing;
            const float advance = advance_width * scale;
            if (pen_x > rect[0] && pen_x + advance > rect[0] + width_limit) {
                pen_x = rect[0];
                baseline_y += line_height;
                if (baseline_y - ascent > max_y) {
                    break;
                }
            }

            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            stbtt_GetGlyphBitmapBox(
                &raster_font->info,
                glyph_index,
                scale,
                scale,
                &x0,
                &y0,
                &x1,
                &y1);
            const int glyph_width = x1 - x0;
            const int glyph_height = y1 - y0;
            if (glyph_width > 0 && glyph_height > 0) {
                std::vector<u8> bitmap(static_cast<std::size_t>(glyph_width) * static_cast<std::size_t>(glyph_height), 0);
                stbtt_MakeGlyphBitmap(
                    &raster_font->info,
                    bitmap.data(),
                    glyph_width,
                    glyph_height,
                    glyph_width,
                    scale,
                    scale,
                    glyph_index);
                const s32 dst_x = static_cast<s32>(std::floor(pen_x)) + x0;
                const s32 dst_y = static_cast<s32>(std::floor(baseline_y)) + y0;
                for (int row = 0; row < glyph_height; ++row) {
                    const s32 py = dst_y + row;
                    if (py < 0 || py >= context.height) {
                        continue;
                    }
                    for (int col = 0; col < glyph_width; ++col) {
                        const s32 px = dst_x + col;
                        if (px < 0 || px >= context.width) {
                            continue;
                        }
                        const u8 coverage = bitmap[static_cast<std::size_t>(row) * static_cast<std::size_t>(glyph_width)
                            + static_cast<std::size_t>(col)];
                        if (coverage == 0) {
                            continue;
                        }
                        const std::size_t offset =
                            static_cast<std::size_t>(py) * static_cast<std::size_t>(context.bytes_per_row)
                            + static_cast<std::size_t>(px) * 4;
                        if (offset + 3 >= pixels.size()) {
                            continue;
                        }
                        blend_pixel(pixels.data() + offset, draw_color, static_cast<float>(coverage) / 255.0f);
                    }
                }
            }
            pen_x += advance;
        }

        memory_.WriteBuffer(context.data, pixels);
        if (runtime_->debug_text_draw_logs < 32) {
            ++runtime_->debug_text_draw_logs;
            Log("[text] drawText ctx=" + Hex32(context_handle)
                + " font=" + Hex32(font_handle)
                + " len=" + std::to_string(text.size())
                + " rect=(" + std::to_string(rect[0]) + "," + std::to_string(rect[1]) + ","
                + std::to_string(rect[2]) + "," + std::to_string(rect[3]) + ")"
                + " color=(" + std::to_string(draw_color[0]) + "," + std::to_string(draw_color[1]) + ","
                + std::to_string(draw_color[2]) + "," + std::to_string(draw_color[3]) + ")");
        }
        return true;
    };
    auto draw_image_into_context = [&](const u32 context_handle,
                                       const u32 image_handle,
                                       const std::array<float, 4>& rect,
                                       const float alpha) {
        auto ctx_it = runtime_->graphics_contexts.find(context_handle);
        if (ctx_it == runtime_->graphics_contexts.end() || ctx_it->second.data == 0) {
            return false;
        }
        u32 resolved_image_handle = image_handle;
        if (const auto object_it = host_objects_.find(image_handle); object_it != host_objects_.end()) {
            if (const auto cg_it = object_it->second.dict.find("CGImage"); cg_it != object_it->second.dict.end()) {
                resolved_image_handle = cg_it->second;
            } else if (object_it->second.backing_store != 0) {
                resolved_image_handle = object_it->second.backing_store;
            }
        }
        const auto image_it = runtime_->graphics_images.find(resolved_image_handle);
        if (image_it == runtime_->graphics_images.end() || image_it->second.width <= 0 || image_it->second.height <= 0) {
            return false;
        }
        if (image_it->second.skip_draw) {
            if (runtime_->debug_image_draw_logs < 48) {
                ++runtime_->debug_image_draw_logs;
                Log("[text] skip disabled text image draw image=" + Hex32(resolved_image_handle)
                    + " dst=(" + std::to_string(rect[0]) + "," + std::to_string(rect[1]) + ","
                    + std::to_string(rect[2]) + "," + std::to_string(rect[3]) + ")");
            }
            return true;
        }

        RuntimeState::GraphicsContext& context = ctx_it->second;
        auto pixels = memory_.ReadBuffer(
            context.data,
            static_cast<std::size_t>(context.bytes_per_row) * static_cast<std::size_t>(context.height));
        const s32 dst_x = static_cast<s32>(std::floor(rect[0]));
        const s32 dst_y = static_cast<s32>(std::floor(rect[1]));
        const s32 dst_w = std::max<s32>(1, static_cast<s32>(std::ceil(rect[2])));
        const s32 dst_h = std::max<s32>(1, static_cast<s32>(std::ceil(rect[3])));
        const std::size_t src_stride = static_cast<std::size_t>(
            image_it->second.bytes_per_row > 0
                ? image_it->second.bytes_per_row
                : image_it->second.width * 4);
        for (s32 y = 0; y < dst_h; ++y) {
            const s32 py = dst_y + y;
            if (py < 0 || py >= context.height) {
                continue;
            }
            const s32 src_y = std::clamp<s32>(
                static_cast<s32>((static_cast<long long>(y) * image_it->second.height) / dst_h),
                0,
                image_it->second.height - 1);
            for (s32 x = 0; x < dst_w; ++x) {
                const s32 px = dst_x + x;
                if (px < 0 || px >= context.width) {
                    continue;
                }
                const s32 src_x = std::clamp<s32>(
                    static_cast<s32>((static_cast<long long>(x) * image_it->second.width) / dst_w),
                    0,
                    image_it->second.width - 1);
                const std::size_t src_offset =
                    static_cast<std::size_t>(src_y) * src_stride
                    + static_cast<std::size_t>(src_x) * 4;
                const std::size_t dst_offset =
                    static_cast<std::size_t>(py) * static_cast<std::size_t>(context.bytes_per_row)
                    + static_cast<std::size_t>(px) * 4;
                if (src_offset + 3 >= image_it->second.pixels.size() || dst_offset + 3 >= pixels.size()) {
                    continue;
                }
                const std::array<float, 4> src_color{
                    static_cast<float>(image_it->second.pixels[src_offset + 0]) / 255.0f,
                    static_cast<float>(image_it->second.pixels[src_offset + 1]) / 255.0f,
                    static_cast<float>(image_it->second.pixels[src_offset + 2]) / 255.0f,
                    static_cast<float>(image_it->second.pixels[src_offset + 3]) / 255.0f,
                };
                blend_pixel(pixels.data() + dst_offset, src_color, alpha);
            }
        }
        memory_.WriteBuffer(context.data, pixels);
        if (runtime_->debug_image_draw_logs < 48) {
            ++runtime_->debug_image_draw_logs;
            Log("[text] drawImage ctx=" + Hex32(context_handle)
                + " image=" + Hex32(resolved_image_handle)
                + " src=" + std::to_string(image_it->second.width) + "x" + std::to_string(image_it->second.height)
                + " dst=(" + std::to_string(rect[0]) + "," + std::to_string(rect[1]) + ","
                + std::to_string(rect[2]) + "," + std::to_string(rect[3]) + ")"
                + " alpha=" + std::to_string(alpha));
        }
        return true;
    };
    const bool wants_string_shadow =
        !kDisableHostTextRenderingProbe
        &&
        !has_object
        && (selector_name == "sizeWithFont:"
            || selector_name == "sizeWithFont:constrainedToSize:"
            || selector_name == "sizeWithFont:constrainedToSize:lineBreakMode:")
        && DecodeNSString(self).has_value();
    if (!has_object && !is_class
        && ((!kDisableHostTextRenderingProbe && is_kind_of_named_class("UILabel"))
            || is_kind_of_named_class("UIImage")
            || (!kDisableHostTextRenderingProbe && is_kind_of_named_class("UIFont"))
            || is_kind_of_named_class("UIColor")
            || wants_string_shadow)) {
        if (kDisableHostTextRenderingProbe && runtime_->debug_text_draw_logs < 32) {
            ++runtime_->debug_text_draw_logs;
            Log("[text] shadow object class=" + class_name
                + " sel=" + selector_name
                + " self=" + Hex32(self));
        }
        host_objects_[self] = HostObject{
            .kind = wants_string_shadow ? ObjKind::String : ObjKind::Generic,
            .class_name = wants_string_shadow ? "NSString" : class_name,
            .isa = 0,
        };
        if (wants_string_shadow) {
            host_objects_[self].string_value = DecodeNSString(self).value_or("");
            class_name = "NSString";
        }
        object_it = host_objects_.find(self);
        has_object = true;
    }

    if (selector_name == "retain" || selector_name == "autorelease" || selector_name == "release"
        || selector_name == "dealloc" || selector_name == "init") {
        return self;
    }
    if (selector_name == "class") {
        if (objc_abi_ != nullptr && objc_abi_->IsGuestObject(self)) {
            return objc_abi_->IsaOf(self);
        }
        return is_class ? self : EnsureClass(class_name);
    }
    if (selector_name == "superclass") {
        if (objc_abi_ != nullptr) {
            const u32 superclass = objc_abi_->SuperclassOf(self);
            if (superclass != 0) {
                return superclass;
            }
        }
        return class_name == "NSObject" ? 0 : EnsureClass("NSObject");
    }
    auto host_class_is_kind_of = [&](const std::string& lhs, const std::string& rhs) {
        if (lhs == rhs) {
            return true;
        }
        if (rhs == "NSObject") {
            return true;
        }
        if ((lhs == "NSMutableDictionary" || lhs == "NSUserDefaults") && rhs == "NSDictionary") {
            return true;
        }
        if (lhs == "NSMutableArray" && rhs == "NSArray") {
            return true;
        }
        if (lhs == "NSMutableString" && rhs == "NSString") {
            return true;
        }
        return false;
    };

    if (selector_name == "respondsToSelector:" || selector_name == "isKindOfClass:" || selector_name == "isMemberOfClass:") {
        if (objc_abi_ != nullptr) {
            if (selector_name == "respondsToSelector:") {
                return objc_abi_->RespondsToSelector(self, ReadGuestCString(Arg(2)), false, 0) ? 1u : 0u;
            }
            if (selector_name == "isKindOfClass:") {
                return objc_abi_->IsKindOfClass(self, Arg(2)) ? 1u : 0u;
            }
            if (selector_name == "isMemberOfClass:") {
                return (objc_abi_->IsGuestObject(self) ? objc_abi_->IsaOf(self) : self) == Arg(2) ? 1u : 0u;
            }
        }
        if (selector_name == "respondsToSelector:") {
            return 1;
        }
        const auto class_it = host_objects_.find(Arg(2));
        const std::string rhs = class_it == host_objects_.end() ? "NSObject" : class_it->second.class_name;
        if (selector_name == "isMemberOfClass:") {
            return class_name == rhs ? 1u : 0u;
        }
        return host_class_is_kind_of(class_name, rhs) ? 1u : 0u;
    }
    if (selector_name == "hash") {
        return self;
    }
    if (selector_name == "description") {
        return EnsureNSString(DescribeNSObject(self));
    }

    if (is_class) {
        if (!kDisableHostTextRenderingProbe && class_name == "UIFont") {
            auto make_font = [&](const std::string& name, const float size) {
                const u32 font = make_instance("UIFont", ObjKind::Generic);
                HostObject& font_object = host_objects_[font];
                font_object.string_value = name;
                font_object.number_value = size > 0.0f ? size : 16.0f;
                return font;
            };
            if (selector_name == "systemFontOfSize:" || selector_name == "boldSystemFontOfSize:") {
                return make_font("Helvetica", BitsToFloat(Arg(2)));
            }
            if (selector_name == "fontWithName:size:") {
                return make_font(DecodeNSString(Arg(2)).value_or("Helvetica"), BitsToFloat(Arg(3)));
            }
        }
        if (class_name == "UIColor") {
            if (selector_name == "blackColor") {
                return ensure_uicolor({0.0f, 0.0f, 0.0f, 1.0f});
            }
            if (selector_name == "whiteColor") {
                return ensure_uicolor({1.0f, 1.0f, 1.0f, 1.0f});
            }
            if (selector_name == "clearColor") {
                return ensure_uicolor({0.0f, 0.0f, 0.0f, 0.0f});
            }
            if (selector_name == "redColor") {
                return ensure_uicolor({1.0f, 0.0f, 0.0f, 1.0f});
            }
            if (selector_name == "greenColor") {
                return ensure_uicolor({0.0f, 1.0f, 0.0f, 1.0f});
            }
            if (selector_name == "blueColor") {
                return ensure_uicolor({0.0f, 0.0f, 1.0f, 1.0f});
            }
            if (selector_name == "colorWithWhite:alpha:") {
                const float white = std::clamp(BitsToFloat(Arg(2)), 0.0f, 1.0f);
                const float alpha = std::clamp(BitsToFloat(Arg(3)), 0.0f, 1.0f);
                return ensure_uicolor({white, white, white, alpha});
            }
            if (selector_name == "colorWithRed:green:blue:alpha:") {
                return ensure_uicolor({
                    std::clamp(BitsToFloat(Arg(2)), 0.0f, 1.0f),
                    std::clamp(BitsToFloat(Arg(3)), 0.0f, 1.0f),
                    std::clamp(BitsToFloat(Arg(4)), 0.0f, 1.0f),
                    std::clamp(BitsToFloat(Arg(5)), 0.0f, 1.0f),
                });
            }
        }
        if (class_name == "UIImage"
            && (selector_name == "imageWithCGImage:" || selector_name == "imageWithCGImage:scale:orientation:")) {
            const u32 image = make_instance("UIImage", ObjKind::Generic);
            host_objects_[image].backing_store = Arg(2);
            host_objects_[image].dict["CGImage"] = Arg(2);
            if (selector_name == "imageWithCGImage:scale:orientation:") {
                host_objects_[image].dict["scale.bits"] = Arg(3);
                host_objects_[image].dict["orientation"] = Arg(4);
            }
            return image;
        }
        if (selector_name == "alloc") {
            if (objc_abi_ != nullptr && objc_abi_->IsGuestClass(self)) {
                const u32 result = objc_abi_->AllocateInstance(self, class_name + ".alloc");
                if (trace_shims_) {
                    Log("objc -> " + Hex32(result));
                }
                return result;
            }
            const u32 result = make_instance(class_name, default_kind_for_class(class_name));
            if (trace_shims_) {
                Log("objc -> " + Hex32(result));
            }
            return result;
        }
        if (selector_name == "new") {
            if (objc_abi_ != nullptr && objc_abi_->IsGuestClass(self)) {
                const u32 result = objc_abi_->AllocateInstance(self, class_name + ".new");
                if (trace_shims_) {
                    Log("objc -> " + Hex32(result));
                }
                return result;
            }
            const u32 result = make_instance(class_name, default_kind_for_class(class_name));
            if (trace_shims_) {
                Log("objc -> " + Hex32(result));
            }
            return result;
        }
        if (selector_name == "layerClass" && (class_name.find("GL") != std::string::npos || class_name.find("EAGL") != std::string::npos)) {
            return EnsureClass("CAEAGLLayer");
        }
        if (selector_name == "sharedApplication") {
            const u32 application = ensure_singleton(runtime_->main_application, class_name, ObjKind::Generic);
            if (runtime_->main_delegate != 0) {
                host_objects_[application].dict["setDelegate:"] = runtime_->main_delegate;
            }
            return application;
        }
        if (selector_name == "defaultCenter") {
            return ensure_singleton(runtime_->notification_center, "NSNotificationCenter", ObjKind::Generic);
        }
        if (selector_name == "mainBundle") {
            const u32 bundle = ensure_singleton(runtime_->main_bundle, "NSBundle", ObjKind::Generic);
            host_objects_[bundle].string_value = guest_home_;
            return bundle;
        }
        if (selector_name == "standardUserDefaults") {
            return ensure_singleton(runtime_->standard_user_defaults, "NSUserDefaults", ObjKind::Dictionary);
        }
        if (selector_name == "defaultManager") {
            return ensure_singleton(runtime_->default_file_manager, "NSFileManager", ObjKind::Generic);
        }
        if ((class_name == "NSData" || class_name == "NSMutableData")
            && (selector_name == "dataWithContentsOfFile:" || selector_name == "dataWithContentsOfURL:")) {
            const auto path = DecodeNSString(Arg(2)).value_or(ReadGuestCString(Arg(2)));
            if (path.empty()) {
                return 0;
            }
            const auto data = load_guest_file_bytes(path);
            return data ? EnsureNSData(*data) : 0u;
        }
        if (selector_name == "sharedHTTPCookieStorage") {
            return ensure_singleton(runtime_->shared_cookie_storage, "NSHTTPCookieStorage", ObjKind::Generic);
        }
        if (selector_name == "currentDevice") {
            return ensure_singleton(runtime_->current_device, "UIDevice", ObjKind::Generic);
        }
        if (selector_name == "mainScreen") {
            const u32 screen = ensure_singleton(runtime_->main_screen, "UIScreen", ObjKind::Generic);
            host_objects_[screen].dict["scale.bits"] = FloatToBits(runtime_->screen_scale);
            host_objects_[screen].dict["nativeScale.bits"] = FloatToBits(runtime_->screen_scale);
            host_objects_[screen].dict["setBounds:.w.bits"] = FloatToBits(static_cast<float>(runtime_->screen_width_points));
            host_objects_[screen].dict["setBounds:.h.bits"] = FloatToBits(static_cast<float>(runtime_->screen_height_points));
            return screen;
        }
        if (selector_name == "processInfo") {
            return ensure_singleton(runtime_->process_info, "NSProcessInfo", ObjKind::Generic);
        }
        if (selector_name == "currentRunLoop") {
            return ensure_singleton(runtime_->current_run_loop, "NSRunLoop", ObjKind::Generic);
        }
        if (selector_name == "currentThread" || selector_name == "mainThread") {
            const u32 thread = ensure_singleton(runtime_->current_thread, "NSThread", ObjKind::Generic);
            host_objects_[thread].dict["isMainThread"] = 1;
            return thread;
        }
        if (class_name == "NSThread" && selector_name == "detachNewThreadSelector:toTarget:withObject:") {
            const std::string detached_selector = ReadGuestCString(Arg(2));
            if (!detached_selector.empty() && Arg(3) != 0) {
                if (StartsWith(detached_selector, "sendSessionsToServer")) {
                    if (trace_shims_) {
                        Log("NSThread suppress analytics selector=" + detached_selector
                            + " target=" + Hex32(Arg(3)));
                    }
                    return 0;
                }
                queued_objc_callbacks_.push_back(PendingObjcCallback{
                    .receiver = Arg(3),
                    .selector_name = detached_selector,
                    .args = {Arg(4)},
                });
                if (trace_shims_) {
                    Log("NSThread queued target=" + Hex32(Arg(3))
                        + " selector=" + detached_selector
                        + " object=" + Hex32(Arg(4)));
                }
            }
            return 0;
        }
        if (class_name == "NSDate" && selector_name == "date") {
            const auto now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            return EnsureDate(now);
        }
        if (class_name == "CADisplayLink" && selector_name == "displayLinkWithTarget:selector:") {
            const u32 link_object = make_instance("CADisplayLink", ObjKind::Generic);
            runtime_->display_links[link_object] = RuntimeState::DisplayLink{
                .object = link_object,
                .target = Arg(2),
                .selector = Arg(3),
                .run_loop = 0,
                .mode = 0,
                .frame_interval = 1,
                .frame_counter = 0,
                .active = false,
                .paused = false,
            };
            host_objects_[link_object].dict["setTarget:"] = Arg(2);
            host_objects_[link_object].dict["setSelector:"] = Arg(3);
            host_objects_[link_object].dict["setFrameInterval:"] = 1;
            host_objects_[link_object].dict["setPaused:"] = 0;
            if (trace_shims_) {
                Log("CADisplayLink target=" + Hex32(Arg(2)) + " selector=" + ReadGuestCString(Arg(3)));
            }
            return link_object;
        }
        if (class_name == "EAGLContext" && selector_name == "currentContext") {
            return runtime_->current_eagl_context;
        }
        if (class_name == "EAGLContext" && selector_name == "setCurrentContext:") {
            runtime_->current_eagl_context = Arg(2);
            if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported() && runtime_->current_eagl_context != 0) {
                runtime_->host_gl->EnsureWindow(
                    static_cast<int>(runtime_->screen_width_points),
                    static_cast<int>(runtime_->screen_height_points),
                    binary_path_.filename().string());
                runtime_->host_gl->MakeCurrent();
            }
            return 1;
        }
        if (selector_name == "stringWithUTF8String:" || selector_name == "stringWithCString:encoding:") {
            return EnsureNSString(ReadGuestCString(Arg(2)));
        }
        if (selector_name == "stringWithFormat:") {
            if (const auto text = DecodeNSString(Arg(2))) {
                return EnsureNSString(FormatVarArgsString(*text, 3, [this](const std::size_t index) {
                    return Arg(index);
                }));
            }
            return EnsureNSString("");
        }
        if (selector_name == "URLWithString:" || selector_name == "fileURLWithPath:") {
            if (auto text = DecodeNSString(Arg(2))) {
                const u32 object = make_instance("NSURL", ObjKind::String);
                host_objects_[object].string_value = *text;
                return object;
            }
        }
        if (selector_name == "array" || selector_name == "arrayWithObject:") {
            std::vector<u32> values;
            if (selector_name == "arrayWithObject:") {
                values.push_back(Arg(2));
            }
            return EnsureArray(values);
        }
        if (selector_name == "dictionary") {
            return EnsureDictionary({});
        }
        if (selector_name == "dictionaryWithCapacity:") {
            return EnsureDictionaryOfClass({}, class_name);
        }
        if (selector_name == "dictionaryWithDictionary:") {
            const auto source = host_objects_.find(Arg(2));
            if (source == host_objects_.end() || source->second.kind != ObjKind::Dictionary) {
                return EnsureDictionaryOfClass({}, class_name);
            }
            return EnsureDictionaryOfClass(source->second.dict, class_name);
        }
        if (selector_name == "dictionaryWithObjectsAndKeys:") {
            std::unordered_map<std::string, u32> values;
            for (std::size_t index = 2;; index += 2) {
                const u32 value = Arg(index);
                const u32 key_object = Arg(index + 1);
                if (value == 0 || key_object == 0) {
                    break;
                }
                if (const auto key = DecodeNSString(key_object)) {
                    values[*key] = value;
                }
            }
            return EnsureDictionaryOfClass(values, class_name);
        }
        if (selector_name == "data") {
            return EnsureNSData({});
        }
        if (class_name == "NSKeyedArchiver" && selector_name == "archivedDataWithRootObject:") {
            return ArchiveObject(Arg(2));
        }
        if (class_name == "NSKeyedUnarchiver" && selector_name == "unarchiveObjectWithData:") {
            return UnarchiveObject(Arg(2));
        }
        if (class_name == "NSError" && selector_name == "errorWithDomain:code:userInfo:") {
            return EnsureNSError(DecodeNSString(Arg(2)).value_or("NSError"), static_cast<s32>(Arg(3)), Arg(4));
        }
        if (selector_name == "numberWithInt:" || selector_name == "numberWithUnsignedInt:") {
            return EnsureNumber(static_cast<double>(static_cast<s32>(Arg(2))));
        }
        if (selector_name == "numberWithDouble:" || selector_name == "numberWithFloat:") {
            return EnsureNumber(selector_name == "numberWithDouble:"
                    ? BitCastFromU64<double>(Pack64(Arg(2), Arg(3)))
                    : static_cast<double>(BitsToFloat(Arg(2))));
        }
        if (selector_name == "numberWithBool:") {
            return EnsureBoolean(Arg(2) != 0);
        }
        if (class_name == "NSProcessInfo" && selector_name == "globallyUniqueString") {
            return EnsureNSString(GenerateUuidString() + "-" + GenerateHexToken(4));
        }
        if (class_name == "UIPasteboard" && selector_name == "pasteboardWithName:create:") {
            const std::string name = DecodeNSString(Arg(2)).value_or("");
            if (name.empty()) {
                return 0;
            }
            if (const auto it = runtime_->named_pasteboards.find(name); it != runtime_->named_pasteboards.end() && host_objects_.count(it->second) != 0) {
                return it->second;
            }
            if (Arg(3) == 0) {
                return 0;
            }
            const u32 pasteboard = make_instance("UIPasteboard", ObjKind::Generic);
            host_objects_[pasteboard].string_value = name;
            host_objects_[pasteboard].dict["persistent"] = 0;
            runtime_->named_pasteboards[name] = pasteboard;
            return pasteboard;
        }
        if (selector_name == "null") {
            return make_instance("NSNull", ObjKind::Generic);
        }
    }

    if (!has_object) {
        return 0;
    }

    HostObject& object = object_it->second;
    auto queue_objc_callback = [&](const u32 receiver, std::string callback_selector, std::vector<u32> callback_args) {
        if (receiver == 0 || callback_selector.empty()) {
            return;
        }
        queued_objc_callbacks_.push_back(PendingObjcCallback{
            .receiver = receiver,
            .selector_name = std::move(callback_selector),
            .args = std::move(callback_args),
        });
    };
    auto popup_delegate = [&]() -> u32 {
        const auto delegate_it = object.dict.find("setDelegate:");
        return delegate_it == object.dict.end() ? 0 : delegate_it->second;
    };
    auto popup_cancel_button_index = [&]() -> s32 {
        const auto it = object.dict.find("cancelButtonIndex");
        return it == object.dict.end() ? -1 : static_cast<s32>(it->second);
    };
    auto popup_preferred_button_index = [&]() -> s32 {
        const s32 cancel_button_index = popup_cancel_button_index();
        if (cancel_button_index >= 0 && static_cast<std::size_t>(cancel_button_index) < object.items.size()) {
            return cancel_button_index;
        }
        const auto first_other_it = object.dict.find("firstOtherButtonIndex");
        if (first_other_it != object.dict.end() && static_cast<std::size_t>(first_other_it->second) < object.items.size()) {
            return static_cast<s32>(first_other_it->second);
        }
        return object.items.empty() ? -1 : 0;
    };
    auto queue_popup_present_callbacks = [&]() {
        const u32 delegate = popup_delegate();
        if (delegate == 0) {
            return;
        }
        if (class_name == "UIAlertView") {
            queue_objc_callback(delegate, "willPresentAlertView:", {self});
            queue_objc_callback(delegate, "didPresentAlertView:", {self});
        } else if (class_name == "UIActionSheet") {
            queue_objc_callback(delegate, "willPresentActionSheet:", {self});
            queue_objc_callback(delegate, "didPresentActionSheet:", {self});
        }
    };
    auto auto_confirm_popup = [&]() {
        const bool is_alert = class_name == "UIAlertView";
        const bool is_action_sheet = class_name == "UIActionSheet";
        if (!is_alert && !is_action_sheet) {
            return;
        }
        const u32 delegate = popup_delegate();
        const s32 preferred_button_index = popup_preferred_button_index();
        const u32 button_index = preferred_button_index >= 0 ? static_cast<u32>(preferred_button_index) : 0u;
        Log("objc auto-confirm [" + class_name + " show] delegate=" + Hex32(delegate)
            + " button=" + std::to_string(button_index));
        if (delegate == 0) {
            return;
        }
        if (is_alert) {
            queue_objc_callback(delegate, "willPresentAlertView:", {self});
            queue_objc_callback(delegate, "didPresentAlertView:", {self});
            queue_objc_callback(delegate, "alertView:clickedButtonAtIndex:", {self, button_index});
            queue_objc_callback(delegate, "alertView:willDismissWithButtonIndex:", {self, button_index});
            queue_objc_callback(delegate, "alertView:didDismissWithButtonIndex:", {self, button_index});
        } else {
            queue_objc_callback(delegate, "willPresentActionSheet:", {self});
            queue_objc_callback(delegate, "didPresentActionSheet:", {self});
            queue_objc_callback(delegate, "actionSheet:clickedButtonAtIndex:", {self, button_index});
            queue_objc_callback(delegate, "actionSheet:willDismissWithButtonIndex:", {self, button_index});
            queue_objc_callback(delegate, "actionSheet:didDismissWithButtonIndex:", {self, button_index});
        }
    };
    auto show_host_popup = [&]() -> bool {
        const bool is_alert = class_name == "UIAlertView";
        const bool is_action_sheet = class_name == "UIActionSheet";
        if ((!is_alert && !is_action_sheet)
            || object.dict["hostPopupPending"] != 0
            || object.dict["hostPopupVisible"] != 0) {
            return false;
        }

        HostPopupRequest request;
        request.token = next_host_popup_token_++;
        request.class_name = class_name;
        request.title = DecodeNSString(object.dict["title"]).value_or("");
        request.message = DecodeNSString(object.dict["message"]).value_or("");
        request.cancel_button_index = popup_cancel_button_index();
        request.preferred_button_index = popup_preferred_button_index();
        request.buttons.reserve(object.items.size());
        for (const u32 item : object.items) {
            request.buttons.push_back(DecodeNSString(item).value_or(""));
        }
        if (request.buttons.empty()) {
            return false;
        }
        if (!RequestHostPopup(request)) {
            return false;
        }

        active_host_popups_[request.token] = self;
        object.dict["hostPopupToken"] = request.token;
        object.dict["hostPopupPending"] = 1;
        object.dict["hostPopupVisible"] = 1;
        object.dict["hostPopupDismissed"] = 0;
        Log("objc host popup [" + class_name + "] token=" + std::to_string(request.token)
            + " title=\"" + request.title + "\" buttons=" + std::to_string(request.buttons.size()));
        queue_popup_present_callbacks();
        return true;
    };

    if (class_name == "UIAlertView"
        && selector_name == "initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:") {
        object.dict["title"] = Arg(2);
        object.dict["message"] = Arg(3);
        object.dict["setDelegate:"] = Arg(4);
        object.items.clear();
        object.dict["cancelButtonIndex"] = 0;
        if (Arg(5) != 0) {
            object.items.push_back(Arg(5));
        }
        for (std::size_t index = 6; index < 16; ++index) {
            const u32 button = Arg(index);
            if (button == 0) {
                break;
            }
            if (!object.dict.contains("firstOtherButtonIndex")) {
                object.dict["firstOtherButtonIndex"] = static_cast<u32>(object.items.size());
            }
            object.items.push_back(button);
        }
        if (!object.dict.contains("firstOtherButtonIndex")) {
            object.dict["firstOtherButtonIndex"] = object.items.empty() ? 0u : object.dict["cancelButtonIndex"];
        }
        return self;
    }
    if (class_name == "UIActionSheet"
        && selector_name == "initWithTitle:delegate:cancelButtonTitle:destructiveButtonTitle:otherButtonTitles:") {
        object.dict["title"] = Arg(2);
        object.dict["setDelegate:"] = Arg(3);
        object.items.clear();
        if (Arg(5) != 0) {
            object.dict["destructiveButtonIndex"] = static_cast<u32>(object.items.size());
            object.items.push_back(Arg(5));
        }
        for (std::size_t index = 6; index < 16; ++index) {
            const u32 button = Arg(index);
            if (button == 0) {
                break;
            }
            if (!object.dict.contains("firstOtherButtonIndex")) {
                object.dict["firstOtherButtonIndex"] = static_cast<u32>(object.items.size());
            }
            object.items.push_back(button);
        }
        if (Arg(4) != 0) {
            object.dict["cancelButtonIndex"] = static_cast<u32>(object.items.size());
            object.items.push_back(Arg(4));
        }
        if (!object.dict.contains("firstOtherButtonIndex")) {
            object.dict["firstOtherButtonIndex"] = object.items.empty() ? 0u : 0u;
        }
        return self;
    }
    if ((class_name == "UIAlertView" || class_name == "UIActionSheet") && selector_name == "addButtonWithTitle:") {
        if (Arg(2) != 0) {
            if (!object.dict.contains("firstOtherButtonIndex")) {
                object.dict["firstOtherButtonIndex"] = static_cast<u32>(object.items.size());
            }
            object.items.push_back(Arg(2));
        }
        return object.items.empty() ? 0u : static_cast<u32>(object.items.size() - 1);
    }
    if ((class_name == "UIAlertView" || class_name == "UIActionSheet")
        && (selector_name == "show" || selector_name == "showInView:")) {
        if (show_host_popup()) {
            return 0;
        }
        auto_confirm_popup();
        return 0;
    }
    if ((class_name == "UIAlertView" || class_name == "UIActionSheet")
        && selector_name == "dismissWithClickedButtonIndex:animated:") {
        const auto token_it = object.dict.find("hostPopupToken");
        if (token_it != object.dict.end() && token_it->second != 0) {
            active_host_popups_.erase(token_it->second);
            RequestHostPopupDismiss(token_it->second);
            object.dict["hostPopupToken"] = 0;
        }
        object.dict["hostPopupPending"] = 0;
        object.dict["hostPopupVisible"] = 0;
        object.dict["hostPopupDismissed"] = 1;
        return 0;
    }
    if ((class_name == "UIAlertView" || class_name == "UIActionSheet") && selector_name == "numberOfButtons") {
        return static_cast<u32>(object.items.size());
    }
    if ((class_name == "UIAlertView" || class_name == "UIActionSheet")
        && (selector_name == "cancelButtonIndex" || selector_name == "firstOtherButtonIndex" || selector_name == "destructiveButtonIndex")) {
        const auto it = object.dict.find(selector_name);
        return it == object.dict.end() ? 0u : it->second;
    }
    if ((class_name == "UIAlertView" || class_name == "UIActionSheet") && selector_name == "buttonTitleAtIndex:") {
        const std::size_t index = Arg(2);
        return index < object.items.size() ? object.items[index] : 0u;
    }
    if (!kDisableHostTextRenderingProbe && is_kind_of_named_class("UIFont")) {
        if (selector_name == "fontWithSize:") {
            object.number_value = BitsToFloat(Arg(2));
            if (object.number_value <= 0.0) {
                object.number_value = 16.0;
            }
            return self;
        }
        if (selector_name == "pointSize") {
            SetReturnU32(FloatToBits(font_size_for_handle(self)));
            return cpu_->Regs()[0];
        }
    }
    if (class_name == "UIColor") {
        if (selector_name == "CGColor") {
            const auto it = object.dict.find("CGColor");
            return it == object.dict.end() ? 0u : it->second;
        }
        if ((selector_name == "set" || selector_name == "setFill") && !runtime_->graphics_context_stack.empty()) {
            if (const auto rgba = color_components_for_handle(self)) {
                runtime_->graphics_contexts[runtime_->graphics_context_stack.back()].fill_color = *rgba;
            }
            return 0;
        }
        if (selector_name == "setStroke" && !runtime_->graphics_context_stack.empty()) {
            if (const auto rgba = color_components_for_handle(self)) {
                runtime_->graphics_contexts[runtime_->graphics_context_stack.back()].stroke_color = *rgba;
            }
            return 0;
        }
    }
    if (is_kind_of_named_class("UIImage")) {
        if (selector_name == "initWithCGImage:" || selector_name == "initWithCGImage:scale:orientation:") {
            object.backing_store = Arg(2);
            object.dict["CGImage"] = Arg(2);
            if (selector_name == "initWithCGImage:scale:orientation:") {
                object.dict["scale.bits"] = Arg(3);
                object.dict["orientation"] = Arg(4);
            }
            return self;
        }
        if (selector_name == "CGImage") {
            const auto it = object.dict.find("CGImage");
            return it == object.dict.end() ? object.backing_store : it->second;
        }
        if (selector_name == "size") {
            const u32 cg_image = object.dict.contains("CGImage") ? object.dict["CGImage"] : object.backing_store;
            const auto image_it = runtime_->graphics_images.find(cg_image);
            const float width = image_it == runtime_->graphics_images.end() ? 0.0f : static_cast<float>(image_it->second.width);
            const float height = image_it == runtime_->graphics_images.end() ? 0.0f : static_cast<float>(image_it->second.height);
            if (stret) {
                memory_.Write32(stret_buffer + 0, FloatToBits(width));
                memory_.Write32(stret_buffer + 4, FloatToBits(height));
                return 0;
            }
            cpu_->Regs()[0] = FloatToBits(width);
            cpu_->Regs()[1] = FloatToBits(height);
            return cpu_->Regs()[0];
        }
        if (selector_name == "drawAtPoint:") {
            const u32 cg_image = object.dict.contains("CGImage") ? object.dict["CGImage"] : object.backing_store;
            const auto image_it = runtime_->graphics_images.find(cg_image);
            if (image_it != runtime_->graphics_images.end()) {
                draw_image_into_context(
                    current_graphics_context(),
                    self,
                    {BitsToFloat(Arg(2)), BitsToFloat(Arg(3)),
                        static_cast<float>(image_it->second.width), static_cast<float>(image_it->second.height)},
                    1.0f);
            }
            return 0;
        }
        if (selector_name == "drawInRect:" || selector_name == "drawInRect:blendMode:alpha:") {
            const float alpha = selector_name == "drawInRect:blendMode:alpha:" ? BitsToFloat(Arg(7)) : 1.0f;
            draw_image_into_context(
                current_graphics_context(),
                self,
                {BitsToFloat(Arg(2)), BitsToFloat(Arg(3)), BitsToFloat(Arg(4)), BitsToFloat(Arg(5))},
                alpha);
            return 0;
        }
    }
    if (!kDisableHostTextRenderingProbe
        && (class_name == "NSString" || class_name == "NSMutableString")
        && (selector_name == "sizeWithFont:"
            || selector_name == "sizeWithFont:constrainedToSize:"
            || selector_name == "sizeWithFont:constrainedToSize:lineBreakMode:")) {
        const std::string text = DecodeNSString(self).value_or(object.string_value);
        const float max_width =
            selector_name == "sizeWithFont:" ? 0.0f : BitsToFloat(Arg(3));
        const auto measurement = measure_text(text, Arg(2), max_width);
        if (stret) {
            memory_.Write32(stret_buffer + 0, FloatToBits(measurement.width));
            memory_.Write32(stret_buffer + 4, FloatToBits(measurement.height));
            return 0;
        }
        cpu_->Regs()[0] = FloatToBits(measurement.width);
        cpu_->Regs()[1] = FloatToBits(measurement.height);
        return cpu_->Regs()[0];
    }
    if (!kDisableHostTextRenderingProbe && is_kind_of_named_class("UILabel")) {
        if (selector_name == "setText:" || selector_name == "setFont:" || selector_name == "setTextColor:"
            || selector_name == "setBackgroundColor:" || selector_name == "setShadowColor:"
            || selector_name == "setLineBreakMode:" || selector_name == "setTextAlignment:") {
            object.dict[selector_name] = Arg(2);
            return 0;
        }
        if (selector_name == "setShadowOffset:") {
            object.dict["setShadowOffset:.w.bits"] = Arg(2);
            object.dict["setShadowOffset:.h.bits"] = Arg(3);
            return 0;
        }
        if (selector_name == "text") {
            const auto it = object.dict.find("setText:");
            return it == object.dict.end() ? 0u : it->second;
        }
        if (selector_name == "font") {
            const auto it = object.dict.find("setFont:");
            return it == object.dict.end() ? 0u : it->second;
        }
        if (selector_name == "textRectForBounds:limitedToNumberOfLines:") {
            const std::string text = DecodeNSString(object.dict.contains("setText:") ? object.dict["setText:"] : 0).value_or("");
            const u32 font = object.dict.contains("setFont:") ? object.dict["setFont:"] : 0;
            const float x = BitsToFloat(Arg(2));
            const float y = BitsToFloat(Arg(3));
            const float width = BitsToFloat(Arg(4));
            const float height = BitsToFloat(Arg(5));
            const auto measurement = measure_text(text, font, width);
            const float measured_width = std::min(width, measurement.width);
            const float measured_height = std::min(height, measurement.height);
            if (stret) {
                memory_.Write32(stret_buffer + 0, FloatToBits(x));
                memory_.Write32(stret_buffer + 4, FloatToBits(y));
                memory_.Write32(stret_buffer + 8, FloatToBits(measured_width));
                memory_.Write32(stret_buffer + 12, FloatToBits(measured_height));
                return 0;
            }
            cpu_->Regs()[0] = FloatToBits(x);
            cpu_->Regs()[1] = FloatToBits(y);
            return cpu_->Regs()[0];
        }
        if (selector_name == "drawTextInRect:") {
            const std::string text = DecodeNSString(object.dict.contains("setText:") ? object.dict["setText:"] : 0).value_or("");
            const u32 font = object.dict.contains("setFont:") ? object.dict["setFont:"] : 0;
            const auto text_color = object.dict.contains("setTextColor:")
                ? color_components_for_handle(object.dict["setTextColor:"])
                : std::nullopt;
            draw_text_into_context(
                current_graphics_context(),
                text,
                font,
                {BitsToFloat(Arg(2)), BitsToFloat(Arg(3)), BitsToFloat(Arg(4)), BitsToFloat(Arg(5))},
                text_color);
            return 0;
        }
        if (selector_name == "sizeToFit") {
            const std::string text = DecodeNSString(object.dict.contains("setText:") ? object.dict["setText:"] : 0).value_or("");
            const u32 font = object.dict.contains("setFont:") ? object.dict["setFont:"] : 0;
            const auto measurement = measure_text(text, font, 0.0f);
            object.dict["setFrame:.w.bits"] = FloatToBits(measurement.width);
            object.dict["setFrame:.h.bits"] = FloatToBits(measurement.height);
            object.dict["setBounds:.w.bits"] = FloatToBits(measurement.width);
            object.dict["setBounds:.h.bits"] = FloatToBits(measurement.height);
            return 0;
        }
    }

    if (class_name == "UITouch") {
        auto touch_float_bits = [&](const std::string& key) {
            const auto it = object.dict.find(key);
            return it == object.dict.end() ? FloatToBits(0.0f) : it->second;
        };
        auto return_touch_point = [&](const std::string& x_key, const std::string& y_key) {
            const u32 x = touch_float_bits(x_key);
            const u32 y = touch_float_bits(y_key);
            if (stret) {
                memory_.Write32(stret_buffer + 0, x);
                memory_.Write32(stret_buffer + 4, y);
                return 0u;
            }
            SetReturnU32(x);
            cpu_->Regs()[1] = y;
            return cpu_->Regs()[0];
        };
        if (selector_name == "locationInView:") {
            return return_touch_point("x.bits", "y.bits");
        }
        if (selector_name == "previousLocationInView:") {
            return return_touch_point("previous_x.bits", "previous_y.bits");
        }
        if (selector_name == "view" || selector_name == "window" || selector_name == "phase" || selector_name == "tapCount") {
            const auto it = object.dict.find(selector_name);
            return it == object.dict.end() ? 0u : it->second;
        }
        if (selector_name == "timestamp") {
            SetReturnDouble(object.number_value);
            return cpu_->Regs()[0];
        }
    }
    if (class_name == "UIEvent") {
        if (selector_name == "allTouches" || selector_name == "touchesForView:" || selector_name == "touchesForWindow:") {
            const auto it = object.dict.find("touches");
            return it == object.dict.end() ? 0u : it->second;
        }
        if (selector_name == "timestamp") {
            SetReturnDouble(object.number_value);
            return cpu_->Regs()[0];
        }
    }
    if (object.kind == ObjKind::Array && class_name == "NSSet") {
        if (selector_name == "anyObject") {
            return object.items.empty() ? 0u : object.items.front();
        }
        if (selector_name == "objectEnumerator") {
            object.dict["enumerator.index"] = 0;
            return self;
        }
        if (selector_name == "allObjects") {
            return EnsureArray(object.items);
        }
        if (selector_name == "containsObject:" || selector_name == "member:") {
            const auto it = std::find(object.items.begin(), object.items.end(), Arg(2));
            if (selector_name == "containsObject:") {
                return it == object.items.end() ? 0u : 1u;
            }
            return it == object.items.end() ? 0u : *it;
        }
    }

    if ((selector_name == "initWithCapacity:" || selector_name == "initWithObjectsAndKeys:" || selector_name == "initWithDictionary:")
        && (class_name == "NSDictionary" || class_name == "NSMutableDictionary" || object.kind == ObjKind::Dictionary)) {
        object.kind = ObjKind::Dictionary;
        if (selector_name == "initWithDictionary:") {
            object.dict.clear();
            if (const auto source = host_objects_.find(Arg(2)); source != host_objects_.end() && source->second.kind == ObjKind::Dictionary) {
                object.dict = source->second.dict;
            }
        } else if (selector_name == "initWithObjectsAndKeys:") {
            object.dict.clear();
            for (std::size_t index = 2;; index += 2) {
                const u32 value = Arg(index);
                const u32 key_object = Arg(index + 1);
                if (value == 0 || key_object == 0) {
                    break;
                }
                if (const auto key = DecodeNSString(key_object)) {
                    object.dict[*key] = value;
                }
            }
        }
        return self;
    }
    if ((selector_name == "initWithCapacity:" || selector_name == "initWithArray:")
        && (class_name == "NSArray" || class_name == "NSMutableArray" || object.kind == ObjKind::Array)) {
        object.kind = ObjKind::Array;
        if (selector_name == "initWithArray:") {
            object.items.clear();
            if (const auto source = host_objects_.find(Arg(2)); source != host_objects_.end() && source->second.kind == ObjKind::Array) {
                object.items = source->second.items;
            }
        }
        return self;
    }

    if (class_name == "EAGLContext" && (selector_name == "initWithAPI:" || selector_name == "initWithAPI:sharegroup:")) {
        object.dict["eagl.api"] = Arg(2);
        runtime_->current_eagl_context = self;
        if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported()) {
            runtime_->host_gl->EnsureWindow(
                static_cast<int>(runtime_->screen_width_points),
                static_cast<int>(runtime_->screen_height_points),
                binary_path_.filename().string());
            runtime_->host_gl->MakeCurrent();
        }
        return self;
    }
    if (selector_name == "layer") {
        if (object.class_name.find("View") != std::string::npos || object.class_name.find("Window") != std::string::npos) {
            const auto layer_it = object.dict.find("layer");
            if (layer_it != object.dict.end() && host_objects_.count(layer_it->second) != 0) {
                return layer_it->second;
            }
            const u32 layer = ensure_main_layer();
            object.dict["layer"] = layer;
            return layer;
        }
    }
    if (selector_name == "view" && object.class_name.find("Controller") != std::string::npos) {
        if (const auto existing = object.dict.find("setView:"); existing != object.dict.end() && existing->second != 0) {
            EnsureGuestAnnotation(existing->second);
            return existing->second;
        }
        if (object.class_name == "GameViewController") {
            const u32 gl_view = ensure_main_gl_view();
            object.dict["setView:"] = gl_view;
            object.dict["setGlView:"] = gl_view;
            return gl_view;
        }
        const auto view_it = object.dict.find("view");
        if (view_it != object.dict.end() && host_objects_.count(view_it->second) != 0) {
            return view_it->second;
        }
        const u32 view = make_instance("UIView", ObjKind::Generic);
        object.dict["view"] = view;
        host_objects_[view].dict["layer"] = ensure_main_layer();
        return view;
    }
    if ((selector_name == "setView:" || selector_name == "setGlView:") && class_name == "GameViewController") {
        object.dict[selector_name] = Arg(2);
        EnsureGuestAnnotation(Arg(2));
        if (const auto view_it = host_objects_.find(Arg(2)); view_it != host_objects_.end() && is_eagl_view_class(view_it->second.class_name)) {
            runtime_->main_gl_view = Arg(2);
            host_objects_[Arg(2)].dict["setDelegate:"] = runtime_->main_delegate;
            Log("touch receiver set to guest gl view " + Hex32(runtime_->main_gl_view)
                + " class=" + view_it->second.class_name);
        }
        return 0;
    }
    if (selector_name == "window") {
        return runtime_->main_window;
    }
    if (class_name == "CADisplayLink" && selector_name == "setFrameInterval:") {
        auto& link = runtime_->display_links[self];
        link.object = self;
        link.frame_interval = std::max(1, static_cast<int>(Arg(2)));
        link.frame_counter = 0;
        object.dict["setFrameInterval:"] = static_cast<u32>(link.frame_interval);
        if (runtime_->debug_display_link_logs < 64) {
            ++runtime_->debug_display_link_logs;
            Log("[displaylink] setFrameInterval link=" + Hex32(self)
                + " interval=" + std::to_string(link.frame_interval));
        }
        return 0;
    }
    if (class_name == "CADisplayLink" && selector_name == "frameInterval") {
        if (const auto it = runtime_->display_links.find(self); it != runtime_->display_links.end()) {
            return static_cast<u32>(std::max(it->second.frame_interval, 1));
        }
        return 1;
    }
    if (class_name == "CADisplayLink" && selector_name == "addToRunLoop:forMode:") {
        auto& link = runtime_->display_links[self];
        link.object = self;
        link.run_loop = Arg(2);
        link.mode = Arg(3);
        link.active = true;
        link.paused = false;
        object.dict["setPaused:"] = 0;
        if (runtime_->debug_display_link_logs < 64) {
            ++runtime_->debug_display_link_logs;
            Log("[displaylink] add link=" + Hex32(self)
                + " target=" + Hex32(link.target)
                + " selector=" + ReadGuestCString(link.selector)
                + " runloop=" + Hex32(link.run_loop)
                + " mode=" + Hex32(link.mode));
        }
        return 0;
    }
    if (class_name == "CADisplayLink" && selector_name == "removeFromRunLoop:forMode:") {
        if (auto it = runtime_->display_links.find(self); it != runtime_->display_links.end()) {
            it->second.active = false;
            it->second.run_loop = 0;
            it->second.mode = 0;
            if (runtime_->debug_display_link_logs < 64) {
                ++runtime_->debug_display_link_logs;
                Log("[displaylink] remove link=" + Hex32(self));
            }
        }
        return 0;
    }
    if (class_name == "CADisplayLink" && selector_name == "invalidate") {
        if (auto it = runtime_->display_links.find(self); it != runtime_->display_links.end()) {
            it->second.active = false;
            it->second.paused = true;
            if (runtime_->debug_display_link_logs < 64) {
                ++runtime_->debug_display_link_logs;
                Log("[displaylink] invalidate link=" + Hex32(self));
            }
        }
        object.dict["setPaused:"] = 1;
        return 0;
    }
    if (class_name == "CADisplayLink" && selector_name == "setPaused:") {
        auto& link = runtime_->display_links[self];
        link.object = self;
        link.paused = Arg(2) != 0;
        object.dict["setPaused:"] = link.paused ? 1u : 0u;
        if (runtime_->debug_display_link_logs < 64) {
            ++runtime_->debug_display_link_logs;
            Log("[displaylink] paused link=" + Hex32(self)
                + " value=" + std::to_string(link.paused ? 1 : 0));
        }
        return 0;
    }
    if (class_name == "CADisplayLink" && (selector_name == "isPaused" || selector_name == "paused")) {
        if (const auto it = runtime_->display_links.find(self); it != runtime_->display_links.end()) {
            return it->second.paused ? 1u : 0u;
        }
        return 0;
    }
    if (class_name == "CADisplayLink" && selector_name == "selector") {
        if (const auto it = runtime_->display_links.find(self); it != runtime_->display_links.end()) {
            return it->second.selector;
        }
        return 0;
    }
    if (class_name == "CADisplayLink" && selector_name == "target") {
        if (const auto it = runtime_->display_links.find(self); it != runtime_->display_links.end()) {
            return it->second.target;
        }
        return 0;
    }
    if (selector_name == "keyWindow" && object.class_name.find("Application") != std::string::npos) {
        return ensure_main_window();
    }
    if (selector_name == "windows" && object.class_name.find("Application") != std::string::npos) {
        return EnsureArray({ensure_main_window()});
    }
    if ((selector_name == "fileExistsAtPath:" || selector_name == "fileExistsAtPath:isDirectory:") && class_name == "NSFileManager") {
        const auto path = DecodeNSString(Arg(2));
        if (!path) {
            return 0;
        }
        const auto host_path = ResolveGuestPath(*path);
        const bool exists = GuestPathExists(*path);
        if (selector_name == "fileExistsAtPath:isDirectory:" && Arg(3) != 0) {
            memory_.Write8(Arg(3), exists && ResolveGuestAssetPath(*path) == std::nullopt && std::filesystem::is_directory(host_path) ? 1 : 0);
        }
        return exists ? 1u : 0u;
    }
    if (selector_name == "makeKeyAndVisible") {
        runtime_->main_window = self;
        return 0;
    }
    if (selector_name == "loadView" && class_name == "GameViewController") {
        const u32 gl_view = ensure_main_gl_view();
        object.dict["setGlView:"] = gl_view;
        object.dict["setView:"] = gl_view;
        return 0;
    }
    if (selector_name == "viewDidLoad" && class_name == "GameViewController") {
        const u32 gl_view = ensure_main_gl_view();
        host_objects_[gl_view].dict["setDelegate:"] = runtime_->main_delegate;
        object.dict["setGlView:"] = gl_view;
        object.dict["setView:"] = gl_view;
        return 0;
    }
    if (selector_name == "glView" && (class_name == "GameViewController" || class_name == "AppController")) {
        return ensure_main_gl_view();
    }
    if (selector_name == "gameViewController" && class_name == "AppController") {
        return ensure_main_view_controller();
    }
    if (selector_name == "application:didFinishLaunchingWithOptions:" && class_name == "AppController") {
        runtime_->main_delegate = self;
        if (runtime_->main_application != 0) {
            host_objects_[runtime_->main_application].dict["setDelegate:"] = self;
        }
        const u32 controller = ensure_main_view_controller();
        const u32 gl_view = ensure_main_gl_view();
        const u32 window = ensure_main_window();
        Log("AppController didFinish self=" + Hex32(self)
            + " app=" + Hex32(Arg(2))
            + " options=" + Hex32(Arg(3))
            + " controller=" + Hex32(controller)
            + " glView=" + Hex32(gl_view)
            + " window=" + Hex32(window));
        object.dict["setWindow:"] = window;
        object.dict["setController:"] = controller;
        object.dict["setGlView:"] = gl_view;
        object.dict["setDelegate:"] = Arg(0);
        host_objects_[gl_view].dict["setDelegate:"] = self;
        host_objects_[window].dict["setRootViewController:"] = controller;
        Log("AppController didFinish -> loadView");
        DispatchObjCMessage(controller, AllocateCString("loadView", "selector"), false, false, 0);
        Log("AppController didFinish -> viewDidLoad");
        DispatchObjCMessage(controller, AllocateCString("viewDidLoad", "selector"), false, false, 0);
        Log("AppController didFinish -> layoutSubviews");
        DispatchObjCMessage(gl_view, AllocateCString("layoutSubviews", "selector"), false, false, 0);
        Log("AppController didFinish -> makeKeyAndVisible");
        DispatchObjCMessage(window, AllocateCString("makeKeyAndVisible", "selector"), false, false, 0);
        Log("AppController didFinish -> startAnimation");
        DispatchObjCMessage(gl_view, AllocateCString("startAnimation", "selector"), false, false, 0);
        return 1;
    }
    if (selector_name == "applicationDidBecomeActive:" && class_name == "AppController") {
        DispatchObjCMessage(ensure_main_gl_view(), AllocateCString("startAnimation", "selector"), false, false, 0);
        return 0;
    }
    if ((selector_name == "applicationWillResignActive:" || selector_name == "applicationDidEnterBackground:" || selector_name == "applicationWillTerminate:")
        && class_name == "AppController") {
        DispatchObjCMessage(ensure_main_gl_view(), AllocateCString("stopAnimation", "selector"), false, false, 0);
        return 0;
    }
    if (selector_name == "applicationWillEnterForeground:" && class_name == "AppController") {
        DispatchObjCMessage(ensure_main_gl_view(), AllocateCString("startAnimation", "selector"), false, false, 0);
        return 0;
    }
    if (selector_name == "draw" && class_name == "AppController") {
        DispatchObjCMessage(ensure_main_gl_view(), AllocateCString("swapBuffers", "selector"), false, false, 0);
        return 0;
    }
    if (selector_name == "update" && class_name == "AppController") {
        return 0;
    }
    if (selector_name == "scale" || selector_name == "nativeScale" || selector_name == "contentScaleFactor" || selector_name == "contentsScale") {
        const auto it = object.dict.find(selector_name + ".bits");
        SetReturnU32(it == object.dict.end() ? FloatToBits(runtime_->screen_scale) : it->second);
        return cpu_->Regs()[0];
    }
    if (selector_name == "setContentScaleFactor:" || selector_name == "setContentsScale:") {
        object.dict[selector_name == "setContentScaleFactor:" ? "contentScaleFactor.bits" : "contentsScale.bits"] = Arg(2);
        return 0;
    }
    if ((selector_name == "bounds" || selector_name == "frame" || selector_name == "applicationFrame") && stret) {
        const std::string rect_key = selector_name == "frame" ? "setFrame:" : "setBounds:";
        const auto rect = get_rect(
            object,
            rect_key,
            static_cast<float>(runtime_->screen_width_points),
            static_cast<float>(runtime_->screen_height_points));
        write_rect(stret_buffer, rect[0], rect[1], rect[2], rect[3]);
        return 0;
    }
    if (selector_name == "setFrame:" || selector_name == "setBounds:") {
        object.dict[selector_name + ".x.bits"] = Arg(2);
        object.dict[selector_name + ".y.bits"] = Arg(3);
        object.dict[selector_name + ".w.bits"] = Arg(4);
        object.dict[selector_name + ".h.bits"] = Arg(5);
        if (self == runtime_->main_window || self == runtime_->main_layer || self == runtime_->main_screen) {
            runtime_->screen_width_points = std::max(1, static_cast<int>(BitsToFloat(Arg(4))));
            runtime_->screen_height_points = std::max(1, static_cast<int>(BitsToFloat(Arg(5))));
        }
        if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported() && (self == runtime_->main_window || self == runtime_->main_layer)) {
            runtime_->host_gl->Resize(
                static_cast<int>(BitsToFloat(Arg(4)) * runtime_->screen_scale),
                static_cast<int>(BitsToFloat(Arg(5)) * runtime_->screen_scale));
        }
        return 0;
    }
    if (selector_name == "bundlePath" || selector_name == "resourcePath") {
        return EnsureNSString(guest_home_);
    }
    if (selector_name == "executablePath") {
        return EnsureNSString(JoinPathForGuest(guest_home_, image_.path().filename().string()));
    }
    if ((selector_name == "pathForResource:ofType:" || selector_name == "pathForResource:ofType:inDirectory:") && class_name == "NSBundle") {
        const std::string resource = DecodeNSString(Arg(2)).value_or("");
        const std::string extension = DecodeNSString(Arg(3)).value_or("");
        const std::string directory = selector_name == "pathForResource:ofType:inDirectory:" ? DecodeNSString(Arg(4)).value_or("") : "";
        std::filesystem::path guest_path = guest_home_;
        if (!directory.empty()) {
            guest_path /= directory;
        }
        guest_path /= resource + (extension.empty() ? "" : "." + extension);
        if (GuestPathExists(guest_path.generic_string())) {
            return EnsureNSString(guest_path.generic_string());
        }
        return 0;
    }
    if (selector_name == "setNeedsDisplay" || selector_name == "display" || selector_name == "setNeedsLayout" || selector_name == "layoutIfNeeded") {
        return 0;
    }
    if (selector_name == "layoutSubviews" && is_eagl_view_class(class_name)) {
        runtime_->main_gl_view = self;
        ensure_drawable_ids();
        object.dict["setContext:"] = ensure_gl_context();
        if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported()) {
            runtime_->host_gl->EnsureWindow(
                static_cast<int>(runtime_->screen_width_points),
                static_cast<int>(runtime_->screen_height_points),
                binary_path_.filename().string());
            runtime_->host_gl->MakeCurrent();
        }
        runtime_->gl_state.framebuffer = runtime_->drawable_framebuffer;
        runtime_->gl_state.renderbuffer = runtime_->drawable_renderbuffer;
        return 0;
    }
    if ((selector_name == "initWithFrame:" || selector_name == "initWithFrame:pixelFormat:" || selector_name == "initWithFrame:pixelFormat:depthFormat:preserveBackbuffer:")
        && is_eagl_view_class(class_name)) {
        runtime_->main_gl_view = self;
        object.dict["setFrame:.x.bits"] = Arg(2);
        object.dict["setFrame:.y.bits"] = Arg(3);
        object.dict["setFrame:.w.bits"] = Arg(4);
        object.dict["setFrame:.h.bits"] = Arg(5);
        object.dict["setBounds:.w.bits"] = Arg(4);
        object.dict["setBounds:.h.bits"] = Arg(5);
        object.dict["setContext:"] = ensure_gl_context();
        if (selector_name != "initWithFrame:") {
            object.dict["setPixelFormat:"] = Arg(6);
        }
        if (selector_name == "initWithFrame:pixelFormat:depthFormat:preserveBackbuffer:") {
            object.dict["setDepthFormat:"] = Arg(7);
            object.dict["setPreserveBackbuffer:"] = Arg(8);
        }
        DispatchObjCMessage(self, AllocateCString("layoutSubviews", "selector"), false, false, 0);
        return self;
    }
    if (selector_name == "setCurrentContext" && is_eagl_view_class(class_name)) {
        runtime_->current_eagl_context = object.dict["setContext:"];
        if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported()) {
            runtime_->host_gl->EnsureWindow(
                static_cast<int>(runtime_->screen_width_points),
                static_cast<int>(runtime_->screen_height_points),
                binary_path_.filename().string());
            runtime_->host_gl->MakeCurrent();
        }
        return 1;
    }
    if (selector_name == "clearCurrentContext" && is_eagl_view_class(class_name)) {
        if (runtime_->current_eagl_context == 0) {
            runtime_->current_eagl_context = object.dict["setContext:"];
        }
        if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported() && runtime_->current_eagl_context != 0) {
            runtime_->host_gl->EnsureWindow(
                static_cast<int>(runtime_->screen_width_points),
                static_cast<int>(runtime_->screen_height_points),
                binary_path_.filename().string());
            runtime_->host_gl->MakeCurrent();
        }
        return 0;
    }
    if (selector_name == "isCurrentContext" && is_eagl_view_class(class_name)) {
        return runtime_->current_eagl_context != 0 && runtime_->current_eagl_context == object.dict["setContext:"] ? 1u : 0u;
    }
    if (selector_name == "startAnimation" && is_eagl_view_class(class_name)) {
        runtime_->main_gl_view = self;
        object.dict["setAnimating:"] = 1;
        DispatchObjCMessage(self, AllocateCString("setCurrentContext", "selector"), false, false, 0);
        DispatchObjCMessage(self, AllocateCString("layoutSubviews", "selector"), false, false, 0);
        if (const auto it = object.dict.find("setDelegate:"); it != object.dict.end() && it->second != 0) {
            DispatchObjCMessage(it->second, AllocateCString("update", "selector"), false, false, 0);
            DispatchObjCMessage(it->second, AllocateCString("draw", "selector"), false, false, 0);
        } else {
            DispatchObjCMessage(self, AllocateCString("swapBuffers", "selector"), false, false, 0);
        }
        return 0;
    }
    if (selector_name == "stopAnimation" && is_eagl_view_class(class_name)) {
        object.dict["setAnimating:"] = 0;
        return 0;
    }
    if (selector_name == "swapBuffers" && is_eagl_view_class(class_name)) {
        runtime_->gl_state.renderbuffer = runtime_->drawable_renderbuffer;
        const u32 context = object.dict["setContext:"] == 0 ? ensure_gl_context() : object.dict["setContext:"];
        const auto saved_r0 = cpu_->Regs()[0];
        const auto saved_r1 = cpu_->Regs()[1];
        const auto saved_r2 = cpu_->Regs()[2];
        cpu_->Regs()[0] = context;
        cpu_->Regs()[1] = AllocateCString("presentRenderbuffer:", "selector");
        cpu_->Regs()[2] = 0x8D41;
        DispatchObjCMessage(context, cpu_->Regs()[1], false, false, 0);
        cpu_->Regs()[0] = saved_r0;
        cpu_->Regs()[1] = saved_r1;
        cpu_->Regs()[2] = saved_r2;
        return 1;
    }
    if (selector_name == "context" && is_eagl_view_class(class_name)) {
        return object.dict["setContext:"];
    }
    if (selector_name == "framebuffer" && is_eagl_view_class(class_name)) {
        ensure_drawable_ids();
        return runtime_->drawable_framebuffer;
    }
    if (selector_name == "surfaceSize" && is_eagl_view_class(class_name)) {
        return EnsureNSString("{" + std::to_string(runtime_->screen_width_points) + ", " + std::to_string(runtime_->screen_height_points) + "}");
    }
    if (selector_name == "renderbufferStorage:fromDrawable:" && class_name == "EAGLContext") {
        runtime_->main_layer = Arg(3) != 0 ? Arg(3) : ensure_main_layer();
        runtime_->drawable_renderbuffer = runtime_->gl_state.renderbuffer;
        auto& renderbuffer = runtime_->gl_renderbuffers[runtime_->drawable_renderbuffer];
        renderbuffer.width = std::max(1, static_cast<s32>(runtime_->screen_width_points * runtime_->screen_scale));
        renderbuffer.height = std::max(1, static_cast<s32>(runtime_->screen_height_points * runtime_->screen_scale));
        if (runtime_->gl_state.viewport[2] == 0 || runtime_->gl_state.viewport[3] == 0) {
            runtime_->gl_state.viewport = {0, 0, renderbuffer.width, renderbuffer.height};
        }
        if (runtime_->gl_state.scissor[2] == 0 || runtime_->gl_state.scissor[3] == 0) {
            runtime_->gl_state.scissor = {0, 0, renderbuffer.width, renderbuffer.height};
        }
        Log("[gl] drawable storage points="
            + std::to_string(runtime_->screen_width_points) + "x" + std::to_string(runtime_->screen_height_points)
            + " scale=" + std::to_string(runtime_->screen_scale)
            + " pixels=" + std::to_string(renderbuffer.width) + "x" + std::to_string(renderbuffer.height));
        if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported()) {
            runtime_->host_gl->EnsureWindow(
                static_cast<int>(runtime_->screen_width_points),
                static_cast<int>(runtime_->screen_height_points),
                binary_path_.filename().string());
            runtime_->host_gl->MakeCurrent();
            if (auto viewport = LookupHostGLProc<void (*)(HostGLint, HostGLint, HostGLsizei, HostGLsizei)>(runtime_->host_gl.get(), "glViewport"); viewport != nullptr) {
                viewport(0, 0, renderbuffer.width, renderbuffer.height);
            }
            if (auto scissor = LookupHostGLProc<void (*)(HostGLint, HostGLint, HostGLsizei, HostGLsizei)>(runtime_->host_gl.get(), "glScissor"); scissor != nullptr) {
                scissor(0, 0, renderbuffer.width, renderbuffer.height);
            }
        }
        return 1;
    }
    if (selector_name == "presentRenderbuffer:" && class_name == "EAGLContext") {
        const auto renderbuffer_it = runtime_->gl_renderbuffers.find(runtime_->drawable_renderbuffer);
        const s32 renderbuffer_width = renderbuffer_it == runtime_->gl_renderbuffers.end() ? 0 : renderbuffer_it->second.width;
        const s32 renderbuffer_height = renderbuffer_it == runtime_->gl_renderbuffers.end() ? 0 : renderbuffer_it->second.height;
        ++runtime_->debug_present_count;
        if (runtime_->gl_state.debug_present_logs < 8) {
            ++runtime_->gl_state.debug_present_logs;
            Log("[gl] presentRenderbuffer viewport="
                + std::to_string(runtime_->gl_state.viewport[0]) + "," + std::to_string(runtime_->gl_state.viewport[1])
                + "," + std::to_string(runtime_->gl_state.viewport[2]) + "," + std::to_string(runtime_->gl_state.viewport[3])
                + " scissor="
                + std::to_string(runtime_->gl_state.scissor[0]) + "," + std::to_string(runtime_->gl_state.scissor[1])
                + "," + std::to_string(runtime_->gl_state.scissor[2]) + "," + std::to_string(runtime_->gl_state.scissor[3])
                + " drawable=" + std::to_string(renderbuffer_width) + "x" + std::to_string(renderbuffer_height));
        }
        if (runtime_->debug_mode_logs < 2048) {
            constexpr u32 kMessageManagerSingleton = 0x001FB668;
            constexpr u32 kGameModeSingleton = 0x001FB66C;
            constexpr u32 kModeManagerSingleton = 0x001FB678;
            constexpr u32 kResourceManagerQueueGlobal = 0x001FBC78;
            const auto read32 = [&](const u32 address) -> u32 {
                return address != 0 && address <= 0xFFFFFFFCu
                    && memory_.IsMapped(address) && memory_.IsMapped(address + 3) ? memory_.Read32(address) : 0;
            };
            const auto count_vector_ptrs = [&](const u32 vector_object) -> u32 {
                const u32 begin = read32(vector_object);
                const u32 end = read32(vector_object + 4);
                if (begin == 0 || end < begin) {
                    return 0;
                }
                return (end - begin) / 4;
            };
            const auto append_game_mode = [&](std::string& out, const std::string& prefix, const u32 mode) {
                if (mode == 0) {
                    out += " " + prefix + "=0x0";
                    return;
                }
                const u32 clips_begin = read32(mode + 0x10);
                const u32 clips_end = read32(mode + 0x14);
                const u32 clips_count = clips_end >= clips_begin ? (clips_end - clips_begin) / 4 : 0;
                const u32 logic = read32(mode + 0x88);
                out += " " + prefix + "=" + Hex32(mode)
                    + " " + prefix + "_base_total=" + std::to_string(read32(mode + 0x04))
                    + " " + prefix + "_listener=" + Hex32(read32(mode + 0x08))
                    + " " + prefix + "_load_home=" + Hex32(read32(mode + 0x1C))
                    + " " + prefix + "_load_avatar=" + Hex32(read32(mode + 0x20))
                    + " " + prefix + "_load_enemy=" + Hex32(read32(mode + 0x24))
                    + " " + prefix + "_load_arg=" + Hex32(read32(mode + 0x28))
                    + " " + prefix + "_load_kind=" + std::to_string(read32(mode + 0x34))
                    + " " + prefix + "_transition=" + std::to_string(read32(mode + 0x7C))
                    + " " + prefix + "_logic=" + Hex32(logic)
                    + " " + prefix + "_logic_state=" + std::to_string(read32(logic + 0x04))
                    + " " + prefix + "_bg=" + Hex32(read32(mode + 0x94))
                    + " " + prefix + "_clips=" + std::to_string(clips_count);
            };
            const u32 message_manager = read32(kMessageManagerSingleton);
            const u32 game_mode = read32(kGameModeSingleton);
            const u32 mode_manager = read32(kModeManagerSingleton);
            const u32 resource_queue = read32(kResourceManagerQueueGlobal);
            const u32 resource_queue_begin = read32(resource_queue);
            const u32 resource_queue_front = read32(resource_queue_begin);
            const u32 resource_queue_count = count_vector_ptrs(resource_queue);
            std::string signature = "mm=" + Hex32(mode_manager)
                + " msg=" + Hex32(message_manager)
                + " gm=" + Hex32(game_mode)
                + " res_queue=" + Hex32(resource_queue)
                + " res_count=" + std::to_string(resource_queue_count)
                + " res_front=" + Hex32(resource_queue_front);
            u32 current_mode_id = 0;
            u32 current_mode = 0;
            u32 pending_mode_id = 0;
            if (mode_manager != 0) {
                current_mode_id = read32(mode_manager + 0x00);
                current_mode = read32(mode_manager + 0x04);
                pending_mode_id = read32(mode_manager + 0x08);
                const u32 loading_screen = read32(mode_manager + 0x0C);
                const u32 buffered_commands_begin = read32(mode_manager + 0x30);
                const u32 buffered_commands_end = read32(mode_manager + 0x34);
                const u32 buffered_command_count =
                    (buffered_commands_begin != 0 && buffered_commands_end >= buffered_commands_begin)
                    ? ((buffered_commands_end - buffered_commands_begin) / 4)
                    : 0;
                signature += " cur_id=" + std::to_string(current_mode_id)
                    + " cur=" + Hex32(current_mode)
                    + " pending=" + std::to_string(pending_mode_id)
                    + " loading=" + Hex32(loading_screen)
                    + " home=" + Hex32(read32(mode_manager + 0x14))
                    + " avatar=" + Hex32(read32(mode_manager + 0x18))
                    + " arg1c=" + Hex32(read32(mode_manager + 0x1C))
                    + " mode_vtbl=" + Hex32(current_mode != 0 ? read32(current_mode) : 0)
                    + " cmd_buf=" + std::to_string(buffered_command_count)
                    + " cmd_begin=" + Hex32(buffered_commands_begin)
                    + " cmd_end=" + Hex32(buffered_commands_end);
            }
            append_game_mode(signature, "gm", game_mode);
            if (current_mode_id == 2 && current_mode != 0) {
                const u32 next_mode_id = read32(current_mode + 0x0C);
                const u32 next_mode = read32(current_mode + 0x10);
                signature += " lm_next_id=" + std::to_string(next_mode_id)
                    + " lm_next=" + Hex32(next_mode)
                    + " lm_box=" + Hex32(read32(current_mode + 0x14));
                append_game_mode(signature, "lm_next", next_mode);
            }
            const bool is_loading_transition = current_mode_id == 2 || pending_mode_id != 0;
            if (signature != runtime_->debug_last_mode_signature
                || (is_loading_transition && (runtime_->debug_present_count % 15) == 0)
                || (runtime_->debug_present_count % 120) == 0) {
                runtime_->debug_last_mode_signature = signature;
                ++runtime_->debug_mode_logs;
                Log("[mode] " + signature);
            }
        }
        if (runtime_->host_gl != nullptr && runtime_->host_gl->IsSupported()) {
            runtime_->host_gl->MakeCurrent();
            if (runtime_->gl_state.framebuffer != runtime_->drawable_framebuffer
                && runtime_->gl_state.debug_present_error_logs < 32) {
                ++runtime_->gl_state.debug_present_error_logs;
                Log("[gl] present while guest fb=" + Hex32(runtime_->gl_state.framebuffer)
                    + " drawable_fb=" + Hex32(runtime_->drawable_framebuffer));
            }
            if (runtime_->debug_present_probe_logs < 8 && renderbuffer_width > 0 && renderbuffer_height > 0) {
                auto read_pixels = LookupHostGLProc<void (*)(HostGLint, HostGLint, HostGLsizei, HostGLsizei, HostGLenum, HostGLenum, void*)>(runtime_->host_gl.get(), "glReadPixels");
                auto pixel_store_i = LookupHostGLProc<void (*)(HostGLenum, HostGLint)>(runtime_->host_gl.get(), "glPixelStorei");
                auto get_error = LookupHostGLProc<HostGLenum (*)()>(runtime_->host_gl.get(), "glGetError");
                if (read_pixels != nullptr) {
                    if (pixel_store_i != nullptr) {
                        pixel_store_i(0x0D05, 1);
                    }
                    const std::array<std::array<s32, 2>, 5> points{{
                        {{0, 0}},
                        {{renderbuffer_width / 2, renderbuffer_height / 2}},
                        {{renderbuffer_width - 1, renderbuffer_height - 1}},
                        {{renderbuffer_width / 4, renderbuffer_height / 4}},
                        {{(renderbuffer_width * 3) / 4, (renderbuffer_height * 3) / 4}},
                    }};
                    u32 nonzero = 0;
                    u32 alpha = 0;
                    u32 checksum = 2166136261u;
                    std::array<HostGLubyte, 4> first_pixel{0, 0, 0, 0};
                    for (const auto& point : points) {
                        std::array<HostGLubyte, 4> pixel{0, 0, 0, 0};
                        read_pixels(
                            static_cast<HostGLint>(std::clamp<s32>(point[0], 0, renderbuffer_width - 1)),
                            static_cast<HostGLint>(std::clamp<s32>(point[1], 0, renderbuffer_height - 1)),
                            1,
                            1,
                            0x1908,
                            0x1401,
                            pixel.data());
                        if (point == points.front()) {
                            first_pixel = pixel;
                        }
                        if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
                            ++nonzero;
                        }
                        if (pixel[3] != 0) {
                            ++alpha;
                        }
                        for (const HostGLubyte byte : pixel) {
                            checksum ^= byte;
                            checksum *= 16777619u;
                        }
                    }
                    const u32 read_error = get_error != nullptr ? static_cast<u32>(get_error()) : 0u;
                    ++runtime_->debug_present_probe_logs;
                    Log("[gl] present probe fb=" + Hex32(runtime_->gl_state.framebuffer)
                        + " samples_nonzero=" + std::to_string(nonzero)
                        + " alpha_nonzero=" + std::to_string(alpha)
                        + " first_rgba="
                        + std::to_string(first_pixel[0]) + ","
                        + std::to_string(first_pixel[1]) + ","
                        + std::to_string(first_pixel[2]) + ","
                        + std::to_string(first_pixel[3])
                        + " checksum=" + Hex32(checksum)
                        + " error=" + Hex32(read_error));
                }
            }
            runtime_->host_gl->Present();
            runtime_->host_gl->PumpEvents();
        }
        return 1;
    }

    if (selector_name == "UTF8String" || selector_name == "cString" || selector_name == "cStringUsingEncoding:") {
        if (object.kind == ObjKind::String && object.backing_store == 0) {
            object.backing_store = AllocateCString(object.string_value, "NSString.bytes");
            memory_.Write32(self + 4, object.backing_store);
            memory_.Write32(self + 8, object.backing_store);
            memory_.Write32(self + 12, static_cast<u32>(object.string_value.size()));
        }
        return object.backing_store;
    }
    if (selector_name == "length") {
        if (object.kind == ObjKind::String) {
            return static_cast<u32>(object.string_value.size());
        }
        if (object.kind == ObjKind::Data) {
            return static_cast<u32>(object.bytes.size());
        }
        return 0;
    }
    if (selector_name == "lengthOfBytesUsingEncoding:" && object.kind == ObjKind::String) {
        return static_cast<u32>(object.string_value.size());
    }
    if (selector_name == "characterAtIndex:" && object.kind == ObjKind::String) {
        const u32 index = Arg(2);
        if (index >= object.string_value.size()) {
            return 0;
        }
        return static_cast<unsigned char>(object.string_value[index]);
    }
    if (selector_name == "count") {
        if (object.kind == ObjKind::Array) {
            return static_cast<u32>(object.items.size());
        }
        if (object.kind == ObjKind::Dictionary) {
            return static_cast<u32>(object.dict.size());
        }
        return 0;
    }
    if (selector_name == "lastObject" && object.kind == ObjKind::Array) {
        return object.items.empty() ? 0u : object.items.back();
    }
    if ((selector_name == "firstObject" || selector_name == "anyObject") && object.kind == ObjKind::Array) {
        return object.items.empty() ? 0u : object.items.front();
    }
    if (selector_name == "objectEnumerator" && object.kind == ObjKind::Array) {
        object.dict["enumerator.index"] = 0;
        return self;
    }
    if (selector_name == "nextObject" && object.kind == ObjKind::Array) {
        const u32 index = object.dict.contains("enumerator.index") ? object.dict["enumerator.index"] : 0u;
        if (index >= object.items.size()) {
            return 0;
        }
        object.dict["enumerator.index"] = index + 1;
        return object.items[index];
    }
    if (selector_name == "allObjects" && object.kind == ObjKind::Array) {
        return EnsureArray(object.items);
    }
    if ((selector_name == "allKeys" || selector_name == "keyEnumerator") && object.kind == ObjKind::Dictionary) {
        std::vector<u32> keys;
        keys.reserve(object.dict.size());
        for (const auto& [key, _] : object.dict) {
            keys.push_back(EnsureNSString(key));
        }
        return EnsureArray(keys);
    }
    if ((selector_name == "allValues" || selector_name == "objectEnumerator") && object.kind == ObjKind::Dictionary) {
        std::vector<u32> values;
        values.reserve(object.dict.size());
        for (const auto& [_, value] : object.dict) {
            values.push_back(value);
        }
        return EnsureArray(values);
    }
    if (selector_name == "countByEnumeratingWithState:objects:count:"
        && (object.kind == ObjKind::Array || object.kind == ObjKind::Dictionary)) {
        const u32 state_ptr = Arg(2);
        const u32 objects_ptr = Arg(3);
        const u32 max_count = Arg(4);
        if (state_ptr == 0 || objects_ptr == 0 || max_count == 0) {
            return 0;
        }

        std::vector<u32> values;
        if (object.kind == ObjKind::Array) {
            values = object.items;
        } else {
            values.reserve(object.dict.size());
            for (const auto& [key, _] : object.dict) {
                values.push_back(EnsureNSString(key));
            }
        }

        const u32 index = memory_.Read32(state_ptr);
        if (index >= values.size()) {
            return 0;
        }
        const u32 count = std::min<u32>(max_count, static_cast<u32>(values.size()) - index);
        for (u32 i = 0; i < count; ++i) {
            memory_.Write32(objects_ptr + i * 4, values[index + i]);
        }
        memory_.Write32(state_ptr, index + count);
        memory_.Write32(state_ptr + 4, objects_ptr);
        memory_.Write32(state_ptr + 8, state_ptr + 12);
        memory_.Write32(state_ptr + 12, static_cast<u32>(values.size()));
        return count;
    }
    if (selector_name == "objectAtIndex:" && object.kind == ObjKind::Array) {
        const u32 index = Arg(2);
        return index < object.items.size() ? object.items[index] : 0;
    }
    if (selector_name == "addObject:" && object.kind == ObjKind::Array) {
        object.items.push_back(Arg(2));
        return 0;
    }
    if ((selector_name == "objectForKey:" || selector_name == "valueForKey:") && object.kind == ObjKind::Dictionary) {
        const auto key = DecodeNSString(Arg(2));
        if (!key) {
            return 0;
        }
        const auto it = object.dict.find(*key);
        return it == object.dict.end() ? 0 : it->second;
    }
    if ((selector_name == "setObject:forKey:" || selector_name == "setValue:forKey:") && object.kind == ObjKind::Dictionary) {
        const auto key = DecodeNSString(Arg(3));
        if (key) {
            object.dict[*key] = Arg(2);
        }
        return 0;
    }
    if (selector_name == "removeObjectForKey:" && object.kind == ObjKind::Dictionary) {
        if (const auto key = DecodeNSString(Arg(2))) {
            object.dict.erase(*key);
        }
        return 0;
    }
    if (selector_name == "keysSortedByValueUsingSelector:" && object.kind == ObjKind::Dictionary) {
        std::vector<std::string> keys;
        keys.reserve(object.dict.size());
        for (const auto& [key, _] : object.dict) {
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end(), [&](const std::string& lhs, const std::string& rhs) {
            const auto lhs_it = object.dict.find(lhs);
            const auto rhs_it = object.dict.find(rhs);
            const u32 lhs_value = lhs_it == object.dict.end() ? 0 : lhs_it->second;
            const u32 rhs_value = rhs_it == object.dict.end() ? 0 : rhs_it->second;

            const auto host_compare = [&](const u32 left, const u32 right) -> int {
                const auto left_it = host_objects_.find(left);
                const auto right_it = host_objects_.find(right);
                if (left_it != host_objects_.end() && right_it != host_objects_.end()) {
                    if ((left_it->second.kind == ObjKind::Number || left_it->second.kind == ObjKind::Boolean)
                        && (right_it->second.kind == ObjKind::Number || right_it->second.kind == ObjKind::Boolean)) {
                        if (left_it->second.number_value < right_it->second.number_value) {
                            return -1;
                        }
                        if (left_it->second.number_value > right_it->second.number_value) {
                            return 1;
                        }
                        return 0;
                    }
                }
                const std::string left_text = DescribeNSObject(left);
                const std::string right_text = DescribeNSObject(right);
                if (left_text < right_text) {
                    return -1;
                }
                if (left_text > right_text) {
                    return 1;
                }
                return 0;
            };

            const int cmp = host_compare(lhs_value, rhs_value);
            return cmp == 0 ? lhs < rhs : cmp < 0;
        });
        std::vector<u32> key_objects;
        key_objects.reserve(keys.size());
        for (const auto& key : keys) {
            key_objects.push_back(EnsureNSString(key));
        }
        return EnsureArray(key_objects);
    }
    if (selector_name == "bytes" && object.kind == ObjKind::Data) {
        return object.backing_store;
    }
    if (selector_name == "dataUsingEncoding:" && object.kind == ObjKind::String) {
        return EnsureNSData(std::vector<u8>(object.string_value.begin(), object.string_value.end()));
    }
    if (selector_name == "writeToFile:atomically:") {
        const auto path = DecodeNSString(Arg(2));
        if (!path) {
            return 0;
        }
        std::ofstream output(ResolveGuestPath(*path), std::ios::binary);
        if (!output) {
            return 0;
        }
        if (object.kind == ObjKind::String) {
            output.write(object.string_value.data(), static_cast<std::streamsize>(object.string_value.size()));
        } else if (object.kind == ObjKind::Data) {
            output.write(reinterpret_cast<const char*>(object.bytes.data()), static_cast<std::streamsize>(object.bytes.size()));
        }
        return 1;
    }
    if (selector_name == "initWithContentsOfFile:") {
        const auto path = DecodeNSString(Arg(2));
        if (!path) {
            return 0;
        }
        const auto data = load_guest_file_bytes(*path);
        if (!data) {
            return 0;
        }
        if (object.kind == ObjKind::Data) {
            object.bytes = *data;
            object.backing_store = AllocateData(static_cast<u32>(data->size() == 0 ? 1 : data->size()), 4, "NSData.file");
            if (!data->empty()) {
                memory_.WriteBuffer(object.backing_store, *data);
            }
            memory_.Write32(self + 4, object.backing_store);
            memory_.Write32(self + 8, static_cast<u32>(data->size()));
            return self;
        }
        return EnsureNSData(*data);
    }
    if (selector_name == "contentsAtPath:" && class_name == "NSFileManager") {
        const auto path = DecodeNSString(Arg(2));
        if (!path) {
            return 0;
        }
        const auto data = load_guest_file_bytes(*path);
        return data ? EnsureNSData(*data) : 0u;
    }
    if (selector_name == "contentsOfDirectoryAtPath:error:" && class_name == "NSFileManager") {
        const auto path = DecodeNSString(Arg(2));
        if (!path) {
            if (Arg(3) != 0) {
                memory_.Write32(Arg(3), EnsureNSError("NSCocoaErrorDomain", ENOENT, EnsureDictionary({{"description", EnsureNSString("Invalid directory path")}})));
            }
            return 0;
        }
        const auto host_path = ResolveGuestPath(*path);
        std::error_code error;
        std::vector<u32> entries;
        if (std::filesystem::exists(host_path, error) && std::filesystem::is_directory(host_path, error)) {
            for (const auto& entry : std::filesystem::directory_iterator(host_path, error)) {
                entries.push_back(EnsureNSString(entry.path().filename().generic_string()));
            }
            std::sort(entries.begin(), entries.end(), [&](const u32 lhs, const u32 rhs) {
                return DescribeNSObject(lhs) < DescribeNSObject(rhs);
            });
            if (Arg(3) != 0) {
                memory_.Write32(Arg(3), 0);
            }
            return EnsureArray(entries);
        }
        if (Arg(3) != 0) {
            memory_.Write32(Arg(3), EnsureNSError("NSCocoaErrorDomain", error.value() == 0 ? ENOENT : error.value(), EnsureDictionary({{"description", EnsureNSString("Unable to enumerate directory")}})));
        }
        return 0;
    }
    if (selector_name == "createDirectoryAtPath:withIntermediateDirectories:attributes:error:" && class_name == "NSFileManager") {
        const auto path = DecodeNSString(Arg(2));
        if (!path) {
            if (Arg(5) != 0) {
                memory_.Write32(Arg(5), EnsureNSError("NSCocoaErrorDomain", EINVAL, EnsureDictionary({{"description", EnsureNSString("Invalid directory path")}})));
            }
            return 0;
        }
        const auto host_path = ResolveGuestPath(*path);
        std::error_code error;
        const bool ok = Arg(3) != 0
            ? std::filesystem::create_directories(host_path, error) || std::filesystem::is_directory(host_path, error)
            : (std::filesystem::create_directory(host_path, error) || std::filesystem::is_directory(host_path, error));
        if (Arg(5) != 0) {
            memory_.Write32(Arg(5), ok ? 0u : EnsureNSError("NSCocoaErrorDomain", error.value() == 0 ? EIO : error.value(), EnsureDictionary({{"description", EnsureNSString("Unable to create directory")}})));
        }
        return ok ? 1u : 0u;
    }
    if (selector_name == "stringByAppendingString:" && object.kind == ObjKind::String) {
        const auto rhs = DecodeNSString(Arg(2));
        return EnsureNSString(object.string_value + rhs.value_or(""));
    }
    if ((selector_name == "appendString:" || selector_name == "setString:") && object.kind == ObjKind::String) {
        const std::string rhs = DecodeNSString(Arg(2)).value_or("");
        if (selector_name == "appendString:") {
            object.string_value += rhs;
        } else {
            object.string_value = rhs;
        }
        object.backing_store = 0;
        memory_.Write32(self + 4, 0);
        memory_.Write32(self + 8, 0);
        memory_.Write32(self + 12, static_cast<u32>(object.string_value.size()));
        return self;
    }
    if (selector_name == "deleteCharactersInRange:" && object.kind == ObjKind::String) {
        const u32 location = Arg(2);
        const u32 length = Arg(3);
        if (location < object.string_value.size()) {
            object.string_value.erase(
                static_cast<std::size_t>(location),
                static_cast<std::size_t>(std::min<u32>(length, static_cast<u32>(object.string_value.size()) - location)));
        }
        object.backing_store = 0;
        memory_.Write32(self + 4, 0);
        memory_.Write32(self + 8, 0);
        memory_.Write32(self + 12, static_cast<u32>(object.string_value.size()));
        return self;
    }
    if (selector_name == "stringByAppendingPathComponent:" && object.kind == ObjKind::String) {
        const auto rhs = DecodeNSString(Arg(2)).value_or("");
        const auto joined = (std::filesystem::path(object.string_value) / rhs).generic_string();
        return EnsureNSString(joined);
    }
    if (selector_name == "stringByDeletingLastPathComponent" && object.kind == ObjKind::String) {
        return EnsureNSString(std::filesystem::path(object.string_value).parent_path().generic_string());
    }
    if (selector_name == "lastPathComponent" && object.kind == ObjKind::String) {
        return EnsureNSString(std::filesystem::path(object.string_value).filename().generic_string());
    }
    if (selector_name == "pathExtension" && object.kind == ObjKind::String) {
        auto ext = std::filesystem::path(object.string_value).extension().generic_string();
        if (!ext.empty() && ext.front() == '.') {
            ext.erase(ext.begin());
        }
        return EnsureNSString(ext);
    }
    if (selector_name == "compare:" && object.kind == ObjKind::String) {
        const auto rhs = DecodeNSString(Arg(2)).value_or("");
        if (object.string_value < rhs) {
            return static_cast<u32>(-1);
        }
        if (object.string_value > rhs) {
            return 1;
        }
        return 0;
    }
    if (selector_name == "compare:" && (object.kind == ObjKind::Number || object.kind == ObjKind::Boolean)) {
        const auto rhs_it = host_objects_.find(Arg(2));
        const double rhs = rhs_it == host_objects_.end() ? 0.0 : rhs_it->second.number_value;
        if (object.number_value < rhs) {
            return static_cast<u32>(-1);
        }
        if (object.number_value > rhs) {
            return 1;
        }
        return 0;
    }
    if (selector_name == "isEqualToString:" && object.kind == ObjKind::String) {
        const auto rhs = DecodeNSString(Arg(2));
        return rhs && *rhs == object.string_value ? 1u : 0u;
    }
    if (selector_name == "globallyUniqueString" && class_name == "NSProcessInfo") {
        return EnsureNSString(GenerateUuidString() + "-" + GenerateHexToken(4));
    }
    if (selector_name == "isMainThread" && class_name == "NSThread") {
        const auto it = object.dict.find("isMainThread");
        return it != object.dict.end() && it->second != 0 ? 1u : (self == runtime_->current_thread ? 1u : 0u);
    }
    if ((selector_name == "localizedDescription" || selector_name == "domain") && class_name == "NSError") {
        const auto key = selector_name == "localizedDescription" ? std::string("description") : std::string("domain");
        const auto it = object.dict.find(key);
        return it == object.dict.end() ? EnsureNSString(object.string_value) : it->second;
    }
    if (selector_name == "code" && class_name == "NSError") {
        return static_cast<u32>(object.number_value);
    }
    if (selector_name == "userInfo" && class_name == "NSError") {
        const auto it = object.dict.find("userInfo");
        return it == object.dict.end() ? 0u : it->second;
    }
    if (selector_name == "timeIntervalSince1970" && class_name == "NSDate") {
        SetReturnDouble(object.number_value);
        return cpu_->Regs()[0];
    }
    if (class_name == "UIPasteboard" && selector_name == "setPersistent:") {
        object.dict["persistent"] = Arg(2) != 0 ? 1u : 0u;
        return 0;
    }
    if (class_name == "UIPasteboard" && selector_name == "persistent") {
        const auto it = object.dict.find("persistent");
        return it != object.dict.end() && it->second != 0 ? 1u : 0u;
    }
    if (class_name == "UIPasteboard" && selector_name == "dataForPasteboardType:") {
        const auto type = DecodeNSString(Arg(2)).value_or("");
        if (type.empty()) {
            return 0;
        }
        const auto it = object.dict.find("pb:" + type);
        return it == object.dict.end() ? 0u : it->second;
    }
    if (class_name == "UIPasteboard" && selector_name == "setData:forPasteboardType:") {
        const auto type = DecodeNSString(Arg(3)).value_or("");
        if (!type.empty()) {
            object.dict["pb:" + type] = Arg(2);
        }
        return 0;
    }
    if (selector_name == "absoluteString" || selector_name == "path") {
        return EnsureNSString(object.string_value);
    }
    if (selector_name == "compare:options:" && object.kind == ObjKind::String) {
        const auto rhs = DecodeNSString(Arg(2)).value_or("");
        if (object.string_value < rhs) {
            return static_cast<u32>(-1);
        }
        if (object.string_value > rhs) {
            return 1;
        }
        return 0;
    }
    if (selector_name == "rangeOfString:" && object.kind == ObjKind::String && stret) {
        const auto needle = DecodeNSString(Arg(2)).value_or("");
        const std::size_t position = object.string_value.find(needle);
        memory_.Write32(stret_buffer + 0, position == std::string::npos ? 0xFFFFFFFFu : static_cast<u32>(position));
        memory_.Write32(stret_buffer + 4, position == std::string::npos ? 0u : static_cast<u32>(needle.size()));
        return 0;
    }
    if (selector_name == "intValue" || selector_name == "integerValue") {
        return static_cast<u32>(object.number_value);
    }
    if (selector_name == "boolValue") {
        return object.boolean_value ? 1u : 0u;
    }
    if (selector_name == "floatValue" || selector_name == "doubleValue") {
        SetReturnDouble(object.number_value);
        return cpu_->Regs()[0];
    }
    if (selector_name == "setObject:forKeyedSubscript:" && object.kind == ObjKind::Dictionary) {
        const auto key = DecodeNSString(Arg(3));
        if (key) {
            object.dict[*key] = Arg(2);
        }
        return 0;
    }
    if (class_name == "UIDevice" && selector_name == "systemVersion") {
        return EnsureNSString("4.2");
    }
    if (class_name == "UIDevice" && selector_name == "systemName") {
        return EnsureNSString("iPhone OS");
    }
    if (class_name == "UIDevice" && selector_name == "model") {
        return EnsureNSString("iPhone");
    }
    if (class_name == "UIDevice" && selector_name == "machine") {
        return EnsureNSString("iPhone3,1");
    }
    if (selector_name == "objectForKeyedSubscript:" && object.kind == ObjKind::Dictionary) {
        const auto key = DecodeNSString(Arg(2));
        if (!key) {
            return 0;
        }
        const auto it = object.dict.find(*key);
        return it == object.dict.end() ? 0 : it->second;
    }
    if (selector_name.size() > 4 && StartsWith(selector_name, "set") && selector_name.back() == ':') {
        object.dict[selector_name] = Arg(2);
        return 0;
    }
    if (const auto property_it = object.dict.find(property_key_for_getter(selector_name)); property_it != object.dict.end()) {
        return property_it->second;
    }
    if (selector_name == "stret" && stret) {
        memory_.Write32(stret_buffer, 0);
        return 0;
    }

    if (trace_shims_) {
        Log("objc fallback [" + class_name + " " + selector_name + "] -> " + Hex32(self));
    }
    return self;
}

u32 Emulator::HandleGenericFunction(const std::string& name) {
    auto handle_objc_message_send = [&](const u32 raw_self, const u32 selector, const bool is_super, const bool stret, const u32 stret_buffer) {
        const u32 receiver = is_super ? memory_.Read32(raw_self) : raw_self;
        const u32 current_class = is_super ? memory_.Read32(raw_self + 4) : 0;
        EnsureGuestAnnotation(receiver);
        const std::string selector_name = ReadGuestCString(selector);
        ObserveObjcKeyboardMessage(receiver, selector_name);

        u32 override_result = 0;
        if (TryHandleObjcOverride(receiver, selector_name, &override_result)) {
            cpu_->Regs()[12] = cpu_->Regs()[14];
            cpu_->Regs()[0] = override_result;
            return override_result;
        }

        if (objc_abi_ != nullptr) {
            if (const auto imp = objc_abi_->LookupMethodImp(receiver, selector_name, is_super, current_class)) {
                if (trace_shims_) {
                    const std::string class_name = objc_abi_->ClassNameForReceiver(receiver).value_or("NSObject");
                    Log("objc guest " + Hex32(receiver) + " [" + class_name + " " + selector_name + "] -> " + Hex32(*imp));
                }
                if (is_super) {
                    cpu_->Regs()[0] = receiver;
                }
                cpu_->Regs()[12] = *imp;
                return cpu_->Regs()[0];
            }
        }

        cpu_->Regs()[12] = cpu_->Regs()[14];
        return DispatchObjCMessage(raw_self, selector, is_super, stret, stret_buffer);
    };

    if (name == "_objc_msgSend") {
        return handle_objc_message_send(Arg(0), Arg(1), false, false, 0);
    }
    if (name == "_objc_msgSendSuper2") {
        return handle_objc_message_send(Arg(0), Arg(1), true, false, 0);
    }
    if (name == "_objc_msgSend_stret") {
        return handle_objc_message_send(Arg(1), Arg(2), false, true, Arg(0));
    }
    if (name == "_objc_getProperty") {
        if (objc_abi_ != nullptr && (objc_abi_->IsGuestObject(Arg(0)) || objc_abi_->IsGuestClass(Arg(0)) || objc_abi_->IsGuestMetaClass(Arg(0)))) {
            return memory_.Read32(Arg(0) + Arg(2));
        }
        const auto it = host_objects_.find(Arg(0));
        if (it == host_objects_.end()) {
            return 0;
        }
        const auto prop = it->second.properties_by_offset.find(Arg(2));
        return prop == it->second.properties_by_offset.end() ? 0 : prop->second;
    }
    if (name == "_objc_setProperty") {
        if (objc_abi_ != nullptr && (objc_abi_->IsGuestObject(Arg(0)) || objc_abi_->IsGuestClass(Arg(0)) || objc_abi_->IsGuestMetaClass(Arg(0)))) {
            memory_.Write32(Arg(0) + Arg(2), Arg(3));
            return 0;
        }
        host_objects_[Arg(0)].properties_by_offset[Arg(2)] = Arg(3);
        return 0;
    }
    if (name == "_objc_setAssociatedObject") {
        host_objects_[Arg(0)].properties_by_offset[Arg(1)] = Arg(2);
        return 0;
    }
    if (name == "_objc_sync_enter" || name == "_objc_sync_exit") {
        return 0;
    }
    if (name == "_objc_copyStruct") {
        memory_.WriteBuffer(Arg(0), memory_.ReadBuffer(Arg(1), Arg(2)));
        return 0;
    }
    if (name == "_objc_begin_catch") {
        return Arg(0);
    }
    if (name == "_objc_end_catch" || name == "_objc_exception_rethrow" || name == "_objc_enumerationMutation") {
        return 0;
    }
    if (name == "_objc_exception_throw" || name == "___cxa_pure_virtual" || name == "__ZSt9terminatev"
        || name == "___cxa_rethrow" || name == "___cxa_call_unexpected"
        || name == "__ZSt17__throw_bad_allocv" || name == "__ZSt20__throw_length_errorPKc") {
        exit_code_ = 1;
        Stop(Dynarmic::HaltReason::UserDefined3);
        return 0;
    }
    if (name == "_NSHomeDirectory") {
        return EnsureNSString(guest_home_);
    }
    if (name == "_NSTemporaryDirectory") {
        return EnsureNSString(guest_tmp_);
    }
    if (name == "_NSClassFromString") {
        return EnsureClass(DecodeNSString(Arg(0)).value_or("NSObject"));
    }
    if (name == "_NSSelectorFromString") {
        return AllocateCString(DecodeNSString(Arg(0)).value_or(""), "selector");
    }
    if (name == "_NSStringFromClass") {
        const auto it = host_objects_.find(Arg(0));
        return EnsureNSString(it == host_objects_.end() ? "NSObject" : it->second.class_name);
    }
    if (name == "_NSStringFromSelector") {
        return EnsureNSString(ReadGuestCString(Arg(0)));
    }
    if (name == "_NSSearchPathForDirectoriesInDomains") {
        return EnsureArray({EnsureNSString(JoinPathForGuest(guest_home_, "Documents"))});
    }
    if (name == "_NSLog") {
        if (auto text = DecodeNSString(Arg(0))) {
            Log("NSLog: " + *text);
        }
        return 0;
    }
    if (name == "_NSLogv") {
        if (auto text = DecodeNSString(Arg(0))) {
            Log("NSLogv: " + *text);
        }
        return 0;
    }
    if (name == "_UIApplicationMain") {
        const std::string principal_class = DecodeNSString(Arg(2)).value_or("UIApplication");
        const std::string delegate_class = DecodeNSString(Arg(3)).value_or("AppController");
        return BeginUIApplicationMain(principal_class, delegate_class);
    }
    if (name == "_NSStringFromCGSize") {
        const float w = BitsToFloat(Arg(0));
        const float h = BitsToFloat(Arg(1));
        return EnsureNSString("{" + std::to_string(w) + ", " + std::to_string(h) + "}");
    }
    if (name == "_NSStringFromCGRect") {
        const float x = BitsToFloat(Arg(0));
        const float y = BitsToFloat(Arg(1));
        const float w = BitsToFloat(Arg(2));
        const float h = BitsToFloat(Arg(3));
        return EnsureNSString("{{" + std::to_string(x) + ", " + std::to_string(y) + "}, {" + std::to_string(w) + ", " + std::to_string(h) + "}}");
    }
    auto update_generated_bitmap_skip = [&](RuntimeState::GraphicsImage& image, const std::string& label) {
        const BitmapContentStats stats =
            AnalyzeBitmapContent(image.width, image.height, image.bytes_per_row, image.pixels);
        image.skip_draw = ShouldSkipGeneratedBitmapImage(stats);
        if (runtime_->debug_uigraphics_image_logs < 48) {
            ++runtime_->debug_uigraphics_image_logs;
            Log("[text] " + label
                + " size=" + std::to_string(image.width) + "x" + std::to_string(image.height)
                + " stride=" + std::to_string(image.bytes_per_row)
                + " sampled=" + std::to_string(stats.sampled)
                + " alpha=" + std::to_string(stats.alpha_nonzero)
                + " rgb=" + std::to_string(stats.rgb_nonzero)
                + " visible=" + std::to_string(stats.visible_rgb_nonzero)
                + " dark=" + std::to_string(stats.opaque_dark)
                + " skip=" + std::to_string(image.skip_draw ? 1 : 0));
        }
    };
    auto make_uigraphics_bitmap_context = [&](const float width_points, const float height_points, const float scale) {
        const u32 handle = AllocateData(kObjcObjectSize, 4, "UIGraphics.CGContext");
        host_objects_[handle] = HostObject{
            .kind = ObjKind::Generic,
            .class_name = "CGContext",
            .isa = EnsureClass("NSObject")
        };
        memory_.Write32(handle, EnsureClass("NSObject"));
        auto& context = runtime_->graphics_contexts[handle];
        const float resolved_scale = scale > 0.0f ? scale : std::max(1.0f, runtime_->screen_scale);
        context.width = std::max<s32>(1, static_cast<s32>(std::ceil(std::max(0.0f, width_points) * resolved_scale)));
        context.height = std::max<s32>(1, static_cast<s32>(std::ceil(std::max(0.0f, height_points) * resolved_scale)));
        context.bytes_per_row = std::max<s32>(4, context.width * 4);
        context.data = AllocateData(
            static_cast<u32>(std::max<s32>(1, context.bytes_per_row * context.height)),
            4,
            "UIGraphics.CGContext.data");
        if (runtime_->debug_uigraphics_logs < 32) {
            ++runtime_->debug_uigraphics_logs;
            Log("[text] UIGraphicsBeginImageContext ctx=" + Hex32(handle)
                + " points=" + std::to_string(width_points) + "x" + std::to_string(height_points)
                + " scale=" + std::to_string(resolved_scale)
                + " pixels=" + std::to_string(context.width) + "x" + std::to_string(context.height));
        }
        return handle;
    };
    auto make_uiimage_from_context = [&](const u32 context_handle) -> u32 {
        const auto ctx_it = runtime_->graphics_contexts.find(context_handle);
        if (ctx_it == runtime_->graphics_contexts.end()) {
            return 0;
        }
        const u32 cg_image = AllocateData(kObjcObjectSize, 4, "UIGraphics.CGImage");
        host_objects_[cg_image] = HostObject{
            .kind = ObjKind::Generic,
            .class_name = "CGImage",
            .isa = EnsureClass("NSObject")
        };
        memory_.Write32(cg_image, EnsureClass("NSObject"));
        auto& image = runtime_->graphics_images[cg_image];
        image.width = ctx_it->second.width;
        image.height = ctx_it->second.height;
        image.bytes_per_row = ctx_it->second.bytes_per_row;
        if (ctx_it->second.data != 0 && ctx_it->second.bytes_per_row > 0 && ctx_it->second.height > 0) {
            image.pixels = memory_.ReadBuffer(
                ctx_it->second.data,
                static_cast<std::size_t>(ctx_it->second.bytes_per_row) * static_cast<std::size_t>(ctx_it->second.height));
        }
        update_generated_bitmap_skip(image, "UIGraphicsGetImageFromCurrentImageContext image=" + Hex32(cg_image));
        if (runtime_->debug_uigraphics_image_logs < 32 && !image.pixels.empty()) {
            ++runtime_->debug_uigraphics_image_logs;
            const std::array<std::array<s32, 2>, 5> points{{
                {{0, 0}},
                {{image.width / 2, image.height / 2}},
                {{std::max(0, image.width - 1), std::max(0, image.height - 1)}},
                {{image.width / 4, image.height / 4}},
                {{(image.width * 3) / 4, (image.height * 3) / 4}},
            }};
            u32 nonzero = 0;
            u32 alpha = 0;
            u32 checksum = 2166136261u;
            std::array<u8, 4> first_pixel{0, 0, 0, 0};
            for (const auto& point : points) {
                const s32 px = std::clamp<s32>(point[0], 0, std::max(0, image.width - 1));
                const s32 py = std::clamp<s32>(point[1], 0, std::max(0, image.height - 1));
                const std::size_t offset =
                    static_cast<std::size_t>(py)
                        * static_cast<std::size_t>(image.bytes_per_row > 0 ? image.bytes_per_row : image.width * 4)
                    + static_cast<std::size_t>(px) * 4;
                if (offset + 3 >= image.pixels.size()) {
                    continue;
                }
                const std::array<u8, 4> pixel{
                    image.pixels[offset + 0],
                    image.pixels[offset + 1],
                    image.pixels[offset + 2],
                    image.pixels[offset + 3],
                };
                if (point == points.front()) {
                    first_pixel = pixel;
                }
                if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
                    ++nonzero;
                }
                if (pixel[3] != 0) {
                    ++alpha;
                }
                for (const u8 byte : pixel) {
                    checksum ^= byte;
                    checksum *= 16777619u;
                }
            }
            Log("[text] UIGraphicsGetImageFromCurrentImageContext ctx=" + Hex32(context_handle)
                + " image=" + Hex32(cg_image)
                + " size=" + std::to_string(image.width) + "x" + std::to_string(image.height)
                + " samples_nonzero=" + std::to_string(nonzero)
                + " alpha_nonzero=" + std::to_string(alpha)
                + " first_rgba="
                + std::to_string(first_pixel[0]) + ","
                + std::to_string(first_pixel[1]) + ","
                + std::to_string(first_pixel[2]) + ","
                + std::to_string(first_pixel[3])
                + " checksum=" + Hex32(checksum));
        }

        const u32 ui_image = AllocateData(kObjcObjectSize, 4, "UIGraphics.UIImage");
        host_objects_[ui_image] = HostObject{
            .kind = ObjKind::Generic,
            .class_name = "UIImage",
            .isa = EnsureClass("UIImage")
        };
        host_objects_[ui_image].backing_store = cg_image;
        host_objects_[ui_image].dict["CGImage"] = cg_image;
        memory_.Write32(ui_image, EnsureClass("UIImage"));
        return ui_image;
    };
    if (name == "_UIGraphicsBeginImageContext" || name == "_UIGraphicsBeginImageContextWithOptions") {
        const float width = BitsToFloat(Arg(0));
        const float height = BitsToFloat(Arg(1));
        const float scale = name == "_UIGraphicsBeginImageContextWithOptions" ? BitsToFloat(Arg(3)) : 0.0f;
        runtime_->graphics_context_stack.push_back(make_uigraphics_bitmap_context(width, height, scale));
        return 0;
    }
    if (name == "_UIGraphicsEndImageContext") {
        if (!runtime_->graphics_context_stack.empty()) {
            runtime_->graphics_context_stack.pop_back();
        }
        return 0;
    }
    if (name == "_UIGraphicsGetCurrentContext") {
        return runtime_->graphics_context_stack.empty() ? 0u : runtime_->graphics_context_stack.back();
    }
    if (name == "_UIGraphicsGetImageFromCurrentImageContext") {
        return runtime_->graphics_context_stack.empty() ? 0u : make_uiimage_from_context(runtime_->graphics_context_stack.back());
    }
    if (name == "_UIImagePNGRepresentation") {
        return EnsureNSData({});
    }
    if (name == "_malloc" || name == "__Znwm" || name == "__Znam") {
        const u32 size = std::max<u32>(Arg(0), 1);
        const u32 ptr = AllocateGuest(heap_cursor_, size, 16, kPermRead | kPermWrite, "heap");
        heap_alloc_sizes_[ptr] = size;
        return ptr;
    }
    if (name == "_realloc") {
        const u32 old_ptr = Arg(0);
        const u32 new_size = std::max<u32>(Arg(1), 1);
        const u32 new_ptr = AllocateGuest(heap_cursor_, new_size, 16, kPermRead | kPermWrite, "heap.realloc");
        u32 copy_size = new_size;
        if (const auto it = heap_alloc_sizes_.find(old_ptr); it != heap_alloc_sizes_.end()) {
            copy_size = std::min(it->second, new_size);
            heap_alloc_sizes_.erase(it);
        }
        if (old_ptr != 0) {
            memory_.WriteBuffer(new_ptr, memory_.ReadBuffer(old_ptr, copy_size));
        }
        heap_alloc_sizes_[new_ptr] = new_size;
        return new_ptr;
    }
    if (name == "_free" || name == "__ZdlPv" || name == "__ZdaPv") {
        heap_alloc_sizes_.erase(Arg(0));
        return 0;
    }
    if (name == "_memcpy" || name == "_memmove") {
        memory_.WriteBuffer(Arg(0), memory_.ReadBuffer(Arg(1), Arg(2)));
        return Arg(0);
    }
    if (name == "_bzero") {
        std::vector<u8> zeros(Arg(1), 0);
        memory_.WriteBuffer(Arg(0), zeros);
        return 0;
    }
    if (name == "_memset") {
        std::vector<u8> bytes(Arg(2), static_cast<u8>(Arg(1)));
        memory_.WriteBuffer(Arg(0), bytes);
        return Arg(0);
    }
    if (name == "_memset_pattern16") {
        const auto pattern = memory_.ReadBuffer(Arg(1), 16);
        std::vector<u8> out(Arg(2));
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = pattern[i % 16];
        }
        memory_.WriteBuffer(Arg(0), out);
        return Arg(0);
    }
    if (name == "_memcmp") {
        const auto lhs = memory_.ReadBuffer(Arg(0), Arg(2));
        const auto rhs = memory_.ReadBuffer(Arg(1), Arg(2));
        for (u32 i = 0; i < Arg(2); ++i) {
            if (lhs[i] != rhs[i]) {
                return static_cast<u32>(static_cast<s32>(lhs[i]) - static_cast<s32>(rhs[i]));
            }
        }
        return 0;
    }
    if (name == "_strlen") {
        return GuestCStringLength(memory_, Arg(0));
    }
    if (name == "_strnlen") {
        return GuestCStringLength(memory_, Arg(0), Arg(1));
    }
    if (name == "_strcmp" || name == "_strcasecmp" || name == "_strncmp") {
        if (name == "_strncmp") {
            return static_cast<u32>(CompareGuestCStrings(memory_, Arg(0), Arg(1), false, Arg(2)));
        }
        return static_cast<u32>(CompareGuestCStrings(memory_, Arg(0), Arg(1), name == "_strcasecmp"));
    }
    if (name == "_strcpy" || name == "_strncpy" || name == "___strcpy_chk") {
        if (name == "_strncpy") {
            const u32 limit = Arg(2);
            for (u32 index = 0; index < limit; ++index) {
                const u8 value = memory_.Read8(Arg(1) + index);
                memory_.Write8(Arg(0) + index, value);
                if (value == 0) {
                    for (u32 pad = index + 1; pad < limit; ++pad) {
                        memory_.Write8(Arg(0) + pad, 0);
                    }
                    return Arg(0);
                }
            }
            return Arg(0);
        }
        const u32 limit = name == "___strcpy_chk" ? Arg(2) : kMaxGuestCString;
        return CopyGuestCString(memory_, Arg(0), Arg(1), limit);
    }
    if (name == "___strcat_chk") {
        const u32 total_limit = Arg(2) == 0 ? kMaxGuestCString : Arg(2);
        const u32 dest_length = GuestCStringLength(memory_, Arg(0), total_limit);
        if (dest_length >= total_limit) {
            memory_.Write8(Arg(0) + total_limit - 1, 0);
            return Arg(0);
        }
        CopyGuestCString(memory_, Arg(0) + dest_length, Arg(1), total_limit - dest_length);
        return Arg(0);
    }
    if (name == "_strcspn") {
        std::array<bool, 256> reject{};
        for (u32 index = 0; index < kMaxGuestCString; ++index) {
            const u8 value = memory_.Read8(Arg(1) + index);
            if (value == 0) {
                break;
            }
            reject[value] = true;
        }
        u32 index = 0;
        for (; index < kMaxGuestCString; ++index) {
            const u8 value = memory_.Read8(Arg(0) + index);
            if (value == 0 || reject[value]) {
                break;
            }
        }
        return index;
    }
    if (name == "_strstr") {
        const u32 needle_length = GuestCStringLength(memory_, Arg(1));
        if (needle_length == 0) {
            return Arg(0);
        }
        const u32 haystack_length = GuestCStringLength(memory_, Arg(0));
        if (needle_length > haystack_length) {
            return 0;
        }
        for (u32 offset = 0; offset + needle_length <= haystack_length; ++offset) {
            if (CompareGuestCStrings(memory_, Arg(0) + offset, Arg(1), false, needle_length) == 0) {
                return Arg(0) + offset;
            }
        }
        return 0;
    }
    if (name == "_atoi") {
        return ParseGuestAtoi(memory_, Arg(0));
    }
    if (name == "_strtod") {
        const double value = std::strtod(ReadGuestCString(Arg(0)).c_str(), nullptr);
        SetReturnDouble(value);
        return cpu_->Regs()[0];
    }
    if (name == "_strtol") {
        return static_cast<u32>(std::strtol(ReadGuestCString(Arg(0)).c_str(), nullptr, static_cast<int>(Arg(2))));
    }
    if (name == "_printf" || name == "_vprintf" || name == "_vfprintf" || name == "_snprintf" || name == "_vsnprintf"
        || name == "___snprintf_chk" || name == "___sprintf_chk") {
        HandlePrintfLike(name);
        return cpu_->Regs()[0];
    }
    if (name == "_sscanf") {
        return static_cast<u32>(HandleScanfLike(name));
    }
    if (name == "_puts") {
        const std::string text = ReadGuestCString(Arg(0));
        if (text.rfind("[error]", 0) == 0) {
            last_guest_error_ = text;
        }
        Log(text);
        return 0;
    }
    if (name == "_fputs") {
        const std::string text = ReadGuestCString(Arg(0));
        std::FILE* file = libc_abi_->LookupHostFile(Arg(1));
        if (file == nullptr) {
            return static_cast<u32>(-1);
        }
        const int rc = std::fputs(text.c_str(), file);
        if (rc < 0) {
            libc_abi_->SetFileError(Arg(1));
            return static_cast<u32>(-1);
        }
        libc_abi_->SyncFileAfterWrite(Arg(1));
        return 0;
    }
    if (name == "_perror") {
        Log("perror: " + ReadGuestCString(Arg(0)));
        return 0;
    }
    if (name == "_fopen") {
        const std::string guest_path = ReadGuestCString(Arg(0));
        const auto host_path = ResolveGuestPath(guest_path);
        const std::string mode = ReadGuestCString(Arg(1));
        const bool is_music_path = guest_path.find("music/") != std::string::npos
            || (guest_path.size() >= 4 && guest_path.compare(guest_path.size() - 4, 4, ".mp3") == 0);
        const bool is_globals_csv = guest_path.find("globals.csv") != std::string::npos
            || host_path.filename() == "globals.csv";
        const bool is_res_path = guest_path.find("/res/") != std::string::npos
            || guest_path.rfind("res/", 0) == 0;
        const bool is_font_path = IsFontPath(guest_path);
        if (trace_shims_ || is_music_path || is_globals_csv || is_font_path) {
            Log("fopen guest=" + guest_path + " host=" + host_path.string() + " mode=" + mode);
        }
        const bool read_only = mode.find('r') != std::string::npos
            && mode.find('+') == std::string::npos
            && mode.find('w') == std::string::npos
            && mode.find('a') == std::string::npos;
        if (read_only && read_asset_) {
            if (const auto asset_path = ResolveGuestAssetPath(guest_path)) {
                if (auto asset_bytes = read_asset_(*asset_path)) {
                    const u32 guest_file = libc_abi_->OpenMemoryFile(std::move(*asset_bytes), host_path, "FILE*:asset");
                    if (guest_file != 0) {
                        if (is_globals_csv || is_res_path || is_font_path) {
                            Log("fopen asset guest=" + guest_path + " asset=" + *asset_path + " bytes="
                                + std::to_string(file_handles_[guest_file].memory_bytes == nullptr ? 0 : file_handles_[guest_file].memory_bytes->size()));
                        }
                        return guest_file;
                    }
                }
            }
        }
        if (const std::filesystem::path parent = host_path.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        const u32 guest_file = libc_abi_->OpenFile(host_path, mode, "FILE*");
        if (guest_file == 0) {
            SetErrno(errno);
            if (is_globals_csv || is_res_path) {
                Log("fopen failed guest=" + guest_path + " host=" + host_path.string()
                    + " mode=" + mode + " errno=" + std::to_string(errno));
            }
            return 0;
        }
        return guest_file;
    }
    if (name == "_fclose") {
        const int rc = libc_abi_->CloseFile(Arg(0));
        return rc == 0 ? 0u : static_cast<u32>(-1);
    }
    if (name == "_fread") {
        const u32 size = Arg(1) * Arg(2);
        std::FILE* file = libc_abi_->LookupHostFile(Arg(3));
        if (file == nullptr) {
            return 0;
        }
        std::vector<u8> data(size);
        const std::size_t read = std::fread(data.data(), 1, size, file);
        memory_.WriteBuffer(Arg(0), std::span<const u8>(data.data(), read));
        const auto handle_it = file_handles_.find(Arg(3));
        if (handle_it != file_handles_.end()) {
            if (trace_shims_ && handle_it->second.path.filename() == "fingerprint.json") {
                const std::size_t head_size = std::min<std::size_t>(read, 96);
                Log("fread fingerprint bytes=" + std::to_string(read)
                    + " head=" + std::string(reinterpret_cast<const char*>(data.data()), head_size));
            }
            if (handle_it->second.path.filename() == "globals.csv") {
                const std::size_t head_size = std::min<std::size_t>(read, 240);
                Log("fread globals.csv bytes=" + std::to_string(read)
                    + " head=" + OneLineForLog(std::string(reinterpret_cast<const char*>(data.data()), head_size), 240));
            }
        }
        libc_abi_->SyncFileAfterRead(Arg(3), read, std::feof(file) != 0);
        return static_cast<u32>(Arg(1) == 0 ? 0 : read / Arg(1));
    }
    if (name == "_fwrite") {
        const u32 size = Arg(1) * Arg(2);
        std::FILE* file = libc_abi_->LookupHostFile(Arg(3));
        if (file == nullptr) {
            return 0;
        }
        const auto data = memory_.ReadBuffer(Arg(0), size);
        const std::size_t written = std::fwrite(data.data(), 1, size, file);
        libc_abi_->SyncFileAfterWrite(Arg(3));
        return static_cast<u32>(Arg(1) == 0 ? 0 : written / Arg(1));
    }
    if (name == "_fseek") {
        std::FILE* file = libc_abi_->LookupHostFile(Arg(0));
        if (file == nullptr) {
            return static_cast<u32>(-1);
        }
        const int rc = std::fseek(file, static_cast<long>(Arg(1)), static_cast<int>(Arg(2)));
        if (rc == 0) {
            libc_abi_->SyncFileAfterRead(Arg(0), 1, false);
        } else {
            libc_abi_->SetFileError(Arg(0));
        }
        return static_cast<u32>(rc);
    }
    if (name == "_ftell") {
        std::FILE* file = libc_abi_->LookupHostFile(Arg(0));
        return file == nullptr ? 0u : static_cast<u32>(std::ftell(file));
    }
    if (name == "_fflush") {
        std::FILE* file = libc_abi_->LookupHostFile(Arg(0));
        if (file == nullptr) {
            return 0;
        }
        const int rc = std::fflush(file);
        if (rc == 0) {
            libc_abi_->SyncFileAfterWrite(Arg(0));
        } else {
            libc_abi_->SetFileError(Arg(0));
        }
        return static_cast<u32>(rc);
    }
    if (name == "_feof") {
        std::FILE* file = libc_abi_->LookupHostFile(Arg(0));
        return file == nullptr ? 1u : static_cast<u32>(std::feof(file));
    }
    if (name == "_fgets") {
        std::FILE* file = libc_abi_->LookupHostFile(Arg(2));
        if (file == nullptr) {
            return 0;
        }
        std::vector<char> buffer(Arg(1), '\0');
        if (!std::fgets(buffer.data(), static_cast<int>(Arg(1)), file)) {
            libc_abi_->SyncFileAfterRead(Arg(2), 0, std::feof(file) != 0);
            return 0;
        }
        if (const auto handle_it = file_handles_.find(Arg(2));
            handle_it != file_handles_.end() && handle_it->second.path.filename() == "globals.csv") {
            const std::string line(buffer.data());
            if (line.find("ALLIANCE") != std::string::npos || line.find("Name") != std::string::npos) {
                Log("fgets globals.csv line=" + OneLineForLog(line, 220));
            }
        }
        memory_.WriteBuffer(Arg(0), std::span<const u8>(reinterpret_cast<const u8*>(buffer.data()), std::strlen(buffer.data()) + 1));
        libc_abi_->SyncFileAfterRead(Arg(2), std::strlen(buffer.data()), std::feof(file) != 0);
        return Arg(0);
    }
    if (name == "_fileno") {
        return static_cast<u32>(libc_abi_->Fileno(Arg(0)));
    }
    if (name == "_remove") {
        std::error_code ec;
        const bool ok = std::filesystem::remove(ResolveGuestPath(ReadGuestCString(Arg(0))), ec);
        return ok && !ec ? 0 : static_cast<u32>(-1);
    }
    if (name == "_rename") {
        std::error_code ec;
        std::filesystem::rename(ResolveGuestPath(ReadGuestCString(Arg(0))), ResolveGuestPath(ReadGuestCString(Arg(1))), ec);
        return ec ? static_cast<u32>(-1) : 0;
    }
    if (name == "_time") {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        if (Arg(0) != 0) {
            memory_.Write32(Arg(0), static_cast<u32>(now));
        }
        return static_cast<u32>(now);
    }
    if (name == "_gettimeofday") {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(now);
        const auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(now - secs);
        memory_.Write32(Arg(0), static_cast<u32>(secs.count()));
        memory_.Write32(Arg(0) + 4, static_cast<u32>(usecs.count()));
        return 0;
    }
    if (name == "_rand") {
        return static_cast<u32>(std::rand());
    }
    if (name == "_srand") {
        std::srand(Arg(0));
        return 0;
    }
    if (name == "_arc4random") {
        static std::mt19937 generator{std::random_device{}()};
        return generator();
    }
    if (name == "___tolower") {
        return static_cast<u32>(std::tolower(static_cast<unsigned char>(Arg(0))));
    }
    if (name == "___maskrune") {
        const unsigned char c = static_cast<unsigned char>(Arg(0));
        const u32 mask = Arg(1);
        u32 out = 0;
        if ((mask & 0x00000100u) != 0 && std::isalpha(c)) {
            out |= 0x00000100u;
        }
        if ((mask & 0x00000400u) != 0 && std::isdigit(c)) {
            out |= 0x00000400u;
        }
        if ((mask & 0x00004000u) != 0 && std::isspace(c)) {
            out |= 0x00004000u;
        }
        if ((mask & 0x00008000u) != 0 && std::isupper(c)) {
            out |= 0x00008000u;
        }
        if ((mask & 0x00010000u) != 0 && std::islower(c)) {
            out |= 0x00010000u;
        }
        return out;
    }
    if (name == "_sin" || name == "_cos" || name == "_atan2" || name == "_pow"
        || name == "_ceil" || name == "_exp2" || name == "_ldexp" || name == "_fmax") {
        return HandleMathFunction(name);
    }
    if (name == "_sinf" || name == "_cosf" || name == "_powf" || name == "_ceilf" || name == "_floorf" || name == "_roundf") {
        return HandleMathFunction(name);
    }
    if (name == "_abort") {
        exit_code_ = 134;
        Stop(Dynarmic::HaltReason::UserDefined2);
        return 0;
    }
    if (name == "___assert_rtn") {
        const std::string func = ReadGuestCString(Arg(0));
        const std::string file = ReadGuestCString(Arg(1));
        const u32 line = Arg(2);
        const std::string expr = ReadGuestCString(Arg(3));
        const bool is_debugger_error = func == "error"
            && file.find("Debugger.cpp") != std::string::npos
            && line == 196;
        const bool suppress_known_nonfatal = is_debugger_error
            && (last_guest_error_.find("Cant create render target") != std::string::npos
                || last_guest_error_.find("Can't load music:") != std::string::npos);
        if (suppress_known_nonfatal) {
            Log("suppressed debugger assert after: " + last_guest_error_);
            suppress_next_unwind_resume_ = true;
            last_guest_error_.clear();
            return 0;
        }
        Log("assertion failed: " + func + " " + file + ":" + std::to_string(line) + " (" + expr + ")");
        exit_code_ = 134;
        Stop(Dynarmic::HaltReason::UserDefined2);
        return 0;
    }
    if (name == "_exit") {
        exit_code_ = static_cast<int>(Arg(0));
        saw_exit_ = true;
        Stop(Dynarmic::HaltReason::UserDefined2);
        return 0;
    }
    if (name == "_atexit") {
        if (Arg(0) != 0) {
            runtime_->atexit_callbacks.emplace_back(Arg(0), 0);
        }
        return 0;
    }
    if (name == "dyld_stub_binder") {
        return 0;
    }
    if (name == "___error") {
        return errno_address_;
    }
    if (name == "_gethostname") {
        const std::string host = "localhost";
        const u32 limit = Arg(1);
        const u32 bytes = std::min<u32>(limit == 0 ? 0 : limit - 1, static_cast<u32>(host.size()));
        memory_.WriteBuffer(Arg(0), std::span<const u8>(reinterpret_cast<const u8*>(host.data()), bytes));
        memory_.Write8(Arg(0) + bytes, 0);
        return 0;
    }
    if (name == "_sysctlbyname") {
        const std::string query = ReadGuestCString(Arg(0));
        const std::string value = query == "hw.machine" ? "iPhone4,1" : "ClashLoader";
        if (Arg(3) != 0) {
            return static_cast<u32>(-1);
        }
        if (Arg(2) != 0) {
            const u32 size = memory_.Read32(Arg(2));
            if (Arg(1) != 0 && size >= value.size() + 1) {
                memory_.WriteBuffer(Arg(1), std::span<const u8>(reinterpret_cast<const u8*>(value.c_str()), value.size() + 1));
            }
            memory_.Write32(Arg(2), static_cast<u32>(value.size() + 1));
        }
        return 0;
    }
    if (name == "_sysctl") {
        return static_cast<u32>(-1);
    }
    if (name == "__Block_object_assign") {
        if (Arg(0) != 0) {
            memory_.Write32(Arg(0), Arg(1));
        }
        return 0;
    }
    if (name == "__Block_object_dispose") {
        return 0;
    }
    if (name == "_dispatch_time") {
        const u64 when = Pack64(Arg(0), Arg(1));
        const s64 delta = static_cast<s64>(Pack64(Arg(2), Arg(3)));
        SetReturnU64(static_cast<u64>(static_cast<s64>(when) + delta));
        return cpu_->Regs()[0];
    }
    if (name == "_dispatch_async" || name == "_dispatch_after") {
        return 0;
    }
    if (name == "_socket") {
        if (!EnsureSocketSubsystem()) {
            SetErrno(HostSocketErrno());
            return static_cast<u32>(-1);
        }
        const NativeSocket socket_fd = ::socket(static_cast<int>(Arg(0)), static_cast<int>(Arg(1)), static_cast<int>(Arg(2)));
        if (socket_fd == kInvalidNativeSocket) {
            SetErrno(HostSocketErrno());
            return static_cast<u32>(-1);
        }
        const u32 guest_fd = runtime_->next_socket_fd++;
        runtime_->sockets[guest_fd] = RuntimeState::SocketHandle{
            .socket = socket_fd,
            .family = static_cast<int>(Arg(0)),
            .type = static_cast<int>(Arg(1)),
            .protocol = static_cast<int>(Arg(2)),
        };
        return guest_fd;
    }
    if (name == "_connect") {
        const auto it = runtime_->sockets.find(Arg(0));
        if (it == runtime_->sockets.end()) {
            SetErrno(EBADF);
            return static_cast<u32>(-1);
        }
        auto sockaddr_bytes = memory_.ReadBuffer(Arg(1), Arg(2));
        const int result = ::connect(it->second.socket, reinterpret_cast<const sockaddr*>(sockaddr_bytes.data()), static_cast<socklen_t>(Arg(2)));
        if (result != 0) {
            SetErrno(HostSocketErrno());
            if (runtime_->debug_net_logs < 128) {
                ++runtime_->debug_net_logs;
                Log("[net] connect fd=" + std::to_string(Arg(0))
                    + " result=-1 errno=" + std::to_string(HostSocketErrno()));
            }
            return static_cast<u32>(-1);
        }
        if (runtime_->debug_net_logs < 128) {
            ++runtime_->debug_net_logs;
            std::string endpoint = "unknown";
            if (sockaddr_bytes.size() >= sizeof(sockaddr_in)) {
                sockaddr_in sin{};
                std::memcpy(&sin, sockaddr_bytes.data(), sizeof(sin));
                if (sin.sin_family == AF_INET) {
                    char host[INET_ADDRSTRLEN] = {};
                    if (::inet_ntop(AF_INET, &sin.sin_addr, host, sizeof(host)) != nullptr) {
                        endpoint = std::string(host) + ":" + std::to_string(ntohs(sin.sin_port));
                    }
                }
            }
            Log("[net] connect fd=" + std::to_string(Arg(0))
                + " endpoint=" + endpoint
                + " result=0");
        }
        return 0;
    }
    if (name == "_send") {
        const auto it = runtime_->sockets.find(Arg(0));
        if (it == runtime_->sockets.end()) {
            SetErrno(EBADF);
            return static_cast<u32>(-1);
        }
        const auto bytes = memory_.ReadBuffer(Arg(1), Arg(2));
        const auto sent = ::send(it->second.socket, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), static_cast<int>(Arg(3)));
        if (sent < 0) {
            SetErrno(HostSocketErrno());
            if (runtime_->debug_net_logs < 256) {
                ++runtime_->debug_net_logs;
                Log("[net] send fd=" + std::to_string(Arg(0))
                    + " size=" + std::to_string(bytes.size())
                    + " result=-1 errno=" + std::to_string(HostSocketErrno()));
            }
            return static_cast<u32>(-1);
        }
        if (runtime_->debug_net_logs < 256) {
            ++runtime_->debug_net_logs;
            Log("[net] send fd=" + std::to_string(Arg(0))
                + " result=" + std::to_string(sent)
                + " " + DescribePiranhaPacket(std::span<const u8>(bytes.data(), bytes.size())));
        }
        return static_cast<u32>(sent);
    }
    if (name == "_recv") {
        const auto it = runtime_->sockets.find(Arg(0));
        if (it == runtime_->sockets.end()) {
            SetErrno(EBADF);
            return static_cast<u32>(-1);
        }
        std::vector<u8> bytes(Arg(2));
        const auto received = ::recv(it->second.socket, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), static_cast<int>(Arg(3)));
        if (received < 0) {
            SetErrno(HostSocketErrno());
            if (runtime_->debug_net_logs < 256) {
                ++runtime_->debug_net_logs;
                Log("[net] recv fd=" + std::to_string(Arg(0))
                    + " request=" + std::to_string(bytes.size())
                    + " result=-1 errno=" + std::to_string(HostSocketErrno()));
            }
            return static_cast<u32>(-1);
        }
        if (received > 0) {
            memory_.WriteBuffer(Arg(1), std::span<const u8>(bytes.data(), static_cast<std::size_t>(received)));
            if (runtime_->debug_net_logs < 256) {
                ++runtime_->debug_net_logs;
                Log("[net] recv fd=" + std::to_string(Arg(0))
                    + " result=" + std::to_string(received)
                    + " " + DescribePiranhaPacket(std::span<const u8>(bytes.data(), static_cast<std::size_t>(received))));
            }
        } else if (runtime_->debug_net_logs < 256) {
            ++runtime_->debug_net_logs;
            Log("[net] recv fd=" + std::to_string(Arg(0))
                + " request=" + std::to_string(bytes.size())
                + " result=0");
        }
        return static_cast<u32>(received);
    }
    if (name == "_setsockopt") {
        const auto it = runtime_->sockets.find(Arg(0));
        if (it == runtime_->sockets.end()) {
            SetErrno(EBADF);
            return static_cast<u32>(-1);
        }
        const auto bytes = memory_.ReadBuffer(Arg(3), Arg(4));
        const int result = ::setsockopt(it->second.socket, static_cast<int>(Arg(1)), static_cast<int>(Arg(2)),
            reinterpret_cast<const char*>(bytes.data()), static_cast<socklen_t>(bytes.size()));
        if (result != 0) {
            SetErrno(HostSocketErrno());
            return static_cast<u32>(-1);
        }
        return 0;
    }
    if (name == "_socketpair") {
#if defined(_WIN32)
        SetErrno(ENOSYS);
        return static_cast<u32>(-1);
#else
        int pair[2]{-1, -1};
        if (::socketpair(static_cast<int>(Arg(0)), static_cast<int>(Arg(1)), static_cast<int>(Arg(2)), pair) != 0) {
            SetErrno(errno);
            return static_cast<u32>(-1);
        }
        const u32 first = runtime_->next_socket_fd++;
        const u32 second = runtime_->next_socket_fd++;
        runtime_->sockets[first] = RuntimeState::SocketHandle{pair[0], static_cast<int>(Arg(0)), static_cast<int>(Arg(1)), static_cast<int>(Arg(2))};
        runtime_->sockets[second] = RuntimeState::SocketHandle{pair[1], static_cast<int>(Arg(0)), static_cast<int>(Arg(1)), static_cast<int>(Arg(2))};
        memory_.Write32(Arg(3), first);
        memory_.Write32(Arg(3) + 4, second);
        return 0;
#endif
    }
    if (name == "_getaddrinfo") {
        addrinfo hints{};
        addrinfo* hint_ptr = nullptr;
        if (Arg(2) != 0) {
            hints.ai_flags = static_cast<int>(memory_.Read32(Arg(2) + 0));
            hints.ai_family = static_cast<int>(memory_.Read32(Arg(2) + 4));
            hints.ai_socktype = static_cast<int>(memory_.Read32(Arg(2) + 8));
            hints.ai_protocol = static_cast<int>(memory_.Read32(Arg(2) + 12));
            hint_ptr = &hints;
        }
        addrinfo* results = nullptr;
        const std::string node = Arg(0) == 0 ? std::string{} : ReadGuestCString(Arg(0));
        const std::string service = Arg(1) == 0 ? std::string{} : ReadGuestCString(Arg(1));
        const int result = ::getaddrinfo(node.empty() ? nullptr : node.c_str(), service.empty() ? nullptr : service.c_str(), hint_ptr, &results);
        if (result != 0) {
            SetErrno(result);
            return static_cast<u32>(result);
        }
        u32 head = 0;
        u32 previous = 0;
        for (addrinfo* current = results; current != nullptr; current = current->ai_next) {
            const u32 node_addr = AllocateData(32, 4, "addrinfo");
            const u32 sockaddr_addr = AllocateData(static_cast<u32>(current->ai_addrlen), 4, "sockaddr");
            memory_.WriteBuffer(sockaddr_addr, std::span<const u8>(reinterpret_cast<const u8*>(current->ai_addr), current->ai_addrlen));
            const u32 canon = current->ai_canonname == nullptr ? 0 : AllocateCString(current->ai_canonname, "addrinfo.canonname");
            memory_.Write32(node_addr + 0, static_cast<u32>(current->ai_flags));
            memory_.Write32(node_addr + 4, static_cast<u32>(current->ai_family));
            memory_.Write32(node_addr + 8, static_cast<u32>(current->ai_socktype));
            memory_.Write32(node_addr + 12, static_cast<u32>(current->ai_protocol));
            memory_.Write32(node_addr + 16, static_cast<u32>(current->ai_addrlen));
            memory_.Write32(node_addr + 20, canon);
            memory_.Write32(node_addr + 24, sockaddr_addr);
            memory_.Write32(node_addr + 28, 0);
            if (head == 0) {
                head = node_addr;
            }
            if (previous != 0) {
                memory_.Write32(previous + 28, node_addr);
            }
            previous = node_addr;
        }
        memory_.Write32(Arg(3), head);
        ::freeaddrinfo(results);
        return 0;
    }
    if (name == "_getnameinfo") {
        auto sockaddr_bytes = memory_.ReadBuffer(Arg(0), Arg(1));
        std::array<char, NI_MAXHOST> host{};
        std::array<char, NI_MAXSERV> serv{};
        const int result = ::getnameinfo(reinterpret_cast<const sockaddr*>(sockaddr_bytes.data()), static_cast<socklen_t>(Arg(1)),
            host.data(), static_cast<socklen_t>(host.size()), serv.data(), static_cast<socklen_t>(serv.size()), static_cast<int>(Arg(6)));
        if (result != 0) {
            SetErrno(result);
            return static_cast<u32>(result);
        }
        if (Arg(2) != 0 && Arg(3) != 0) {
            const std::size_t bytes = std::min<std::size_t>(std::strlen(host.data()), Arg(3) - 1);
            memory_.WriteBuffer(Arg(2), std::span<const u8>(reinterpret_cast<const u8*>(host.data()), bytes));
            memory_.Write8(Arg(2) + static_cast<u32>(bytes), 0);
        }
        if (Arg(4) != 0 && Arg(5) != 0) {
            const std::size_t bytes = std::min<std::size_t>(std::strlen(serv.data()), Arg(5) - 1);
            memory_.WriteBuffer(Arg(4), std::span<const u8>(reinterpret_cast<const u8*>(serv.data()), bytes));
            memory_.Write8(Arg(4) + static_cast<u32>(bytes), 0);
        }
        return 0;
    }
    if (name == "_getifaddrs") {
        const u32 node = AllocateData(28, 4, "ifaddrs");
        const u32 name_ptr = AllocateCString("lo0", "ifaddrs.name");
        const u32 addr_ptr = AllocateData(16, 4, "ifaddrs.addr");
        const u32 mask_ptr = AllocateData(16, 4, "ifaddrs.netmask");
        memory_.Write8(addr_ptr + 0, 16);
        memory_.Write8(addr_ptr + 1, 2);
        memory_.Write32(addr_ptr + 4, 0x0100007Fu);
        memory_.Write8(mask_ptr + 0, 16);
        memory_.Write8(mask_ptr + 1, 2);
        memory_.Write32(mask_ptr + 4, 0x000000FFu);
        memory_.Write32(node + 0, 0);
        memory_.Write32(node + 4, name_ptr);
        memory_.Write32(node + 8, 0x8u);
        memory_.Write32(node + 12, addr_ptr);
        memory_.Write32(node + 16, mask_ptr);
        memory_.Write32(node + 20, 0);
        memory_.Write32(node + 24, 0);
        memory_.Write32(Arg(0), node);
        return 0;
    }
    if (name == "_freeaddrinfo" || name == "_freeifaddrs") {
        return 0;
    }
    if (name == "_if_nametoindex") {
        return 1;
    }
    if (name == "_inet_aton") {
        const std::string text = ReadGuestCString(Arg(0));
        unsigned int a = 0;
        unsigned int b = 0;
        unsigned int c = 0;
        unsigned int d = 0;
        if (std::sscanf(text.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
            return 0;
        }
        const u32 value = (a << 24) | (b << 16) | (c << 8) | d;
        if (Arg(1) != 0) {
            memory_.Write32(Arg(1), value);
        }
        return 1;
    }
    if (name == "_inet_ntop") {
        if (Arg(0) != 2 || Arg(1) == 0 || Arg(2) == 0 || Arg(3) == 0) {
            return 0;
        }
        const u32 value = memory_.Read32(Arg(1));
        const std::string text = std::to_string((value >> 24) & 0xFF) + "."
            + std::to_string((value >> 16) & 0xFF) + "."
            + std::to_string((value >> 8) & 0xFF) + "."
            + std::to_string(value & 0xFF);
        const std::size_t bytes = std::min<std::size_t>(text.size(), Arg(3) - 1);
        memory_.WriteBuffer(Arg(2), std::span<const u8>(reinterpret_cast<const u8*>(text.data()), bytes));
        memory_.Write8(Arg(2) + static_cast<u32>(bytes), 0);
        return Arg(2);
    }
    if (name == "_setxattr" || name == "_signal") {
        return 0;
    }
    if (name == "_close") {
        if (const auto socket_it = runtime_->sockets.find(Arg(0)); socket_it != runtime_->sockets.end()) {
            CloseNativeSocket(socket_it->second.socket);
            runtime_->sockets.erase(socket_it);
            return 0;
        }
        const u32 guest_file = libc_abi_->LookupGuestFileByDescriptor(static_cast<int>(Arg(0)));
        if (guest_file == 0) {
            SetErrno(EBADF);
            return static_cast<u32>(-1);
        }
        return static_cast<u32>(libc_abi_->CloseFile(guest_file) == 0 ? 0 : -1);
    }
    if (name == "_fcntl") {
        return 0;
    }
    if (name == "_pthread_mutex_init" || name == "_pthread_mutex_destroy" || name == "_pthread_mutex_lock" || name == "_pthread_mutex_unlock") {
        if (name == "_pthread_mutex_init") {
            return static_cast<u32>(libc_abi_->PthreadMutexInit(Arg(0)));
        }
        if (name == "_pthread_mutex_destroy") {
            return static_cast<u32>(libc_abi_->PthreadMutexDestroy(Arg(0)));
        }
        if (name == "_pthread_mutex_lock") {
            return static_cast<u32>(libc_abi_->PthreadMutexLock(Arg(0)));
        }
        return static_cast<u32>(libc_abi_->PthreadMutexUnlock(Arg(0)));
    }
    if (name == "_pthread_create") {
        return static_cast<u32>(libc_abi_->PthreadCreate(Arg(0), Arg(1), Arg(2), Arg(3)));
    }
    if (name == "_stat" || name == "_statfs") {
        const std::string guest_path = ReadGuestCString(Arg(0));
        const auto host_path = ResolveGuestPath(guest_path);
        const bool is_music_path = guest_path.find("music/") != std::string::npos
            || (guest_path.size() >= 4 && guest_path.compare(guest_path.size() - 4, 4, ".mp3") == 0);
        const bool is_font_path = IsFontPath(guest_path);
        if (trace_shims_ || is_music_path || is_font_path) {
            Log(name.substr(1) + " guest=" + guest_path + " host=" + host_path.string());
        }
        const bool exists = GuestPathExists(guest_path);
        if (!exists) {
            SetErrno(ENOENT);
            return static_cast<u32>(-1);
        }
        if (Arg(1) != 0) {
            std::vector<u8> zeros(256, 0);
            memory_.WriteBuffer(Arg(1), zeros);
        }
        return 0;
    }
    if (name == "_backtrace") {
        const u32 buffer = Arg(0);
        const u32 size = Arg(1);
        if (buffer == 0 || size == 0) {
            return 0;
        }
        const std::array<u32, 3> frames{
            cpu_->Regs()[15],
            cpu_->Regs()[14],
            return_stub_,
        };
        const u32 count = std::min<u32>(size, static_cast<u32>(frames.size()));
        for (u32 i = 0; i < count; ++i) {
            memory_.Write32(buffer + i * 4, frames[i]);
        }
        return count;
    }
    if (name == "_backtrace_symbols_fd") {
        auto file_it = file_handles_.find(Arg(2));
        if (file_it == file_handles_.end()) {
            return 0;
        }
        for (u32 i = 0; i < Arg(1); ++i) {
            const u32 frame = memory_.Read32(Arg(0) + i * 4);
            const std::string line = "[" + std::to_string(i) + "] " + Hex32(frame) + "\n";
            std::fwrite(line.data(), 1, line.size(), file_it->second.file);
        }
        return 0;
    }
    if (name == "_regcomp") {
        RuntimeState::RegexEntry entry;
        entry.pattern = ReadGuestCString(Arg(1));
        entry.cflags = static_cast<int>(Arg(2));
        try {
            std::regex_constants::syntax_option_type options = std::regex_constants::ECMAScript;
            if ((entry.cflags & 1) != 0) {
                options |= std::regex_constants::icase;
            }
            entry.compiled.emplace(entry.pattern, options);
            entry.last_error = "success";
        } catch (const std::regex_error& error) {
            entry.compiled.reset();
            entry.last_error = error.what();
            runtime_->regex_entries[Arg(0)] = std::move(entry);
            return 1;
        }
        runtime_->regex_entries[Arg(0)] = std::move(entry);
        return 0;
    }
    if (name == "_regexec") {
        const auto it = runtime_->regex_entries.find(Arg(0));
        if (it == runtime_->regex_entries.end() || !it->second.compiled) {
            return 1;
        }
        std::cmatch match;
        const std::string text = ReadGuestCString(Arg(1));
        const bool matched = std::regex_search(text.c_str(), match, *it->second.compiled);
        if (matched && Arg(3) != 0) {
            const u32 limit = std::min<u32>(Arg(2), static_cast<u32>(match.size()));
            for (u32 i = 0; i < limit; ++i) {
                memory_.Write32(Arg(3) + i * 8, static_cast<u32>(match.position(i)));
                memory_.Write32(Arg(3) + i * 8 + 4, static_cast<u32>(match.position(i) + match.length(i)));
            }
        }
        return matched ? 0 : 1;
    }
    if (name == "_regerror") {
        std::string text = "regex error";
        if (const auto it = runtime_->regex_entries.find(Arg(1)); it != runtime_->regex_entries.end() && !it->second.last_error.empty()) {
            text = it->second.last_error;
        }
        text.push_back('\0');
        const u32 bytes = std::min<u32>(Arg(3), static_cast<u32>(text.size()));
        if (Arg(2) != 0 && bytes != 0) {
            memory_.WriteBuffer(Arg(2), std::span<const u8>(reinterpret_cast<const u8*>(text.data()), bytes));
        }
        return static_cast<u32>(text.size());
    }
    if (name == "_regfree") {
        runtime_->regex_entries.erase(Arg(0));
        return 0;
    }
    if (name == "_select$DARWIN_EXTSN") {
        return 0;
    }
    if (name == "_qsort") {
        const u32 base = Arg(0);
        const u32 count = Arg(1);
        const u32 size = Arg(2);
        if (base == 0 || count < 2 || size == 0) {
            return 0;
        }
        std::vector<std::vector<u8>> items;
        items.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            items.push_back(memory_.ReadBuffer(base + i * size, size));
        }
        std::sort(items.begin(), items.end(), [](const auto& lhs, const auto& rhs) {
            return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
        });
        for (u32 i = 0; i < count; ++i) {
            memory_.WriteBuffer(base + i * size, items[i]);
        }
        return 0;
    }
    if (name == "_NSLog" || name == "_NSLogv") {
        return 0;
    }
    // --- ARC runtime functions ---
    if (name == "_objc_retain" || name == "_objc_retainAutorelease" || name == "_objc_retainAutoreleaseReturnValue"
        || name == "_objc_retainAutoreleasedReturnValue" || name == "_objc_retainBlock") {
        return Arg(0);
    }
    if (name == "_objc_autorelease" || name == "_objc_autoreleaseReturnValue") {
        return Arg(0);
    }
    if (name == "_objc_release") {
        return 0;
    }
    if (name == "_objc_storeStrong") {
        if (Arg(0) != 0) {
            memory_.Write32(Arg(0), Arg(1));
        }
        return 0;
    }
    if (name == "_objc_initWeak" || name == "_objc_storeWeak") {
        if (Arg(0) != 0) {
            memory_.Write32(Arg(0), Arg(1));
        }
        return Arg(1);
    }
    if (name == "_objc_loadWeakRetained") {
        return Arg(0) != 0 ? memory_.Read32(Arg(0)) : 0;
    }
    if (name == "_objc_destroyWeak" || name == "_objc_copyWeak") {
        if (name == "_objc_copyWeak" && Arg(0) != 0 && Arg(1) != 0) {
            memory_.Write32(Arg(0), memory_.Read32(Arg(1)));
        }
        return 0;
    }
    if (name == "_objc_msgSendSuper2_stret") {
        const u32 stret_buffer = Arg(0);
        const u32 raw_self = Arg(1);
        const u32 selector = Arg(2);
        const u32 receiver = memory_.Read32(raw_self);
        const u32 current_class = memory_.Read32(raw_self + 4);
        EnsureGuestAnnotation(receiver);
        const std::string selector_name = ReadGuestCString(selector);

        u32 override_result = 0;
        if (TryHandleObjcOverride(receiver, selector_name, &override_result)) {
            cpu_->Regs()[12] = cpu_->Regs()[14];
            cpu_->Regs()[0] = override_result;
            return override_result;
        }

        if (objc_abi_ != nullptr) {
            if (const auto imp = objc_abi_->LookupMethodImp(receiver, selector_name, true, current_class)) {
                cpu_->Regs()[0] = receiver;
                cpu_->Regs()[12] = *imp;
                return cpu_->Regs()[0];
            }
        }

        cpu_->Regs()[12] = cpu_->Regs()[14];
        return DispatchObjCMessage(raw_self, selector, true, true, stret_buffer);
    }
    if (name == "_class_getMethodImplementation") {
        return 0;
    }
    if (name == "_protocol_conformsToProtocol" || name == "_protocol_copyMethodDescriptionList" || name == "_protocol_copyProtocolList") {
        return 0;
    }
    // --- GCD dispatch ---
    if (name == "_dispatch_once") {
        const u32 predicate = Arg(0);
        if (predicate != 0 && memory_.Read32(predicate) == 0) {
            memory_.Write32(predicate, 1);
            // Block at Arg(1) - invoke is at offset 12
            if (Arg(1) != 0) {
                const u32 invoke = memory_.Read32(Arg(1) + 12);
                if (invoke != 0 && invoke != return_stub_) {
                    cpu_->Regs()[0] = Arg(1);
                    cpu_->Regs()[12] = invoke;
                }
            }
        }
        return 0;
    }
    if (name == "_dispatch_sync") {
        if (Arg(1) != 0) {
            const u32 invoke = memory_.Read32(Arg(1) + 12);
            if (invoke != 0 && invoke != return_stub_) {
                cpu_->Regs()[0] = Arg(1);
                cpu_->Regs()[12] = invoke;
            }
        }
        return 0;
    }
    if (name == "_dispatch_get_global_queue" || name == "_dispatch_get_current_queue" || name == "_dispatch_queue_create") {
        return AllocateData(0x20, 4, "dispatch_queue");
    }
    if (name == "_dispatch_release" || name == "_dispatch_set_target_queue") {
        return 0;
    }
    // --- calloc ---
    if (name == "_calloc") {
        const u32 count = Arg(0);
        const u32 size = Arg(1);
        const u32 total = std::max<u32>(count * size, 1);
        const u32 ptr = AllocateGuest(heap_cursor_, total, 16, kPermRead | kPermWrite, "heap.calloc");
        std::vector<u8> zeros(total, 0);
        memory_.WriteBuffer(ptr, zeros);
        heap_alloc_sizes_[ptr] = total;
        return ptr;
    }
    // --- stack canary ---
    if (name == "___stack_chk_fail") {
        Log("stack smashing detected");
        exit_code_ = 134;
        Stop(Dynarmic::HaltReason::UserDefined2);
        return 0;
    }
    // --- additional math ---
    if (name == "_floor" || name == "_fmod" || name == "_round" || name == "_log10" || name == "_fmaxf" || name == "_exp2f" || name == "_logf") {
        return HandleMathFunction(name);
    }
    // --- posix additions ---
    if (name == "___toupper") {
        return static_cast<u32>(std::toupper(static_cast<unsigned char>(Arg(0))));
    }
    if (name == "_getpid") {
        return 42;
    }
    if (name == "_getprogname") {
        return AllocateCString("SpookyPop", "progname");
    }
    if (name == "_strchr") {
        const u32 s = Arg(0);
        const u8 c = static_cast<u8>(Arg(1));
        for (u32 i = 0; i < kMaxGuestCString; ++i) {
            const u8 ch = memory_.Read8(s + i);
            if (ch == c) {
                return s + i;
            }
            if (ch == 0) {
                if (c == 0) {
                    return s + i;
                }
                return 0;
            }
        }
        return 0;
    }
    if (name == "_strdup") {
        const u32 len = GuestCStringLength(memory_, Arg(0));
        const u32 ptr = AllocateGuest(heap_cursor_, len + 1, 16, kPermRead | kPermWrite, "heap.strdup");
        memory_.WriteBuffer(ptr, memory_.ReadBuffer(Arg(0), len + 1));
        heap_alloc_sizes_[ptr] = len + 1;
        return ptr;
    }
    if (name == "_strerror") {
        return AllocateCString("Unknown error", "strerror");
    }
    if (name == "_usleep") {
        return 0;
    }
    if (name == "_uname") {
        if (Arg(0) != 0) {
            std::vector<u8> zeros(390, 0);
            memory_.WriteBuffer(Arg(0), zeros);
            const std::string sysname = "Darwin";
            memory_.WriteBuffer(Arg(0), std::span<const u8>(reinterpret_cast<const u8*>(sysname.c_str()), sysname.size() + 1));
        }
        return 0;
    }
    if (name == "_unlink" || name == "_remove") {
        const std::string guest_path = ReadGuestCString(Arg(0));
        const auto host_path = ResolveGuestPath(guest_path);
        std::filesystem::remove(host_path);
        return 0;
    }
    if (name == "_write") {
        return Arg(2);
    }
    if (name == "_read") {
        const u32 guest_file = libc_abi_->LookupGuestFileByDescriptor(static_cast<int>(Arg(0)));
        std::FILE* file = libc_abi_->LookupHostFile(guest_file);
        if (file == nullptr) {
            SetErrno(EBADF);
            return static_cast<u32>(-1);
        }
        std::vector<u8> data(Arg(2));
        const std::size_t read = std::fread(data.data(), 1, data.size(), file);
        if (read != 0) {
            memory_.WriteBuffer(Arg(1), std::span<const u8>(data.data(), read));
        }
        libc_abi_->SyncFileAfterRead(guest_file, read, std::feof(file) != 0);
        if (read == 0 && std::ferror(file) != 0) {
            SetErrno(errno == 0 ? EIO : errno);
            return static_cast<u32>(-1);
        }
        return static_cast<u32>(read);
    }
    if (name == "_open") {
        const std::string guest_path = ReadGuestCString(Arg(0));
        const auto host_path = ResolveGuestPath(guest_path);
        const u32 guest_flags = Arg(1);
        const bool is_font_path = IsFontPath(guest_path);
        if (trace_shims_ || is_font_path) {
            Log("open guest=" + guest_path + " host=" + host_path.string() + " flags=" + Hex32(guest_flags));
        }

        if (GuestOpenIsReadOnly(guest_flags) && read_asset_) {
            if (const auto asset_path = ResolveGuestAssetPath(guest_path)) {
                if (auto asset_bytes = read_asset_(*asset_path)) {
                    const u32 guest_file = libc_abi_->OpenMemoryFile(std::move(*asset_bytes), host_path, "fd:asset");
                    if (guest_file != 0) {
                        if (is_font_path) {
                            Log("open asset guest=" + guest_path + " asset=" + *asset_path + " fd=" + std::to_string(libc_abi_->Fileno(guest_file)));
                        }
                        return static_cast<u32>(libc_abi_->Fileno(guest_file));
                    }
                }
            }
        }

        if (const auto parent = host_path.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        const u32 guest_file = libc_abi_->OpenFile(host_path, GuestOpenModeString(guest_flags), "fd");
        if (guest_file == 0) {
            SetErrno(errno);
            if (is_font_path) {
                Log("open failed guest=" + guest_path + " host=" + host_path.string() + " errno=" + std::to_string(errno));
            }
            return static_cast<u32>(-1);
        }
        if (is_font_path) {
            Log("open host guest=" + guest_path + " fd=" + std::to_string(libc_abi_->Fileno(guest_file)));
        }
        return static_cast<u32>(libc_abi_->Fileno(guest_file));
    }
    if (name == "_opendir") {
        return 0;
    }
    if (name == "_closedir" || name == "_readdir") {
        return 0;
    }
    if (name == "_fstat") {
        const u32 guest_file = libc_abi_->LookupGuestFileByDescriptor(static_cast<int>(Arg(0)));
        if (guest_file == 0) {
            SetErrno(EBADF);
            return static_cast<u32>(-1);
        }
        if (Arg(1) != 0) {
            std::vector<u8> zeros(108, 0);
            memory_.WriteBuffer(Arg(1), zeros);

            u64 size = 0;
            if (const auto file_it = file_handles_.find(guest_file); file_it != file_handles_.end()) {
                if (file_it->second.memory_bytes != nullptr) {
                    size = static_cast<u64>(file_it->second.memory_bytes->size());
                } else if (file_it->second.file != nullptr) {
                    const long current = std::ftell(file_it->second.file);
                    if (std::fseek(file_it->second.file, 0, SEEK_END) == 0) {
                        const long end = std::ftell(file_it->second.file);
                        if (end >= 0) {
                            size = static_cast<u64>(end);
                        }
                    }
                    if (current >= 0) {
                        std::fseek(file_it->second.file, current, SEEK_SET);
                    }
                }
            }

            // Darwin armv7 struct stat layout from the iPhoneOS SDK:
            // st_mode @ 4, st_nlink @ 6, st_size @ 60, st_blocks @ 68, st_blksize @ 76.
            memory_.Write16(Arg(1) + 4, 0100644);
            memory_.Write16(Arg(1) + 6, 1);
            memory_.Write64(Arg(1) + 60, size);
            memory_.Write64(Arg(1) + 68, (size + 511) / 512);
            memory_.Write32(Arg(1) + 76, 4096);
        }
        return 0;
    }
    if (name == "_fprintf") {
        HandlePrintfLike(name);
        return cpu_->Regs()[0];
    }
    if (name == "_sprintf") {
        HandlePrintfLike(name);
        return cpu_->Regs()[0];
    }
    if (name == "_putchar") {
        Log(std::string(1, static_cast<char>(Arg(0))));
        return Arg(0);
    }
    if (name == "_raise" || name == "_sigaction" || name == "_sigaltstack") {
        return 0;
    }
    if (name == "_setjmp" || name == "_longjmp") {
        return 0;
    }
    if (name == "_drand48") {
        SetReturnDouble(static_cast<double>(std::rand()) / RAND_MAX);
        return cpu_->Regs()[0];
    }
    if (name == "_srand48") {
        std::srand(Arg(0));
        return 0;
    }
    if (name == "_mkstemp") {
        return static_cast<u32>(-1);
    }
    if (name == "_mmap") {
        const u32 length = Arg(1);
        if (length == 0) {
            SetErrno(EINVAL);
            return static_cast<u32>(-1);
        }

        constexpr u32 kGuestProtRead = 0x1;
        constexpr u32 kGuestProtWrite = 0x2;
        constexpr u32 kGuestProtExec = 0x4;
        const u32 prot = Arg(2);
        u8 permissions = 0;
        if ((prot & kGuestProtRead) != 0) {
            permissions |= kPermRead;
        }
        if ((prot & kGuestProtWrite) != 0) {
            permissions |= kPermWrite;
        }
        if ((prot & kGuestProtExec) != 0) {
            permissions |= kPermExec;
        }
        if (permissions == 0) {
            permissions = kPermRead;
        }

        const u32 address = AllocateData(length, GuestMemory::kPageSize, "mmap");
        const int fd = static_cast<int>(Arg(4));
        const u32 offset = Arg(5);
        if (fd >= 0) {
            const u32 guest_file = libc_abi_->LookupGuestFileByDescriptor(fd);
            std::FILE* file = libc_abi_->LookupHostFile(guest_file);
            if (file == nullptr) {
                SetErrno(EBADF);
                return static_cast<u32>(-1);
            }

            std::vector<u8> bytes(length, 0);
            const long current = std::ftell(file);
            if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0) {
                SetErrno(errno == 0 ? EINVAL : errno);
                return static_cast<u32>(-1);
            }
            const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
            memory_.WriteBuffer(address, std::span<const u8>(bytes.data(), read));
            if (current >= 0) {
                std::fseek(file, current, SEEK_SET);
            }
        }

        memory_.Protect(address, length, permissions);
        return address;
    }
    if (name == "_munmap" || name == "_vm_allocate" || name == "_vm_deallocate"
        || name == "_vm_map" || name == "_vm_protect" || name == "_vm_read_overwrite") {
        return 0;
    }
    if (name == "_mach_absolute_time") {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const u64 ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        SetReturnU64(ns);
        return cpu_->Regs()[0];
    }
    if (name == "_mach_timebase_info") {
        if (Arg(0) != 0) {
            memory_.Write32(Arg(0), 1);
            memory_.Write32(Arg(0) + 4, 1);
        }
        return 0;
    }
    if (name == "_mach_host_self" || name == "_mach_thread_self") {
        return 0x103;
    }
    if (name == "_host_page_size") {
        if (Arg(1) != 0) {
            memory_.Write32(Arg(1), 4096);
        }
        return 0;
    }
    if (name == "_host_statistics" || name == "_task_info" || name == "_task_threads"
        || name == "_task_get_exception_ports" || name == "_task_swap_exception_ports"
        || name == "_thread_get_state" || name == "_thread_get_exception_ports"
        || name == "_thread_swap_exception_ports" || name == "_thread_resume"
        || name == "_thread_suspend") {
        return 0;
    }
    if (name == "_mach_msg" || name == "_mach_port_allocate" || name == "_mach_port_deallocate"
        || name == "_mach_port_insert_right" || name == "_mach_port_mod_refs"
        || name == "_mach_port_move_member" || name == "_mach_port_request_notification"
        || name == "_mach_make_memory_entry_64") {
        return 0;
    }
    if (name == "_pthread_attr_init" || name == "_pthread_attr_destroy" || name == "_pthread_attr_setdetachstate") {
        return 0;
    }
    if (name == "_pthread_cond_init" || name == "_pthread_cond_destroy" || name == "_pthread_cond_signal" || name == "_pthread_cond_wait") {
        return 0;
    }
    if (name == "_pthread_join" || name == "_pthread_mach_thread_np") {
        return 0;
    }
    if (name == "_dladdr") {
        return 0;
    }
    if (name == "__NSGetExecutablePath") {
        const std::string path = "/var/containers/Bundle/Application/SpookyPop.app/SpookyPop";
        if (Arg(0) != 0 && Arg(1) != 0) {
            const u32 bufsize = memory_.Read32(Arg(1));
            if (bufsize >= path.size() + 1) {
                memory_.WriteBuffer(Arg(0), std::span<const u8>(reinterpret_cast<const u8*>(path.c_str()), path.size() + 1));
                return 0;
            }
            memory_.Write32(Arg(1), static_cast<u32>(path.size() + 1));
        }
        return static_cast<u32>(-1);
    }
    if (name == "__dyld_image_count") {
        return 1;
    }
    if (name == "__dyld_get_image_header" || name == "__dyld_get_image_name" || name == "__dyld_get_image_vmaddr_slide") {
        return 0;
    }
    if (name == "__dyld_register_func_for_add_image" || name == "__dyld_register_func_for_remove_image") {
        return 0;
    }
    if (name == "_gai_strerror") {
        return AllocateCString("unknown error", "gai_strerror");
    }
    if (name == "_getsectdatafromheader" || name == "_getsectdatafromheader_64") {
        return 0;
    }
    if (name == "_exception_raise" || name == "_exception_raise_state" || name == "_exception_raise_state_identity") {
        return 0;
    }
    if (name == "_socketpair") {
        return static_cast<u32>(-1);
    }
    if (name == "_system") {
        return 0;
    }
    if (name == "_asl_new" || name == "_asl_search") {
        return 0;
    }
    if (name == "_asl_free" || name == "_asl_set_query" || name == "_aslresponse_free") {
        return 0;
    }
    if (name == "_asl_get" || name == "_asl_key" || name == "_aslresponse_next") {
        return 0;
    }
    if (name == "_OSAtomicAdd32Barrier") {
        const s32 amount = static_cast<s32>(Arg(0));
        const u32 ptr = Arg(1);
        if (ptr != 0) {
            const s32 old_value = static_cast<s32>(memory_.Read32(ptr));
            const s32 new_value = old_value + amount;
            memory_.Write32(ptr, static_cast<u32>(new_value));
            return static_cast<u32>(new_value);
        }
        return 0;
    }
    if (name == "_OSAtomicCompareAndSwap32Barrier" || name == "_OSAtomicCompareAndSwapPtrBarrier") {
        const u32 old_value = Arg(0);
        const u32 new_value = Arg(1);
        const u32 ptr = Arg(2);
        if (ptr != 0 && memory_.Read32(ptr) == old_value) {
            memory_.Write32(ptr, new_value);
            return 1;
        }
        return 0;
    }
    if (name == "_OSMemoryBarrier" || name == "_OSSpinLockLock" || name == "_OSSpinLockUnlock") {
        return 0;
    }
    if (name == "_CCCrypt") {
        return 0;
    }
    if (name == "_NSGetUncaughtExceptionHandler" || name == "_NSSetUncaughtExceptionHandler") {
        return 0;
    }
    if (name == "_NSIntersectionRange") {
        // NSRange(Arg(0), Arg(1)) intersect NSRange(Arg(2), Arg(3))
        const u32 loc1 = Arg(0), len1 = Arg(1);
        const u32 loc2 = Arg(2), len2 = Arg(3);
        const u32 start = std::max(loc1, loc2);
        const u32 end1 = loc1 + len1;
        const u32 end2 = loc2 + len2;
        const u32 end = std::min(end1, end2);
        if (start < end) {
            SetReturnU64(Pack64(start, end - start));
        } else {
            SetReturnU64(0);
        }
        return cpu_->Regs()[0];
    }
    if (name == "__NSDictionaryOfVariableBindings") {
        return EnsureDictionary({});
    }
    // --- sqlite3 (real native implementation) ---
    if (StartsWith(name, "_sqlite3_")) {
        if (name == "_sqlite3_open" || name == "_sqlite3_open_v2") {
            const std::string guest_path = ReadGuestCString(Arg(0));
            const auto host_path = ResolveGuestPath(guest_path);
            if (const auto parent = host_path.parent_path(); !parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            sqlite3* db = nullptr;
            int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
            if (name == "_sqlite3_open_v2") {
                flags = static_cast<int>(Arg(2));
            }
            const int rc = (name == "_sqlite3_open_v2")
                ? sqlite3_open_v2(host_path.string().c_str(), &db, flags, nullptr)
                : sqlite3_open(host_path.string().c_str(), &db);
            Log("sqlite3_open: " + host_path.string() + " rc=" + std::to_string(rc));
            if (rc == SQLITE_OK && db != nullptr) {
                const u32 handle = AllocateData(0x10, 4, "sqlite3");
                if (Arg(1) != 0) {
                    memory_.Write32(Arg(1), handle);
                }
                runtime_->sqlite_dbs[handle] = db;
                return SQLITE_OK;
            }
            if (db != nullptr) {
                sqlite3_close(db);
            }
            if (Arg(1) != 0) {
                memory_.Write32(Arg(1), 0);
            }
            return static_cast<u32>(rc);
        }
        if (name == "_sqlite3_close") {
            const u32 guest_db = Arg(0);
            if (const auto it = runtime_->sqlite_dbs.find(guest_db); it != runtime_->sqlite_dbs.end()) {
                // Finalize all statements for this db
                std::vector<u32> stmts_to_remove;
                for (const auto& [guest_stmt, db_guest] : runtime_->sqlite_stmt_to_db) {
                    if (db_guest == guest_db) {
                        stmts_to_remove.push_back(guest_stmt);
                    }
                }
                for (const u32 guest_stmt : stmts_to_remove) {
                    if (const auto si = runtime_->sqlite_stmts.find(guest_stmt); si != runtime_->sqlite_stmts.end()) {
                        sqlite3_finalize(si->second);
                        runtime_->sqlite_stmts.erase(si);
                    }
                    runtime_->sqlite_stmt_to_db.erase(guest_stmt);
                }
                const int rc = sqlite3_close(it->second);
                runtime_->sqlite_dbs.erase(it);
                return static_cast<u32>(rc);
            }
            return SQLITE_OK;
        }
        if (name == "_sqlite3_prepare_v2") {
            // Args: db, sql, nbyte, ppStmt, pzTail
            const u32 guest_db = Arg(0);
            const std::string sql = ReadGuestCString(Arg(1));
            const auto db_it = runtime_->sqlite_dbs.find(guest_db);
            if (db_it == runtime_->sqlite_dbs.end()) {
                if (Arg(3) != 0) {
                    memory_.Write32(Arg(3), 0);
                }
                return SQLITE_ERROR;
            }
            sqlite3_stmt* stmt = nullptr;
            const int rc = sqlite3_prepare_v2(db_it->second, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr);
            if (rc == SQLITE_OK && stmt != nullptr) {
                const u32 guest_stmt = AllocateData(0x10, 4, "sqlite3_stmt");
                runtime_->sqlite_stmts[guest_stmt] = stmt;
                runtime_->sqlite_stmt_to_db[guest_stmt] = guest_db;
                if (Arg(3) != 0) {
                    memory_.Write32(Arg(3), guest_stmt);
                }
            } else {
                if (Arg(3) != 0) {
                    memory_.Write32(Arg(3), 0);
                }
            }
            if (trace_shims_) {
                Log("sqlite3_prepare_v2: \"" + sql.substr(0, 120) + "\" rc=" + std::to_string(rc));
            }
            return static_cast<u32>(rc);
        }
        if (name == "_sqlite3_finalize") {
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                const int rc = sqlite3_finalize(it->second);
                runtime_->sqlite_stmts.erase(it);
                runtime_->sqlite_stmt_to_db.erase(guest_stmt);
                return static_cast<u32>(rc);
            }
            return SQLITE_OK;
        }
        if (name == "_sqlite3_reset") {
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                return static_cast<u32>(sqlite3_reset(it->second));
            }
            return SQLITE_OK;
        }
        if (name == "_sqlite3_step") {
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                return static_cast<u32>(sqlite3_step(it->second));
            }
            return SQLITE_DONE;
        }
        if (name == "_sqlite3_exec") {
            // Args: db, sql, callback, callback_arg, errmsg
            const u32 guest_db = Arg(0);
            const std::string sql = ReadGuestCString(Arg(1));
            const auto db_it = runtime_->sqlite_dbs.find(guest_db);
            if (db_it == runtime_->sqlite_dbs.end()) {
                return SQLITE_ERROR;
            }
            char* errmsg = nullptr;
            const int rc = sqlite3_exec(db_it->second, sql.c_str(), nullptr, nullptr, &errmsg);
            if (trace_shims_) {
                Log("sqlite3_exec: \"" + sql.substr(0, 120) + "\" rc=" + std::to_string(rc));
            }
            if (errmsg != nullptr) {
                if (Arg(4) != 0) {
                    const u32 err_str = AllocateCString(errmsg, "sqlite3_errmsg");
                    memory_.Write32(Arg(4), err_str);
                }
                sqlite3_free(errmsg);
            } else if (Arg(4) != 0) {
                memory_.Write32(Arg(4), 0);
            }
            return static_cast<u32>(rc);
        }
        if (name == "_sqlite3_bind_int") {
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                return static_cast<u32>(sqlite3_bind_int(it->second, static_cast<int>(Arg(1)), static_cast<int>(Arg(2))));
            }
            return SQLITE_OK;
        }
        if (name == "_sqlite3_bind_double") {
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                const double value = BitCastFromU64<double>(Pack64(Arg(1), Arg(2)));
                return static_cast<u32>(sqlite3_bind_double(it->second, static_cast<int>(Arg(3)), value));
            }
            return SQLITE_OK;
        }
        if (name == "_sqlite3_bind_text") {
            // Args: stmt, col, text, nbyte, destructor
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                const std::string text = ReadGuestCString(Arg(2));
                return static_cast<u32>(sqlite3_bind_text(it->second, static_cast<int>(Arg(1)),
                    text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT));
            }
            return SQLITE_OK;
        }
        if (name == "_sqlite3_bind_blob") {
            // Args: stmt, col, data, nbyte, destructor
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                const auto bytes = memory_.ReadBuffer(Arg(2), Arg(3));
                return static_cast<u32>(sqlite3_bind_blob(it->second, static_cast<int>(Arg(1)),
                    bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT));
            }
            return SQLITE_OK;
        }
        if (name == "_sqlite3_column_int") {
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                return static_cast<u32>(sqlite3_column_int(it->second, static_cast<int>(Arg(1))));
            }
            return 0;
        }
        if (name == "_sqlite3_column_double") {
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                SetReturnDouble(sqlite3_column_double(it->second, static_cast<int>(Arg(1))));
            } else {
                SetReturnDouble(0.0);
            }
            return cpu_->Regs()[0];
        }
        if (name == "_sqlite3_column_text") {
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                const unsigned char* text = sqlite3_column_text(it->second, static_cast<int>(Arg(1)));
                if (text != nullptr) {
                    return AllocateCString(reinterpret_cast<const char*>(text), "sqlite3_column_text");
                }
            }
            return 0;
        }
        if (name == "_sqlite3_column_blob") {
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                const int col = static_cast<int>(Arg(1));
                const void* blob = sqlite3_column_blob(it->second, col);
                const int size = sqlite3_column_bytes(it->second, col);
                if (blob != nullptr && size > 0) {
                    const u32 ptr = AllocateData(static_cast<u32>(size), 4, "sqlite3_blob");
                    memory_.WriteBuffer(ptr, std::span<const u8>(static_cast<const u8*>(blob), static_cast<std::size_t>(size)));
                    return ptr;
                }
            }
            return 0;
        }
        if (name == "_sqlite3_column_bytes") {
            const u32 guest_stmt = Arg(0);
            if (const auto it = runtime_->sqlite_stmts.find(guest_stmt); it != runtime_->sqlite_stmts.end()) {
                return static_cast<u32>(sqlite3_column_bytes(it->second, static_cast<int>(Arg(1))));
            }
            return 0;
        }
        if (name == "_sqlite3_errmsg") {
            const u32 guest_db = Arg(0);
            if (const auto it = runtime_->sqlite_dbs.find(guest_db); it != runtime_->sqlite_dbs.end()) {
                const char* msg = sqlite3_errmsg(it->second);
                if (msg != nullptr) {
                    return AllocateCString(msg, "sqlite3_errmsg");
                }
            }
            return AllocateCString("not an error", "sqlite3_errmsg");
        }
        return 0;
    }
    // --- CATransform3D functions ---
    if (name == "_CATransform3DMakeRotation" || name == "_CATransform3DMakeTranslation") {
        // Return identity in stret buffer (Arg(0)), 64 bytes
        if (Arg(0) != 0) {
            std::vector<u8> zeros(64, 0);
            memory_.WriteBuffer(Arg(0), zeros);
            // Set diagonal to 1.0f
            memory_.Write32(Arg(0) + 0, FloatToBits(1.0f));
            memory_.Write32(Arg(0) + 20, FloatToBits(1.0f));
            memory_.Write32(Arg(0) + 40, FloatToBits(1.0f));
            memory_.Write32(Arg(0) + 60, FloatToBits(1.0f));
        }
        return Arg(0);
    }
    // --- CMTimeMake ---
    if (name == "_CMTimeMake") {
        // Returns a CMTime struct via stret (value=int64, timescale=int32, flags=uint32, epoch=int64)
        // Stub: return zero CMTime
        return 0;
    }
    if (name == "_CFAbsoluteTimeGetCurrent" || StartsWith(name, "_CF")) {
        return HandleCoreFoundationFunction(name);
    }
    if (StartsWith(name, "_Sec") || StartsWith(name, "_SCNetwork") || StartsWith(name, "_CC_") || name == "_CCHmac") {
        return HandleSecurityFunction(name);
    }
    if (StartsWith(name, "_CG") || StartsWith(name, "_CT")
        || StartsWith(name, "_gl") || StartsWith(name, "_al") || StartsWith(name, "_alc")
        || StartsWith(name, "_Audio") || StartsWith(name, "_ExtAudio")) {
        return HandleGraphicsFunction(name);
    }
    if (StartsWith(name, "__Unwind_") || StartsWith(name, "___cxa_") || StartsWith(name, "___gxx")
        || StartsWith(name, "___objc_personality_v0") || StartsWith(name, "___div") || StartsWith(name, "___fix")
        || StartsWith(name, "___float") || StartsWith(name, "___mod") || StartsWith(name, "___udiv") || StartsWith(name, "___umod")
        || StartsWith(name, "__ZN") || StartsWith(name, "__ZSt") || StartsWith(name, "__ZTI")) {
        return HandleCppRuntimeFunction(name);
    }

    if (trace_shims_) {
        Log("fallback shim for " + name);
    }
    return 0;
}


u32 Emulator::HandleCoreFoundationFunction(const std::string& name) {
    if (StartsWith(name, "_CG") || StartsWith(name, "_CT")) {
        return HandleGraphicsFunction(name);
    }

    auto make_runtime_object = [this](const std::string& class_name, const std::string& tag) {
        const u32 object = AllocateData(kObjcObjectSize, 4, tag);
        host_objects_[object] = HostObject{
            .kind = ObjKind::Generic,
            .class_name = class_name,
            .isa = EnsureClass(class_name)
        };
        memory_.Write32(object, EnsureClass(class_name));
        return object;
    };

    if (name == "_CFAbsoluteTimeGetCurrent") {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        const double unix_seconds = std::chrono::duration_cast<std::chrono::microseconds>(now).count() / 1000000.0;
        SetReturnDouble(unix_seconds - 978307200.0);
        return cpu_->Regs()[0];
    }
    if (name == "_CFArrayCreateMutable") {
        return EnsureArray({});
    }
    if (name == "_CFArrayCreate") {
        std::vector<u32> values;
        for (u32 i = 0; i < Arg(2); ++i) {
            values.push_back(memory_.Read32(Arg(1) + i * 4));
        }
        return EnsureArray(values);
    }
    if (name == "_CFArrayAppendValue") {
        host_objects_[Arg(0)].items.push_back(Arg(1));
        return 0;
    }
    if (name == "_CFArrayGetCount") {
        return static_cast<u32>(host_objects_[Arg(0)].items.size());
    }
    if (name == "_CFArrayGetValueAtIndex") {
        const auto& items = host_objects_[Arg(0)].items;
        return Arg(1) < items.size() ? items[Arg(1)] : 0;
    }
    if (name == "_CFArrayRemoveAllValues") {
        host_objects_[Arg(0)].items.clear();
        return 0;
    }
    if (name == "_CFBitVectorCreate") {
        const u32 object = make_runtime_object("CFBitVector", "CFBitVector");
        const u32 num_bits = Arg(2);
        RuntimeState::BitVector bit_vector;
        bit_vector.bits.resize((num_bits + 7) / 8, 0);
        if (Arg(1) != 0 && !bit_vector.bits.empty()) {
            bit_vector.bits = memory_.ReadBuffer(Arg(1), bit_vector.bits.size());
        }
        runtime_->bit_vectors[object] = std::move(bit_vector);
        return object;
    }
    if (name == "_CFBitVectorCreateMutableCopy") {
        const u32 object = make_runtime_object("CFBitVector", "CFBitVector.mutable");
        if (const auto it = runtime_->bit_vectors.find(Arg(2)); it != runtime_->bit_vectors.end()) {
            runtime_->bit_vectors[object] = it->second;
        } else {
            runtime_->bit_vectors[object] = RuntimeState::BitVector{};
        }
        return object;
    }
    if (name == "_CFBitVectorGetBitAtIndex") {
        const auto it = runtime_->bit_vectors.find(Arg(0));
        if (it == runtime_->bit_vectors.end()) {
            return 0;
        }
        const u32 index = Arg(1);
        const u32 byte_index = index / 8;
        const u32 bit_index = index % 8;
        if (byte_index >= it->second.bits.size()) {
            return 0;
        }
        return (it->second.bits[byte_index] >> bit_index) & 1u;
    }
    if (name == "_CFBitVectorSetBitAtIndex") {
        auto& bit_vector = runtime_->bit_vectors[Arg(0)];
        const u32 index = Arg(1);
        const u32 byte_index = index / 8;
        const u32 bit_index = index % 8;
        if (byte_index >= bit_vector.bits.size()) {
            bit_vector.bits.resize(byte_index + 1, 0);
        }
        if (Arg(2) != 0) {
            bit_vector.bits[byte_index] |= static_cast<u8>(1u << bit_index);
        } else {
            bit_vector.bits[byte_index] &= static_cast<u8>(~(1u << bit_index));
        }
        return 0;
    }
    if (name == "_CFDataCreate") {
        return EnsureNSData(memory_.ReadBuffer(Arg(1), Arg(2)));
    }
    if (name == "_CFDataCreateWithBytesNoCopy") {
        return EnsureNSData(memory_.ReadBuffer(Arg(1), Arg(2)));
    }
    if (name == "_CFDataGetLength") {
        return static_cast<u32>(host_objects_[Arg(0)].bytes.size());
    }
    if (name == "_CFDataGetBytePtr") {
        return host_objects_[Arg(0)].backing_store;
    }
    auto dictionary_key_name = [&](const u32 key) {
        if (const auto text = DecodeNSString(key)) {
            return *text;
        }
        const std::string c_string = ReadGuestCString(key);
        if (!c_string.empty()) {
            return c_string;
        }
        return Hex32(key);
    };

    if (name == "_CFDictionaryCreate" || name == "_CFDictionaryCreateMutable") {
        std::unordered_map<std::string, u32> dict;
        if (name == "_CFDictionaryCreate") {
            for (u32 i = 0; i < Arg(3); ++i) {
                const u32 key_ptr = memory_.Read32(Arg(1) + i * 4);
                const u32 value = memory_.Read32(Arg(2) + i * 4);
                dict[dictionary_key_name(key_ptr)] = value;
            }
        }
        return EnsureDictionary(dict);
    }
    if (name == "_CFDictionaryGetValue") {
        const auto dict_it = host_objects_.find(Arg(0));
        if (dict_it == host_objects_.end() || dict_it->second.kind != ObjKind::Dictionary) {
            return 0;
        }
        const auto value_it = dict_it->second.dict.find(dictionary_key_name(Arg(1)));
        return value_it == dict_it->second.dict.end() ? 0 : value_it->second;
    }
    if (name == "_CFDictionarySetValue") {
        auto dict_it = host_objects_.find(Arg(0));
        if (dict_it == host_objects_.end()) {
            return 0;
        }
        dict_it->second.kind = ObjKind::Dictionary;
        dict_it->second.dict[dictionary_key_name(Arg(1))] = Arg(2);
        return 0;
    }
    if (name == "_CFStringCreateMutable") {
        return EnsureNSString("");
    }
    if (name == "_CFStringCreateWithCString") {
        return EnsureNSString(ReadGuestCString(Arg(1)));
    }
    if (name == "_CFStringAppendCharacters") {
        HostObject& object = host_objects_[Arg(0)];
        for (u32 i = 0; i < Arg(2); ++i) {
            object.string_value.push_back(static_cast<char>(memory_.Read16(Arg(1) + i * 2) & 0xFF));
        }
        object.backing_store = AllocateCString(object.string_value, "CFString.mutable");
        memory_.Write32(Arg(0) + 4, object.backing_store);
        memory_.Write32(Arg(0) + 8, static_cast<u32>(object.string_value.size()));
        return 0;
    }
    if (name == "_CFStringAppendFormat") {
        if (auto suffix = DecodeNSString(Arg(1))) {
            host_objects_[Arg(0)].string_value += *suffix;
        }
        return 0;
    }
    if (name == "_CFStringLowercase") {
        HostObject& object = host_objects_[Arg(0)];
        object.string_value = Lowercase(object.string_value);
        return 0;
    }
    if (name == "_CFStringGetLength") {
        return static_cast<u32>(DecodeNSString(Arg(0)).value_or("").size());
    }
    if (name == "_CFStringGetCStringPtr") {
        auto it = host_objects_.find(Arg(0));
        if (it == host_objects_.end()) {
            return 0;
        }
        if (it->second.backing_store == 0) {
            it->second.backing_store = AllocateCString(it->second.string_value, "CFString.cstr");
        }
        return it->second.backing_store;
    }
    if (name == "_CFStringGetCharactersPtr") {
        auto it = host_objects_.find(Arg(0));
        if (it == host_objects_.end()) {
            return 0;
        }
        if (it->second.backing_store == 0) {
            it->second.backing_store = AllocateData(
                static_cast<u32>(std::max<std::size_t>(1, it->second.string_value.size() * 2)),
                2,
                "CFString.utf16");
            for (std::size_t i = 0; i < it->second.string_value.size(); ++i) {
                memory_.Write16(it->second.backing_store + static_cast<u32>(i * 2), static_cast<u16>(static_cast<unsigned char>(it->second.string_value[i])));
            }
        }
        return it->second.backing_store;
    }
    if (name == "_CFStringCreateWithSubstring") {
        const std::string source = DecodeNSString(Arg(1)).value_or("");
        const std::size_t location = std::min<std::size_t>(Arg(2), source.size());
        const std::size_t length = std::min<std::size_t>(Arg(3), source.size() - location);
        return EnsureNSString(source.substr(location, length));
    }
    if (name == "_CFStringGetBytes") {
        const std::string source = DecodeNSString(Arg(0)).value_or("");
        const std::size_t location = std::min<std::size_t>(Arg(1), source.size());
        const std::size_t length = std::min<std::size_t>(Arg(2), source.size() - location);
        const std::string_view slice(source.data() + location, length);
        const u32 buffer = Arg(6);
        const u32 max_len = Arg(7);
        const u32 bytes_to_copy = static_cast<u32>(std::min<std::size_t>(slice.size(), max_len));
        if (buffer != 0 && bytes_to_copy != 0) {
            memory_.WriteBuffer(buffer, std::span<const u8>(reinterpret_cast<const u8*>(slice.data()), bytes_to_copy));
        }
        if (Arg(8) != 0) {
            memory_.Write32(Arg(8), bytes_to_copy);
        }
        return bytes_to_copy;
    }
    if (name == "_CFAttributedStringCreate") {
        const u32 object = make_runtime_object("CFAttributedString", "CFAttributedString");
        auto& attributed = host_objects_[object];
        attributed.string_value = DecodeNSString(Arg(1)).value_or(ReadGuestCString(Arg(1)));
        attributed.backing_store = Arg(1);
        if (const auto attributes_it = host_objects_.find(Arg(2)); attributes_it != host_objects_.end()
            && attributes_it->second.kind == ObjKind::Dictionary) {
            attributed.dict = attributes_it->second.dict;
        }
        return object;
    }
    if (name == "_CFAttributedStringSetAttribute") {
        auto it = host_objects_.find(Arg(0));
        if (it == host_objects_.end()) {
            return 0;
        }
        const std::string key = dictionary_key_name(Arg(3));
        if (!key.empty()) {
            it->second.dict[key] = Arg(4);
        }
        return 0;
    }
    if (name == "_CFAttributedStringRemoveAttribute") {
        auto it = host_objects_.find(Arg(0));
        if (it == host_objects_.end()) {
            return 0;
        }
        const std::string key = dictionary_key_name(Arg(3));
        if (!key.empty()) {
            it->second.dict.erase(key);
        }
        return 0;
    }
    if (name == "_CFURLCreateWithFileSystemPath" || name == "_CFURLCreateFromFileSystemRepresentation") {
        return EnsureNSString(DecodeNSString(Arg(1)).value_or(ReadGuestCString(Arg(1))));
    }
    if (name == "_CFURLCreateStringByAddingPercentEscapes" || name == "_CFURLCreateStringByReplacingPercentEscapesUsingEncoding") {
        return Arg(1);
    }
    if (name == "_CFUUIDCreate") {
        return EnsureNSString(GenerateUuidString());
    }
    if (name == "_CFUUIDCreateString") {
        return EnsureNSString(DecodeNSString(Arg(1)).value_or(GenerateUuidString()));
    }
    if (name == "_CFRelease") {
        return 0;
    }
    if (name == "_CFBundleGetMainBundle") {
        return EnsureClass("NSBundle");
    }
    if (name == "_CFBundleGetIdentifier") {
        return EnsureNSString("com.supercell.clashofclans");
    }
    if (name == "_CFBundleGetVersionNumber") {
        return 170;
    }
    if (name == "_CFLocaleGetSystem") {
        return EnsureClass("NSLocale");
    }
    if (name == "_CFReadStreamCreateWithFile") {
        const u32 object = make_runtime_object("CFReadStream", "CFReadStream");
        RuntimeState::ReadStream stream;
        const auto guest_path = DecodeNSString(Arg(1)).value_or(ReadGuestCString(Arg(1)));
        stream.guest_path = guest_path;
        stream.path = ResolveGuestPath(guest_path);
        runtime_->read_streams[object] = std::move(stream);
        return object;
    }
    if (name == "_CFReadStreamOpen") {
        auto& stream = runtime_->read_streams[Arg(0)];
        stream.cursor = 0;
        stream.data.clear();
        auto data = ReadGuestFileBytes(stream.guest_path);
        if (!data) {
            return 0;
        }
        stream.data = std::move(*data);
        stream.open = true;
        return 1;
    }
    if (name == "_CFReadStreamRead") {
        auto it = runtime_->read_streams.find(Arg(0));
        if (it == runtime_->read_streams.end() || !it->second.open) {
            return static_cast<u32>(-1);
        }
        RuntimeState::ReadStream& stream = it->second;
        const u32 remaining = static_cast<u32>(stream.data.size()) > stream.cursor
            ? static_cast<u32>(stream.data.size()) - stream.cursor
            : 0;
        const u32 bytes = std::min<u32>(remaining, Arg(2));
        if (bytes != 0) {
            memory_.WriteBuffer(Arg(1), std::span<const u8>(stream.data.data() + stream.cursor, bytes));
        }
        stream.cursor += bytes;
        return bytes;
    }
    if (name == "_CFReadStreamClose") {
        if (auto it = runtime_->read_streams.find(Arg(0)); it != runtime_->read_streams.end()) {
            it->second.open = false;
        }
        return 0;
    }
    if (name == "_CFRunLoopGetCurrent") {
        return EnsureClass("NSRunLoop");
    }
    return 0;
}

u32 Emulator::HandlePosixFunction(const std::string& /*name*/) {
    return 0;
}

u32 Emulator::HandleMathFunction(const std::string& name) {
    auto read_double = [this](const std::size_t base) {
        return BitCastFromU64<double>(Pack64(Arg(base), Arg(base + 1)));
    };
    auto read_float = [this](const std::size_t index) {
        return BitsToFloat(Arg(index));
    };

    if (name == "_sin") {
        SetReturnDouble(std::sin(read_double(0)));
        return cpu_->Regs()[0];
    }
    if (name == "_cos") {
        SetReturnDouble(std::cos(read_double(0)));
        return cpu_->Regs()[0];
    }
    if (name == "_atan2") {
        SetReturnDouble(std::atan2(read_double(0), read_double(2)));
        return cpu_->Regs()[0];
    }
    if (name == "_pow") {
        SetReturnDouble(std::pow(read_double(0), read_double(2)));
        return cpu_->Regs()[0];
    }
    if (name == "_ceil") {
        SetReturnDouble(std::ceil(read_double(0)));
        return cpu_->Regs()[0];
    }
    if (name == "_exp2") {
        SetReturnDouble(std::exp2(read_double(0)));
        return cpu_->Regs()[0];
    }
    if (name == "_ldexp") {
        SetReturnDouble(std::ldexp(read_double(0), static_cast<int>(Arg(2))));
        return cpu_->Regs()[0];
    }
    if (name == "_fmax") {
        SetReturnDouble(std::fmax(read_double(0), read_double(2)));
        return cpu_->Regs()[0];
    }
    if (name == "_sinf") {
        SetReturnU32(FloatToBits(std::sin(read_float(0))));
        return cpu_->Regs()[0];
    }
    if (name == "_cosf") {
        SetReturnU32(FloatToBits(std::cos(read_float(0))));
        return cpu_->Regs()[0];
    }
    if (name == "_powf") {
        SetReturnU32(FloatToBits(std::pow(read_float(0), read_float(1))));
        return cpu_->Regs()[0];
    }
    if (name == "_ceilf") {
        SetReturnU32(FloatToBits(std::ceil(read_float(0))));
        return cpu_->Regs()[0];
    }
    if (name == "_floorf") {
        SetReturnU32(FloatToBits(std::floor(read_float(0))));
        return cpu_->Regs()[0];
    }
    if (name == "_roundf") {
        SetReturnU32(FloatToBits(std::round(read_float(0))));
        return cpu_->Regs()[0];
    }
    if (name == "_floor") {
        SetReturnDouble(std::floor(read_double(0)));
        return cpu_->Regs()[0];
    }
    if (name == "_fmod") {
        SetReturnDouble(std::fmod(read_double(0), read_double(2)));
        return cpu_->Regs()[0];
    }
    if (name == "_round") {
        SetReturnDouble(std::round(read_double(0)));
        return cpu_->Regs()[0];
    }
    if (name == "_log10") {
        SetReturnDouble(std::log10(read_double(0)));
        return cpu_->Regs()[0];
    }
    if (name == "_fmaxf") {
        SetReturnU32(FloatToBits(std::fmax(read_float(0), read_float(1))));
        return cpu_->Regs()[0];
    }
    if (name == "_exp2f") {
        SetReturnU32(FloatToBits(std::exp2(read_float(0))));
        return cpu_->Regs()[0];
    }
    if (name == "_logf") {
        SetReturnU32(FloatToBits(std::log(read_float(0))));
        return cpu_->Regs()[0];
    }
    return 0;
}


u32 Emulator::HandleGraphicsFunction(const std::string& name) {
    auto read_arg_float = [this](const std::size_t index) {
        return BitsToFloat(Arg(index));
    };
    auto write_float = [this](const u32 address, const float value) {
        memory_.Write32(address, FloatToBits(value));
    };
    auto make_runtime_object = [this](const std::string& class_name, const std::string& tag) {
        const u32 object = AllocateData(kObjcObjectSize, 4, tag);
        host_objects_[object] = HostObject{
            .kind = ObjKind::Generic,
            .class_name = class_name,
            .isa = EnsureClass("NSObject")
        };
        memory_.Write32(object, EnsureClass("NSObject"));
        return object;
    };
    auto update_generated_bitmap_skip = [&](RuntimeState::GraphicsImage& image, const std::string& label) {
        const BitmapContentStats stats =
            AnalyzeBitmapContent(image.width, image.height, image.bytes_per_row, image.pixels);
        image.skip_draw = ShouldSkipGeneratedBitmapImage(stats);
        if (runtime_->debug_uigraphics_image_logs < 48) {
            ++runtime_->debug_uigraphics_image_logs;
            Log("[text] " + label
                + " size=" + std::to_string(image.width) + "x" + std::to_string(image.height)
                + " stride=" + std::to_string(image.bytes_per_row)
                + " sampled=" + std::to_string(stats.sampled)
                + " alpha=" + std::to_string(stats.alpha_nonzero)
                + " rgb=" + std::to_string(stats.rgb_nonzero)
                + " visible=" + std::to_string(stats.visible_rgb_nonzero)
                + " dark=" + std::to_string(stats.opaque_dark)
                + " skip=" + std::to_string(image.skip_draw ? 1 : 0));
        }
    };
    auto read_transform = [&](const std::size_t base) {
        std::array<float, 6> transform{};
        for (std::size_t i = 0; i < transform.size(); ++i) {
            transform[i] = read_arg_float(base + i);
        }
        return transform;
    };
    auto write_transform = [&](const u32 address, const std::array<float, 6>& transform) {
        for (std::size_t i = 0; i < transform.size(); ++i) {
            write_float(address + static_cast<u32>(i * 4), transform[i]);
        }
    };
    auto concat_transform = [](const std::array<float, 6>& lhs, const std::array<float, 6>& rhs) {
        return std::array<float, 6>{
            lhs[0] * rhs[0] + lhs[1] * rhs[2],
            lhs[0] * rhs[1] + lhs[1] * rhs[3],
            lhs[2] * rhs[0] + lhs[3] * rhs[2],
            lhs[2] * rhs[1] + lhs[3] * rhs[3],
            lhs[4] * rhs[0] + lhs[5] * rhs[2] + rhs[4],
            lhs[4] * rhs[1] + lhs[5] * rhs[3] + rhs[5],
        };
    };
    auto read_rect = [&](const std::size_t base) {
        return std::array<float, 4>{
            read_arg_float(base + 0),
            read_arg_float(base + 1),
            read_arg_float(base + 2),
            read_arg_float(base + 3),
        };
    };
    auto write_rect = [&](const u32 address, const std::array<float, 4>& rect) {
        for (std::size_t i = 0; i < rect.size(); ++i) {
            write_float(address + static_cast<u32>(i * 4), rect[i]);
        }
    };
    std::function<std::optional<std::array<float, 4>>(u32)> color_components_for_handle =
        [&](const u32 color_handle) -> std::optional<std::array<float, 4>> {
            if (color_handle == 0) {
                return std::nullopt;
            }
            if (const auto image_it = runtime_->graphics_images.find(color_handle);
                image_it != runtime_->graphics_images.end() && image_it->second.components.size() >= 4) {
                return std::array<float, 4>{
                    image_it->second.components[0],
                    image_it->second.components[1],
                    image_it->second.components[2],
                    image_it->second.components[3],
                };
            }
            if (const auto object_it = host_objects_.find(color_handle); object_it != host_objects_.end()) {
                if (const auto cg_it = object_it->second.dict.find("CGColor"); cg_it != object_it->second.dict.end()) {
                    return color_components_for_handle(cg_it->second);
                }
            }
            return std::nullopt;
        };
    auto fill_context_rect = [&](RuntimeState::GraphicsContext& context, const std::array<float, 4>& rect, const std::array<float, 4>& color, const bool clear) {
        if (context.data == 0 || context.width <= 0 || context.height <= 0 || context.bytes_per_row <= 0) {
            return;
        }
        if (runtime_->debug_fill_rect_logs < 32) {
            ++runtime_->debug_fill_rect_logs;
            Log("[text] fillRect size=" + std::to_string(context.width) + "x" + std::to_string(context.height)
                + " rect=(" + std::to_string(rect[0]) + "," + std::to_string(rect[1]) + ","
                + std::to_string(rect[2]) + "," + std::to_string(rect[3]) + ")"
                + " clear=" + std::to_string(clear ? 1 : 0)
                + " color=(" + std::to_string(color[0]) + "," + std::to_string(color[1]) + ","
                + std::to_string(color[2]) + "," + std::to_string(color[3]) + ")");
        }
        auto pixels = memory_.ReadBuffer(context.data, static_cast<std::size_t>(context.bytes_per_row) * static_cast<std::size_t>(context.height));
        const s32 x0 = std::max<s32>(0, static_cast<s32>(std::floor(rect[0])));
        const s32 y0 = std::max<s32>(0, static_cast<s32>(std::floor(rect[1])));
        const s32 x1 = std::min<s32>(context.width, static_cast<s32>(std::ceil(rect[0] + rect[2])));
        const s32 y1 = std::min<s32>(context.height, static_cast<s32>(std::ceil(rect[1] + rect[3])));
        const std::array<u8, 4> rgba{
            static_cast<u8>(std::clamp(clear ? 0.0f : color[0], 0.0f, 1.0f) * 255.0f),
            static_cast<u8>(std::clamp(clear ? 0.0f : color[1], 0.0f, 1.0f) * 255.0f),
            static_cast<u8>(std::clamp(clear ? 0.0f : color[2], 0.0f, 1.0f) * 255.0f),
            static_cast<u8>(std::clamp(clear ? 0.0f : color[3], 0.0f, 1.0f) * 255.0f),
        };
        for (s32 y = y0; y < y1; ++y) {
            for (s32 x = x0; x < x1; ++x) {
                const std::size_t offset = static_cast<std::size_t>(y) * context.bytes_per_row + static_cast<std::size_t>(x) * 4;
                if (offset + 3 >= pixels.size()) {
                    continue;
                }
                pixels[offset + 0] = rgba[0];
                pixels[offset + 1] = rgba[1];
                pixels[offset + 2] = rgba[2];
                pixels[offset + 3] = rgba[3];
            }
        }
        memory_.WriteBuffer(context.data, pixels);
    };
    auto apply_transform_to_point = [](const std::array<float, 6>& transform, const float x, const float y) {
        return std::array<float, 2>{
            x * transform[0] + y * transform[2] + transform[4],
            x * transform[1] + y * transform[3] + transform[5],
        };
    };
    auto blend_rgba = [](u8* dst, const std::array<float, 4>& color, const u8 coverage) {
        if (coverage == 0) {
            return;
        }
        const float src_alpha = std::clamp(color[3], 0.0f, 1.0f) * (static_cast<float>(coverage) / 255.0f);
        if (src_alpha <= 0.0f) {
            return;
        }
        const float dst_alpha = static_cast<float>(dst[3]) / 255.0f;
        const float out_alpha = src_alpha + dst_alpha * (1.0f - src_alpha);
        const auto src_channel = [&](const std::size_t index) {
            return std::clamp(color[index], 0.0f, 1.0f);
        };
        if (out_alpha <= 0.0f) {
            dst[0] = 0;
            dst[1] = 0;
            dst[2] = 0;
            dst[3] = 0;
            return;
        }
        for (std::size_t i = 0; i < 3; ++i) {
            const float dst_channel = static_cast<float>(dst[i]) / 255.0f;
            const float out_channel =
                (src_channel(i) * src_alpha + dst_channel * dst_alpha * (1.0f - src_alpha)) / out_alpha;
            dst[i] = static_cast<u8>(std::clamp(out_channel, 0.0f, 1.0f) * 255.0f);
        }
        dst[3] = static_cast<u8>(std::clamp(out_alpha, 0.0f, 1.0f) * 255.0f);
    };
    auto looks_like_font_object = [](const HostObject& object) {
        return object.class_name.find("Font") != std::string::npos
            || object.class_name == "CGDataProvider";
    };
    auto fallback_font_handle = [&]() -> u32 {
        for (const auto& [handle, font] : runtime_->raster_fonts) {
            if (font.initialized) {
                return handle;
            }
        }
        for (const auto& [handle, object] : host_objects_) {
            if (looks_like_font_object(object) && !object.bytes.empty()) {
                return handle;
            }
            if (object.backing_store != 0) {
                const auto backing_it = host_objects_.find(object.backing_store);
                if (backing_it != host_objects_.end()
                    && looks_like_font_object(backing_it->second)
                    && !backing_it->second.bytes.empty()) {
                    return handle;
                }
            }
        }
        return 0;
    };
    auto resolve_font_bytes = [&](const u32 requested_font_handle) -> const std::vector<u8>* {
        const auto try_handle = [&](const u32 handle) -> const std::vector<u8>* {
            const auto font_it = host_objects_.find(handle);
            if (font_it == host_objects_.end()) {
                return nullptr;
            }
            if (!font_it->second.bytes.empty()) {
                return &font_it->second.bytes;
            }
            if (font_it->second.backing_store != 0) {
                const auto backing_it = host_objects_.find(font_it->second.backing_store);
                if (backing_it != host_objects_.end() && !backing_it->second.bytes.empty()) {
                    return &backing_it->second.bytes;
                }
            }
            return nullptr;
        };
        if (const std::vector<u8>* bytes = try_handle(requested_font_handle)) {
            return bytes;
        }
        if (const u32 fallback = fallback_font_handle()) {
            return try_handle(fallback);
        }
        return nullptr;
    };
    auto font_size_for_handle = [&](const u32 requested_font_handle) {
        const u32 font_handle = requested_font_handle != 0 ? requested_font_handle : fallback_font_handle();
        const auto it = host_objects_.find(font_handle);
        if (it != host_objects_.end() && it->second.number_value > 0.0) {
            return static_cast<float>(it->second.number_value);
        }
        return 16.0f;
    };
    auto ensure_raster_font = [&](const u32 requested_font_handle) -> RuntimeState::RasterFont* {
        const u32 font_handle = requested_font_handle != 0 ? requested_font_handle : fallback_font_handle();
        if (font_handle == 0) {
            return nullptr;
        }
        if (auto it = runtime_->raster_fonts.find(font_handle); it != runtime_->raster_fonts.end() && it->second.initialized) {
            return &it->second;
        }
        const std::vector<u8>* bytes = resolve_font_bytes(font_handle);
        if (bytes == nullptr || bytes->empty()) {
            return nullptr;
        }
        RuntimeState::RasterFont raster_font;
        raster_font.bytes = *bytes;
        const int font_offset = stbtt_GetFontOffsetForIndex(raster_font.bytes.data(), 0);
        if (font_offset < 0 || stbtt_InitFont(&raster_font.info, raster_font.bytes.data(), font_offset) == 0) {
            return nullptr;
        }
        stbtt_GetFontVMetrics(&raster_font.info, &raster_font.ascent, &raster_font.descent, &raster_font.line_gap);
        raster_font.initialized = true;
        auto [it, _] = runtime_->raster_fonts.insert_or_assign(font_handle, std::move(raster_font));
        return &it->second;
    };
    auto draw_glyphs_to_context = [&](const u32 font_handle, const u32 glyphs_ptr, const u32 positions_ptr, const u32 count, const u32 context_handle) {
        if (!kEnableHostCoreTextRendering) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                Log("[text] host CoreText glyph rendering disabled for probe");
            }
            return true;
        }
        if (glyphs_ptr == 0 || count == 0) {
            return true;
        }
        auto ctx_it = runtime_->graphics_contexts.find(context_handle);
        if (ctx_it == runtime_->graphics_contexts.end()) {
            return false;
        }
        RuntimeState::GraphicsContext& context = ctx_it->second;
        if (context.data == 0 || context.width <= 0 || context.height <= 0 || context.bytes_per_row <= 0) {
            return false;
        }
        const u32 resolved_font_handle = font_handle != 0 ? font_handle : fallback_font_handle();
        RuntimeState::RasterFont* raster_font = ensure_raster_font(resolved_font_handle);
        if (raster_font == nullptr) {
            static u32 missing_font_logs = 0;
            if (missing_font_logs < 8) {
                ++missing_font_logs;
                Log("[text] CTFontDrawGlyphs missing raster font requested=" + Hex32(font_handle)
                    + " resolved=" + Hex32(resolved_font_handle)
                    + " count=" + std::to_string(count));
            }
            return false;
        }
        bool log_this_draw = false;
        if (runtime_->debug_text_draw_logs < 32) {
            ++runtime_->debug_text_draw_logs;
            log_this_draw = true;
            std::string pos_summary = "none";
            if (positions_ptr != 0) {
                pos_summary =
                    std::to_string(BitsToFloat(memory_.Read32(positions_ptr + 0))) + ","
                    + std::to_string(BitsToFloat(memory_.Read32(positions_ptr + 4)));
            }
            Log("[text] CTFontDrawGlyphs ctx=" + Hex32(context_handle)
                + " font=" + Hex32(font_handle)
                + " resolved=" + Hex32(resolved_font_handle)
                + " count=" + std::to_string(count)
                + " size=" + std::to_string(context.width) + "x" + std::to_string(context.height)
                + " text_pos=" + std::to_string(context.text_position[0]) + "," + std::to_string(context.text_position[1])
                + " first_pos=" + pos_summary);
        }

        auto pixels = memory_.ReadBuffer(
            context.data,
            static_cast<std::size_t>(context.bytes_per_row) * static_cast<std::size_t>(context.height));
        const float font_size = font_size_for_handle(resolved_font_handle);
        const float transform_scale_x = std::sqrt(context.transform[0] * context.transform[0] + context.transform[1] * context.transform[1]);
        const float transform_scale_y = std::sqrt(context.transform[2] * context.transform[2] + context.transform[3] * context.transform[3]);
        const float text_scale_x = std::sqrt(context.text_matrix[0] * context.text_matrix[0] + context.text_matrix[1] * context.text_matrix[1]);
        const float text_scale_y = std::sqrt(context.text_matrix[2] * context.text_matrix[2] + context.text_matrix[3] * context.text_matrix[3]);
        const float scale_x = std::max(0.01f, font_size * std::max(0.01f, transform_scale_x) * std::max(0.01f, text_scale_x));
        const float scale_y = std::max(0.01f, font_size * std::max(0.01f, transform_scale_y) * std::max(0.01f, text_scale_y));
        const float stb_scale_x = stbtt_ScaleForPixelHeight(&raster_font->info, scale_x);
        const float stb_scale_y = stbtt_ScaleForPixelHeight(&raster_font->info, scale_y);
        const auto effective_transform = concat_transform(context.transform, context.text_matrix);
        std::array<float, 4> glyph_color = context.fill_color;
        if (glyph_color[3] > 0.0f
            && glyph_color[0] == 0.0f
            && glyph_color[1] == 0.0f
            && glyph_color[2] == 0.0f) {
            glyph_color = {1.0f, 1.0f, 1.0f, glyph_color[3]};
        }

        float pen_x = context.text_position[0];
        float pen_y = context.text_position[1];
        std::size_t blended_pixels = 0;
        s32 blended_min_x = context.width;
        s32 blended_min_y = context.height;
        s32 blended_max_x = -1;
        s32 blended_max_y = -1;
        const bool y_axis_already_bitmap_down = effective_transform[3] < 0.0f;
        for (u32 i = 0; i < count; ++i) {
            const int glyph_index = static_cast<int>(memory_.Read16(glyphs_ptr + i * 2));
            float glyph_x = pen_x;
            float glyph_y = pen_y;
            if (positions_ptr != 0) {
                glyph_x = BitsToFloat(memory_.Read32(positions_ptr + i * 8 + 0));
                glyph_y = BitsToFloat(memory_.Read32(positions_ptr + i * 8 + 4));
            }

            const auto transformed = apply_transform_to_point(effective_transform, glyph_x, glyph_y);
            const float bitmap_x = transformed[0];
            const float bitmap_baseline_y = y_axis_already_bitmap_down
                ? transformed[1]
                : static_cast<float>(context.height) - transformed[1];
            const float subpixel_x = bitmap_x - std::floor(bitmap_x);
            const float subpixel_y = bitmap_baseline_y - std::floor(bitmap_baseline_y);
            int x0 = 0;
            int y0 = 0;
            int x1 = 0;
            int y1 = 0;
            stbtt_GetGlyphBitmapBoxSubpixel(
                &raster_font->info,
                glyph_index,
                stb_scale_x,
                stb_scale_y,
                subpixel_x,
                subpixel_y,
                &x0,
                &y0,
                &x1,
                &y1);
            const int width = x1 - x0;
            const int height = y1 - y0;
            if (width > 0 && height > 0) {
                std::vector<u8> bitmap(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
                stbtt_MakeGlyphBitmapSubpixel(
                    &raster_font->info,
                    bitmap.data(),
                    width,
                    height,
                    width,
                    stb_scale_x,
                    stb_scale_y,
                    subpixel_x,
                    subpixel_y,
                    glyph_index);
                const s32 dst_x = static_cast<s32>(std::floor(bitmap_x)) + x0;
                const s32 dst_y = static_cast<s32>(std::floor(bitmap_baseline_y)) + y0;
                for (int row = 0; row < height; ++row) {
                    const s32 py = dst_y + row;
                    if (py < 0 || py >= context.height) {
                        continue;
                    }
                    for (int col = 0; col < width; ++col) {
                        const s32 px = dst_x + col;
                        if (px < 0 || px >= context.width) {
                            continue;
                        }
                        const u8 coverage = bitmap[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(col)];
                        if (coverage == 0) {
                            continue;
                        }
                        const std::size_t offset =
                            static_cast<std::size_t>(py) * static_cast<std::size_t>(context.bytes_per_row)
                            + static_cast<std::size_t>(px) * 4;
                        if (offset + 3 >= pixels.size()) {
                            continue;
                        }
                        blend_rgba(pixels.data() + offset, glyph_color, coverage);
                        ++blended_pixels;
                        blended_min_x = std::min(blended_min_x, px);
                        blended_min_y = std::min(blended_min_y, py);
                        blended_max_x = std::max(blended_max_x, px);
                        blended_max_y = std::max(blended_max_y, py);
                    }
                }
            }

            if (positions_ptr == 0) {
                int advance_width = 0;
                int left_side_bearing = 0;
                stbtt_GetGlyphHMetrics(&raster_font->info, glyph_index, &advance_width, &left_side_bearing);
                (void)left_side_bearing;
                pen_x += advance_width * stb_scale_x;
            }
        }

        if (positions_ptr == 0) {
            context.text_position = {pen_x, pen_y};
        }
        memory_.WriteBuffer(context.data, pixels);
        if (log_this_draw && blended_pixels > 0) {
            Log("[text] CTFontDrawGlyphs pixels=" + std::to_string(blended_pixels)
                + " bbox=" + std::to_string(blended_min_x) + "," + std::to_string(blended_min_y)
                + "-" + std::to_string(blended_max_x) + "," + std::to_string(blended_max_y)
                + " y_flip=" + std::to_string(y_axis_already_bitmap_down ? 0 : 1)
                + " color=" + std::to_string(glyph_color[0]) + ","
                + std::to_string(glyph_color[1]) + ","
                + std::to_string(glyph_color[2]) + ","
                + std::to_string(glyph_color[3]));
        }
        return true;
    };
    HostGLBackend* const host_gl = runtime_->host_gl.get();
    const bool use_host_gl = host_gl != nullptr && host_gl->IsSupported();
    if (use_host_gl) {
        host_gl->EnsureWindow(
            std::max(1, static_cast<int>(runtime_->screen_width_points)),
            std::max(1, static_cast<int>(runtime_->screen_height_points)),
            binary_path_.filename().string());
        host_gl->MakeCurrent();
    }
    auto texture_bytes_per_pixel = [](const u32 format, const u32 type) -> u32 {
        constexpr u32 kGlAlpha = 0x1906;
        constexpr u32 kGlRgb = 0x1907;
        constexpr u32 kGlRgba = 0x1908;
        constexpr u32 kGlLuminance = 0x1909;
        constexpr u32 kGlLuminanceAlpha = 0x190A;
        constexpr u32 kGlUnsignedByte = 0x1401;
        constexpr u32 kGlUnsignedShort4444 = 0x8033;
        constexpr u32 kGlUnsignedShort5551 = 0x8034;
        constexpr u32 kGlUnsignedShort565 = 0x8363;

        switch (type) {
        case kGlUnsignedByte:
            switch (format) {
            case kGlAlpha:
            case kGlLuminance:
                return 1;
            case kGlLuminanceAlpha:
                return 2;
            case kGlRgb:
                return 3;
            case kGlRgba:
                return 4;
            default:
                return 4;
            }
        case kGlUnsignedShort4444:
        case kGlUnsignedShort5551:
        case kGlUnsignedShort565:
            return 2;
        default:
            return 4;
        }
    };
    auto graphics_context_for_source = [&](const u32 source) -> const RuntimeState::GraphicsContext* {
        if (source == 0) {
            return nullptr;
        }
        for (const auto& [_, context] : runtime_->graphics_contexts) {
            if (context.data == 0 || context.bytes_per_row <= 0 || context.height <= 0) {
                continue;
            }
            const std::size_t byte_size =
                static_cast<std::size_t>(context.bytes_per_row) * static_cast<std::size_t>(context.height);
            const u32 base = context.data;
            const u32 end = base + static_cast<u32>(std::min<std::size_t>(byte_size, 0xFFFFFFFFu - base));
            if (source >= base && source < end) {
                return &context;
            }
        }
        return nullptr;
    };
    auto graphics_context_handle_for_source = [&](const u32 source) -> u32 {
        if (source == 0) {
            return 0;
        }
        for (const auto& [handle, context] : runtime_->graphics_contexts) {
            if (context.data == 0 || context.bytes_per_row <= 0 || context.height <= 0) {
                continue;
            }
            const std::size_t byte_size =
                static_cast<std::size_t>(context.bytes_per_row) * static_cast<std::size_t>(context.height);
            const u32 base = context.data;
            const u32 end = base + static_cast<u32>(std::min<std::size_t>(byte_size, 0xFFFFFFFFu - base));
            if (source >= base && source < end) {
                return handle;
            }
        }
        return 0;
    };
    auto describe_texture_source = [&](const u32 source) {
        const RuntimeState::GraphicsContext* const context = graphics_context_for_source(source);
        if (context == nullptr) {
            return std::string{};
        }
        return " ctx=" + Hex32(graphics_context_handle_for_source(source))
            + " ctx_data=" + Hex32(context->data)
            + " ctx_size=" + std::to_string(context->width) + "x" + std::to_string(context->height)
            + " ctx_stride=" + std::to_string(context->bytes_per_row)
            + " ctx_off=" + std::to_string(source - context->data);
    };
    auto read_texture_upload = [&](const u32 source,
                                   const u32 width,
                                   const u32 height,
                                   const u32 format,
                                   const u32 type) {
        const u32 bpp = texture_bytes_per_pixel(format, type);
        const std::size_t tight_bytes = std::max<std::size_t>(
            1,
            static_cast<std::size_t>(std::max<u32>(1, width))
                * static_cast<std::size_t>(std::max<u32>(1, height))
                * static_cast<std::size_t>(bpp));
        if (source == 0) {
            return std::vector<u8>(tight_bytes, 0);
        }
        const RuntimeState::GraphicsContext* const context = graphics_context_for_source(source);
        if (context != nullptr
            && format == 0x1908
            && type == 0x1401
            && bpp == 4
            && context->bytes_per_row >= static_cast<s32>(width * bpp)) {
            const u32 offset = source - context->data;
            const u32 row_offset = static_cast<u32>(offset % static_cast<u32>(context->bytes_per_row));
            const u32 first_row = static_cast<u32>(offset / static_cast<u32>(context->bytes_per_row));
            if (row_offset + width * bpp <= static_cast<u32>(context->bytes_per_row)
                && first_row + height <= static_cast<u32>(std::max(0, context->height))) {
                std::vector<u8> packed(tight_bytes, 0);
                const std::size_t row_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(bpp);
                for (u32 row = 0; row < height; ++row) {
                    const u32 src =
                        context->data
                        + (first_row + row) * static_cast<u32>(context->bytes_per_row)
                        + row_offset;
                    const auto row_data = memory_.ReadBuffer(src, row_bytes);
                    const std::size_t dst = static_cast<std::size_t>(row) * row_bytes;
                    if (dst + row_data.size() <= packed.size()) {
                        std::copy(row_data.begin(), row_data.end(), packed.begin() + static_cast<std::ptrdiff_t>(dst));
                    }
                }
                return packed;
            }
        }
        return memory_.ReadBuffer(source, tight_bytes);
    };
    auto drain_host_gl_errors = [&](const char* label) {
        if (!use_host_gl) {
            return;
        }
        if (auto fn = LookupHostGLProc<HostGLenum (*)()>(host_gl, "glGetError"); fn != nullptr) {
            bool reported = false;
            while (true) {
                const u32 host_error = static_cast<u32>(fn());
                if (host_error == 0) {
                    break;
                }
                if (!reported && runtime_->gl_state.debug_stale_error_logs < 16) {
                    ++runtime_->gl_state.debug_stale_error_logs;
                    Log("[gl] cleared stale error before " + std::string(label) + ": " + Hex32(host_error));
                    reported = true;
                }
            }
        }
    };
    auto log_host_gl_errors = [&](const char* label, u32& budget) {
        if (!use_host_gl || budget >= 32) {
            return;
        }
        if (auto fn = LookupHostGLProc<HostGLenum (*)()>(host_gl, "glGetError"); fn != nullptr) {
            while (budget < 32) {
                const u32 host_error = static_cast<u32>(fn());
                if (host_error == 0) {
                    break;
                }
                ++budget;
                Log("[gl-error] " + std::string(label) + " error=" + Hex32(host_error)
                    + " fb=" + Hex32(runtime_->gl_state.framebuffer)
                    + " rb=" + Hex32(runtime_->gl_state.renderbuffer));
            }
        }
    };
    auto host_buffer_id = [&](const u32 guest_id) -> HostGLuint {
        if (guest_id == 0) {
            return 0;
        }
        const auto it = runtime_->gl_buffers.find(guest_id);
        return it == runtime_->gl_buffers.end() ? 0u : static_cast<HostGLuint>(it->second.host_id);
    };
    auto host_texture_id = [&](const u32 guest_id) -> HostGLuint {
        if (guest_id == 0) {
            return 0;
        }
        const auto it = runtime_->gl_textures.find(guest_id);
        return it == runtime_->gl_textures.end() ? 0u : static_cast<HostGLuint>(it->second.host_id);
    };
    auto host_framebuffer_id = [&](const u32 guest_id) -> HostGLuint {
        if (guest_id == 0 || guest_id == runtime_->drawable_framebuffer) {
            return 0;
        }
        const auto it = runtime_->gl_framebuffers.find(guest_id);
        return it == runtime_->gl_framebuffers.end() ? 0u : static_cast<HostGLuint>(it->second.host_id);
    };
    auto host_renderbuffer_id = [&](const u32 guest_id) -> HostGLuint {
        if (guest_id == 0 || guest_id == runtime_->drawable_renderbuffer) {
            return 0;
        }
        const auto it = runtime_->gl_renderbuffers.find(guest_id);
        return it == runtime_->gl_renderbuffers.end() ? 0u : static_cast<HostGLuint>(it->second.host_id);
    };
    auto guest_framebuffer_status = [&](const u32 guest_id) -> u32 {
        constexpr u32 kFramebufferComplete = 0x8CD5;
        constexpr u32 kFramebufferIncompleteAttachment = 0x8CD6;
        constexpr u32 kFramebufferMissingAttachment = 0x8CD7;
        constexpr u32 kColorAttachment0 = 0x8CE0;

        if (guest_id == 0 || guest_id == runtime_->drawable_framebuffer) {
            return kFramebufferComplete;
        }

        const auto framebuffer_it = runtime_->gl_framebuffers.find(guest_id);
        if (framebuffer_it == runtime_->gl_framebuffers.end()) {
            return kFramebufferMissingAttachment;
        }

        const auto attachment_it = framebuffer_it->second.attachments.find(kColorAttachment0);
        if (attachment_it == framebuffer_it->second.attachments.end() || attachment_it->second == 0) {
            return kFramebufferMissingAttachment;
        }

        const u32 attachment_id = attachment_it->second;
        if (const auto texture_it = runtime_->gl_textures.find(attachment_id); texture_it != runtime_->gl_textures.end()) {
            return (texture_it->second.width > 0 && texture_it->second.height > 0)
                ? kFramebufferComplete
                : kFramebufferIncompleteAttachment;
        }
        if (const auto renderbuffer_it = runtime_->gl_renderbuffers.find(attachment_id); renderbuffer_it != runtime_->gl_renderbuffers.end()) {
            return (renderbuffer_it->second.width > 0 && renderbuffer_it->second.height > 0)
                ? kFramebufferComplete
                : kFramebufferIncompleteAttachment;
        }

        return kFramebufferIncompleteAttachment;
    };
    auto host_shader_id = [&](const u32 guest_id) -> HostGLuint {
        const auto it = runtime_->gl_shaders.find(guest_id);
        return it == runtime_->gl_shaders.end() ? 0u : static_cast<HostGLuint>(it->second.host_id);
    };
    auto host_program_id = [&](const u32 guest_id) -> HostGLuint {
        const auto it = runtime_->gl_programs.find(guest_id);
        return it == runtime_->gl_programs.end() ? 0u : static_cast<HostGLuint>(it->second.host_id);
    };
    auto active_texture_unit = [&]() -> u32 {
        constexpr u32 kGlTexture0 = 0x84C0;
        return runtime_->gl_state.active_texture >= kGlTexture0 ? runtime_->gl_state.active_texture - kGlTexture0 : 0;
    };
    auto texture_binding_key = [&](const u32 unit, const u32 target) -> u64 {
        return (static_cast<u64>(unit) << 32) | static_cast<u64>(target);
    };
    auto lookup_bound_texture = [&](const u32 target) -> u32 {
        const auto it = runtime_->gl_state.bound_textures.find(texture_binding_key(active_texture_unit(), target));
        return it == runtime_->gl_state.bound_textures.end() ? 0u : it->second;
    };
    auto set_bound_texture = [&](const u32 target, const u32 texture) {
        runtime_->gl_state.bound_textures[texture_binding_key(active_texture_unit(), target)] = texture;
    };
    auto is_power_of_two = [](const s32 value) {
        return value > 0 && (static_cast<u32>(value) & (static_cast<u32>(value) - 1u)) == 0;
    };
    auto is_mipmap_filter = [](const s32 value) {
        switch (value) {
        case 0x2700:  // GL_NEAREST_MIPMAP_NEAREST
        case 0x2701:  // GL_LINEAR_MIPMAP_NEAREST
        case 0x2702:  // GL_NEAREST_MIPMAP_LINEAR
        case 0x2703:  // GL_LINEAR_MIPMAP_LINEAR
            return true;
        default:
            return false;
        }
    };
    auto apply_host_texture_compat = [&](const u32 target, const u32 texture) {
        constexpr u32 kGlTexture2D = 0x0DE1;
        constexpr u32 kGlTextureMagFilter = 0x2800;
        constexpr u32 kGlTextureMinFilter = 0x2801;
        constexpr u32 kGlTextureWrapS = 0x2802;
        constexpr u32 kGlTextureWrapT = 0x2803;
        constexpr s32 kGlLinear = 0x2601;
        constexpr s32 kGlRepeat = 0x2901;
        constexpr s32 kGlMirroredRepeat = 0x8370;
        constexpr s32 kGlClampToEdge = 0x812F;
        constexpr s32 kDefaultMagFilter = kGlLinear;
        constexpr s32 kDefaultMinFilter = 0x2702;  // GL_NEAREST_MIPMAP_LINEAR
        constexpr s32 kDefaultWrap = kGlRepeat;

        if (!use_host_gl || target != kGlTexture2D || texture == 0) {
            return;
        }

        const auto texture_it = runtime_->gl_textures.find(texture);
        if (texture_it == runtime_->gl_textures.end()) {
            return;
        }

        auto gl_tex_parameter_i = LookupHostGLProc<void (*)(HostGLenum, HostGLenum, HostGLint)>(host_gl, "glTexParameteri");
        if (gl_tex_parameter_i == nullptr) {
            return;
        }

        const auto& object = texture_it->second;
        const bool npot = !is_power_of_two(object.width) || !is_power_of_two(object.height);
        const auto read_param = [&](const u32 pname, const s32 fallback) {
            const auto it = object.iparams.find(pname);
            return it == object.iparams.end() ? fallback : it->second;
        };

        const s32 desired_mag = read_param(kGlTextureMagFilter, kDefaultMagFilter);
        const s32 desired_min = read_param(kGlTextureMinFilter, kDefaultMinFilter);
        const s32 desired_wrap_s = read_param(kGlTextureWrapS, kDefaultWrap);
        const s32 desired_wrap_t = read_param(kGlTextureWrapT, kDefaultWrap);

        s32 host_min = desired_min;
        s32 host_wrap_s = desired_wrap_s;
        s32 host_wrap_t = desired_wrap_t;
        bool adjusted = false;

        if (!object.mipmaps_generated && is_mipmap_filter(host_min)) {
            host_min = desired_mag == 0x2600 ? 0x2600 : kGlLinear;
            adjusted = true;
        }
        if (npot) {
            if (host_wrap_s == kGlRepeat || host_wrap_s == kGlMirroredRepeat) {
                host_wrap_s = kGlClampToEdge;
                adjusted = true;
            }
            if (host_wrap_t == kGlRepeat || host_wrap_t == kGlMirroredRepeat) {
                host_wrap_t = kGlClampToEdge;
                adjusted = true;
            }
            if (is_mipmap_filter(host_min)) {
                host_min = desired_mag == 0x2600 ? 0x2600 : kGlLinear;
                adjusted = true;
            }
        }

        gl_tex_parameter_i(static_cast<HostGLenum>(target), static_cast<HostGLenum>(kGlTextureMagFilter), static_cast<HostGLint>(desired_mag));
        gl_tex_parameter_i(static_cast<HostGLenum>(target), static_cast<HostGLenum>(kGlTextureMinFilter), static_cast<HostGLint>(host_min));
        gl_tex_parameter_i(static_cast<HostGLenum>(target), static_cast<HostGLenum>(kGlTextureWrapS), static_cast<HostGLint>(host_wrap_s));
        gl_tex_parameter_i(static_cast<HostGLenum>(target), static_cast<HostGLenum>(kGlTextureWrapT), static_cast<HostGLint>(host_wrap_t));

        if (adjusted && runtime_->gl_state.debug_texture_compat_logs < 24) {
            ++runtime_->gl_state.debug_texture_compat_logs;
            Log("[gl] texture compat guest_tex=" + Hex32(texture)
                + " size=" + std::to_string(object.width) + "x" + std::to_string(object.height)
                + " desired_min=" + Hex32(static_cast<u32>(desired_min))
                + " host_min=" + Hex32(static_cast<u32>(host_min))
                + " wrap_s=" + Hex32(static_cast<u32>(desired_wrap_s)) + "->" + Hex32(static_cast<u32>(host_wrap_s))
                + " wrap_t=" + Hex32(static_cast<u32>(desired_wrap_t)) + "->" + Hex32(static_cast<u32>(host_wrap_t))
                + " mipmaps=" + std::to_string(object.mipmaps_generated ? 1 : 0));
        }
    };
    auto read_index = [&](const std::vector<u8>& bytes, const u32 type, const u32 index) -> u32 {
        const std::size_t element_size = GlTypeSize(type);
        const std::size_t offset = static_cast<std::size_t>(index) * element_size;
        if (element_size == 1) {
            return offset < bytes.size() ? bytes[offset] : 0;
        }
        if (element_size == 2) {
            return offset + 1 < bytes.size()
                ? static_cast<u32>(bytes[offset]) | (static_cast<u32>(bytes[offset + 1]) << 8)
                : 0;
        }
        if (element_size == 4) {
            return offset + 3 < bytes.size()
                ? static_cast<u32>(bytes[offset])
                    | (static_cast<u32>(bytes[offset + 1]) << 8)
                    | (static_cast<u32>(bytes[offset + 2]) << 16)
                    | (static_cast<u32>(bytes[offset + 3]) << 24)
                : 0;
        }
        return 0;
    };
    auto configure_client_arrays = [&](const u32 first_vertex, const u32 vertex_count) {
        if (!use_host_gl) {
            return;
        }
        auto gl_bind_buffer = LookupHostGLProc<void (*)(HostGLenum, HostGLuint)>(host_gl, "glBindBuffer");
        auto gl_vertex_attrib_pointer = LookupHostGLProc<void (*)(HostGLuint, HostGLint, HostGLenum, HostGLboolean, HostGLsizei, const void*)>(host_gl, "glVertexAttribPointer");
        if (gl_bind_buffer == nullptr || gl_vertex_attrib_pointer == nullptr) {
            return;
        }
        for (auto& [index, attrib] : runtime_->gl_state.vertex_attribs) {
            if (!attrib.enabled || attrib.buffer != 0) {
                continue;
            }
            const std::size_t component_size = GlTypeSize(attrib.type);
            const std::size_t element_size = component_size * static_cast<std::size_t>(std::max<s32>(1, attrib.size));
            const std::size_t stride = attrib.stride > 0 ? static_cast<std::size_t>(attrib.stride) : element_size;
            const std::size_t total_size = (vertex_count == 0 || element_size == 0)
                ? 0
                : stride * static_cast<std::size_t>(vertex_count - 1) + element_size;
            attrib.scratch.clear();
            if (attrib.pointer != 0 && total_size != 0) {
                attrib.scratch = memory_.ReadBuffer(attrib.pointer + static_cast<u32>(stride * first_vertex), total_size);
            }
            gl_bind_buffer(0x8892, 0);
            gl_vertex_attrib_pointer(
                static_cast<HostGLuint>(index),
                static_cast<HostGLint>(attrib.size),
                static_cast<HostGLenum>(attrib.type),
                attrib.normalized ? 1 : 0,
                static_cast<HostGLsizei>(attrib.stride),
                attrib.scratch.empty() ? nullptr : attrib.scratch.data());
        }
        gl_bind_buffer(0x8892, host_buffer_id(runtime_->gl_state.bound_buffers[0x8892]));
    };

    if (name == "_CGAffineTransformMakeRotation") {
        const float angle = read_arg_float(1);
        write_transform(Arg(0), {std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0.0f, 0.0f});
        return Arg(0);
    }
    if (name == "_CGAffineTransformMakeScale") {
        write_transform(Arg(0), {read_arg_float(1), 0.0f, 0.0f, read_arg_float(2), 0.0f, 0.0f});
        return Arg(0);
    }
    if (name == "_CGAffineTransformMakeTranslation") {
        write_transform(Arg(0), {1.0f, 0.0f, 0.0f, 1.0f, read_arg_float(1), read_arg_float(2)});
        return Arg(0);
    }
    if (name == "_CGAffineTransformConcat") {
        write_transform(Arg(0), concat_transform(read_transform(1), read_transform(7)));
        return Arg(0);
    }
    if (name == "_CGAffineTransformRotate") {
        const auto transform = read_transform(1);
        const float angle = read_arg_float(7);
        const std::array<float, 6> rotation{std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), 0.0f, 0.0f};
        write_transform(Arg(0), concat_transform(transform, rotation));
        return Arg(0);
    }
    if (name == "_CGAffineTransformScale") {
        const auto transform = read_transform(1);
        const std::array<float, 6> scale{read_arg_float(7), 0.0f, 0.0f, read_arg_float(8), 0.0f, 0.0f};
        write_transform(Arg(0), concat_transform(transform, scale));
        return Arg(0);
    }
    if (name == "_CGRectInset") {
        const auto rect = read_rect(1);
        const float dx = read_arg_float(5);
        const float dy = read_arg_float(6);
        write_rect(Arg(0), {rect[0] + dx, rect[1] + dy, rect[2] - dx * 2.0f, rect[3] - dy * 2.0f});
        return Arg(0);
    }
    if (name == "_CGRectOffset") {
        const auto rect = read_rect(1);
        write_rect(Arg(0), {rect[0] + read_arg_float(5), rect[1] + read_arg_float(6), rect[2], rect[3]});
        return Arg(0);
    }
    if (name == "_CGRectContainsPoint") {
        const auto rect = read_rect(0);
        const float px = read_arg_float(4);
        const float py = read_arg_float(5);
        return px >= rect[0] && py >= rect[1] && px <= rect[0] + rect[2] && py <= rect[1] + rect[3] ? 1u : 0u;
    }
    if (name == "_CGRectEqualToRect") {
        return read_rect(0) == read_rect(4) ? 1u : 0u;
    }
    if (name == "_CGRectGetHeight") {
        SetReturnU32(FloatToBits(read_arg_float(3)));
        return cpu_->Regs()[0];
    }
    if (name == "_CGRectGetMinX") {
        SetReturnU32(FloatToBits(read_arg_float(0)));
        return cpu_->Regs()[0];
    }
    if (name == "_CGRectGetMinY") {
        SetReturnU32(FloatToBits(read_arg_float(1)));
        return cpu_->Regs()[0];
    }
    if (name == "_CGRectGetWidth") {
        SetReturnU32(FloatToBits(read_arg_float(2)));
        return cpu_->Regs()[0];
    }
    if (name == "_CGDataProviderCreateWithCFData") {
        const auto data_it = host_objects_.find(Arg(0));
        if (data_it == host_objects_.end() || data_it->second.kind != ObjKind::Data) {
            return 0;
        }
        const u32 handle = make_runtime_object("CGDataProvider", "CGDataProvider");
        auto& object = host_objects_[handle];
        object.backing_store = Arg(0);
        object.bytes = data_it->second.bytes;
        return handle;
    }
    if (name == "_CGFontCreateWithDataProvider") {
        const auto provider_it = host_objects_.find(Arg(0));
        if (provider_it == host_objects_.end() || provider_it->second.bytes.empty()) {
            return 0;
        }
        const u32 handle = make_runtime_object("CGFont", "CGFont");
        auto& object = host_objects_[handle];
        object.backing_store = Arg(0);
        object.bytes = provider_it->second.bytes;
        return handle;
    }
    if (name == "_CTFontCreateWithGraphicsFont") {
        const u32 graphics_font = Arg(0);
        if (graphics_font == 0) {
            return 0;
        }
        const u32 handle = make_runtime_object("CTFont", "CTFont");
        auto& object = host_objects_[handle];
        object.backing_store = graphics_font;
        object.number_value = read_arg_float(1);
        if (object.number_value <= 0.0) {
            object.number_value = 16.0;
        }
        if (const auto font_it = host_objects_.find(graphics_font); font_it != host_objects_.end()) {
            object.bytes = font_it->second.bytes;
            object.string_value = font_it->second.string_value;
        }
        return handle;
    }
    if (name == "_CTFontCreateWithName") {
        const u32 handle = make_runtime_object("CTFont", "CTFont.named");
        auto& object = host_objects_[handle];
        object.string_value = DecodeNSString(Arg(0)).value_or("Helvetica");
        object.number_value = read_arg_float(1);
        if (object.number_value <= 0.0) {
            object.number_value = 16.0;
        }
        return handle;
    }
    if (name == "_CTFontCreateWithFontDescriptor") {
        const u32 handle = make_runtime_object("CTFont", "CTFont.descriptor");
        auto& object = host_objects_[handle];
        object.number_value = read_arg_float(1);
        if (object.number_value <= 0.0f) {
            object.number_value = 16.0f;
        }
        if (const auto descriptor_it = host_objects_.find(Arg(0)); descriptor_it != host_objects_.end()) {
            object.backing_store = descriptor_it->second.backing_store;
            object.bytes = descriptor_it->second.bytes;
            object.string_value = descriptor_it->second.string_value;
            if (object.bytes.empty() && object.backing_store != 0) {
                if (const auto backing_it = host_objects_.find(object.backing_store); backing_it != host_objects_.end()) {
                    if (object.bytes.empty()) {
                        object.bytes = backing_it->second.bytes;
                    }
                    if (object.string_value.empty()) {
                        object.string_value = backing_it->second.string_value;
                    }
                }
            }
        }
        return handle;
    }
    if (name == "_CTFontCreateCopyWithAttributes") {
        const auto source_it = host_objects_.find(Arg(0));
        if (source_it == host_objects_.end()) {
            return 0;
        }
        const u32 handle = make_runtime_object("CTFont", "CTFont.copy");
        auto& object = host_objects_[handle];
        object = source_it->second;
        object.class_name = "CTFont";
        object.isa = EnsureClass("CTFont");
        const float size = read_arg_float(1);
        if (size > 0.0f) {
            object.number_value = size;
        }
        memory_.Write32(handle, EnsureClass("CTFont"));
        return handle;
    }
    if (name == "_CTFontCopyFamilyName") {
        const auto it = host_objects_.find(Arg(0));
        return EnsureNSString(it != host_objects_.end() && !it->second.string_value.empty() ? it->second.string_value : "Helvetica");
    }
    if (name == "_CTFontCopyFontDescriptor" || name == "_CTFontDescriptorCreateWithAttributes") {
        const u32 handle = make_runtime_object("CTFontDescriptor", name);
        host_objects_[handle].backing_store = Arg(0);
        if (const auto source_it = host_objects_.find(Arg(0)); source_it != host_objects_.end()) {
            host_objects_[handle].bytes = source_it->second.bytes;
            host_objects_[handle].string_value = source_it->second.string_value;
        }
        return handle;
    }
    if (name == "_CTFontGetAscent") {
        const float size = font_size_for_handle(Arg(0));
        if (!kUseRasterCoreTextMetrics) {
            SetReturnU32(FloatToBits(size * 0.8f));
            return cpu_->Regs()[0];
        }
        if (RuntimeState::RasterFont* const raster_font = ensure_raster_font(Arg(0)); raster_font != nullptr) {
            SetReturnU32(FloatToBits(raster_font->ascent * stbtt_ScaleForPixelHeight(&raster_font->info, size)));
            return cpu_->Regs()[0];
        }
        SetReturnU32(FloatToBits(size * 0.8f));
        return cpu_->Regs()[0];
    }
    if (name == "_CTFontGetDescent") {
        const float size = font_size_for_handle(Arg(0));
        if (!kUseRasterCoreTextMetrics) {
            SetReturnU32(FloatToBits(size * 0.2f));
            return cpu_->Regs()[0];
        }
        if (RuntimeState::RasterFont* const raster_font = ensure_raster_font(Arg(0)); raster_font != nullptr) {
            SetReturnU32(FloatToBits(-raster_font->descent * stbtt_ScaleForPixelHeight(&raster_font->info, size)));
            return cpu_->Regs()[0];
        }
        SetReturnU32(FloatToBits(size * 0.2f));
        return cpu_->Regs()[0];
    }
    if (name == "_CTFontGetLeading") {
        const float size = font_size_for_handle(Arg(0));
        if (!kUseRasterCoreTextMetrics) {
            SetReturnU32(FloatToBits(size * 0.1f));
            return cpu_->Regs()[0];
        }
        if (RuntimeState::RasterFont* const raster_font = ensure_raster_font(Arg(0)); raster_font != nullptr) {
            SetReturnU32(FloatToBits(std::max(0.0f, raster_font->line_gap * stbtt_ScaleForPixelHeight(&raster_font->info, size))));
            return cpu_->Regs()[0];
        }
        SetReturnU32(FloatToBits(size * 0.1f));
        return cpu_->Regs()[0];
    }
    if (name == "_CTFontGetSize") {
        const float size = font_size_for_handle(Arg(0));
        SetReturnU32(FloatToBits(size));
        return cpu_->Regs()[0];
    }
    if (name == "_CTFontGetUnderlineThickness") {
        const float size = font_size_for_handle(Arg(0));
        SetReturnU32(FloatToBits(std::max(size * 0.05f, 1.0f)));
        return cpu_->Regs()[0];
    }
    if (name == "_CTFontGetSymbolicTraits") {
        return 0;
    }
    if (name == "_CTFontGetAdvancesForGlyphs") {
        const float size = font_size_for_handle(Arg(0));
        const RuntimeState::RasterFont* const raster_font = ensure_raster_font(Arg(0));
        float total_advance = 0.0f;
        const float fallback_advance = size * 0.6f;
        const float scale = raster_font != nullptr ? stbtt_ScaleForPixelHeight(&raster_font->info, size) : 0.0f;
        if (Arg(3) != 0) {
            for (u32 i = 0; i < Arg(4); ++i) {
                float advance = fallback_advance;
                if (raster_font != nullptr) {
                    int advance_width = 0;
                    int left_side_bearing = 0;
                    stbtt_GetGlyphHMetrics(&raster_font->info, static_cast<int>(memory_.Read16(Arg(2) + i * 2)), &advance_width, &left_side_bearing);
                    (void)left_side_bearing;
                    advance = advance_width * scale;
                }
                total_advance += advance;
                memory_.Write32(Arg(3) + i * 8 + 0, FloatToBits(advance));
                memory_.Write32(Arg(3) + i * 8 + 4, FloatToBits(0.0f));
            }
        } else if (raster_font != nullptr) {
            for (u32 i = 0; i < Arg(4); ++i) {
                int advance_width = 0;
                int left_side_bearing = 0;
                stbtt_GetGlyphHMetrics(&raster_font->info, static_cast<int>(memory_.Read16(Arg(2) + i * 2)), &advance_width, &left_side_bearing);
                (void)left_side_bearing;
                total_advance += advance_width * scale;
            }
        } else {
            total_advance = fallback_advance * static_cast<float>(Arg(4));
        }
        SetReturnU32(FloatToBits(total_advance));
        return cpu_->Regs()[0];
    }
    if (name == "_CTFontGetBoundingRectsForGlyphs") {
        const u32 out_rect = Arg(0);
        const u32 font = Arg(1);
        const u32 glyphs = Arg(3);
        const u32 bounding_rects = Arg(4);
        const u32 count = Arg(5);
        RuntimeState::RasterFont* const bounds_font = ensure_raster_font(font);
        if (bounds_font == nullptr) {
            const float size = font_size_for_handle(font);
            const float width = size * 0.6f;
            const float height = size;
            if (bounding_rects != 0) {
                for (u32 i = 0; i < count; ++i) {
                    write_float(bounding_rects + i * 16 + 0, 0.0f);
                    write_float(bounding_rects + i * 16 + 4, -size * 0.2f);
                    write_float(bounding_rects + i * 16 + 8, width);
                    write_float(bounding_rects + i * 16 + 12, height);
                }
            }
            write_float(out_rect + 0, 0.0f);
            write_float(out_rect + 4, -size * 0.2f);
            write_float(out_rect + 8, count == 0 ? 0.0f : width);
            write_float(out_rect + 12, count == 0 ? 0.0f : height);
            return out_rect;
        }
        float min_x = 0.0f;
        float min_y = 0.0f;
        float max_x = 0.0f;
        float max_y = 0.0f;
        bool has_rect = false;
        {
            RuntimeState::RasterFont* const raster_font = bounds_font;
            const float scale = stbtt_ScaleForPixelHeight(&raster_font->info, font_size_for_handle(font));
            for (u32 i = 0; i < count; ++i) {
                const int glyph_index = static_cast<int>(memory_.Read16(glyphs + i * 2));
                int x0 = 0;
                int y0 = 0;
                int x1 = 0;
                int y1 = 0;
                stbtt_GetGlyphBox(&raster_font->info, glyph_index, &x0, &y0, &x1, &y1);
                const float rx = x0 * scale;
                const float ry = y0 * scale;
                const float rw = (x1 - x0) * scale;
                const float rh = (y1 - y0) * scale;
                if (bounding_rects != 0) {
                    write_float(bounding_rects + i * 16 + 0, rx);
                    write_float(bounding_rects + i * 16 + 4, ry);
                    write_float(bounding_rects + i * 16 + 8, rw);
                    write_float(bounding_rects + i * 16 + 12, rh);
                }
                if (!has_rect) {
                    min_x = rx;
                    min_y = ry;
                    max_x = rx + rw;
                    max_y = ry + rh;
                    has_rect = true;
                } else {
                    min_x = std::min(min_x, rx);
                    min_y = std::min(min_y, ry);
                    max_x = std::max(max_x, rx + rw);
                    max_y = std::max(max_y, ry + rh);
                }
            }
        }
        write_float(out_rect + 0, min_x);
        write_float(out_rect + 4, min_y);
        write_float(out_rect + 8, has_rect ? (max_x - min_x) : 0.0f);
        write_float(out_rect + 12, has_rect ? (max_y - min_y) : 0.0f);
        return out_rect;
    }
    if (name == "_CTFontDrawGlyphs") {
        return draw_glyphs_to_context(Arg(0), Arg(1), Arg(2), Arg(3), Arg(4)) ? 1u : 0u;
    }
    if (name == "_CGPathCreateMutable") {
        return make_runtime_object("CGPath", "CGPath");
    }
    if (name == "_CGPathAddRect") {
        auto& path = host_objects_[Arg(0)];
        path.dict["rect.x.bits"] = FloatToBits(read_arg_float(2));
        path.dict["rect.y.bits"] = FloatToBits(read_arg_float(3));
        path.dict["rect.w.bits"] = FloatToBits(read_arg_float(4));
        path.dict["rect.h.bits"] = FloatToBits(read_arg_float(5));
        return 0;
    }
    // --- CoreText CTLine/CTRun/CTFrame pipeline ---
    auto build_ct_line = [&](const u32 attr_string_handle) -> u32 {
        const u32 line_handle = make_runtime_object("CTLine", "CTLine");
        auto& line = runtime_->ct_lines[line_handle];
        line.attributed_string = attr_string_handle;
        const auto as_it = host_objects_.find(attr_string_handle);
        if (as_it == host_objects_.end()) {
            line.text = "?";
            return line_handle;
        }
        const auto& as_obj = as_it->second;
        line.text = as_obj.string_value.empty()
            ? DecodeNSString(as_obj.backing_store).value_or("?")
            : as_obj.string_value;
        auto find_font_handle = [&]() -> u32 {
            for (const char* key : {"NSFont", "CTFont", "NSFontAttributeName", "kCTFontAttributeName"}) {
                if (const auto it = as_obj.dict.find(key); it != as_obj.dict.end()) {
                    return it->second;
                }
            }
            for (const auto& [handle, object] : host_objects_) {
                if (object.class_name == "CTFont"
                    && (!object.bytes.empty() || object.backing_store != 0 || object.number_value > 0.0)) {
                    return handle;
                }
            }
            for (const auto& [handle, object] : host_objects_) {
                if (looks_like_font_object(object) && (!object.bytes.empty() || object.backing_store != 0)) {
                    return handle;
                }
            }
            return 0;
        };

        const u32 discovered_font = find_font_handle();
        const u32 font = discovered_font != 0 ? discovered_font : fallback_font_handle();
        const float font_size = font != 0 ? font_size_for_handle(font) : 16.0f;
        line.font_handle = font;
        line.font_size = font_size;

        RuntimeState::RasterFont* const raster_font = ensure_raster_font(font);
        const float stb_scale = raster_font != nullptr
            ? stbtt_ScaleForPixelHeight(&raster_font->info, font_size)
            : 0.0f;

        RuntimeState::CTGlyphRun run;
        run.font_handle = font;
        run.ascent = font_size * 0.8f;
        run.descent = font_size * 0.2f;
        run.leading = font_size * 0.1f;
        float pen_x = 0.0f;
        for (const unsigned char c : line.text) {
            u16 glyph = static_cast<u16>(c);
            float advance = c == ' ' ? font_size * 0.33f : font_size * 0.55f;
            if (raster_font != nullptr) {
                glyph = static_cast<u16>(stbtt_FindGlyphIndex(&raster_font->info, static_cast<int>(c)));
                int advance_width = 0;
                int left_side_bearing = 0;
                stbtt_GetGlyphHMetrics(&raster_font->info, static_cast<int>(glyph), &advance_width, &left_side_bearing);
                (void)left_side_bearing;
                advance = advance_width * stb_scale;
            }
            run.glyphs.push_back(glyph);
            run.positions.push_back({pen_x, 0.0f});
            pen_x += advance;
        }
        run.width = pen_x;
        line.total_width = pen_x;
        line.ascent = run.ascent;
        line.descent = run.descent;
        line.leading = run.leading;
        line.runs.push_back(std::move(run));
        return line_handle;
    };
    auto draw_ct_line = [&](const u32 line_handle, const u32 context_handle) {
        const auto line_it = runtime_->ct_lines.find(line_handle);
        if (line_it == runtime_->ct_lines.end()) {
            return;
        }
        const auto& line = line_it->second;
        const auto ctx_it = runtime_->graphics_contexts.find(context_handle);
        const float origin_x = ctx_it != runtime_->graphics_contexts.end() ? ctx_it->second.text_position[0] : 0.0f;
        const float origin_y = ctx_it != runtime_->graphics_contexts.end() ? ctx_it->second.text_position[1] : 0.0f;
        for (const auto& run : line.runs) {
            if (run.glyphs.empty()) {
                continue;
            }
            const u32 glyph_count = static_cast<u32>(run.glyphs.size());
            const u32 glyphs_buf = AllocateData(std::max<u32>(1, glyph_count * 2), 2, "ct_glyphs");
            const u32 pos_buf = AllocateData(std::max<u32>(1, glyph_count * 8), 4, "ct_positions");
            for (u32 i = 0; i < glyph_count; ++i) {
                memory_.Write16(glyphs_buf + i * 2, run.glyphs[i]);
                memory_.Write32(pos_buf + i * 8 + 0, FloatToBits(run.positions[i][0] + origin_x));
                memory_.Write32(pos_buf + i * 8 + 4, FloatToBits(run.positions[i][1] + origin_y));
            }
            draw_glyphs_to_context(run.font_handle, glyphs_buf, pos_buf, glyph_count, context_handle);
        }
    };
    if (name == "_CTLineCreateWithAttributedString") {
        return build_ct_line(Arg(0));
    }
    if (name == "_CTLineGetTypographicBounds") {
        const auto it = runtime_->ct_lines.find(Arg(0));
        float asc = 12.0f, desc = 3.0f, lead = 1.0f;
        float width = 50.0f;
        if (it != runtime_->ct_lines.end()) {
            asc = it->second.ascent; desc = it->second.descent;
            lead = it->second.leading; width = it->second.total_width;
        }
        if (Arg(1) != 0) write_float(Arg(1), asc);
        if (Arg(2) != 0) write_float(Arg(2), desc);
        if (Arg(3) != 0) write_float(Arg(3), lead);
        SetReturnDouble(static_cast<double>(width));
        return cpu_->Regs()[0];
    }
    if (name == "_CTLineDraw") {
        if (runtime_->debug_text_draw_logs < 32) {
            ++runtime_->debug_text_draw_logs;
            Log("[text] CTLineDraw line=" + Hex32(Arg(0)) + " ctx=" + Hex32(Arg(1)));
        }
        draw_ct_line(Arg(0), Arg(1));
        return 0;
    }
    if (name == "_CTLineGetImageBounds") {
        const auto it = runtime_->ct_lines.find(Arg(1));
        if (it == runtime_->ct_lines.end()) {
            write_rect(Arg(0), {0.0f, 0.0f, 0.0f, 0.0f});
            return Arg(0);
        }
        write_rect(Arg(0), {0.0f, -it->second.descent, it->second.total_width, it->second.ascent + it->second.descent});
        return Arg(0);
    }
    if (name == "_CTLineGetOffsetForStringIndex") {
        const auto it = runtime_->ct_lines.find(Arg(0));
        if (it == runtime_->ct_lines.end() || it->second.runs.empty()) {
            SetReturnU32(FloatToBits(0.0f));
            return cpu_->Regs()[0];
        }
        const auto& run = it->second.runs.front();
        const u32 index = std::min<u32>(Arg(1), static_cast<u32>(run.positions.size()));
        const float offset = index < run.positions.size() ? run.positions[index][0] : it->second.total_width;
        SetReturnU32(FloatToBits(offset));
        return cpu_->Regs()[0];
    }
    if (name == "_CTLineGetPenOffsetForFlush") {
        const auto it = runtime_->ct_lines.find(Arg(0));
        const float line_width = it == runtime_->ct_lines.end() ? 0.0f : it->second.total_width;
        const float flush_factor = read_arg_float(1);
        const float flush_width = read_arg_float(2);
        SetReturnU32(FloatToBits(std::max(0.0f, flush_width - line_width) * std::clamp(flush_factor, 0.0f, 1.0f)));
        return cpu_->Regs()[0];
    }
    if (name == "_CTLineGetStringIndexForPosition") {
        const auto it = runtime_->ct_lines.find(Arg(0));
        if (it == runtime_->ct_lines.end() || it->second.runs.empty()) {
            return 0;
        }
        const float x = read_arg_float(1);
        const auto& run = it->second.runs.front();
        u32 best_index = static_cast<u32>(run.positions.size());
        for (u32 i = 0; i < run.positions.size(); ++i) {
            if (run.positions[i][0] >= x) {
                best_index = i;
                break;
            }
        }
        return best_index;
    }
    if (name == "_CTLineGetGlyphRuns") {
        const auto it = runtime_->ct_lines.find(Arg(0));
        if (it == runtime_->ct_lines.end() || it->second.runs.empty()) {
            return EnsureArray({});
        }
        // Create a CTRun object for each run and return as array
        std::vector<u32> run_handles;
        for (std::size_t i = 0; i < it->second.runs.size(); ++i) {
            const u32 rh = make_runtime_object("CTRun", "CTRun");
            host_objects_[rh].dict["_line"] = Arg(0);
            host_objects_[rh].dict["_run_index"] = static_cast<u32>(i);
            run_handles.push_back(rh);
        }
        return EnsureArray(run_handles);
    }
    if (name == "_CTLineGetStringRange") {
        // Returns CFRange {location, length} via stret or r0/r1
        const auto it = runtime_->ct_lines.find(Arg(0));
        u32 len = 0;
        if (it != runtime_->ct_lines.end()) len = static_cast<u32>(it->second.text.size());
        cpu_->Regs()[0] = 0;
        cpu_->Regs()[1] = len;
        return 0;
    }
    if (name == "_CTRunGetGlyphCount") {
        const auto& ro = host_objects_[Arg(0)];
        if (auto li = ro.dict.find("_line"); li != ro.dict.end()) {
            if (auto ri = ro.dict.find("_run_index"); ri != ro.dict.end()) {
                const auto line_it = runtime_->ct_lines.find(li->second);
                if (line_it != runtime_->ct_lines.end() && ri->second < line_it->second.runs.size()) {
                    return static_cast<u32>(line_it->second.runs[ri->second].glyphs.size());
                }
            }
        }
        return 0;
    }
	    if (name == "_CTRunGetGlyphs" || name == "_CTRunGetPositions") {
	        const auto& ro = host_objects_[Arg(0)];
	        u32 line_h = 0, run_idx = 0;
	        if (auto li = ro.dict.find("_line"); li != ro.dict.end()) line_h = li->second;
	        if (auto ri = ro.dict.find("_run_index"); ri != ro.dict.end()) run_idx = ri->second;
	        const auto line_it = runtime_->ct_lines.find(line_h);
	        if (line_it != runtime_->ct_lines.end() && run_idx < line_it->second.runs.size()) {
	            const auto& run = line_it->second.runs[run_idx];
	            const u32 start = std::min<u32>(Arg(1), static_cast<u32>(run.glyphs.size()));
	            const u32 available = static_cast<u32>(run.glyphs.size()) - start;
	            const u32 requested = Arg(2);
	            const u32 count = (requested == 0 || requested == 0xFFFFFFFFu)
	                ? available
	                : std::min(requested, available);
	            const u32 buf = Arg(3);
	            if (buf != 0) {
                if (name == "_CTRunGetGlyphs") {
                    for (u32 i = 0; i < count; ++i) {
                        const u32 glyph_index = start + i;
                        memory_.Write16(buf + i * 2, run.glyphs[glyph_index]);
                    }
                } else {
	                    for (u32 i = 0; i < count; ++i) {
	                        memory_.Write32(buf + i * 8 + 0, FloatToBits(run.positions[start + i][0]));
	                        memory_.Write32(buf + i * 8 + 4, FloatToBits(run.positions[start + i][1]));
	                    }
	                }
	            }
        }
        return 0;
    }
    if (name == "_CTRunGetStringIndices") {
        const auto& ro = host_objects_[Arg(0)];
	        u32 line_h = 0, run_idx = 0;
	        if (auto li = ro.dict.find("_line"); li != ro.dict.end()) line_h = li->second;
	        if (auto ri = ro.dict.find("_run_index"); ri != ro.dict.end()) run_idx = ri->second;
	        const auto line_it = runtime_->ct_lines.find(line_h);
	        if (line_it != runtime_->ct_lines.end() && run_idx < line_it->second.runs.size() && Arg(3) != 0) {
	            const auto& run = line_it->second.runs[run_idx];
	            const u32 start = std::min<u32>(Arg(1), static_cast<u32>(run.glyphs.size()));
	            const u32 available = static_cast<u32>(run.glyphs.size()) - start;
	            const u32 requested = Arg(2);
	            const u32 count = (requested == 0 || requested == 0xFFFFFFFFu)
	                ? available
	                : std::min(requested, available);
	            for (u32 i = 0; i < count; ++i) {
	                memory_.Write32(Arg(3) + i * 4, start + i);
	            }
	        }
	        return 0;
    }
    if (name == "_CTRunGetStringRange") {
        const auto& ro = host_objects_[Arg(0)];
        u32 line_h = 0, run_idx = 0;
        if (auto li = ro.dict.find("_line"); li != ro.dict.end()) line_h = li->second;
        if (auto ri = ro.dict.find("_run_index"); ri != ro.dict.end()) run_idx = ri->second;
        const auto line_it = runtime_->ct_lines.find(line_h);
        u32 len = 0;
        if (line_it != runtime_->ct_lines.end() && run_idx < line_it->second.runs.size())
            len = static_cast<u32>(line_it->second.runs[run_idx].glyphs.size());
        cpu_->Regs()[0] = 0;
        cpu_->Regs()[1] = len;
        return 0;
    }
    if (name == "_CTRunGetTypographicBounds") {
        const auto& ro = host_objects_[Arg(0)];
        u32 line_h = 0, run_idx = 0;
        if (auto li = ro.dict.find("_line"); li != ro.dict.end()) line_h = li->second;
        if (auto ri = ro.dict.find("_run_index"); ri != ro.dict.end()) run_idx = ri->second;
        float w = 50, a = 12, d = 3, l = 1;
        const auto line_it = runtime_->ct_lines.find(line_h);
	        if (line_it != runtime_->ct_lines.end() && run_idx < line_it->second.runs.size()) {
	            const auto& run = line_it->second.runs[run_idx];
	            w = run.width; a = run.ascent; d = run.descent; l = run.leading;
	        }
	        if (Arg(3) != 0) write_float(Arg(3), a);
	        if (Arg(4) != 0) write_float(Arg(4), d);
	        if (Arg(5) != 0) write_float(Arg(5), l);
	        SetReturnDouble(static_cast<double>(w));
	        return cpu_->Regs()[0];
	    }
    if (name == "_CTRunGetAdvances") {
        const auto& ro = host_objects_[Arg(0)];
        u32 line_h = 0, run_idx = 0;
	        if (auto li = ro.dict.find("_line"); li != ro.dict.end()) line_h = li->second;
	        if (auto ri = ro.dict.find("_run_index"); ri != ro.dict.end()) run_idx = ri->second;
	        const auto line_it = runtime_->ct_lines.find(line_h);
	        if (line_it != runtime_->ct_lines.end() && run_idx < line_it->second.runs.size() && Arg(3) != 0) {
	            const auto& run = line_it->second.runs[run_idx];
	            const u32 start = std::min<u32>(Arg(1), static_cast<u32>(run.positions.size()));
	            const u32 available = static_cast<u32>(run.positions.size()) - start;
	            const u32 requested = Arg(2);
	            const u32 count = (requested == 0 || requested == 0xFFFFFFFFu)
	                ? available
	                : std::min(requested, available);
	            for (u32 i = 0; i < count; ++i) {
	                const u32 index = start + i;
	                const float current = run.positions[index][0];
	                const float next = (index + 1) < run.positions.size() ? run.positions[index + 1][0] : run.width;
	                memory_.Write32(Arg(3) + i * 8 + 0, FloatToBits(next - current));
	                memory_.Write32(Arg(3) + i * 8 + 4, FloatToBits(0.0f));
	            }
	        }
        return 0;
    }
    if (name == "_CTRunGetAttributes") {
        const auto& run_object = host_objects_[Arg(0)];
        const auto line_it = run_object.dict.find("_line");
        if (line_it == run_object.dict.end()) {
            return EnsureDictionary({});
        }
        const auto ct_line_it = runtime_->ct_lines.find(line_it->second);
        if (ct_line_it == runtime_->ct_lines.end()) {
            return EnsureDictionary({});
        }
        u32 run_font = ct_line_it->second.font_handle;
        if (const auto run_index_it = run_object.dict.find("_run_index");
            run_index_it != run_object.dict.end() && run_index_it->second < ct_line_it->second.runs.size()) {
            run_font = ct_line_it->second.runs[run_index_it->second].font_handle;
        }
        const auto attr_it = host_objects_.find(ct_line_it->second.attributed_string);
        if (attr_it == host_objects_.end()) {
            std::unordered_map<std::string, u32> fallback_attrs;
            if (run_font != 0) {
                fallback_attrs["NSFont"] = run_font;
                fallback_attrs["CTFont"] = run_font;
                fallback_attrs["NSFontAttributeName"] = run_font;
                fallback_attrs["kCTFontAttributeName"] = run_font;
            }
            return EnsureDictionary(fallback_attrs);
        }
        std::unordered_map<std::string, u32> attrs = attr_it->second.dict;
        if (run_font != 0) {
            attrs.try_emplace("NSFont", run_font);
            attrs.try_emplace("CTFont", run_font);
            attrs.try_emplace("NSFontAttributeName", run_font);
            attrs.try_emplace("kCTFontAttributeName", run_font);
        }
        return EnsureDictionary(attrs);
    }
    if (name == "_CTTypesetterCreateWithAttributedString") {
        const u32 handle = make_runtime_object("CTTypesetter", "CTTypesetter");
        host_objects_[handle].dict["_attr_string"] = Arg(0);
        return handle;
    }
    if (name == "_CTTypesetterCreateLine") {
        const auto it = host_objects_.find(Arg(0));
        if (it == host_objects_.end()) {
            return 0;
        }
        const auto attr_it = it->second.dict.find("_attr_string");
        return attr_it == it->second.dict.end() ? 0u : build_ct_line(attr_it->second);
    }
    if (name == "_CTTypesetterSuggestLineBreak") {
        const auto it = host_objects_.find(Arg(0));
        if (it == host_objects_.end()) {
            return 0;
        }
        const auto attr_it = it->second.dict.find("_attr_string");
        if (attr_it == it->second.dict.end()) {
            return 0;
        }
        const auto as_it = host_objects_.find(attr_it->second);
        if (as_it == host_objects_.end()) {
            return 0;
        }
        const std::string& text = as_it->second.string_value;
        const std::size_t start = std::min<std::size_t>(Arg(1), text.size());
        float width_limit = read_arg_float(2);
        if (width_limit <= 0.0f) {
            return static_cast<u32>(text.size() - start);
        }
        float advance = 8.0f;
        u32 font = 0;
        for (const char* key : {"NSFont", "CTFont", "NSFontAttributeName", "kCTFontAttributeName"}) {
            if (const auto fit = as_it->second.dict.find(key); fit != as_it->second.dict.end()) {
                font = fit->second;
                break;
            }
        }
        if (font != 0) {
            advance = font_size_for_handle(font) * 0.5f;
        }
        const u32 fit = static_cast<u32>(std::max<std::size_t>(1, static_cast<std::size_t>(width_limit / std::max(advance, 1.0f))));
        return std::min<u32>(fit, static_cast<u32>(text.size() - start));
    }
    if (name == "_CTLineCreateTruncatedLine") {
        return Arg(0);
    }
    if (name == "_CTParagraphStyleCreate") {
        return make_runtime_object("CTParagraphStyle", "CTParagraphStyle");
    }
    if (name == "_CTFramesetterCreateWithAttributedString") {
        const u32 handle = make_runtime_object("CTFramesetter", "CTFramesetter");
        host_objects_[handle].dict["_attr_string"] = Arg(0);
        return handle;
    }
    if (name == "_CTFramesetterCreateFrame") {
        // Args: framesetter, stringRange(loc,len), path, frameAttributes
        const u32 fs = Arg(0);
        const u32 handle = make_runtime_object("CTFrame", "CTFrame");
        auto& frame = runtime_->ct_frames[handle];
        frame.framesetter = fs;
        if (const auto path_it = host_objects_.find(Arg(3)); path_it != host_objects_.end()) {
            const auto& path = path_it->second.dict;
            const auto x_it = path.find("rect.x.bits");
            const auto y_it = path.find("rect.y.bits");
            const auto w_it = path.find("rect.w.bits");
            const auto h_it = path.find("rect.h.bits");
            if (x_it != path.end() && y_it != path.end() && w_it != path.end() && h_it != path.end()) {
                frame.frame_rect = {
                    BitsToFloat(x_it->second),
                    BitsToFloat(y_it->second),
                    BitsToFloat(w_it->second),
                    BitsToFloat(h_it->second),
                };
            }
        }
        // Get attributed string from framesetter
        u32 attr_str = 0;
        if (auto it = host_objects_.find(fs); it != host_objects_.end()) {
            if (auto ai = it->second.dict.find("_attr_string"); ai != it->second.dict.end())
                attr_str = ai->second;
        }
        if (attr_str != 0) {
            // Build one CTLine for the whole string (simple implementation)
            const u32 line_h = build_ct_line(attr_str);
            frame.line_handles.push_back(line_h);
        }
        return handle;
    }
    if (name == "_CTFrameGetLines") {
        const auto it = runtime_->ct_frames.find(Arg(0));
        if (it == runtime_->ct_frames.end()) return EnsureArray({});
        return EnsureArray(it->second.line_handles);
    }
    if (name == "_CTFrameGetLineOrigins") {
        // Args: frame, range(loc,len), origins_buf
        const auto it = runtime_->ct_frames.find(Arg(0));
        const u32 buf = Arg(3);
        if (it != runtime_->ct_frames.end() && buf != 0) {
            float y = 0;
            for (std::size_t i = 0; i < it->second.line_handles.size(); ++i) {
                write_float(buf + static_cast<u32>(i * 8 + 0), 0.0f);
                const auto li = runtime_->ct_lines.find(it->second.line_handles[i]);
                float line_h = 16.0f;
                if (li != runtime_->ct_lines.end()) line_h = li->second.ascent + li->second.descent + li->second.leading;
                write_float(buf + static_cast<u32>(i * 8 + 4), it->second.frame_rect[3] - y - (li != runtime_->ct_lines.end() ? li->second.ascent : 12.0f));
                y += line_h;
            }
        }
        return 0;
    }
    if (name == "_CTFrameDraw") {
        if (runtime_->debug_text_draw_logs < 32) {
            ++runtime_->debug_text_draw_logs;
            Log("[text] CTFrameDraw frame=" + Hex32(Arg(0)) + " ctx=" + Hex32(Arg(1)));
        }
        const auto it = runtime_->ct_frames.find(Arg(0));
        if (it != runtime_->ct_frames.end()) {
            float y = 0.0f;
            for (const u32 line_h : it->second.line_handles) {
                const auto line_it = runtime_->ct_lines.find(line_h);
                const float ascent = line_it != runtime_->ct_lines.end() ? line_it->second.ascent : 12.0f;
                auto& context = runtime_->graphics_contexts[Arg(1)];
                context.text_position = {it->second.frame_rect[0], it->second.frame_rect[3] - y - ascent};
                draw_ct_line(line_h, Arg(1));
                const float line_height = line_it != runtime_->ct_lines.end()
                    ? line_it->second.ascent + line_it->second.descent + line_it->second.leading
                    : 16.0f;
                y += line_height;
            }
        }
        return 0;
    }
    if (name == "_CTFramesetterSuggestFrameSizeWithConstraints") {
        // Args: framesetter, stringRange(loc,len), attributes, constraints(w,h), fitRange_out
        // Returns CGSize via stret or r0/r1
        float w = 100.0f, h = 20.0f;
        u32 attr_str = 0;
        if (auto fi = host_objects_.find(Arg(0)); fi != host_objects_.end()) {
            if (auto ai = fi->second.dict.find("_attr_string"); ai != fi->second.dict.end())
                attr_str = ai->second;
        }
        if (attr_str != 0) {
            const auto& as_obj = host_objects_[attr_str];
            float fs = 16.0f;
            u32 fh = 0;
            if (auto fit = as_obj.dict.find("NSFont"); fit != as_obj.dict.end()) { fh = fit->second; fs = font_size_for_handle(fh); }
            w = static_cast<float>(as_obj.string_value.size()) * fs * 0.5f;
            h = fs * 1.2f;
        }
        // Write fitRange if provided
        if (Arg(5) != 0) {
            memory_.Write32(Arg(5), 0);
            memory_.Write32(Arg(5) + 4, attr_str != 0 ? static_cast<u32>(host_objects_[attr_str].string_value.size()) : 0u);
        }
        // Return CGSize
        write_float(cpu_->Regs()[13] - 8, w);
        write_float(cpu_->Regs()[13] - 4, h);
        cpu_->Regs()[0] = FloatToBits(w);
        cpu_->Regs()[1] = FloatToBits(h);
        return cpu_->Regs()[0];
    }
    if (name == "_CGColorSpaceCreateDeviceRGB") {
        return make_runtime_object("CGColorSpace", "CGColorSpace");
    }
    if (name == "_CGColorSpaceRelease") {
        host_objects_.erase(Arg(0));
        return 0;
    }
    if (name == "_CGBitmapContextCreate") {
        const u32 handle = make_runtime_object("CGContext", "CGBitmapContext");
        auto& context = runtime_->graphics_contexts[handle];
        context.data = Arg(0);
        context.width = static_cast<s32>(Arg(1));
        context.height = static_cast<s32>(Arg(2));
        context.bits_per_component = Arg(3);
        context.bytes_per_row = static_cast<s32>(Arg(4) == 0 ? Arg(1) * 4 : Arg(4));
        context.color_space = Arg(5);
        context.bitmap_info = Arg(6);
        if (context.data == 0) {
            context.data = AllocateData(static_cast<u32>(std::max<s32>(1, context.bytes_per_row * context.height)), 4, "CGBitmapContext.data");
        }
        if (runtime_->debug_bitmap_context_logs < 32) {
            ++runtime_->debug_bitmap_context_logs;
            Log("[text] CGBitmapContextCreate ctx=" + Hex32(handle)
                + " data=" + Hex32(context.data)
                + " size=" + std::to_string(context.width) + "x" + std::to_string(context.height)
                + " bpc=" + std::to_string(context.bits_per_component)
                + " stride=" + std::to_string(context.bytes_per_row)
                + " bitmapInfo=" + Hex32(context.bitmap_info));
        }
        return handle;
    }
    if (name == "_CGBitmapContextCreateImage") {
        const auto ctx_it = runtime_->graphics_contexts.find(Arg(0));
        if (ctx_it == runtime_->graphics_contexts.end()) {
            return 0;
        }
        const u32 handle = make_runtime_object("CGImage", "CGImage");
        auto& image = runtime_->graphics_images[handle];
        image.width = ctx_it->second.width;
        image.height = ctx_it->second.height;
        image.bytes_per_row = ctx_it->second.bytes_per_row;
        if (ctx_it->second.data != 0 && ctx_it->second.bytes_per_row > 0 && ctx_it->second.height > 0) {
            image.pixels = memory_.ReadBuffer(ctx_it->second.data, static_cast<std::size_t>(ctx_it->second.bytes_per_row) * static_cast<std::size_t>(ctx_it->second.height));
        }
        update_generated_bitmap_skip(image, "CGBitmapContextCreateImage image=" + Hex32(handle));
        if (runtime_->debug_bitmap_context_logs < 32) {
            ++runtime_->debug_bitmap_context_logs;
            Log("[text] CGBitmapContextCreateImage ctx=" + Hex32(Arg(0))
                + " image=" + Hex32(handle)
                + " size=" + std::to_string(image.width) + "x" + std::to_string(image.height)
                + " stride=" + std::to_string(image.bytes_per_row));
        }
        return handle;
    }
    if (name == "_CGImageGetWidth") {
        return runtime_->graphics_images[Arg(0)].width;
    }
    if (name == "_CGImageGetHeight") {
        return runtime_->graphics_images[Arg(0)].height;
    }
    if (name == "_CGImageCreateWithImageInRect") {
        const auto image_it = runtime_->graphics_images.find(Arg(0));
        if (image_it == runtime_->graphics_images.end()) {
            return 0;
        }
        const auto rect = read_rect(1);
        const s32 src_x = std::max<s32>(0, static_cast<s32>(std::floor(rect[0])));
        const s32 src_y = std::max<s32>(0, static_cast<s32>(std::floor(rect[1])));
        const s32 width = std::max<s32>(0, std::min<s32>(image_it->second.width - src_x, static_cast<s32>(std::ceil(rect[2]))));
        const s32 height = std::max<s32>(0, std::min<s32>(image_it->second.height - src_y, static_cast<s32>(std::ceil(rect[3]))));
        const u32 handle = make_runtime_object("CGImage", "CGImage.crop");
        auto& image = runtime_->graphics_images[handle];
        image.width = width;
        image.height = height;
        image.bytes_per_row = std::max<s32>(4, width * 4);
        image.components = image_it->second.components;
        image.pixels.assign(static_cast<std::size_t>(std::max<s32>(0, width * height * 4)), 0);
        const std::size_t src_stride = static_cast<std::size_t>(
            image_it->second.bytes_per_row > 0
                ? image_it->second.bytes_per_row
                : image_it->second.width * 4);
        for (s32 y = 0; y < height; ++y) {
            for (s32 x = 0; x < width; ++x) {
                const std::size_t src =
                    static_cast<std::size_t>(src_y + y) * src_stride
                    + static_cast<std::size_t>(src_x + x) * 4;
                const std::size_t dst = static_cast<std::size_t>(y * width + x) * 4;
                if (src + 3 >= image_it->second.pixels.size() || dst + 3 >= image.pixels.size()) {
                    continue;
                }
                std::copy_n(image_it->second.pixels.data() + src, 4, image.pixels.data() + dst);
            }
        }
        if (image_it->second.skip_draw) {
            image.skip_draw = true;
        } else {
            const BitmapContentStats stats =
                AnalyzeBitmapContent(image.width, image.height, image.bytes_per_row, image.pixels);
            image.skip_draw = ShouldSkipGeneratedBitmapImage(stats);
        }
        return handle;
    }
    if (name == "_CGImageGetAlphaInfo") {
        return 1;  // kCGImageAlphaPremultipliedLast-ish
    }
    if (name == "_CGImageGetBitmapInfo") {
        return 1;
    }
    if (name == "_CGImageGetBitsPerComponent") {
        return 8;
    }
    if (name == "_CGImageGetColorSpace") {
        return make_runtime_object("CGColorSpace", "CGColorSpace");
    }
    if (name == "_CGImageRelease") {
        runtime_->graphics_images.erase(Arg(0));
        host_objects_.erase(Arg(0));
        return 0;
    }
    if (name == "_CGColorGetComponents") {
        std::array<float, 4> components{0.0f, 0.0f, 0.0f, 1.0f};
        if (const auto it = runtime_->graphics_images.find(Arg(0)); it != runtime_->graphics_images.end() && it->second.components.size() >= 4) {
            for (std::size_t i = 0; i < 4; ++i) {
                components[i] = it->second.components[i];
            }
        }
        const u32 data = AllocateData(16, 4, "CGColor.components");
        for (std::size_t i = 0; i < components.size(); ++i) {
            write_float(data + static_cast<u32>(i * 4), components[i]);
        }
        return data;
    }
    if (name == "_CGGradientCreateWithColors") {
        const u32 handle = make_runtime_object("CGGradient", "CGGradient");
        runtime_->graphics_gradients[handle].colors = host_objects_[Arg(1)].items;
        return handle;
    }
    if (name == "_CGGradientRelease") {
        runtime_->graphics_gradients.erase(Arg(0));
        host_objects_.erase(Arg(0));
        return 0;
    }
    if (name == "_CGContextSetGrayFillColor") {
        const float gray = std::clamp(read_arg_float(1), 0.0f, 1.0f);
        const float alpha = std::clamp(read_arg_float(2), 0.0f, 1.0f);
        runtime_->graphics_contexts[Arg(0)].fill_color = {gray, gray, gray, alpha};
        return 0;
    }
    if (name == "_CGContextSetGrayStrokeColor") {
        const float gray = std::clamp(read_arg_float(1), 0.0f, 1.0f);
        const float alpha = std::clamp(read_arg_float(2), 0.0f, 1.0f);
        runtime_->graphics_contexts[Arg(0)].stroke_color = {gray, gray, gray, alpha};
        return 0;
    }
    if (name == "_CGContextSetRGBFillColor") {
        runtime_->graphics_contexts[Arg(0)].fill_color = {read_arg_float(1), read_arg_float(2), read_arg_float(3), read_arg_float(4)};
        return 0;
    }
    if (name == "_CGContextSetRGBStrokeColor") {
        runtime_->graphics_contexts[Arg(0)].stroke_color = {read_arg_float(1), read_arg_float(2), read_arg_float(3), read_arg_float(4)};
        return 0;
    }
    if (name == "_CGContextSetFillColorWithColor" || name == "_CGContextSetStrokeColorWithColor") {
        if (const auto rgba = color_components_for_handle(Arg(1))) {
            if (name == "_CGContextSetFillColorWithColor") {
                runtime_->graphics_contexts[Arg(0)].fill_color = *rgba;
            } else {
                runtime_->graphics_contexts[Arg(0)].stroke_color = *rgba;
            }
        }
        return 0;
    }
    if (name == "_CGContextSetFillColor" || name == "_CGContextSetStrokeColor") {
        std::array<float, 4> color{0.0f, 0.0f, 0.0f, 1.0f};
        if (Arg(1) != 0) {
            for (std::size_t i = 0; i < color.size(); ++i) {
                color[i] = BitsToFloat(memory_.Read32(Arg(1) + static_cast<u32>(i * 4)));
            }
        }
        if (name == "_CGContextSetFillColor") {
            runtime_->graphics_contexts[Arg(0)].fill_color = color;
        } else {
            runtime_->graphics_contexts[Arg(0)].stroke_color = color;
        }
        return 0;
    }
    if (name == "_CGContextSetLineWidth") {
        runtime_->graphics_contexts[Arg(0)].line_width = read_arg_float(1);
        return 0;
    }
    if (name == "_CGContextSaveGState") {
        auto& context = runtime_->graphics_contexts[Arg(0)];
        context.saved_states.push_back(RuntimeState::GraphicsStateSnapshot{
            .fill_color = context.fill_color,
            .stroke_color = context.stroke_color,
            .line_width = context.line_width,
            .transform = context.transform,
            .text_matrix = context.text_matrix,
            .text_position = context.text_position,
        });
        return 0;
    }
    if (name == "_CGContextRestoreGState") {
        auto& context = runtime_->graphics_contexts[Arg(0)];
        if (!context.saved_states.empty()) {
            const auto state = context.saved_states.back();
            context.saved_states.pop_back();
            context.fill_color = state.fill_color;
            context.stroke_color = state.stroke_color;
            context.line_width = state.line_width;
            context.transform = state.transform;
            context.text_matrix = state.text_matrix;
            context.text_position = state.text_position;
        }
        return 0;
    }
    if (name == "_CGContextScaleCTM") {
        auto& context = runtime_->graphics_contexts[Arg(0)];
        const std::array<float, 6> scale{read_arg_float(1), 0.0f, 0.0f, read_arg_float(2), 0.0f, 0.0f};
        context.transform = concat_transform(context.transform, scale);
        return 0;
    }
    if (name == "_CGContextTranslateCTM") {
        auto& context = runtime_->graphics_contexts[Arg(0)];
        const std::array<float, 6> translate{1.0f, 0.0f, 0.0f, 1.0f, read_arg_float(1), read_arg_float(2)};
        context.transform = concat_transform(context.transform, translate);
        return 0;
    }
    if (name == "_CGContextSetTextMatrix") {
        auto& context = runtime_->graphics_contexts[Arg(0)];
        std::array<float, 6> text_matrix{};
        for (std::size_t i = 0; i < text_matrix.size(); ++i) {
            text_matrix[i] = read_arg_float(1 + i);
        }
        context.text_matrix = text_matrix;
        return 0;
    }
    if (name == "_CGContextSetTextPosition") {
        auto& context = runtime_->graphics_contexts[Arg(0)];
        context.text_position = {read_arg_float(1), read_arg_float(2)};
        return 0;
    }
    if (name == "_CGContextFillRect") {
        fill_context_rect(runtime_->graphics_contexts[Arg(0)], read_rect(1), runtime_->graphics_contexts[Arg(0)].fill_color, false);
        return 0;
    }
    if (name == "_CGContextClearRect") {
        fill_context_rect(runtime_->graphics_contexts[Arg(0)], read_rect(1), {}, true);
        return 0;
    }
    if (name == "_CGContextDrawImage") {
        auto ctx_it = runtime_->graphics_contexts.find(Arg(0));
        auto image_it = runtime_->graphics_images.find(Arg(5));
        if (ctx_it == runtime_->graphics_contexts.end() || image_it == runtime_->graphics_images.end() || ctx_it->second.data == 0) {
            return 0;
        }
        if (image_it->second.skip_draw) {
            if (runtime_->debug_image_draw_logs < 48) {
                ++runtime_->debug_image_draw_logs;
                Log("[text] skip disabled text CGContextDrawImage image=" + Hex32(Arg(5)));
            }
            return 0;
        }
        const auto rect = read_rect(1);
        if (runtime_->debug_image_draw_logs < 48) {
            ++runtime_->debug_image_draw_logs;
            Log("[text] CGContextDrawImage ctx=" + Hex32(Arg(0))
                + " image=" + Hex32(Arg(5))
                + " src=" + std::to_string(image_it->second.width) + "x" + std::to_string(image_it->second.height)
                + " dst=(" + std::to_string(rect[0]) + "," + std::to_string(rect[1]) + ","
                + std::to_string(rect[2]) + "," + std::to_string(rect[3]) + ")");
        }
        auto pixels = memory_.ReadBuffer(ctx_it->second.data, static_cast<std::size_t>(ctx_it->second.bytes_per_row) * static_cast<std::size_t>(ctx_it->second.height));
        const s32 dst_x = static_cast<s32>(std::floor(rect[0]));
        const s32 dst_y = static_cast<s32>(std::floor(rect[1]));
        const s32 dst_w = std::max<s32>(1, static_cast<s32>(std::ceil(rect[2])));
        const s32 dst_h = std::max<s32>(1, static_cast<s32>(std::ceil(rect[3])));
        const std::size_t src_stride = static_cast<std::size_t>(
            image_it->second.bytes_per_row > 0
                ? image_it->second.bytes_per_row
                : image_it->second.width * 4);
        for (s32 y = 0; y < dst_h; ++y) {
            const s32 py = dst_y + y;
            if (py < 0 || py >= ctx_it->second.height) {
                continue;
            }
            const s32 src_y = std::clamp<s32>(
                static_cast<s32>((static_cast<long long>(y) * image_it->second.height) / dst_h),
                0,
                image_it->second.height - 1);
            for (s32 x = 0; x < dst_w; ++x) {
                const s32 px = dst_x + x;
                if (px < 0 || px >= ctx_it->second.width) {
                    continue;
                }
                const s32 src_x = std::clamp<s32>(
                    static_cast<s32>((static_cast<long long>(x) * image_it->second.width) / dst_w),
                    0,
                    image_it->second.width - 1);
                const std::size_t src =
                    static_cast<std::size_t>(src_y) * src_stride
                    + static_cast<std::size_t>(src_x) * 4;
                const std::size_t dst =
                    static_cast<std::size_t>(py) * static_cast<std::size_t>(ctx_it->second.bytes_per_row)
                    + static_cast<std::size_t>(px) * 4;
                if (src + 3 >= image_it->second.pixels.size() || dst + 3 >= pixels.size()) {
                    continue;
                }
                const std::array<float, 4> src_color{
                    static_cast<float>(image_it->second.pixels[src + 0]) / 255.0f,
                    static_cast<float>(image_it->second.pixels[src + 1]) / 255.0f,
                    static_cast<float>(image_it->second.pixels[src + 2]) / 255.0f,
                    static_cast<float>(image_it->second.pixels[src + 3]) / 255.0f,
                };
                blend_rgba(pixels.data() + dst, src_color, 255);
            }
        }
        memory_.WriteBuffer(ctx_it->second.data, pixels);
        return 0;
    }
    if (name == "_CGContextRelease") {
        runtime_->graphics_contexts.erase(Arg(0));
        host_objects_.erase(Arg(0));
        return 0;
    }
    if (name == "_CGContextSetShadowWithColor" || name == "_CGContextSetAllowsAntialiasing"
        || name == "_CGContextSetAllowsFontSubpixelPositioning" || name == "_CGContextSetShouldSubpixelQuantizeFonts") {
        return 0;
    }
    if (name == "_CGContextBeginPath" || name == "_CGContextAddArc" || name == "_CGContextAddArcToPoint"
        || name == "_CGContextAddLineToPoint" || name == "_CGContextAddPath" || name == "_CGContextAddRect"
        || name == "_CGContextClip" || name == "_CGContextClosePath" || name == "_CGContextDrawRadialGradient"
        || name == "_CGContextFillPath" || name == "_CGContextMoveToPoint" || name == "_CGContextSetStrokeColorSpace"
        || name == "_CGContextStrokeLineSegments" || name == "_CGContextStrokePath") {
        return 0;
    }

    if (StartsWith(name, "_glGen")) {
        const u32 count = Arg(0);
        const u32 out = Arg(1);
        std::vector<HostGLuint> host_ids(count, 0);
        if (use_host_gl && count != 0) {
            if (name == "_glGenBuffers") {
                if (auto fn = LookupHostGLProc<void (*)(HostGLsizei, HostGLuint*)>(host_gl, "glGenBuffers"); fn != nullptr) {
                    fn(static_cast<HostGLsizei>(count), host_ids.data());
                } else if (runtime_->gl_state.debug_gen_logs < 32) {
                    ++runtime_->gl_state.debug_gen_logs;
                    Log("[gl] missing host proc glGenBuffers");
                }
            } else if (name == "_glGenTextures") {
                if (auto fn = LookupHostGLProc<void (*)(HostGLsizei, HostGLuint*)>(host_gl, "glGenTextures"); fn != nullptr) {
                    fn(static_cast<HostGLsizei>(count), host_ids.data());
                } else if (runtime_->gl_state.debug_gen_logs < 32) {
                    ++runtime_->gl_state.debug_gen_logs;
                    Log("[gl] missing host proc glGenTextures");
                }
            } else if (name == "_glGenFramebuffers" || name == "_glGenFramebuffersOES") {
                if (auto fn = LookupHostGLProc<void (*)(HostGLsizei, HostGLuint*)>(host_gl, "glGenFramebuffers", "glGenFramebuffersEXT"); fn != nullptr) {
                    fn(static_cast<HostGLsizei>(count), host_ids.data());
                } else if (runtime_->gl_state.debug_gen_logs < 32) {
                    ++runtime_->gl_state.debug_gen_logs;
                    Log("[gl] missing host proc glGenFramebuffers");
                }
            } else if (name == "_glGenRenderbuffers" || name == "_glGenRenderbuffersOES") {
                if (auto fn = LookupHostGLProc<void (*)(HostGLsizei, HostGLuint*)>(host_gl, "glGenRenderbuffers", "glGenRenderbuffersEXT"); fn != nullptr) {
                    fn(static_cast<HostGLsizei>(count), host_ids.data());
                } else if (runtime_->gl_state.debug_gen_logs < 32) {
                    ++runtime_->gl_state.debug_gen_logs;
                    Log("[gl] missing host proc glGenRenderbuffers");
                }
            }
            if (!host_ids.empty() && host_ids[0] == 0 && runtime_->gl_state.debug_gen_logs < 16) {
                ++runtime_->gl_state.debug_gen_logs;
                Log("[gl] " + name
                    + " count=" + std::to_string(count)
                    + " host0=" + Hex32(host_ids.empty() ? 0 : host_ids[0])
                    + " use_host=" + std::to_string(use_host_gl ? 1 : 0));
            }
        }
        for (u32 i = 0; i < count; ++i) {
            const u32 id = next_gl_name_++;
            memory_.Write32(out + i * 4, id);
            if (name == "_glGenBuffers") {
                runtime_->gl_buffers[id] = {.host_id = host_ids[i]};
            } else if (name == "_glGenTextures") {
                runtime_->gl_textures[id] = {.host_id = host_ids[i]};
            } else if (name == "_glGenFramebuffers" || name == "_glGenFramebuffersOES") {
                runtime_->gl_framebuffers[id] = {.host_id = host_ids[i]};
            } else if (name == "_glGenRenderbuffers" || name == "_glGenRenderbuffersOES") {
                runtime_->gl_renderbuffers[id] = {.host_id = host_ids[i]};
            }
        }
        return 0;
    }
    if (name == "_glCreateProgram") {
        const u32 id = next_gl_name_++;
        HostGLuint host_id = 0;
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<HostGLuint (*)()>(host_gl, "glCreateProgram"); fn != nullptr) {
                host_id = fn();
            }
        }
        runtime_->gl_programs[id] = {.host_id = host_id};
        return id;
    }
    if (name == "_glCreateShader") {
        const u32 id = next_gl_name_++;
        HostGLuint host_id = 0;
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<HostGLuint (*)(HostGLenum)>(host_gl, "glCreateShader"); fn != nullptr) {
                host_id = fn(static_cast<HostGLenum>(Arg(0)));
            }
        }
        runtime_->gl_shaders[id].type = Arg(0);
        runtime_->gl_shaders[id].host_id = host_id;
        return id;
    }
    if (name == "_glActiveTexture") {
        runtime_->gl_state.active_texture = Arg(0);
        if (Arg(0) != 0x84C0 && runtime_->gl_state.debug_active_texture_logs < 32) {
            ++runtime_->gl_state.debug_active_texture_logs;
            Log("[gl] activeTexture unit=" + std::to_string(active_texture_unit()));
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum)>(host_gl, "glActiveTexture"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)));
            }
        }
        return 0;
    }
    if (name == "_glAttachShader") {
        runtime_->gl_programs[Arg(0)].shaders.push_back(Arg(1));
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLuint, HostGLuint)>(host_gl, "glAttachShader"); fn != nullptr) {
                fn(host_program_id(Arg(0)), host_shader_id(Arg(1)));
            }
        }
        return 0;
    }
    if (name == "_glBindAttribLocation") {
        runtime_->gl_programs[Arg(0)].attributes[Arg(1)] = ReadGuestCString(Arg(2));
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLuint, HostGLuint, const char*)>(host_gl, "glBindAttribLocation"); fn != nullptr) {
                const std::string attribute = ReadGuestCString(Arg(2));
                fn(host_program_id(Arg(0)), static_cast<HostGLuint>(Arg(1)), attribute.c_str());
            }
        }
        return 0;
    }
    if (name == "_glBindBuffer") {
        runtime_->gl_state.bound_buffers[Arg(0)] = Arg(1);
        if (Arg(1) != 0) {
            runtime_->gl_buffers[Arg(1)].target = Arg(0);
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLuint)>(host_gl, "glBindBuffer"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)), host_buffer_id(Arg(1)));
            }
        }
        return 0;
    }
    if (name == "_glBindFramebuffer" || name == "_glBindFramebufferOES") {
        runtime_->gl_state.framebuffer = Arg(1);
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLuint)>(host_gl, "glBindFramebuffer", "glBindFramebufferEXT"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)), host_framebuffer_id(Arg(1)));
            }
        }
        return 0;
    }
    if (name == "_glBindRenderbuffer" || name == "_glBindRenderbufferOES") {
        runtime_->gl_state.renderbuffer = Arg(1);
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLuint)>(host_gl, "glBindRenderbuffer", "glBindRenderbufferEXT"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)), host_renderbuffer_id(Arg(1)));
            }
        }
        return 0;
    }
    if (name == "_glBindTexture") {
        set_bound_texture(Arg(0), Arg(1));
        if (Arg(1) != 0) {
            runtime_->gl_textures[Arg(1)].target = Arg(0);
        }
        if (active_texture_unit() != 0 && runtime_->gl_state.debug_active_texture_logs < 32) {
            ++runtime_->gl_state.debug_active_texture_logs;
            Log("[gl] bindTexture unit=" + std::to_string(active_texture_unit())
                + " target=" + Hex32(Arg(0))
                + " guest_tex=" + Hex32(Arg(1))
                + " host_tex=" + Hex32(host_texture_id(Arg(1))));
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLuint)>(host_gl, "glBindTexture"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)), host_texture_id(Arg(1)));
            }
        }
        return 0;
    }
    if (name == "_glBufferData") {
        const u32 buffer = runtime_->gl_state.bound_buffers[Arg(0)];
        auto& object = runtime_->gl_buffers[buffer];
        object.target = Arg(0);
        object.data = Arg(2) == 0 ? std::vector<u8>(Arg(1), 0) : memory_.ReadBuffer(Arg(2), Arg(1));
        if (use_host_gl && buffer != 0) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLsizeiptr, const void*, HostGLenum)>(host_gl, "glBufferData"); fn != nullptr) {
                fn(
                    static_cast<HostGLenum>(Arg(0)),
                    static_cast<HostGLsizeiptr>(Arg(1)),
                    object.data.empty() ? nullptr : object.data.data(),
                    static_cast<HostGLenum>(Arg(3)));
            }
        }
        return 0;
    }
    if (name == "_glCheckFramebufferStatus" || name == "_glCheckFramebufferStatusOES") {
        const u32 guest_status = guest_framebuffer_status(runtime_->gl_state.framebuffer);
        if (guest_status != 0x8CD5) {
            u32 color_attachment = 0;
            if (const auto framebuffer_it = runtime_->gl_framebuffers.find(runtime_->gl_state.framebuffer);
                framebuffer_it != runtime_->gl_framebuffers.end()) {
                if (const auto attachment_it = framebuffer_it->second.attachments.find(0x8CE0);
                    attachment_it != framebuffer_it->second.attachments.end()) {
                    color_attachment = attachment_it->second;
                }
            }
            Log("[gl] framebuffer incomplete call=" + name
                + " fb=" + Hex32(runtime_->gl_state.framebuffer)
                + " color0=" + Hex32(color_attachment)
                + " status=" + Hex32(guest_status));
        }
        if (runtime_->gl_state.framebuffer == runtime_->drawable_framebuffer) {
            return 0x8CD5;
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<HostGLenum (*)(HostGLenum)>(host_gl, "glCheckFramebufferStatus", "glCheckFramebufferStatusEXT"); fn != nullptr) {
                const u32 host_status = static_cast<u32>(fn(static_cast<HostGLenum>(Arg(0))));
                if (host_status == 0x8CD5) {
                    return host_status;
                }
            }
        }
        return 0x8CD5;
    }
    if (name == "_glClearColor") {
        runtime_->gl_state.clear_color = {read_arg_float(0), read_arg_float(1), read_arg_float(2), read_arg_float(3)};
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLfloat, HostGLfloat, HostGLfloat, HostGLfloat)>(host_gl, "glClearColor"); fn != nullptr) {
                fn(read_arg_float(0), read_arg_float(1), read_arg_float(2), read_arg_float(3));
            }
        }
        return 0;
    }
    if (name == "_glClear") {
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum)>(host_gl, "glClear"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)));
            }
        }
        return 0;
    }
    if (name == "_glBlendEquation") {
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum)>(host_gl, "glBlendEquation"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)));
            }
        }
        return 0;
    }
    if (name == "_glBlendFunc") {
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLenum)>(host_gl, "glBlendFunc"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)), static_cast<HostGLenum>(Arg(1)));
            }
        }
        return 0;
    }
    if (name == "_glDepthMask") {
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLboolean)>(host_gl, "glDepthMask"); fn != nullptr) {
                fn(Arg(0) != 0 ? 1 : 0);
            }
        }
        return 0;
    }
    if (name == "_glDiscardFramebufferEXT" || name == "_glResolveMultisampleFramebufferAPPLE") {
        return 0;
    }
    if (name == "_glCompileShader") {
        auto& shader = runtime_->gl_shaders[Arg(0)];
        shader.compiled = true;
        shader.info_log.clear();
        if (use_host_gl && shader.host_id != 0) {
            const auto gl_shader_source = LookupHostGLProc<void (*)(HostGLuint, HostGLsizei, const char* const*, const HostGLint*)>(host_gl, "glShaderSource");
            const auto gl_compile_shader = LookupHostGLProc<void (*)(HostGLuint)>(host_gl, "glCompileShader");
            const auto gl_get_shader_iv = LookupHostGLProc<void (*)(HostGLuint, HostGLenum, HostGLint*)>(host_gl, "glGetShaderiv");
            const auto gl_get_shader_info_log = LookupHostGLProc<void (*)(HostGLuint, HostGLsizei, HostGLsizei*, char*)>(host_gl, "glGetShaderInfoLog");
            if (gl_shader_source != nullptr && gl_compile_shader != nullptr && gl_get_shader_iv != nullptr && gl_get_shader_info_log != nullptr) {
                const std::string translated = TranslateGlesShaderSource(shader.source);
                const char* source_ptr = translated.c_str();
                gl_shader_source(shader.host_id, 1, &source_ptr, nullptr);
                gl_compile_shader(shader.host_id);

                HostGLint compiled = 0;
                HostGLint log_length = 0;
                gl_get_shader_iv(shader.host_id, 0x8B81, &compiled);
                gl_get_shader_iv(shader.host_id, 0x8B84, &log_length);
                shader.compiled = compiled != 0;
                shader.info_log.clear();
                if (log_length > 1) {
                    std::string log(static_cast<std::size_t>(log_length), '\0');
                    HostGLsizei written = 0;
                    gl_get_shader_info_log(shader.host_id, log_length, &written, log.data());
                    shader.info_log.assign(log.data(), static_cast<std::size_t>(std::max<HostGLsizei>(0, written)));
                }
                const bool interesting_shader = shader.source.find("texOffset") != std::string::npos
                    || shader.source.find("constColor") != std::string::npos
                    || shader.source.find("myPMVMatrix") != std::string::npos;
                if ((!shader.compiled || interesting_shader) && runtime_->gl_state.debug_shader_logs < 16) {
                    ++runtime_->gl_state.debug_shader_logs;
                    Log("[gl] shader compile guest=" + Hex32(Arg(0))
                        + " host=" + Hex32(shader.host_id)
                        + " type=" + Hex32(shader.type)
                        + " ok=" + std::to_string(shader.compiled ? 1 : 0)
                        + " log=" + OneLineForLog(shader.info_log)
                        + " source=" + OneLineForLog(translated));
                }
            }
        }
        return 0;
    }
    if (name == "_glDeleteBuffers") {
        std::vector<HostGLuint> host_ids;
        for (u32 i = 0; i < Arg(0); ++i) {
            const u32 guest_id = memory_.Read32(Arg(1) + i * 4);
            host_ids.push_back(host_buffer_id(guest_id));
            runtime_->gl_buffers.erase(guest_id);
        }
        if (use_host_gl && !host_ids.empty()) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLsizei, const HostGLuint*)>(host_gl, "glDeleteBuffers"); fn != nullptr) {
                fn(static_cast<HostGLsizei>(host_ids.size()), host_ids.data());
            }
        }
        return 0;
    }
    if (name == "_glDeleteFramebuffers" || name == "_glDeleteFramebuffersOES") {
        std::vector<HostGLuint> host_ids;
        for (u32 i = 0; i < Arg(0); ++i) {
            const u32 guest_id = memory_.Read32(Arg(1) + i * 4);
            if (guest_id == runtime_->drawable_framebuffer) {
                runtime_->drawable_framebuffer = 0;
            }
            host_ids.push_back(host_framebuffer_id(guest_id));
            runtime_->gl_framebuffers.erase(guest_id);
        }
        if (use_host_gl && !host_ids.empty()) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLsizei, const HostGLuint*)>(host_gl, "glDeleteFramebuffers", "glDeleteFramebuffersEXT"); fn != nullptr) {
                fn(static_cast<HostGLsizei>(host_ids.size()), host_ids.data());
            }
        }
        return 0;
    }
    if (name == "_glDeleteProgram") {
        const HostGLuint host_id = host_program_id(Arg(0));
        runtime_->gl_programs.erase(Arg(0));
        if (use_host_gl && host_id != 0) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLuint)>(host_gl, "glDeleteProgram"); fn != nullptr) {
                fn(host_id);
            }
        }
        return 0;
    }
    if (name == "_glDeleteRenderbuffers" || name == "_glDeleteRenderbuffersOES") {
        std::vector<HostGLuint> host_ids;
        for (u32 i = 0; i < Arg(0); ++i) {
            const u32 guest_id = memory_.Read32(Arg(1) + i * 4);
            if (guest_id == runtime_->drawable_renderbuffer) {
                runtime_->drawable_renderbuffer = 0;
            }
            host_ids.push_back(host_renderbuffer_id(guest_id));
            runtime_->gl_renderbuffers.erase(guest_id);
        }
        if (use_host_gl && !host_ids.empty()) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLsizei, const HostGLuint*)>(host_gl, "glDeleteRenderbuffers", "glDeleteRenderbuffersEXT"); fn != nullptr) {
                fn(static_cast<HostGLsizei>(host_ids.size()), host_ids.data());
            }
        }
        return 0;
    }
    if (name == "_glDeleteShader") {
        const HostGLuint host_id = host_shader_id(Arg(0));
        runtime_->gl_shaders.erase(Arg(0));
        if (use_host_gl && host_id != 0) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLuint)>(host_gl, "glDeleteShader"); fn != nullptr) {
                fn(host_id);
            }
        }
        return 0;
    }
    if (name == "_glDeleteTextures") {
        std::vector<HostGLuint> host_ids;
        for (u32 i = 0; i < Arg(0); ++i) {
            const u32 guest_id = memory_.Read32(Arg(1) + i * 4);
            host_ids.push_back(host_texture_id(guest_id));
            runtime_->gl_textures.erase(guest_id);
        }
        if (use_host_gl && !host_ids.empty()) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLsizei, const HostGLuint*)>(host_gl, "glDeleteTextures"); fn != nullptr) {
                fn(static_cast<HostGLsizei>(host_ids.size()), host_ids.data());
            }
        }
        return 0;
    }
    if (name == "_glDisable") {
        runtime_->gl_state.enabled_caps.erase(Arg(0));
        if (runtime_->gl_state.debug_cap_logs < 32
            && (Arg(0) == 0x0BE2 || Arg(0) == 0x0C11 || Arg(0) == 0x0B71 || Arg(0) == 0x0B44)) {
            ++runtime_->gl_state.debug_cap_logs;
            Log("[gl] disable cap=" + Hex32(Arg(0))
                + " fb=" + Hex32(runtime_->gl_state.framebuffer)
                + " program=" + Hex32(runtime_->gl_state.current_program));
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum)>(host_gl, "glDisable"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)));
            }
        }
        return 0;
    }
    if (name == "_glDisableVertexAttribArray") {
        runtime_->gl_state.vertex_attribs[Arg(0)].enabled = false;
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLuint)>(host_gl, "glDisableVertexAttribArray"); fn != nullptr) {
                fn(static_cast<HostGLuint>(Arg(0)));
            }
        }
        return 0;
    }
    if (name == "_glEnable") {
        runtime_->gl_state.enabled_caps.insert(Arg(0));
        if (runtime_->gl_state.debug_cap_logs < 32
            && (Arg(0) == 0x0BE2 || Arg(0) == 0x0C11 || Arg(0) == 0x0B71 || Arg(0) == 0x0B44)) {
            ++runtime_->gl_state.debug_cap_logs;
            Log("[gl] enable cap=" + Hex32(Arg(0))
                + " fb=" + Hex32(runtime_->gl_state.framebuffer)
                + " program=" + Hex32(runtime_->gl_state.current_program));
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum)>(host_gl, "glEnable"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)));
            }
        }
        return 0;
    }
    if (name == "_glEnableVertexAttribArray") {
        runtime_->gl_state.vertex_attribs[Arg(0)].enabled = true;
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLuint)>(host_gl, "glEnableVertexAttribArray"); fn != nullptr) {
                fn(static_cast<HostGLuint>(Arg(0)));
            }
        }
        return 0;
    }
    if (name == "_glFramebufferRenderbuffer" || name == "_glFramebufferRenderbufferOES") {
        runtime_->gl_framebuffers[runtime_->gl_state.framebuffer].attachments[Arg(1)] = Arg(3);
        if (Arg(1) == 0x8CE0 && Arg(3) != 0) {
            runtime_->drawable_renderbuffer = Arg(3);
            runtime_->drawable_framebuffer = runtime_->gl_state.framebuffer;
        }
        if (use_host_gl && runtime_->gl_state.framebuffer == runtime_->drawable_framebuffer) {
            if (auto bind_framebuffer = LookupHostGLProc<void (*)(HostGLenum, HostGLuint)>(host_gl, "glBindFramebuffer", "glBindFramebufferEXT"); bind_framebuffer != nullptr) {
                bind_framebuffer(static_cast<HostGLenum>(Arg(0)), 0);
            }
        }
        if (use_host_gl && runtime_->gl_state.framebuffer != runtime_->drawable_framebuffer) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLenum, HostGLenum, HostGLuint)>(host_gl, "glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT"); fn != nullptr) {
                fn(
                    static_cast<HostGLenum>(Arg(0)),
                    static_cast<HostGLenum>(Arg(1)),
                    static_cast<HostGLenum>(Arg(2)),
                    host_renderbuffer_id(Arg(3)));
            }
        }
        return 0;
    }
    if (name == "_glFramebufferTexture2D" || name == "_glFramebufferTexture2DOES") {
        runtime_->gl_framebuffers[runtime_->gl_state.framebuffer].attachments[Arg(1)] = Arg(3);
        if (Arg(1) == 0x8CE0 && runtime_->gl_state.framebuffer != runtime_->drawable_framebuffer) {
            Log("[gl] framebufferTexture2D fb=" + Hex32(runtime_->gl_state.framebuffer)
                + " host_fb=" + Hex32(host_framebuffer_id(runtime_->gl_state.framebuffer))
                + " guest_tex=" + Hex32(Arg(3))
                + " host_tex=" + Hex32(host_texture_id(Arg(3))));
        }
        if (use_host_gl && runtime_->gl_state.framebuffer != runtime_->drawable_framebuffer) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLenum, HostGLenum, HostGLuint, HostGLint)>(host_gl, "glFramebufferTexture2D", "glFramebufferTexture2DEXT"); fn != nullptr) {
                drain_host_gl_errors("glFramebufferTexture2D");
                fn(
                    static_cast<HostGLenum>(Arg(0)),
                    static_cast<HostGLenum>(Arg(1)),
                    static_cast<HostGLenum>(Arg(2)),
                    host_texture_id(Arg(3)),
                    static_cast<HostGLint>(Arg(4)));
                if (auto gl_get_error = LookupHostGLProc<HostGLenum (*)()>(host_gl, "glGetError"); gl_get_error != nullptr) {
                    const u32 host_error = static_cast<u32>(gl_get_error());
                    if (host_error != 0) {
                        Log("[gl] framebufferTexture2D host_error=" + Hex32(host_error));
                    }
                }
            }
        }
        return 0;
    }
    if (name == "_glGetError") {
        const u32 error = runtime_->gl_state.last_error;
        runtime_->gl_state.last_error = 0;
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<HostGLenum (*)()>(host_gl, "glGetError"); fn != nullptr) {
                const u32 host_error = static_cast<u32>(fn());
                if (host_error != 0) {
                    return host_error;
                }
            }
        }
        return error;
    }
    if (name == "_glGetIntegerv") {
        const u32 pname = Arg(0);
        const u32 out = Arg(1);
        if (out == 0) {
            return 0;
        }
        if (pname == 0x0BA2) {
            for (u32 i = 0; i < 4; ++i) {
                memory_.Write32(out + i * 4, static_cast<u32>(runtime_->gl_state.viewport[i]));
            }
        } else if (pname == 0x8B8D) {
            memory_.Write32(out, runtime_->gl_state.current_program);
        } else if (pname == 0x84E0) {
            memory_.Write32(out, runtime_->gl_state.active_texture);
        } else if (pname == 0x8069) {
            memory_.Write32(out, lookup_bound_texture(0x0DE1));
        } else if (pname == 0x8894) {
            memory_.Write32(out, runtime_->gl_state.bound_buffers[0x8892]);
        } else if (pname == 0x8895) {
            memory_.Write32(out, runtime_->gl_state.bound_buffers[0x8893]);
        } else if (pname == 0x8CA6) {
            memory_.Write32(out, runtime_->gl_state.framebuffer);
        } else if (pname == 0x8CA7) {
            memory_.Write32(out, runtime_->gl_state.renderbuffer);
        } else {
            if (use_host_gl) {
                if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLint*)>(host_gl, "glGetIntegerv"); fn != nullptr) {
                    HostGLint value = 0;
                    fn(static_cast<HostGLenum>(pname), &value);
                    memory_.Write32(out, static_cast<u32>(value));
                } else {
                    memory_.Write32(out, 1);
                }
            } else {
                memory_.Write32(out, 1);
            }
        }
        return 0;
    }
    if (name == "_glGetProgramiv") {
        const auto& program = runtime_->gl_programs[Arg(0)];
        const u32 pname = Arg(1);
        const u32 out = Arg(2);
        if (out == 0) {
            return 0;
        }
        if (pname == 0x8B82) {
            memory_.Write32(out, program.linked ? 1u : 0u);
        } else if (pname == 0x8B84) {
            memory_.Write32(out, static_cast<u32>(program.info_log.size() + 1));
        } else if (pname == 0x8B85) {
            memory_.Write32(out, static_cast<u32>(program.shaders.size()));
        } else {
            memory_.Write32(out, 1);
        }
        return 0;
    }
    if (name == "_glGetShaderiv") {
        const auto& shader = runtime_->gl_shaders[Arg(0)];
        const u32 pname = Arg(1);
        const u32 out = Arg(2);
        if (out == 0) {
            return 0;
        }
        if (pname == 0x8B81) {
            memory_.Write32(out, shader.compiled ? 1u : 0u);
        } else if (pname == 0x8B84) {
            memory_.Write32(out, static_cast<u32>(shader.info_log.size() + 1));
        } else if (pname == 0x8B88) {
            memory_.Write32(out, static_cast<u32>(shader.source.size() + 1));
        } else if (pname == 0x8B4F) {
            memory_.Write32(out, shader.type);
        } else {
            memory_.Write32(out, 1);
        }
        return 0;
    }
    if (name == "_glGetProgramInfoLog") {
        const std::string& log = runtime_->gl_programs[Arg(0)].info_log;
        const std::size_t bytes = std::min<std::size_t>(Arg(1) == 0 ? 0 : Arg(1) - 1, log.size());
        if (Arg(2) != 0) {
            memory_.Write32(Arg(2), static_cast<u32>(bytes));
        }
        if (Arg(3) != 0 && Arg(1) != 0) {
            memory_.WriteBuffer(Arg(3), std::span<const u8>(reinterpret_cast<const u8*>(log.data()), bytes));
            memory_.Write8(Arg(3) + static_cast<u32>(bytes), 0);
        }
        return 0;
    }
    if (name == "_glGetShaderInfoLog") {
        const std::string& log = runtime_->gl_shaders[Arg(0)].info_log;
        const std::size_t bytes = std::min<std::size_t>(Arg(1) == 0 ? 0 : Arg(1) - 1, log.size());
        if (Arg(2) != 0) {
            memory_.Write32(Arg(2), static_cast<u32>(bytes));
        }
        if (Arg(3) != 0 && Arg(1) != 0) {
            memory_.WriteBuffer(Arg(3), std::span<const u8>(reinterpret_cast<const u8*>(log.data()), bytes));
            memory_.Write8(Arg(3) + static_cast<u32>(bytes), 0);
        }
        return 0;
    }
    if (name == "_glGetString") {
        std::string value;
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<const HostGLubyte* (*)(HostGLenum)>(host_gl, "glGetString"); fn != nullptr) {
                if (const auto* text = fn(static_cast<HostGLenum>(Arg(0))); text != nullptr) {
                    value = reinterpret_cast<const char*>(text);
                }
            }
        }
        if (value.empty()) {
            switch (Arg(0)) {
            case 0x1F00:  // GL_VENDOR
                value = "Atrasis";
                break;
            case 0x1F01:  // GL_RENDERER
                value = "Atrasis Host GL";
                break;
            case 0x1F02:  // GL_VERSION
                value = "OpenGL ES 2.0 Atrasis";
                break;
            case 0x1F03:  // GL_EXTENSIONS
                value = "GL_OES_texture_npot";
                break;
            case 0x8B8C:  // GL_SHADING_LANGUAGE_VERSION
                value = "OpenGL ES GLSL ES 1.00";
                break;
            default:
                break;
            }
        }
        return value.empty() ? 0u : AllocateCString(value, "glGetString");
    }
    if (name == "_glGetUniformLocation") {
        auto& program = runtime_->gl_programs[Arg(0)];
        const std::string uniform = ReadGuestCString(Arg(1));
        auto it = program.uniform_locations.find(uniform);
        if (it == program.uniform_locations.end()) {
            s32 location = program.next_uniform_location++;
            if (use_host_gl) {
                if (auto fn = LookupHostGLProc<HostGLint (*)(HostGLuint, const char*)>(host_gl, "glGetUniformLocation"); fn != nullptr) {
                    location = fn(program.host_id, uniform.c_str());
                }
            }
            it = program.uniform_locations.emplace(uniform, location).first;
        }
        if ((uniform == "texOffset" || uniform == "constColor" || uniform == "myPMVMatrix" || uniform == "s_texture")
            && runtime_->gl_state.debug_get_uniform_logs < 80) {
            ++runtime_->gl_state.debug_get_uniform_logs;
            Log("[gl] uniform location program=" + Hex32(Arg(0))
                + " host=" + Hex32(program.host_id)
                + " name=" + uniform
                + " location=" + std::to_string(it->second));
        }
        return static_cast<u32>(it->second);
    }
    if (name == "_glLinkProgram") {
        auto& program = runtime_->gl_programs[Arg(0)];
        program.linked = std::all_of(program.shaders.begin(), program.shaders.end(), [&](const u32 shader) {
            const auto it = runtime_->gl_shaders.find(shader);
            return it != runtime_->gl_shaders.end() && it->second.compiled;
        });
        program.info_log = program.linked ? "" : "link failed";
        if (use_host_gl && program.host_id != 0) {
            const auto gl_link_program = LookupHostGLProc<void (*)(HostGLuint)>(host_gl, "glLinkProgram");
            const auto gl_get_program_iv = LookupHostGLProc<void (*)(HostGLuint, HostGLenum, HostGLint*)>(host_gl, "glGetProgramiv");
            const auto gl_get_program_info_log = LookupHostGLProc<void (*)(HostGLuint, HostGLsizei, HostGLsizei*, char*)>(host_gl, "glGetProgramInfoLog");
            if (gl_link_program != nullptr && gl_get_program_iv != nullptr && gl_get_program_info_log != nullptr) {
                gl_link_program(program.host_id);
                HostGLint linked = 0;
                HostGLint log_length = 0;
                gl_get_program_iv(program.host_id, 0x8B82, &linked);
                gl_get_program_iv(program.host_id, 0x8B84, &log_length);
                program.linked = linked != 0;
                program.info_log.clear();
                if (log_length > 1) {
                    std::string log(static_cast<std::size_t>(log_length), '\0');
                    HostGLsizei written = 0;
                    gl_get_program_info_log(program.host_id, log_length, &written, log.data());
                    program.info_log.assign(log.data(), static_cast<std::size_t>(std::max<HostGLsizei>(0, written)));
                }
            }
        }
        bool interesting_program = !program.linked || !program.info_log.empty();
        for (const u32 shader_id : program.shaders) {
            const auto shader_it = runtime_->gl_shaders.find(shader_id);
            if (shader_it != runtime_->gl_shaders.end()
                && (shader_it->second.source.find("texOffset") != std::string::npos
                    || shader_it->second.source.find("constColor") != std::string::npos
                    || shader_it->second.source.find("myPMVMatrix") != std::string::npos)) {
                interesting_program = true;
                break;
            }
        }
        if (interesting_program && runtime_->gl_state.debug_program_logs < 16) {
            ++runtime_->gl_state.debug_program_logs;
            std::string shader_list;
            for (const u32 shader_id : program.shaders) {
                if (!shader_list.empty()) {
                    shader_list += ",";
                }
                shader_list += Hex32(shader_id);
            }
            Log("[gl] program link guest=" + Hex32(Arg(0))
                + " host=" + Hex32(program.host_id)
                + " ok=" + std::to_string(program.linked ? 1 : 0)
                + " shaders=" + shader_list
                + " log=" + OneLineForLog(program.info_log));
        }
        return 0;
    }
    if (name == "_glRenderbufferStorageMultisampleAPPLE") {
        auto& renderbuffer = runtime_->gl_renderbuffers[runtime_->gl_state.renderbuffer];
        renderbuffer.samples = static_cast<s32>(Arg(1));
        renderbuffer.format = Arg(2);
        renderbuffer.width = static_cast<s32>(Arg(3));
        renderbuffer.height = static_cast<s32>(Arg(4));
        if (use_host_gl && runtime_->gl_state.renderbuffer != runtime_->drawable_renderbuffer) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLsizei, HostGLenum, HostGLsizei, HostGLsizei)>(host_gl, "glRenderbufferStorageMultisampleAPPLE"); fn != nullptr) {
                fn(
                    static_cast<HostGLenum>(Arg(0)),
                    static_cast<HostGLsizei>(Arg(1)),
                    static_cast<HostGLenum>(Arg(2)),
                    static_cast<HostGLsizei>(Arg(3)),
                    static_cast<HostGLsizei>(Arg(4)));
            }
        }
        return 0;
    }
    if (name == "_glRenderbufferStorage" || name == "_glRenderbufferStorageOES") {
        auto& renderbuffer = runtime_->gl_renderbuffers[runtime_->gl_state.renderbuffer];
        renderbuffer.samples = 1;
        renderbuffer.format = Arg(1);
        renderbuffer.width = static_cast<s32>(Arg(2));
        renderbuffer.height = static_cast<s32>(Arg(3));
        if (use_host_gl && runtime_->gl_state.renderbuffer != runtime_->drawable_renderbuffer) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLenum, HostGLsizei, HostGLsizei)>(host_gl, "glRenderbufferStorage", "glRenderbufferStorageEXT"); fn != nullptr) {
                fn(
                    static_cast<HostGLenum>(Arg(0)),
                    static_cast<HostGLenum>(Arg(1)),
                    static_cast<HostGLsizei>(Arg(2)),
                    static_cast<HostGLsizei>(Arg(3)));
            }
        }
        return 0;
    }
    if (name == "_glPixelStorei") {
        constexpr u32 kGlPackAlignment = 0x0D05;
        constexpr u32 kGlUnpackAlignment = 0x0CF5;
        if (Arg(0) == kGlUnpackAlignment) {
            runtime_->gl_state.unpack_alignment = static_cast<s32>(Arg(1));
        } else if (Arg(0) == kGlPackAlignment) {
            runtime_->gl_state.pack_alignment = static_cast<s32>(Arg(1));
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLint)>(host_gl, "glPixelStorei"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)), static_cast<HostGLint>(Arg(1)));
            }
        }
        if (Arg(1) != 4 && runtime_->gl_state.debug_pixel_store_logs < 8) {
            ++runtime_->gl_state.debug_pixel_store_logs;
            Log("[gl] pixelStore pname=" + Hex32(Arg(0)) + " value=" + std::to_string(Arg(1)));
        }
        return 0;
    }
    if (name == "_glScissor") {
        runtime_->gl_state.scissor = {static_cast<s32>(Arg(0)), static_cast<s32>(Arg(1)), static_cast<s32>(Arg(2)), static_cast<s32>(Arg(3))};
        Log("[gl] scissor="
            + std::to_string(runtime_->gl_state.scissor[0]) + ","
            + std::to_string(runtime_->gl_state.scissor[1]) + ","
            + std::to_string(runtime_->gl_state.scissor[2]) + ","
            + std::to_string(runtime_->gl_state.scissor[3]));
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLint, HostGLint, HostGLsizei, HostGLsizei)>(host_gl, "glScissor"); fn != nullptr) {
                fn(
                    static_cast<HostGLint>(Arg(0)),
                    static_cast<HostGLint>(Arg(1)),
                    static_cast<HostGLsizei>(Arg(2)),
                    static_cast<HostGLsizei>(Arg(3)));
            }
        }
        return 0;
    }
    if (name == "_glShaderSource") {
        std::string source;
        for (u32 i = 0; i < Arg(1); ++i) {
            const u32 string_ptr = memory_.Read32(Arg(2) + i * 4);
            if (Arg(3) != 0) {
                const u32 length = memory_.Read32(Arg(3) + i * 4);
                const auto bytes = memory_.ReadBuffer(string_ptr, length);
                source.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            } else {
                source += ReadGuestCString(string_ptr);
            }
        }
        runtime_->gl_shaders[Arg(0)].source = source;
        return 0;
    }
    if (name == "_glTexImage2D") {
        constexpr u32 kGlUnsignedShort4444 = 0x8033;
#if !defined(__ANDROID__)
        constexpr u32 kGlRgb = 0x1907;
        constexpr u32 kGlRgba = 0x1908;
        constexpr u32 kGlRgb8 = 0x8051;
        constexpr u32 kGlRgba4 = 0x8056;
        constexpr u32 kGlRgba8 = 0x8058;
#endif
        const u32 texture = lookup_bound_texture(Arg(0));
        auto& object = runtime_->gl_textures[texture];
        if (Arg(1) == 0 || object.width == 0 || object.height == 0) {
            object.width = static_cast<s32>(Arg(3));
            object.height = static_cast<s32>(Arg(4));
            object.format = Arg(6);
            object.type = Arg(7);
        }
        object.mipmaps_generated = Arg(1) != 0;
        if (Arg(1) == 0) {
            object.source_data = Arg(8);
            object.data = read_texture_upload(Arg(8), Arg(3), Arg(4), Arg(6), Arg(7));
        }
        HostGLint host_internal_format = static_cast<HostGLint>(Arg(2));
#if !defined(__ANDROID__)
        if (Arg(2) == kGlRgba && Arg(7) == kGlUnsignedShort4444) {
            host_internal_format = static_cast<HostGLint>(kGlRgba4);
        } else if (Arg(2) == kGlRgba && Arg(7) == 0x1401) {
            host_internal_format = static_cast<HostGLint>(kGlRgba8);
        } else if (Arg(2) == kGlRgb && Arg(7) == 0x1401) {
            host_internal_format = static_cast<HostGLint>(kGlRgb8);
        }
#endif
        if ((Arg(3) >= 1024 && Arg(4) >= 1024) || Arg(7) == kGlUnsignedShort4444) {
            Log("[gl] texImage2D guest_tex=" + Hex32(texture)
                + " host_tex=" + Hex32(host_texture_id(texture))
                + " internal=" + Hex32(Arg(2))
                + " host_internal=" + Hex32(static_cast<u32>(host_internal_format))
                + " format=" + Hex32(Arg(6))
                + " type=" + Hex32(Arg(7))
                + " size=" + std::to_string(Arg(3)) + "x" + std::to_string(Arg(4))
                + " src=" + Hex32(Arg(8))
                + describe_texture_source(Arg(8)));
        }
        if (use_host_gl && texture != 0) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLint, HostGLint, HostGLsizei, HostGLsizei, HostGLint, HostGLenum, HostGLenum, const void*)>(host_gl, "glTexImage2D"); fn != nullptr) {
                drain_host_gl_errors("glTexImage2D");
                if (auto gl_pixel_store_i = LookupHostGLProc<void (*)(HostGLenum, HostGLint)>(host_gl, "glPixelStorei"); gl_pixel_store_i != nullptr) {
                    // Guest uploads are copied out as tightly packed byte spans, so the
                    // host unpack alignment must stay at 1 even if guest state tracked 8.
                    gl_pixel_store_i(0x0CF5, 1);
                }
                fn(
                    static_cast<HostGLenum>(Arg(0)),
                    static_cast<HostGLint>(Arg(1)),
                    host_internal_format,
                    static_cast<HostGLsizei>(Arg(3)),
                    static_cast<HostGLsizei>(Arg(4)),
                    static_cast<HostGLint>(Arg(5)),
                    static_cast<HostGLenum>(Arg(6)),
                    static_cast<HostGLenum>(Arg(7)),
                    object.data.empty() ? nullptr : object.data.data());
                if (auto gl_get_error = LookupHostGLProc<HostGLenum (*)()>(host_gl, "glGetError"); gl_get_error != nullptr) {
                    const u32 host_error = static_cast<u32>(gl_get_error());
                    if (host_error != 0) {
                        Log("[gl] texImage2D host_error=" + Hex32(host_error));
                    }
                }
                apply_host_texture_compat(Arg(0), texture);
            }
        }
        return 0;
    }
    if (name == "_glTexSubImage2D") {
        const u32 texture = lookup_bound_texture(Arg(0));
        auto& object = runtime_->gl_textures[texture];
        std::vector<u8> subimage = read_texture_upload(Arg(8), Arg(4), Arg(5), Arg(6), Arg(7));
        const std::string source_context = describe_texture_source(Arg(8));
        if (runtime_->gl_state.debug_texture_upload_logs < 64
            && (!source_context.empty()
                || Arg(4) >= 128
                || Arg(5) >= 128
                || object.width >= 1024
                || object.height >= 1024)) {
            ++runtime_->gl_state.debug_texture_upload_logs;
            Log("[gl] texSubImage2D guest_tex=" + Hex32(texture)
                + " host_tex=" + Hex32(host_texture_id(texture))
                + " level=" + std::to_string(Arg(1))
                + " xy=" + std::to_string(Arg(2)) + "," + std::to_string(Arg(3))
                + " size=" + std::to_string(Arg(4)) + "x" + std::to_string(Arg(5))
                + " format=" + Hex32(Arg(6))
                + " type=" + Hex32(Arg(7))
                + " src=" + Hex32(Arg(8))
                + source_context);
        }
        if (Arg(1) == 0 && texture != 0 && !object.data.empty()) {
            const std::size_t row_bytes = static_cast<std::size_t>(std::max<u32>(1, Arg(4))) * texture_bytes_per_pixel(Arg(6), Arg(7));
            for (u32 row = 0; row < Arg(5); ++row) {
                const std::size_t src_offset = static_cast<std::size_t>(row) * row_bytes;
                const std::size_t dst_offset = static_cast<std::size_t>(Arg(3) + row) * static_cast<std::size_t>(object.width) * texture_bytes_per_pixel(object.format, object.type)
                    + static_cast<std::size_t>(Arg(2)) * texture_bytes_per_pixel(object.format, object.type);
                if (src_offset + row_bytes > subimage.size() || dst_offset + row_bytes > object.data.size()) {
                    break;
                }
                std::copy_n(subimage.data() + src_offset, row_bytes, object.data.data() + dst_offset);
            }
        }
        if (use_host_gl && texture != 0) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLint, HostGLint, HostGLint, HostGLsizei, HostGLsizei, HostGLenum, HostGLenum, const void*)>(host_gl, "glTexSubImage2D"); fn != nullptr) {
                if (auto gl_pixel_store_i = LookupHostGLProc<void (*)(HostGLenum, HostGLint)>(host_gl, "glPixelStorei"); gl_pixel_store_i != nullptr) {
                    gl_pixel_store_i(0x0CF5, 1);
                }
                fn(
                    static_cast<HostGLenum>(Arg(0)),
                    static_cast<HostGLint>(Arg(1)),
                    static_cast<HostGLint>(Arg(2)),
                    static_cast<HostGLint>(Arg(3)),
                    static_cast<HostGLsizei>(Arg(4)),
                    static_cast<HostGLsizei>(Arg(5)),
                    static_cast<HostGLenum>(Arg(6)),
                    static_cast<HostGLenum>(Arg(7)),
                    subimage.empty() ? nullptr : subimage.data());
                apply_host_texture_compat(Arg(0), texture);
            }
        }
        return 0;
    }
    if (name == "_glGenerateMipmap") {
        const u32 texture = lookup_bound_texture(Arg(0));
        if (texture != 0) {
            runtime_->gl_textures[texture].mipmaps_generated = true;
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum)>(host_gl, "glGenerateMipmap"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)));
            }
            apply_host_texture_compat(Arg(0), texture);
        }
        if (runtime_->gl_state.debug_texture_compat_logs < 24) {
            ++runtime_->gl_state.debug_texture_compat_logs;
            Log("[gl] generateMipmap target=" + Hex32(Arg(0)) + " guest_tex=" + Hex32(texture));
        }
        return 0;
    }
    if (name == "_glTexParameterf") {
        const u32 texture = lookup_bound_texture(Arg(0));
        runtime_->gl_textures[texture].fparams[Arg(1)] = read_arg_float(2);
        runtime_->gl_textures[texture].iparams[Arg(1)] = static_cast<s32>(std::lround(read_arg_float(2)));
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLenum, HostGLfloat)>(host_gl, "glTexParameterf"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)), static_cast<HostGLenum>(Arg(1)), read_arg_float(2));
            }
            apply_host_texture_compat(Arg(0), texture);
        }
        return 0;
    }
    if (name == "_glTexParameteri") {
        const u32 texture = lookup_bound_texture(Arg(0));
        runtime_->gl_textures[texture].iparams[Arg(1)] = static_cast<s32>(Arg(2));
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLenum, HostGLint)>(host_gl, "glTexParameteri"); fn != nullptr) {
                fn(static_cast<HostGLenum>(Arg(0)), static_cast<HostGLenum>(Arg(1)), static_cast<HostGLint>(Arg(2)));
            }
            apply_host_texture_compat(Arg(0), texture);
        }
        return 0;
    }
    if (name == "_glUniform1i") {
        if (Arg(1) != 0 && runtime_->gl_state.debug_uniform1i_logs < 32) {
            ++runtime_->gl_state.debug_uniform1i_logs;
            Log("[gl] uniform1i program=" + Hex32(runtime_->gl_state.current_program)
                + " location=" + std::to_string(static_cast<s32>(Arg(0)))
                + " value=" + std::to_string(static_cast<s32>(Arg(1))));
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLint, HostGLint)>(host_gl, "glUniform1i"); fn != nullptr) {
                fn(static_cast<HostGLint>(Arg(0)), static_cast<HostGLint>(Arg(1)));
            }
        }
        return 0;
    }
    if (name == "_glUniform2f") {
        if (runtime_->gl_state.debug_uniform2f_logs < 8) {
            const s32 location = static_cast<s32>(Arg(0));
            std::string uniform_name;
            const auto program_it = runtime_->gl_programs.find(runtime_->gl_state.current_program);
            if (program_it != runtime_->gl_programs.end()) {
                for (const auto& [name, loc] : program_it->second.uniform_locations) {
                    if (loc == location) {
                        uniform_name = name;
                        break;
                    }
                }
            }
            if (uniform_name == "texOffset") {
                ++runtime_->gl_state.debug_uniform2f_logs;
                Log("[gl] uniform2f program=" + Hex32(runtime_->gl_state.current_program)
                    + " name=" + uniform_name
                    + " location=" + std::to_string(location)
                    + " value=" + std::to_string(read_arg_float(1)) + ","
                    + std::to_string(read_arg_float(2)));
            }
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLint, HostGLfloat, HostGLfloat)>(host_gl, "glUniform2f"); fn != nullptr) {
                fn(static_cast<HostGLint>(Arg(0)), read_arg_float(1), read_arg_float(2));
            }
        }
        return 0;
    }
    if (name == "_glUniform4f") {
        if (runtime_->gl_state.debug_uniform4f_logs < 8) {
            const s32 location = static_cast<s32>(Arg(0));
            std::string uniform_name;
            const auto program_it = runtime_->gl_programs.find(runtime_->gl_state.current_program);
            if (program_it != runtime_->gl_programs.end()) {
                for (const auto& [name, loc] : program_it->second.uniform_locations) {
                    if (loc == location) {
                        uniform_name = name;
                        break;
                    }
                }
            }
            if (uniform_name == "constColor") {
                ++runtime_->gl_state.debug_uniform4f_logs;
                Log("[gl] uniform4f program=" + Hex32(runtime_->gl_state.current_program)
                    + " name=" + uniform_name
                    + " location=" + std::to_string(location)
                    + " value=" + std::to_string(read_arg_float(1)) + ","
                    + std::to_string(read_arg_float(2)) + ","
                    + std::to_string(read_arg_float(3)) + ","
                    + std::to_string(read_arg_float(4)));
            }
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLint, HostGLfloat, HostGLfloat, HostGLfloat, HostGLfloat)>(host_gl, "glUniform4f"); fn != nullptr) {
                fn(static_cast<HostGLint>(Arg(0)), read_arg_float(1), read_arg_float(2), read_arg_float(3), read_arg_float(4));
            }
        }
        return 0;
    }
    if (name == "_glUniformMatrix4fv") {
        const u32 address = Arg(3);
        const auto bytes = memory_.ReadBuffer(address, static_cast<std::size_t>(Arg(1)) * 16 * sizeof(float));
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLint, HostGLsizei, HostGLboolean, const HostGLfloat*)>(host_gl, "glUniformMatrix4fv"); fn != nullptr) {
                fn(
                    static_cast<HostGLint>(Arg(0)),
                    static_cast<HostGLsizei>(Arg(1)),
                    Arg(2) != 0 ? 1 : 0,
                    reinterpret_cast<const HostGLfloat*>(bytes.data()));
            }
        }
        return 0;
    }
    if (name == "_glUseProgram") {
        runtime_->gl_state.current_program = Arg(0);
        const auto signature = Hex32(Arg(0)) + "|" + Hex32(runtime_->gl_state.framebuffer);
        if (runtime_->gl_state.debug_program_signatures.insert(signature).second
            && runtime_->gl_state.debug_program_signatures.size() <= 48) {
            Log("[gl] useProgram program=" + Hex32(Arg(0))
                + " host=" + Hex32(host_program_id(Arg(0)))
                + " fb=" + Hex32(runtime_->gl_state.framebuffer));
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLuint)>(host_gl, "glUseProgram"); fn != nullptr) {
                fn(host_program_id(Arg(0)));
            }
        }
        return 0;
    }
    if (name == "_glVertexAttribPointer") {
        auto& attrib = runtime_->gl_state.vertex_attribs[Arg(0)];
        attrib.size = static_cast<s32>(Arg(1));
        attrib.type = Arg(2);
        attrib.normalized = Arg(3) != 0;
        attrib.stride = static_cast<s32>(Arg(4));
        attrib.pointer = Arg(5);
        attrib.buffer = runtime_->gl_state.bound_buffers[0x8892];
        if (use_host_gl && attrib.buffer != 0) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLuint, HostGLint, HostGLenum, HostGLboolean, HostGLsizei, const void*)>(host_gl, "glVertexAttribPointer"); fn != nullptr) {
                fn(
                    static_cast<HostGLuint>(Arg(0)),
                    static_cast<HostGLint>(Arg(1)),
                    static_cast<HostGLenum>(Arg(2)),
                    Arg(3) != 0 ? 1 : 0,
                    static_cast<HostGLsizei>(Arg(4)),
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(Arg(5))));
            }
        }
        return 0;
    }
    if (name == "_glDrawArrays") {
        if (runtime_->gl_state.debug_draw_call_logs < 32) {
            ++runtime_->gl_state.debug_draw_call_logs;
            Log("[gl] drawArrays mode=" + Hex32(Arg(0))
                + " first=" + std::to_string(Arg(1))
                + " count=" + std::to_string(Arg(2))
                + " fb=" + Hex32(runtime_->gl_state.framebuffer));
        }
        if (use_host_gl) {
            configure_client_arrays(Arg(1), Arg(2));
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLint, HostGLsizei)>(host_gl, "glDrawArrays"); fn != nullptr) {
                drain_host_gl_errors("glDrawArrays");
                fn(static_cast<HostGLenum>(Arg(0)), static_cast<HostGLint>(Arg(1)), static_cast<HostGLsizei>(Arg(2)));
                log_host_gl_errors("glDrawArrays", runtime_->gl_state.debug_draw_error_logs);
            }
        }
        return 0;
    }
    if (name == "_glDrawElements") {
        if (runtime_->gl_state.debug_draw_call_logs < 32) {
            ++runtime_->gl_state.debug_draw_call_logs;
            Log("[gl] drawElements mode=" + Hex32(Arg(0))
                + " count=" + std::to_string(Arg(1))
                + " type=" + Hex32(Arg(2))
                + " fb=" + Hex32(runtime_->gl_state.framebuffer));
        }
        if (use_host_gl) {
            const u32 count = Arg(1);
            const u32 index_type = Arg(2);
            const u32 indices = Arg(3);
            const u32 element_buffer = runtime_->gl_state.bound_buffers[0x8893];
            const u32 bound_texture_2d = lookup_bound_texture(0x0DE1);
            const std::size_t index_size = GlTypeSize(index_type);
            std::vector<u8> index_scratch;
            const void* index_pointer = reinterpret_cast<const void*>(static_cast<uintptr_t>(indices));
            u32 vertex_count = count;
            if (element_buffer == 0 && index_size != 0 && count != 0) {
                index_scratch = memory_.ReadBuffer(indices, static_cast<std::size_t>(count) * index_size);
                index_pointer = index_scratch.data();
                u32 max_index = 0;
                for (u32 i = 0; i < count; ++i) {
                    max_index = std::max(max_index, read_index(index_scratch, index_type, i));
                }
                vertex_count = max_index + 1;
            } else if (element_buffer != 0 && index_size != 0 && count != 0) {
                const auto buffer_it = runtime_->gl_buffers.find(element_buffer);
                if (buffer_it != runtime_->gl_buffers.end()) {
                    const auto& element_data = buffer_it->second.data;
                    const std::size_t byte_offset = indices;
                    const std::size_t byte_count = static_cast<std::size_t>(count) * index_size;
                    if (byte_offset < element_data.size()) {
                        const std::size_t available = std::min(byte_count, element_data.size() - byte_offset);
                        std::vector<u8> index_probe(
                            element_data.begin() + static_cast<std::ptrdiff_t>(byte_offset),
                            element_data.begin() + static_cast<std::ptrdiff_t>(byte_offset + available));
                        u32 max_index = 0;
                        for (u32 i = 0; i < count; ++i) {
                            max_index = std::max(max_index, read_index(index_probe, index_type, i));
                        }
                        vertex_count = max_index + 1;
                    }
                }
            }
            bool bound_render_target_texture = false;
            for (const auto& [framebuffer_id, framebuffer] : runtime_->gl_framebuffers) {
                if (framebuffer_id == runtime_->gl_state.framebuffer) {
                    continue;
                }
                for (const auto& [attachment, texture_id] : framebuffer.attachments) {
                    if (attachment == 0x8CE0 && texture_id == bound_texture_2d) {
                        bound_render_target_texture = true;
                        break;
                    }
                }
                if (bound_render_target_texture) {
                    break;
                }
            }
            if (count >= 96 && runtime_->gl_state.debug_large_draw_logs < 12) {
                ++runtime_->gl_state.debug_large_draw_logs;
                Log("[gl] drawElements detail count=" + std::to_string(count)
                    + " ebo=" + Hex32(element_buffer)
                    + " vertex_count=" + std::to_string(vertex_count)
                    + " active_unit=" + std::to_string(active_texture_unit())
                    + " tex2d=" + Hex32(bound_texture_2d)
                    + " program=" + Hex32(runtime_->gl_state.current_program));
                for (const auto& [index, attrib] : runtime_->gl_state.vertex_attribs) {
                    if (!attrib.enabled) {
                        continue;
                    }
                    Log("[gl]  attrib index=" + std::to_string(index)
                        + " buffer=" + Hex32(attrib.buffer)
                        + " ptr=" + Hex32(attrib.pointer)
                        + " stride=" + std::to_string(attrib.stride)
                        + " size=" + std::to_string(attrib.size)
                        + " type=" + Hex32(attrib.type)
                        + " normalized=" + std::to_string(attrib.normalized ? 1 : 0));
                }
            }
            if (count <= 6 && (runtime_->gl_state.framebuffer != runtime_->drawable_framebuffer || bound_render_target_texture)) {
                const auto texture_it = runtime_->gl_textures.find(bound_texture_2d);
                const auto signature = Hex32(runtime_->gl_state.framebuffer)
                    + "|" + Hex32(bound_texture_2d)
                    + "|" + Hex32(runtime_->gl_state.current_program)
                    + "|" + std::to_string(runtime_->gl_state.enabled_caps.contains(0x0BE2) ? 1 : 0)
                    + "|" + std::to_string(runtime_->gl_state.enabled_caps.contains(0x0C11) ? 1 : 0)
                    + "|" + std::to_string(bound_render_target_texture ? 1 : 0);
                if (runtime_->gl_state.debug_quad_signatures.insert(signature).second
                    && runtime_->gl_state.debug_quad_draw_logs < 24) {
                    ++runtime_->gl_state.debug_quad_draw_logs;
                    Log("[gl] quad pass count=" + std::to_string(count)
                        + " fb=" + Hex32(runtime_->gl_state.framebuffer)
                        + " drawable_fb=" + Hex32(runtime_->drawable_framebuffer)
                        + " tex2d=" + Hex32(bound_texture_2d)
                        + " host_tex=" + Hex32(host_texture_id(bound_texture_2d))
                        + " tex_size=" + std::to_string(texture_it == runtime_->gl_textures.end() ? 0 : texture_it->second.width)
                        + "x" + std::to_string(texture_it == runtime_->gl_textures.end() ? 0 : texture_it->second.height)
                        + " program=" + Hex32(runtime_->gl_state.current_program)
                        + " blend=" + std::to_string(runtime_->gl_state.enabled_caps.contains(0x0BE2) ? 1 : 0)
                        + " scissor_test=" + std::to_string(runtime_->gl_state.enabled_caps.contains(0x0C11) ? 1 : 0)
                        + " depth_test=" + std::to_string(runtime_->gl_state.enabled_caps.contains(0x0B71) ? 1 : 0)
                        + " rt_tex=" + std::to_string(bound_render_target_texture ? 1 : 0)
                        + " viewport=" + std::to_string(runtime_->gl_state.viewport[0]) + ","
                        + std::to_string(runtime_->gl_state.viewport[1]) + ","
                        + std::to_string(runtime_->gl_state.viewport[2]) + ","
                        + std::to_string(runtime_->gl_state.viewport[3])
                        + " scissor=" + std::to_string(runtime_->gl_state.scissor[0]) + ","
                        + std::to_string(runtime_->gl_state.scissor[1]) + ","
                        + std::to_string(runtime_->gl_state.scissor[2]) + ","
                        + std::to_string(runtime_->gl_state.scissor[3]));
                    for (const auto& [index, attrib] : runtime_->gl_state.vertex_attribs) {
                        if (!attrib.enabled) {
                            continue;
                        }
                        Log("[gl]  quad attrib index=" + std::to_string(index)
                            + " buffer=" + Hex32(attrib.buffer)
                            + " ptr=" + Hex32(attrib.pointer)
                            + " stride=" + std::to_string(attrib.stride)
                            + " size=" + std::to_string(attrib.size)
                            + " type=" + Hex32(attrib.type)
                            + " normalized=" + std::to_string(attrib.normalized ? 1 : 0));
                    }
                }
            }
            configure_client_arrays(0, vertex_count);
            if (auto bind_buffer = LookupHostGLProc<void (*)(HostGLenum, HostGLuint)>(host_gl, "glBindBuffer"); bind_buffer != nullptr) {
                bind_buffer(0x8893, host_buffer_id(element_buffer));
            }
            if (auto fn = LookupHostGLProc<void (*)(HostGLenum, HostGLsizei, HostGLenum, const void*)>(host_gl, "glDrawElements"); fn != nullptr) {
                drain_host_gl_errors("glDrawElements");
                fn(
                    static_cast<HostGLenum>(Arg(0)),
                    static_cast<HostGLsizei>(count),
                    static_cast<HostGLenum>(index_type),
                    index_pointer);
                log_host_gl_errors("glDrawElements", runtime_->gl_state.debug_draw_error_logs);
                const auto probe_signature = Hex32(runtime_->gl_state.framebuffer)
                    + "|" + Hex32(bound_texture_2d)
                    + "|" + Hex32(runtime_->gl_state.current_program)
                    + "|" + std::to_string(runtime_->gl_state.enabled_caps.contains(0x0BE2) ? 1 : 0)
                    + "|" + std::to_string(runtime_->gl_state.enabled_caps.contains(0x0C11) ? 1 : 0);
                if (count <= 6
                    && runtime_->gl_state.framebuffer != runtime_->drawable_framebuffer
                    && runtime_->gl_state.debug_fbo_probe_logs < 16
                    && runtime_->gl_state.debug_fbo_probe_signatures.insert(probe_signature).second) {
                    auto read_pixels = LookupHostGLProc<void (*)(HostGLint, HostGLint, HostGLsizei, HostGLsizei, HostGLenum, HostGLenum, void*)>(host_gl, "glReadPixels");
                    auto pixel_store_i = LookupHostGLProc<void (*)(HostGLenum, HostGLint)>(host_gl, "glPixelStorei");
                    auto get_error = LookupHostGLProc<HostGLenum (*)()>(host_gl, "glGetError");
                    if (read_pixels != nullptr) {
                        if (pixel_store_i != nullptr) {
                            pixel_store_i(0x0D05, 1);
                        }
                        const s32 width = std::max<s32>(1, runtime_->gl_state.viewport[2]);
                        const s32 height = std::max<s32>(1, runtime_->gl_state.viewport[3]);
                        const std::array<std::array<s32, 2>, 5> points{{
                            {{0, 0}},
                            {{width / 2, height / 2}},
                            {{width - 1, height - 1}},
                            {{width / 4, height / 4}},
                            {{(width * 3) / 4, (height * 3) / 4}},
                        }};
                        u32 nonzero = 0;
                        u32 alpha = 0;
                        u32 checksum = 2166136261u;
                        std::array<HostGLubyte, 4> first_pixel{0, 0, 0, 0};
                        for (const auto& point : points) {
                            std::array<HostGLubyte, 4> pixel{0, 0, 0, 0};
                            read_pixels(
                                static_cast<HostGLint>(std::clamp<s32>(point[0], 0, width - 1)),
                                static_cast<HostGLint>(std::clamp<s32>(point[1], 0, height - 1)),
                                1,
                                1,
                                0x1908,
                                0x1401,
                                pixel.data());
                            if (point == points.front()) {
                                first_pixel = pixel;
                            }
                            if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) {
                                ++nonzero;
                            }
                            if (pixel[3] != 0) {
                                ++alpha;
                            }
                            for (const HostGLubyte byte : pixel) {
                                checksum ^= byte;
                                checksum *= 16777619u;
                            }
                        }
                        u32 read_error = 0;
                        if (get_error != nullptr) {
                            read_error = static_cast<u32>(get_error());
                        }
                        ++runtime_->gl_state.debug_fbo_probe_logs;
                        Log("[gl] fbo probe fb=" + Hex32(runtime_->gl_state.framebuffer)
                            + " tex2d=" + Hex32(bound_texture_2d)
                            + " program=" + Hex32(runtime_->gl_state.current_program)
                            + " samples_nonzero=" + std::to_string(nonzero)
                            + " alpha_nonzero=" + std::to_string(alpha)
                            + " first_rgba="
                            + std::to_string(first_pixel[0]) + ","
                            + std::to_string(first_pixel[1]) + ","
                            + std::to_string(first_pixel[2]) + ","
                            + std::to_string(first_pixel[3])
                            + " checksum=" + Hex32(checksum)
                            + " error=" + Hex32(read_error));
                    }
                }
            }
        }
        return 0;
    }
    if (name == "_glViewport") {
        runtime_->gl_state.viewport = {static_cast<s32>(Arg(0)), static_cast<s32>(Arg(1)), static_cast<s32>(Arg(2)), static_cast<s32>(Arg(3))};
        if (runtime_->gl_state.debug_viewport_logs < 16) {
            ++runtime_->gl_state.debug_viewport_logs;
            Log("[gl] viewport="
                + std::to_string(runtime_->gl_state.viewport[0]) + ","
                + std::to_string(runtime_->gl_state.viewport[1]) + ","
                + std::to_string(runtime_->gl_state.viewport[2]) + ","
                + std::to_string(runtime_->gl_state.viewport[3]));
        }
        if (use_host_gl) {
            if (auto fn = LookupHostGLProc<void (*)(HostGLint, HostGLint, HostGLsizei, HostGLsizei)>(host_gl, "glViewport"); fn != nullptr) {
                fn(
                    static_cast<HostGLint>(Arg(0)),
                    static_cast<HostGLint>(Arg(1)),
                    static_cast<HostGLsizei>(Arg(2)),
                    static_cast<HostGLsizei>(Arg(3)));
            }
            const auto drawable_renderbuffer_it = runtime_->gl_renderbuffers.find(runtime_->drawable_renderbuffer);
            const s32 drawable_width = drawable_renderbuffer_it == runtime_->gl_renderbuffers.end() ? 0 : drawable_renderbuffer_it->second.width;
            const s32 drawable_height = drawable_renderbuffer_it == runtime_->gl_renderbuffers.end() ? 0 : drawable_renderbuffer_it->second.height;
            const bool stale_drawable_scissor =
                runtime_->gl_state.framebuffer != 0
                && runtime_->gl_state.framebuffer != runtime_->drawable_framebuffer
                && runtime_->gl_state.scissor[0] == 0
                && runtime_->gl_state.scissor[1] == 0
                && runtime_->gl_state.scissor[2] == drawable_width
                && runtime_->gl_state.scissor[3] == drawable_height
                && runtime_->gl_state.viewport[2] > runtime_->gl_state.scissor[2]
                && runtime_->gl_state.viewport[3] > runtime_->gl_state.scissor[3];
            if (stale_drawable_scissor) {
                if (auto scissor_fn = LookupHostGLProc<void (*)(HostGLint, HostGLint, HostGLsizei, HostGLsizei)>(host_gl, "glScissor"); scissor_fn != nullptr) {
                    scissor_fn(
                        static_cast<HostGLint>(runtime_->gl_state.viewport[0]),
                        static_cast<HostGLint>(runtime_->gl_state.viewport[1]),
                        static_cast<HostGLsizei>(runtime_->gl_state.viewport[2]),
                        static_cast<HostGLsizei>(runtime_->gl_state.viewport[3]));
                    Log("[gl] host scissor workaround fb=" + Hex32(runtime_->gl_state.framebuffer)
                        + " rect=" + std::to_string(runtime_->gl_state.viewport[0]) + ","
                        + std::to_string(runtime_->gl_state.viewport[1]) + ","
                        + std::to_string(runtime_->gl_state.viewport[2]) + ","
                        + std::to_string(runtime_->gl_state.viewport[3]));
                }
            }
        }
        return 0;
    }

    auto store_al_buffer = [&](const u32 buffer_id, const u32 format, const u32 data, const u32 size, const u32 frequency) {
        auto& buffer = runtime_->al_buffers[buffer_id];
        buffer.format = format;
        buffer.frequency = frequency;
        buffer.path = runtime_->debug_last_audio_path;
        buffer.data = data == 0 || size == 0 ? std::vector<u8>{} : memory_.ReadBuffer(data, size);
        if (!runtime_->debug_last_audio_path.empty() && runtime_->debug_last_audio_path.find("Button_click_13.caf") != std::string::npos) {
            Log("alBufferData path=" + runtime_->debug_last_audio_path
                + " buffer=" + Hex32(buffer_id)
                + " bytes=" + std::to_string(buffer.data.size())
                + " freq=" + std::to_string(frequency)
                + " format=" + Hex32(format));
        }
        return 0u;
    };
    auto refresh_al_source_state = [&](RuntimeState::AlSource& source) {
        if (source.playing && source.has_stop_time && std::chrono::steady_clock::now() >= source.stop_time) {
            source.playing = false;
            source.has_stop_time = false;
            source.ints[0x1010] = 0x1014;
        }
    };

    if (name == "_alBufferData" || name == "_alBufferDataStatic") {
        return store_al_buffer(Arg(0), Arg(1), Arg(2), Arg(3), Arg(4));
    }
    if (name == "_alGenBuffers" || name == "_alGenSources") {
        for (u32 i = 0; i < Arg(0); ++i) {
            const u32 id = next_audio_name_++;
            memory_.Write32(Arg(1) + i * 4, id);
            if (name == "_alGenBuffers") {
                runtime_->al_buffers[id] = {};
            } else {
                runtime_->al_sources[id] = {};
            }
        }
        return 0;
    }
    if (name == "_alDeleteBuffers" || name == "_alDeleteSources") {
        for (u32 i = 0; i < Arg(0); ++i) {
            const u32 id = memory_.Read32(Arg(1) + i * 4);
            if (name == "_alDeleteBuffers") {
                runtime_->al_buffers.erase(id);
            } else {
                runtime_->al_sources.erase(id);
            }
        }
        return 0;
    }
    if (name == "_alGetError") {
        return 0;
    }
    if (name == "_alGetString") {
        return AllocateCString("OpenAL HLE", "alGetString");
    }
    if (name == "_alIsExtensionPresent") {
        return 1;
    }
    if (name == "_alGetSourcei") {
        if (Arg(2) != 0) {
            auto& source = runtime_->al_sources[Arg(0)];
            refresh_al_source_state(source);
            if (Arg(1) == 0x1010 && !source.ints.contains(0x1010)) {
                source.ints[0x1010] = source.playing ? 0x1012 : 0x1014;
            }
            memory_.Write32(Arg(2), source.ints[Arg(1)]);
        }
        return 0;
    }
    if (name == "_alListener3f" || name == "_alListenerf" || name == "_alListenerfv"
        || name == "_alDistanceModel" || name == "_alDopplerFactor" || name == "_alDopplerVelocity") {
        return 0;
    }
    if (name == "_alSourcePlay") {
        auto& source = runtime_->al_sources[Arg(0)];
        source.playing = true;
        source.has_stop_time = false;
        source.ints[0x1010] = 0x1012;
        const u32 buffer_id = source.ints[0x1009];
        if (const auto buffer_it = runtime_->al_buffers.find(buffer_id); buffer_it != runtime_->al_buffers.end()) {
            const RuntimeState::AlBuffer& buffer = buffer_it->second;
            if (!buffer.data.empty() && buffer.format != 0 && buffer.frequency != 0) {
                if (runtime_->debug_sfx_logs < 64) {
                    ++runtime_->debug_sfx_logs;
                    Log("sfx play source=" + Hex32(Arg(0))
                        + " buffer=" + Hex32(buffer_id)
                        + " bytes=" + std::to_string(buffer.data.size())
                        + " freq=" + std::to_string(buffer.frequency));
                }
                PlayHostAudio(buffer.data, buffer.format, buffer.frequency);
                if (const auto format = DecodeAlFormat(buffer.format)) {
                    const u64 duration_ms = HostAudioDurationMs(buffer.data.size(), *format, buffer.frequency);
                    source.stop_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max<u64>(duration_ms, 1));
                    source.has_stop_time = true;
                }
            }
        }
        return 0;
    }
    if (name == "_alSourceStop" || name == "_alSourcePause" || name == "_alSourceRewind") {
        auto& source = runtime_->al_sources[Arg(0)];
        source.playing = false;
        source.has_stop_time = false;
        source.ints[0x1010] = name == "_alSourcePause" ? 0x1013 : 0x1014;
        return 0;
    }
    if (name == "_alSourcef" || name == "_alSource3f" || name == "_alSourcefv") {
        runtime_->al_sources[Arg(0)].floats[Arg(1)] = read_arg_float(2);
        return 0;
    }
    if (name == "_alSourcei") {
        runtime_->al_sources[Arg(0)].ints[Arg(1)] = static_cast<s32>(Arg(2));
        return 0;
    }
    if (name == "_alSourceQueueBuffers") {
        if (Arg(1) != 0 && Arg(2) != 0) {
            runtime_->al_sources[Arg(0)].ints[0x1009] = static_cast<s32>(memory_.Read32(Arg(2)));
        }
        return 0;
    }
    if (name == "_alSourceUnqueueBuffers") {
        for (u32 i = 0; i < Arg(1); ++i) {
            memory_.Write32(Arg(2) + i * 4, static_cast<u32>(runtime_->al_sources[Arg(0)].ints[0x1009]));
        }
        return 0;
    }
    if (name == "_alcOpenDevice") {
        const u32 handle = make_runtime_object("ALDevice", "ALDevice");
        runtime_->al_devices[handle].name = Arg(0) == 0 ? "default" : ReadGuestCString(Arg(0));
        Log("alcOpenDevice -> " + Hex32(handle));
        return handle;
    }
    if (name == "_alcCreateContext") {
        const u32 handle = make_runtime_object("ALContext", "ALContext");
        runtime_->al_contexts[handle].device = Arg(0);
        Log("alcCreateContext device=" + Hex32(Arg(0)) + " -> " + Hex32(handle));
        return handle;
    }
    if (name == "_alcDestroyContext") {
        runtime_->al_contexts.erase(Arg(0));
        host_objects_.erase(Arg(0));
        return 0;
    }
    if (name == "_alcCloseDevice") {
        runtime_->al_devices.erase(Arg(0));
        host_objects_.erase(Arg(0));
        return 1;
    }
    if (name == "_alcGetProcAddress") {
        const std::string proc_name = Arg(1) == 0 ? "" : ReadGuestCString(Arg(1));
        if (proc_name == "alBufferDataStatic" || proc_name == "alBufferData") {
            Log("openal proc " + proc_name + " provided");
            return RegisterFunctionShim("__host_" + proc_name, [this, store_al_buffer] {
                store_al_buffer(Arg(0), Arg(1), Arg(2), Arg(3), Arg(4));
            });
        }
        return 0;
    }
    if (name == "_alcGetError") {
        return 0;
    }
    if (name == "_alcGetCurrentContext") {
        return runtime_->al_current_context;
    }
    if (name == "_alcGetContextsDevice") {
        const auto it = runtime_->al_contexts.find(Arg(0));
        return it == runtime_->al_contexts.end() ? 0 : it->second.device;
    }
    if (name == "_alcGetIntegerv") {
        if (Arg(2) == 0 || Arg(3) == 0) {
            return 0;
        }
        const u32 param = Arg(1);
        const u32 value = param == 0x1000 ? 1 : param == 0x1001 ? 1 : 0;
        memory_.Write32(Arg(3), value);
        return 0;
    }
    if (name == "_alcGetString") {
        return AllocateCString("OpenAL HLE ALC_EXT_EFX ALC_EXT_CAPTURE", "alcGetString");
    }
    if (name == "_alcIsExtensionPresent") {
        return 1;
    }
    if (name == "_alcMakeContextCurrent") {
        runtime_->al_current_context = Arg(0);
        if (runtime_->al_contexts.count(Arg(0)) != 0) {
            runtime_->al_contexts[Arg(0)].current = true;
        }
        Log("alcMakeContextCurrent context=" + Hex32(Arg(0)) + " -> 1");
        return 1;
    }
    if (name == "_alcProcessContext" || name == "_alcSuspendContext") {
        return 0;
    }

    if (name == "_AudioSessionInitialize") {
        runtime_->audio_session.initialized = true;
        return 0;
    }
    if (name == "_AudioSessionSetActive") {
        runtime_->audio_session.active = Arg(0) != 0;
        return 0;
    }
    if (name == "_AudioSessionSetProperty") {
        runtime_->audio_session.properties[Arg(0)] = memory_.ReadBuffer(Arg(2), Arg(1));
        return 0;
    }
    if (name == "_AudioSessionGetProperty") {
        const auto it = runtime_->audio_session.properties.find(Arg(0));
        if (Arg(1) != 0) {
            memory_.Write32(Arg(1), it == runtime_->audio_session.properties.end() ? 0 : static_cast<u32>(it->second.size()));
        }
        if (it != runtime_->audio_session.properties.end() && Arg(2) != 0) {
            memory_.WriteBuffer(Arg(2), it->second);
        }
        return 0;
    }
    if (name == "_AudioQueueNewOutput") {
        // Keep the guest out of the AudioQueue streaming callback path for now. We play
        // selected music files directly on the Android host side when AudioFileOpenURL sees them.
        if (Arg(6) != 0) {
            memory_.Write32(Arg(6), 0);
        }
        Log("audio queue disabled callback=" + Hex32(Arg(1)));
        return 1;
    }
    if (name == "_AudioQueueAllocateBuffer") {
        const u32 handle = make_runtime_object("AudioQueueBuffer", "AudioQueueBuffer");
        auto& buffer = runtime_->audio_queue_buffers[handle];
        buffer.capacity = Arg(1);
        buffer.data = AllocateData(std::max<u32>(Arg(1), 1), 4, "AudioQueueBuffer.data");
        buffer.data_size = 0;
        memory_.Write32(handle + 0, buffer.capacity);
        memory_.Write32(handle + 4, buffer.data);
        memory_.Write32(handle + 8, buffer.data_size);
        memory_.Write32(handle + 12, 0);
        memory_.Write32(handle + 16, 0);
        memory_.Write32(handle + 20, 0);
        memory_.Write32(handle + 24, 0);
        if (Arg(2) != 0) {
            memory_.Write32(Arg(2), handle);
        }
        return 0;
    }
    if (name == "_AudioQueueEnqueueBuffer") {
        runtime_->audio_queues[Arg(0)].buffers.push_back(Arg(1));
        runtime_->audio_queue_buffers[Arg(1)].data_size = memory_.Read32(Arg(1) + 8);
        return 0;
    }
    if (name == "_AudioQueueFreeBuffer") {
        runtime_->audio_queue_buffers.erase(Arg(1));
        host_objects_.erase(Arg(1));
        return 0;
    }
    if (name == "_AudioQueueDispose") {
        StopHostAudioFile();
        runtime_->audio_queues.erase(Arg(0));
        host_objects_.erase(Arg(0));
        return 0;
    }
    if (name == "_AudioQueuePause") {
        runtime_->audio_queues[Arg(0)].running = false;
        StopHostAudioFile();
        return 0;
    }
    if (name == "_AudioQueueSetParameter") {
        runtime_->audio_queues[Arg(0)].parameters[Arg(1)] = read_arg_float(2);
        return 0;
    }
    if (name == "_AudioQueueSetProperty") {
        runtime_->audio_queues[Arg(0)].properties[Arg(1)] = memory_.ReadBuffer(Arg(2), Arg(3));
        return 0;
    }
    if (name == "_AudioQueueStart") {
        runtime_->audio_queues[Arg(0)].running = true;
        if (!runtime_->last_music_path.empty()) {
            const bool ok = PlayHostAudioFile(runtime_->last_music_path);
            Log(std::string("music start host=") + runtime_->last_music_path.string() + " ok=" + (ok ? "1" : "0"));
        }
        return 0;
    }
    if (name == "_AudioQueueStop") {
        runtime_->audio_queues[Arg(0)].running = false;
        StopHostAudioFile();
        return 0;
    }
    auto load_audio_file = [this](RuntimeState::AudioFile& file, const std::string& guest_path, const bool is_ext_audio) {
        file = RuntimeState::AudioFile{};
        file.guest_path = guest_path;
        file.path = ResolveGuestPath(guest_path);
        file.is_ext_audio = is_ext_audio;

        if (const auto bytes = ReadGuestFileBytes(guest_path)) {
            file.raw_bytes = *bytes;
        } else if (const auto bytes = ReadFileBytes(file.path)) {
            file.raw_bytes = *bytes;
        }

        if (!file.raw_bytes.empty()) {
            if (auto decoded = DecodeAudioBytes(file.raw_bytes)) {
                file.decoded_audio = *decoded;
                file.file_format = MakeFileDataDescription(*decoded);
                file.client_format = file.file_format;
                file.file_type = decoded->file_format;
                file.max_packet_size = file.file_format.bytes_per_packet;
                file.frame_count = AudioFrameCount(*decoded);
                file.packet_count = file.frame_count;
                file.has_client_format = true;
                file.data = decoded->pcm;
            } else if (auto sniffed = SniffMp3StreamDescription(file.raw_bytes)) {
                file.file_format = *sniffed;
                file.file_type = sniffed->format_id;
                file.max_packet_size = sniffed->bytes_per_packet;
                file.packet_count = 0;
                file.frame_count = 0;
                file.has_client_format = false;
                file.data = file.raw_bytes;
            } else {
                file.data = file.raw_bytes;
            }
        }

        file.cursor = 0;
        return !file.raw_bytes.empty();
    };
    auto audio_property_size = [](const u32 property_id, const bool extended_file) -> u32 {
        if (extended_file) {
            switch (property_id) {
            case kExtAudioFilePropertyFileDataFormat:
            case kExtAudioFilePropertyClientDataFormat:
                return 40;
            case kExtAudioFilePropertyAudioFile:
            case kExtAudioFilePropertyFileMaxPacketSize:
            case kExtAudioFilePropertyClientMaxPacketSize:
                return 4;
            case kExtAudioFilePropertyFileLengthFrames:
                return 8;
            default:
                return 0;
            }
        }

        switch (property_id) {
        case kAudioFilePropertyFileFormat:
        case kAudioFilePropertyMaximumPacketSize:
            return 4;
        case kAudioFilePropertyAudioDataByteCount:
        case kAudioFilePropertyAudioDataPacketCount:
            return 8;
        case kAudioFilePropertyDataFormat:
            return 40;
        default:
            return 4;
        }
    };
    auto is_debug_sound_path = [](const std::string& lower_guest_path) {
        return lower_guest_path.find("/sfx/") != std::string::npos
            || lower_guest_path.rfind("sfx/", 0) == 0
            || EndsWith(lower_guest_path, ".caf")
            || EndsWith(lower_guest_path, ".wav");
    };
    auto is_target_sound_path = [](const std::string& lower_guest_path) {
        return lower_guest_path.find("button_click_13.caf") != std::string::npos;
    };
    if (name == "_AudioFileOpenURL" || name == "_ExtAudioFileOpenURL") {
        const bool is_ext_audio = name == "_ExtAudioFileOpenURL";
        const u32 handle = make_runtime_object(is_ext_audio ? "ExtAudioFile" : "AudioFile", name);
        auto& file = runtime_->audio_files[handle];
        const auto guest_path = DecodeNSString(Arg(0)).value_or(ReadGuestCString(Arg(0)));
        const bool loaded = load_audio_file(file, guest_path, is_ext_audio);
        const std::string lower_guest_path = Lowercase(guest_path);
        if (is_target_sound_path(lower_guest_path) || (is_debug_sound_path(lower_guest_path) && runtime_->debug_sfx_logs < 128)) {
            ++runtime_->debug_sfx_logs;
            Log(name.substr(1) + " guest=" + guest_path
                + " host=" + file.path.string()
                + " loaded=" + (loaded ? "1" : "0")
                + " decoded=" + (file.decoded_audio.has_value() ? "1" : "0")
                + " data=" + std::to_string(file.data.size())
                + " raw=" + std::to_string(file.raw_bytes.size())
                + " packets=" + std::to_string(file.packet_count)
                + " bpp=" + std::to_string(file.max_packet_size));
        }
        if (lower_guest_path.find("music/") != std::string::npos
            || (lower_guest_path.size() >= 4 && lower_guest_path.compare(lower_guest_path.size() - 4, 4, ".mp3") == 0)) {
            runtime_->last_music_path = file.path;
            Log(name.substr(1) + " guest=" + guest_path + " host=" + file.path.string());
            if (!runtime_->music_autoplay_started && lower_guest_path.find("home_music.mp3") != std::string::npos) {
                const bool ok = PlayHostAudioFile(file.path);
                runtime_->music_autoplay_started = ok;
                Log(std::string("music autoplay host=") + file.path.string() + " ok=" + (ok ? "1" : "0"));
            }
        }
        const u32 out_ptr = is_ext_audio ? Arg(1) : Arg(3);
        if (!loaded || (is_ext_audio && !file.decoded_audio.has_value())) {
            Log(name.substr(1) + " failed guest=" + guest_path + " host=" + file.path.string()
                + " loaded=" + (loaded ? "1" : "0")
                + " decoded=" + (file.decoded_audio.has_value() ? "1" : "0"));
            runtime_->audio_files.erase(handle);
            host_objects_.erase(handle);
            return is_ext_audio ? kExtAudioFileErrorInvalidDataFormat : 1;
        }
        if (out_ptr != 0) {
            memory_.Write32(out_ptr, handle);
        }
        return 0;
    }
    if (name == "_AudioFileClose" || name == "_ExtAudioFileDispose") {
        runtime_->audio_files.erase(Arg(0));
        host_objects_.erase(Arg(0));
        return 0;
    }
    if (name == "_AudioFileGetPropertyInfo") {
        const u32 property_id = Arg(1);
        if (Arg(2) != 0) {
            memory_.Write32(Arg(2), audio_property_size(property_id, false));
        }
        if (Arg(3) != 0) {
            memory_.Write32(Arg(3), 0);
        }
        return 0;
    }
    if (name == "_ExtAudioFileGetProperty") {
        const auto it = runtime_->audio_files.find(Arg(0));
        if (it == runtime_->audio_files.end()) {
            return 1;
        }
        const RuntimeState::AudioFile& file = it->second;
        const std::string lower_guest_path = Lowercase(file.guest_path);
        const u32 property_id = Arg(1);
        const u32 size_ptr = Arg(2);
        const u32 out_ptr = Arg(3);
        const u32 required_size = audio_property_size(property_id, true);
        if (is_target_sound_path(lower_guest_path) || (is_debug_sound_path(lower_guest_path) && runtime_->debug_sfx_logs < 128)) {
            ++runtime_->debug_sfx_logs;
            Log("ExtAudioFileGetProperty path=" + file.guest_path
                + " property=" + Hex32(property_id)
                + " size=" + std::to_string(required_size));
        }
        if (size_ptr != 0) {
            memory_.Write32(size_ptr, required_size);
        }

        switch (property_id) {
        case kExtAudioFilePropertyFileDataFormat:
            if (out_ptr != 0) {
                memory_.WriteBuffer(out_ptr, EncodeAudioStreamBasicDescription(file.file_format));
            }
            return 0;
        case kExtAudioFilePropertyClientDataFormat:
            if (out_ptr != 0) {
                const auto& description = file.has_client_format ? file.client_format : file.file_format;
                memory_.WriteBuffer(out_ptr, EncodeAudioStreamBasicDescription(description));
            }
            return 0;
        case kExtAudioFilePropertyAudioFile:
            if (out_ptr != 0) {
                memory_.Write32(out_ptr, Arg(0));
            }
            return 0;
        case kExtAudioFilePropertyFileMaxPacketSize:
            if (out_ptr != 0) {
                memory_.Write32(out_ptr, file.max_packet_size);
            }
            return 0;
        case kExtAudioFilePropertyClientMaxPacketSize:
            if (out_ptr != 0) {
                const u32 packet_size = file.has_client_format ? file.client_format.bytes_per_packet : file.max_packet_size;
                memory_.Write32(out_ptr, packet_size);
            }
            return 0;
        case kExtAudioFilePropertyFileLengthFrames:
            if (out_ptr != 0) {
                const u64 frames = file.has_client_format ? AudioFrameCount(file.data, file.client_format) : file.frame_count;
                memory_.Write64(out_ptr, frames);
            }
            return 0;
        default:
            Log("ExtAudioFileGetProperty unsupported property=" + Hex32(property_id) + " path=" + file.path.string());
            return 0;
        }
    }
    if (name == "_AudioFileGetProperty") {
        const auto it = runtime_->audio_files.find(Arg(0));
        if (it == runtime_->audio_files.end()) {
            return 1;
        }
        const RuntimeState::AudioFile& file = it->second;
        const std::string lower_guest_path = Lowercase(file.guest_path);
        const u32 property_id = Arg(1);
        const u32 size_ptr = Arg(2);
        const u32 out_ptr = Arg(3);
        const u32 required_size = audio_property_size(property_id, false);
        if (is_target_sound_path(lower_guest_path) || (is_debug_sound_path(lower_guest_path) && runtime_->debug_sfx_logs < 128)) {
            ++runtime_->debug_sfx_logs;
            Log("AudioFileGetProperty path=" + file.guest_path
                + " property=" + Hex32(property_id)
                + " size=" + std::to_string(required_size));
        }
        if (size_ptr != 0) {
            memory_.Write32(size_ptr, required_size);
        }

        switch (property_id) {
        case kAudioFilePropertyFileFormat:
            if (out_ptr != 0) {
                memory_.Write32(out_ptr, file.file_type != 0 ? file.file_type : file.file_format.format_id);
            }
            return 0;
        case kAudioFilePropertyDataFormat:
            if (out_ptr != 0) {
                memory_.WriteBuffer(out_ptr, EncodeAudioStreamBasicDescription(file.file_format));
            }
            return 0;
        case kAudioFilePropertyAudioDataByteCount:
            if (out_ptr != 0) {
                memory_.Write64(out_ptr, static_cast<u64>(file.data.size()));
            }
            return 0;
        case kAudioFilePropertyAudioDataPacketCount:
            if (out_ptr != 0) {
                memory_.Write64(out_ptr, file.packet_count);
            }
            return 0;
        case kAudioFilePropertyMaximumPacketSize:
            if (out_ptr != 0) {
                memory_.Write32(out_ptr, file.max_packet_size);
            }
            return 0;
        default:
            Log("AudioFileGetProperty unsupported property=" + Hex32(property_id) + " path=" + file.path.string());
            return 0;
        }
    }
    if (name == "_ExtAudioFileSetProperty") {
        auto it = runtime_->audio_files.find(Arg(0));
        if (it == runtime_->audio_files.end()) {
            return 1;
        }
        RuntimeState::AudioFile& file = it->second;
        const std::string lower_guest_path = Lowercase(file.guest_path);
        const u32 property_id = Arg(1);
        if (property_id == kExtAudioFilePropertyClientDataFormat) {
            if (!file.decoded_audio.has_value()) {
                return kExtAudioFileErrorInvalidDataFormat;
            }
            const auto bytes = memory_.ReadBuffer(Arg(3), Arg(2));
            const auto description = DecodeAudioStreamBasicDescription(bytes);
            if (is_target_sound_path(lower_guest_path) || (is_debug_sound_path(lower_guest_path) && runtime_->debug_sfx_logs < 128)) {
                ++runtime_->debug_sfx_logs;
                Log("ExtAudioFileSetProperty path=" + file.guest_path
                    + " property=" + Hex32(property_id)
                    + " channels=" + std::to_string(description ? description->channels_per_frame : 0)
                    + " bits=" + std::to_string(description ? description->bits_per_channel : 0)
                    + " bytes_per_frame=" + std::to_string(description ? description->bytes_per_frame : 0)
                    + " sample_rate=" + std::to_string(description ? description->sample_rate : 0.0));
            }
            if (!description || description->format_id != kAudioFormatLinearPcm) {
                return kExtAudioFileErrorNonPcmClientFormat;
            }
            if (!IsSupportedLinearPcmDescription(*description)) {
                return kExtAudioFileErrorInvalidDataFormat;
            }
            const auto converted = ConvertPcmForClientFormat(*file.decoded_audio, *description);
            if (!converted) {
                Log("ExtAudioFileSetProperty failed client format path=" + file.path.string());
                return kExtAudioFileErrorInvalidDataFormat;
            }
            file.client_format = *description;
            file.has_client_format = true;
            file.data = *converted;
            file.cursor = 0;
            return 0;
        }
        return 0;
    }
    if (name == "_AudioFileReadPackets") {
        const auto it = runtime_->audio_files.find(Arg(0));
        if (it == runtime_->audio_files.end()) {
            return 1;
        }
        const RuntimeState::AudioFile& file = it->second;
        const std::string lower_guest_path = Lowercase(file.guest_path);
        const u32 out_num_bytes_ptr = Arg(2);
        const u64 starting_packet = Pack64(Arg(4), Arg(5));
        const u32 io_num_packets_ptr = Arg(6);
        const u32 out_buffer = Arg(7);
        const u32 bytes_per_packet = std::max<u32>(file.max_packet_size, 1);
        const u64 start_offset_u64 = starting_packet * static_cast<u64>(bytes_per_packet);
        const std::size_t start_offset = static_cast<std::size_t>(std::min<u64>(start_offset_u64, file.data.size()));
        const std::size_t remaining = file.data.size() - start_offset;
        const u32 requested_packets = io_num_packets_ptr != 0 ? memory_.Read32(io_num_packets_ptr) : 0;
        std::size_t bytes = requested_packets == 0
            ? remaining
            : std::min<std::size_t>(remaining, static_cast<std::size_t>(requested_packets) * bytes_per_packet);
        if (file.max_packet_size != 0) {
            bytes -= bytes % bytes_per_packet;
        }
        if (bytes != 0 && out_buffer != 0) {
            memory_.WriteBuffer(out_buffer, std::span<const u8>(file.data.data() + static_cast<std::ptrdiff_t>(start_offset), bytes));
        }
        if (bytes != 0) {
            runtime_->debug_last_audio_path = file.guest_path;
        }
        if (out_num_bytes_ptr != 0) {
            memory_.Write32(out_num_bytes_ptr, static_cast<u32>(bytes));
        }
        if (io_num_packets_ptr != 0) {
            const u32 packets_read = file.max_packet_size == 0
                ? (bytes == 0 ? 0u : 1u)
                : static_cast<u32>(bytes / bytes_per_packet);
            memory_.Write32(io_num_packets_ptr, packets_read);
        }
        if (is_target_sound_path(lower_guest_path) || (is_debug_sound_path(lower_guest_path) && runtime_->debug_sfx_logs < 128)) {
            ++runtime_->debug_sfx_logs;
            Log("AudioFileReadPackets path=" + file.guest_path
                + " start_packet=" + std::to_string(starting_packet)
                + " requested_packets=" + std::to_string(io_num_packets_ptr != 0 ? memory_.Read32(io_num_packets_ptr) : 0)
                + " bytes=" + std::to_string(bytes)
                + " bytes_per_packet=" + std::to_string(bytes_per_packet));
        }
        return 0;
    }
    if (name == "_ExtAudioFileRead") {
        auto it = runtime_->audio_files.find(Arg(0));
        if (it == runtime_->audio_files.end()) {
            return 1;
        }
        auto& file = it->second;
        const std::string lower_guest_path = Lowercase(file.guest_path);
        const u32 buffer_list = Arg(2);
        if (buffer_list == 0) {
            if (Arg(1) != 0) {
                memory_.Write32(Arg(1), 0);
            }
            return 0;
        }
        const u32 requested_frames = Arg(1) != 0 ? memory_.Read32(Arg(1)) : 0;
        const AudioStreamBasicDescriptionData description = file.has_client_format ? file.client_format : file.file_format;
        const u32 bytes_per_frame = std::max<u32>(1, description.bytes_per_frame);
        const u32 number_buffers = memory_.Read32(buffer_list + 0);
        if (number_buffers == 0) {
            if (Arg(1) != 0) {
                memory_.Write32(Arg(1), 0);
            }
            return 0;
        }
        const u32 data_ptr = memory_.Read32(buffer_list + 12);
        const u32 data_byte_size = memory_.Read32(buffer_list + 8);
        const u32 remaining = static_cast<u32>(file.data.size()) > file.cursor
            ? static_cast<u32>(file.data.size()) - file.cursor
            : 0;
        const u32 requested_bytes = requested_frames == 0 ? data_byte_size : requested_frames * bytes_per_frame;
        const u32 bytes = std::min<u32>(requested_bytes, std::min<u32>(data_byte_size, remaining))
            - (std::min<u32>(requested_bytes, std::min<u32>(data_byte_size, remaining)) % bytes_per_frame);
        if (bytes != 0) {
            memory_.WriteBuffer(data_ptr, std::span<const u8>(file.data.data() + file.cursor, bytes));
        }
        if (Arg(1) != 0) {
            memory_.Write32(Arg(1), bytes / bytes_per_frame);
        }
        memory_.Write32(buffer_list + 4, description.channels_per_frame);
        memory_.Write32(buffer_list + 8, bytes);
        file.cursor += bytes;
        if (bytes != 0) {
            runtime_->debug_last_audio_path = file.guest_path;
        }
        if (is_target_sound_path(lower_guest_path) || (is_debug_sound_path(lower_guest_path) && runtime_->debug_sfx_logs < 128)) {
            ++runtime_->debug_sfx_logs;
            Log("ExtAudioFileRead path=" + file.guest_path
                + " requested_frames=" + std::to_string(requested_frames)
                + " bytes=" + std::to_string(bytes)
                + " bytes_per_frame=" + std::to_string(bytes_per_frame)
                + " cursor=" + std::to_string(file.cursor));
        }
        return 0;
    }
    return 0;
}

u32 Emulator::HandleSecurityFunction(const std::string& name) {
    constexpr u32 kErrSecSuccess = 0;
    constexpr u32 kErrSecDuplicateItem = static_cast<u32>(-25299);
    constexpr u32 kErrSecItemNotFound = static_cast<u32>(-25300);

    auto make_security_object = [this](const std::string& kind, const std::string& tag) {
        const u32 object = AllocateData(kObjcObjectSize, 4, tag);
        host_objects_[object] = HostObject{
            .kind = ObjKind::Generic,
            .class_name = kind,
            .isa = EnsureClass("NSObject")
        };
        memory_.Write32(object, EnsureClass("NSObject"));
        runtime_->security_objects[object].kind = kind;
        return object;
    };

    auto copy_digest_output = [this](const u32 out, const std::vector<u8>& bytes) {
        if (out != 0 && !bytes.empty()) {
            memory_.WriteBuffer(out, bytes);
        }
    };

    auto extract_data = [this](const u32 object) -> std::vector<u8> {
        if (object == 0) {
            return {};
        }
        if (const auto it = host_objects_.find(object); it != host_objects_.end()) {
            if (it->second.kind == ObjKind::Data) {
                return it->second.bytes;
            }
            if (it->second.kind == ObjKind::String) {
                return std::vector<u8>(it->second.string_value.begin(), it->second.string_value.end());
            }
        }
        const std::string text = ReadGuestCString(object);
        return std::vector<u8>(text.begin(), text.end());
    };

    auto equal_values = [this](const u32 lhs, const u32 rhs) {
        if (lhs == rhs) {
            return true;
        }
        const auto lhs_string = DecodeNSString(lhs);
        const auto rhs_string = DecodeNSString(rhs);
        if (lhs_string || rhs_string) {
            return lhs_string == rhs_string;
        }
        const auto lhs_it = host_objects_.find(lhs);
        const auto rhs_it = host_objects_.find(rhs);
        if (lhs_it != host_objects_.end() && rhs_it != host_objects_.end()) {
            if (lhs_it->second.kind == ObjKind::Data && rhs_it->second.kind == ObjKind::Data) {
                return lhs_it->second.bytes == rhs_it->second.bytes;
            }
            if (lhs_it->second.kind == ObjKind::Number && rhs_it->second.kind == ObjKind::Number) {
                return lhs_it->second.number_value == rhs_it->second.number_value;
            }
            if (lhs_it->second.kind == ObjKind::Boolean && rhs_it->second.kind == ObjKind::Boolean) {
                return lhs_it->second.boolean_value == rhs_it->second.boolean_value;
            }
        }
        return false;
    };

    auto query_matches_item = [&](const std::unordered_map<std::string, u32>& query, const RuntimeState::KeychainItem& item) {
        for (const auto& [key, value] : query) {
            if (key == "kSecReturnData"
                || key == "kSecReturnRef"
                || key == "kSecReturnAttributes"
                || key == "kSecReturnPersistentRef"
                || key == "kSecMatchLimit"
                || key == "kSecMatchLimitAll"
                || key == "kSecMatchLimitOne"
                || key == "kSecUseOperationPrompt"
                || key == "kSecUseAuthenticationUI"
                || key == "kSecUseNoAuthenticationUI"
                || key == "kSecAttrSynchronizableAny") {
                continue;
            }
            if (key == "kSecValueData") {
                if (extract_data(value) != item.data) {
                    return false;
                }
                continue;
            }
            const auto it = item.attrs.find(key);
            if (it == item.attrs.end() || !equal_values(it->second, value)) {
                return false;
            }
        }
        return true;
    };

    auto compute_digest = [&](const std::string& algorithm, const std::vector<u8>& data) {
        return CryptoCompat::Digest(algorithm, data);
    };

    auto compute_hmac = [&](const u32 algorithm, const std::vector<u8>& key, const std::vector<u8>& data) {
        return CryptoCompat::Hmac(algorithm, key, data);
    };

    if (name == "_SecItemAdd") {
        const auto& dict = host_objects_[Arg(0)].dict;
        for (const auto& item : runtime_->keychain_items) {
            if (query_matches_item(dict, item)) {
                return kErrSecDuplicateItem;
            }
        }
        RuntimeState::KeychainItem item;
        item.attrs = dict;
        if (const auto it = dict.find("kSecValueData"); it != dict.end()) {
            item.data = extract_data(it->second);
        }
        runtime_->keychain_items.push_back(item);
        if (Arg(1) != 0) {
            const u32 ref = make_security_object("SecKeychainItemRef", "SecKeychainItemRef");
            runtime_->security_objects[ref].fields["index"] = static_cast<u32>(runtime_->keychain_items.size() - 1);
            memory_.Write32(Arg(1), ref);
        }
        return kErrSecSuccess;
    }
    if (name == "_SecItemCopyMatching") {
        const auto& dict = host_objects_[Arg(0)].dict;
        for (std::size_t index = 0; index < runtime_->keychain_items.size(); ++index) {
            const auto& item = runtime_->keychain_items[index];
            if (!query_matches_item(dict, item)) {
                continue;
            }
            if (Arg(1) != 0) {
                if (const auto return_data = dict.find("kSecReturnData"); return_data != dict.end() && host_objects_.count(return_data->second) != 0) {
                    memory_.Write32(Arg(1), EnsureNSData(item.data));
                } else if (const auto return_ref = dict.find("kSecReturnRef"); return_ref != dict.end() && host_objects_.count(return_ref->second) != 0) {
                    const u32 ref = make_security_object("SecKeychainItemRef", "SecKeychainItemRef");
                    runtime_->security_objects[ref].fields["index"] = static_cast<u32>(index);
                    memory_.Write32(Arg(1), ref);
                } else {
                    memory_.Write32(Arg(1), EnsureNSData(item.data));
                }
            }
            return kErrSecSuccess;
        }
        return kErrSecItemNotFound;
    }
    if (name == "_SecItemDelete") {
        const auto& dict = host_objects_[Arg(0)].dict;
        const auto before = runtime_->keychain_items.size();
        runtime_->keychain_items.erase(
            std::remove_if(runtime_->keychain_items.begin(), runtime_->keychain_items.end(), [&](const auto& item) {
                return query_matches_item(dict, item);
            }),
            runtime_->keychain_items.end());
        return before == runtime_->keychain_items.size() ? kErrSecItemNotFound : kErrSecSuccess;
    }
    if (name == "_SecCertificateCreateWithData") {
        const u32 object = make_security_object("SecCertificate", "SecCertificate");
        runtime_->security_objects[object].bytes = extract_data(Arg(1));
        return object;
    }
    if (name == "_SecPolicyCreateBasicX509") {
        return make_security_object("SecPolicy", "SecPolicy.BasicX509");
    }
    if (name == "_SecTrustCreateWithCertificates") {
        const u32 object = make_security_object("SecTrust", "SecTrust");
        runtime_->security_objects[object].refs = {Arg(0), Arg(1)};
        if (Arg(2) != 0) {
            memory_.Write32(Arg(2), object);
        }
        return kErrSecSuccess;
    }
    if (name == "_SecTrustEvaluate") {
        if (Arg(1) != 0) {
            memory_.Write32(Arg(1), 1);
        }
        return kErrSecSuccess;
    }
    if (name == "_SecTrustCopyPublicKey") {
        const u32 object = make_security_object("SecKey", "SecKey.Public");
        runtime_->security_objects[object].refs = {Arg(0)};
        runtime_->security_objects[object].fields["block_size"] = 128;
        return object;
    }
    if (name == "_SecKeyGetBlockSize") {
        if (const auto it = runtime_->security_objects.find(Arg(0)); it != runtime_->security_objects.end()) {
            if (const auto block_size = it->second.fields.find("block_size"); block_size != it->second.fields.end()) {
                return block_size->second;
            }
        }
        return 128;
    }
    if (name == "_SecKeyEncrypt" || name == "_SecKeyDecrypt") {
        const u32 input = Arg(2);
        const u32 input_size = Arg(3);
        const u32 output = Arg(4);
        const u32 output_size_ptr = Arg(5);
        u32 capacity = output_size_ptr == 0 ? input_size : memory_.Read32(output_size_ptr);
        const u32 bytes = std::min(capacity, input_size);
        if (bytes != 0) {
            memory_.WriteBuffer(output, memory_.ReadBuffer(input, bytes));
        }
        if (output_size_ptr != 0) {
            memory_.Write32(output_size_ptr, bytes);
        }
        return kErrSecSuccess;
    }
    if (name == "_SCNetworkReachabilityCreateWithName") {
        const u32 object = make_security_object("SCNetworkReachability", "SCNetworkReachability.name");
        runtime_->reachability[object].target = ReadGuestCString(Arg(1));
        return object;
    }
    if (name == "_SCNetworkReachabilityCreateWithAddress") {
        const u32 object = make_security_object("SCNetworkReachability", "SCNetworkReachability.address");
        runtime_->reachability[object].address = memory_.ReadBuffer(Arg(1), 16);
        return object;
    }
    if (name == "_SCNetworkReachabilitySetCallback") {
        auto& reachability = runtime_->reachability[Arg(0)];
        reachability.callback = Arg(1);
        reachability.context = Arg(2);
        return 1;
    }
    if (name == "_SCNetworkReachabilityScheduleWithRunLoop") {
        runtime_->reachability[Arg(0)].scheduled = true;
        return 1;
    }
    if (name == "_SCNetworkReachabilityUnscheduleFromRunLoop") {
        runtime_->reachability[Arg(0)].scheduled = false;
        return 1;
    }
    if (name == "_SCNetworkReachabilityGetFlags") {
        if (Arg(1) != 0) {
            memory_.Write32(Arg(1), 2);
        }
        return 1;
    }
    if (name == "_CC_MD5" || name == "_CC_SHA1" || name == "_CC_SHA256") {
        const std::string algorithm = name == "_CC_MD5" ? "MD5" : (name == "_CC_SHA1" ? "SHA1" : "SHA256");
        copy_digest_output(Arg(2), compute_digest(algorithm, memory_.ReadBuffer(Arg(0), Arg(1))));
        return Arg(2);
    }
    if (name == "_CC_SHA1_Init") {
        auto& object = runtime_->security_objects[Arg(0)];
        object.kind = "CC_SHA1_CTX";
        object.bytes.clear();
        return 1;
    }
    if (name == "_CC_SHA1_Update") {
        auto& object = runtime_->security_objects[Arg(0)];
        const auto bytes = memory_.ReadBuffer(Arg(1), Arg(2));
        object.bytes.insert(object.bytes.end(), bytes.begin(), bytes.end());
        return 1;
    }
    if (name == "_CC_SHA1_Final") {
        const auto it = runtime_->security_objects.find(Arg(1));
        if (it == runtime_->security_objects.end()) {
            return 0;
        }
        copy_digest_output(Arg(0), compute_digest("SHA1", it->second.bytes));
        return 1;
    }
    if (name == "_CCHmac") {
        const auto key = memory_.ReadBuffer(Arg(1), Arg(2));
        const auto data = memory_.ReadBuffer(Arg(3), Arg(4));
        copy_digest_output(Arg(5), compute_hmac(Arg(0), key, data));
        return 0;
    }
    return 0;
}

u32 Emulator::HandleCppRuntimeFunction(const std::string& name) {
    auto rb_parent = [this](const u32 node) {
        return node == 0 ? 0u : memory_.Read32(node + 4);
    };
    auto rb_left = [this](const u32 node) {
        return node == 0 ? 0u : memory_.Read32(node + 8);
    };
    auto rb_right = [this](const u32 node) {
        return node == 0 ? 0u : memory_.Read32(node + 12);
    };

    if (name == "__Unwind_SjLj_Register" || name == "__Unwind_SjLj_Unregister") {
        return 0;
    }
    if (name == "__Unwind_SjLj_Resume") {
        if (suppress_next_unwind_resume_) {
            Log("suppressed __Unwind_SjLj_Resume after nonfatal debugger assert");
            suppress_next_unwind_resume_ = false;
            return 0;
        }
        exit_code_ = 1;
        Stop(Dynarmic::HaltReason::UserDefined4);
        return 0;
    }
    if (name == "___cxa_atexit") {
        if (Arg(0) != 0) {
            runtime_->atexit_callbacks.emplace_back(Arg(0), Arg(1));
        }
        return 0;
    }
    if (name == "___gxx_personality_sj0" || name == "___objc_personality_v0") {
        return 0;
    }
    if (name == "___cxa_begin_catch") {
        return Arg(0);
    }
    if (name == "___cxa_end_catch") {
        return 0;
    }
    if (name == "___cxa_guard_acquire") {
        const u32 guard = Arg(0);
        const u32 value = memory_.Read32(guard);
        if (value == 0) {
            memory_.Write32(guard, 1);
            return 1;
        }
        return 0;
    }
    if (name == "___cxa_guard_release" || name == "___cxa_guard_abort") {
        return 0;
    }
    if (name == "___divsi3") {
        return static_cast<u32>(static_cast<s32>(Arg(0)) / std::max<s32>(static_cast<s32>(Arg(1)), 1));
    }
    if (name == "___modsi3") {
        return static_cast<u32>(static_cast<s32>(Arg(0)) % std::max<s32>(static_cast<s32>(Arg(1)), 1));
    }
    if (name == "___udivsi3") {
        return Arg(1) == 0 ? 0 : Arg(0) / Arg(1);
    }
    if (name == "___umodsi3") {
        return Arg(1) == 0 ? 0 : Arg(0) % Arg(1);
    }
    if (name == "___udivdi3") {
        const u64 result = Pack64(Arg(0), Arg(1)) / std::max<u64>(Pack64(Arg(2), Arg(3)), 1);
        SetReturnU64(result);
        return cpu_->Regs()[0];
    }
    if (name == "___fixdfdi") {
        SetReturnU64(static_cast<u64>(static_cast<s64>(BitCastFromU64<double>(Pack64(Arg(0), Arg(1))))));
        return cpu_->Regs()[0];
    }
    if (name == "___fixunsdfdi") {
        SetReturnU64(static_cast<u64>(BitCastFromU64<double>(Pack64(Arg(0), Arg(1)))));
        return cpu_->Regs()[0];
    }
    if (name == "___floatundidf") {
        SetReturnDouble(static_cast<double>(Pack64(Arg(0), Arg(1))));
        return cpu_->Regs()[0];
    }
    if (name == "__ZNSsC1EPKcRKSaIcE") {
        const u32 self = Arg(0);
        const std::string value = ReadGuestCString(Arg(1));
        libstdcpp_abi_->InitializeString(self, value, "std::string.cstr");
        return self;
    }
    if (name == "__ZNSsC1ERKSs") {
        libstdcpp_abi_->CopyString(Arg(0), Arg(1), "std::string.copy");
        return Arg(0);
    }
    if (name == "__ZNSsD1Ev" || name == "__ZNSsD2Ev") {
        libstdcpp_abi_->DestroyString(Arg(0));
        return 0;
    }
    if (name == "__ZNSo5writeEPKci") {
        const auto host_it = host_objects_.find(Arg(0));
        const auto file_it = host_it == host_objects_.end() ? file_handles_.end() : file_handles_.find(host_it->second.backing_store);
        const auto bytes = memory_.ReadBuffer(Arg(1), Arg(2));
        if (file_it != file_handles_.end()) {
            std::fwrite(bytes.data(), 1, bytes.size(), file_it->second.file);
        } else {
            Log(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        }
        return Arg(0);
    }
    if (name == "__ZNKSs7compareEPKc") {
        return static_cast<u32>(libstdcpp_abi_->CompareStringWithCString(Arg(0), Arg(1)));
    }
    if (name == "__ZNKSs7compareERKSs") {
        return static_cast<u32>(libstdcpp_abi_->CompareStrings(Arg(0), Arg(1)));
    }
    if (name == "__ZNKSs6substrEmm") {
        libstdcpp_abi_->Substr(Arg(0), Arg(1), Arg(2), Arg(3), "std::string.substr");
        return Arg(0);
    }
    if (name == "__ZNSs6assignEPKcm") {
        libstdcpp_abi_->AssignFromCString(Arg(0), Arg(1), Arg(2), "std::string.assign");
        return Arg(0);
    }
    if (name == "__ZNSs6assignERKSs") {
        libstdcpp_abi_->AssignFromString(Arg(0), Arg(1), "std::string.assign.copy");
        return Arg(0);
    }
    if (name == "__ZNSs9push_backEc") {
        libstdcpp_abi_->PushBack(Arg(0), static_cast<char>(Arg(1)), "std::string.push_back");
        return 0;
    }
    if (name == "__ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode") {
        const auto host_path = ResolveGuestPath(ReadGuestCString(Arg(1)));
        if (const auto parent = host_path.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent);
        }
        const u32 open_mode = Arg(2);
        const bool append = (open_mode & 0x01u) != 0;
        const bool binary = (open_mode & 0x04u) != 0;
        const bool in = (open_mode & 0x08u) != 0;
        const bool out = (open_mode & 0x10u) != 0;
        const bool trunc = (open_mode & 0x20u) != 0;
        std::string mode = "wb";
        if (in && out) {
            mode = trunc ? "w+b" : "r+b";
        } else if (in) {
            mode = "rb";
        } else if (append) {
            mode = "ab";
        } else if (out) {
            mode = trunc ? "wb" : "ab";
        }
        if (!binary && !mode.empty() && mode.back() == 'b') {
            mode.pop_back();
        }
        std::FILE* file = std::fopen(host_path.string().c_str(), mode.c_str());
        host_objects_[Arg(0)] = HostObject{
            .kind = ObjKind::Generic,
            .class_name = "std::ofstream",
            .backing_store = 0,
        };
        if (file != nullptr) {
            host_objects_[Arg(0)].backing_store = libc_abi_->WrapHostFile(file, host_path, "std::ofstream.FILE", FileDescriptorFromFile(file), 0x0008u);
        }
        return Arg(0);
    }
    if (name == "__ZNSt14basic_ofstreamIcSt11char_traitsIcEE5closeEv") {
        if (const auto it = host_objects_.find(Arg(0)); it != host_objects_.end()) {
            libc_abi_->CloseFile(it->second.backing_store);
            it->second.backing_store = 0;
        }
        return 0;
    }
    if (name == "__ZNSt14basic_ofstreamIcSt11char_traitsIcEED1Ev") {
        if (const auto it = host_objects_.find(Arg(0)); it != host_objects_.end()) {
            libc_abi_->CloseFile(it->second.backing_store);
            host_objects_.erase(it);
        }
        return 0;
    }
    if (name == "__ZNSt15_List_node_base4hookEPS_") {
        const u32 self = Arg(0);
        const u32 position = Arg(1);
        const u32 prev = memory_.Read32(position + 4);
        memory_.Write32(self + 0, position);
        memory_.Write32(self + 4, prev);
        memory_.Write32(prev + 0, self);
        memory_.Write32(position + 4, self);
        return 0;
    }
    if (name == "__ZNSt15_List_node_base6unhookEv") {
        const u32 self = Arg(0);
        const u32 next = memory_.Read32(self + 0);
        const u32 prev = memory_.Read32(self + 4);
        memory_.Write32(prev + 0, next);
        memory_.Write32(next + 4, prev);
        memory_.Write32(self + 0, self);
        memory_.Write32(self + 4, self);
        return 0;
    }
    if (name == "__ZNSt8ios_base4InitC1Ev") {
        return Arg(0);
    }
    if (name == "__ZNSt8ios_base4InitD1Ev") {
        return 0;
    }
    if (name == "__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base" || name == "__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base") {
        u32 x = Arg(0);
        if (rb_right(x) != 0) {
            x = rb_right(x);
            while (rb_left(x) != 0) {
                x = rb_left(x);
            }
            return x;
        }
        u32 y = rb_parent(x);
        while (y != 0 && x == rb_right(y)) {
            x = y;
            y = rb_parent(y);
        }
        if (rb_right(x) != y) {
            x = y;
        }
        return x;
    }
    if (name == "__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base") {
        u32 x = Arg(0);
        if (x == 0) {
            return 0;
        }
        if (memory_.Read8(x) != 0 && rb_parent(x) != 0 && rb_parent(rb_parent(x)) == x) {
            return rb_right(x);
        }
        if (rb_left(x) != 0) {
            x = rb_left(x);
            while (rb_right(x) != 0) {
                x = rb_right(x);
            }
            return x;
        }
        u32 y = rb_parent(x);
        while (y != 0 && x == rb_left(y)) {
            x = y;
            y = rb_parent(y);
        }
        return y;
    }
    if (name == "__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_") {
        const u32 insert_left = Arg(0);
        const u32 x = Arg(1);
        const u32 p = Arg(2);
        const u32 header = Arg(3);
        memory_.Write32(x + 4, p);
        memory_.Write32(x + 8, 0);
        memory_.Write32(x + 12, 0);
        memory_.Write8(x, 0);
        if (insert_left != 0) {
            memory_.Write32(p + 8, x);
            if (p == header) {
                memory_.Write32(header + 4, x);
                memory_.Write32(header + 12, x);
            } else if (p == memory_.Read32(header + 8)) {
                memory_.Write32(header + 8, x);
            }
        } else {
            memory_.Write32(p + 12, x);
            if (p == memory_.Read32(header + 12)) {
                memory_.Write32(header + 12, x);
            }
        }
        return 0;
    }
    if (name == "__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_") {
        const u32 z = Arg(0);
        const u32 header = Arg(1);
        const u32 left = rb_left(z);
        const u32 right = rb_right(z);
        const u32 parent = rb_parent(z);
        const u32 replacement = left != 0 ? left : right;
        if (replacement != 0) {
            memory_.Write32(replacement + 4, parent);
        }
        if (parent != 0) {
            if (rb_left(parent) == z) {
                memory_.Write32(parent + 8, replacement);
            } else if (rb_right(parent) == z) {
                memory_.Write32(parent + 12, replacement);
            }
        }
        if (memory_.Read32(header + 4) == z) {
            memory_.Write32(header + 4, replacement == 0 ? parent : replacement);
        }
        if (memory_.Read32(header + 8) == z) {
            memory_.Write32(header + 8, replacement == 0 ? parent : replacement);
        }
        if (memory_.Read32(header + 12) == z) {
            memory_.Write32(header + 12, replacement == 0 ? parent : replacement);
        }
        return z;
    }
    // --- C++ exception allocation ---
    if (name == "___cxa_allocate_exception") {
        const u32 size = std::max<u32>(Arg(0), 16);
        return AllocateData(size, 4, "cxa_exception");
    }
    if (name == "___cxa_free_exception") {
        return 0;
    }
    if (name == "___cxa_throw" || name == "___cxa_rethrow" || name == "___cxa_call_unexpected") {
        exit_code_ = 1;
        Stop(Dynarmic::HaltReason::UserDefined3);
        return 0;
    }
    // --- compiler-rt float <-> int conversions ---
    if (name == "___fixunssfdi") {
        const float value = BitsToFloat(Arg(0));
        SetReturnU64(static_cast<u64>(value >= 0.0f ? value : 0.0f));
        return cpu_->Regs()[0];
    }
    if (name == "___floatdidf") {
        SetReturnDouble(static_cast<double>(static_cast<s64>(Pack64(Arg(0), Arg(1)))));
        return cpu_->Regs()[0];
    }
    // --- std::runtime_error / std::logic_error ---
    if (name == "__ZNSt13runtime_errorC1ERKSs") {
        host_objects_[Arg(0)] = HostObject{
            .kind = ObjKind::Generic,
            .class_name = "std::runtime_error",
            .string_value = libstdcpp_abi_->ReadString(Arg(1)),
        };
        return Arg(0);
    }
    if (name == "__ZNSt13runtime_errorD1Ev" || name == "__ZNSt13runtime_errorD2Ev" || name == "__ZNSt12logic_errorD1Ev") {
        host_objects_.erase(Arg(0));
        return 0;
    }
    if (name == "__ZSt20__throw_out_of_rangePKc") {
        const std::string message = ReadGuestCString(Arg(0));
        if (message.find("sound which is not yet loaded") != std::string::npos
            || message.find("music which is not yet loaded") != std::string::npos) {
            Log("std::out_of_range suppressed: " + message);
            return 0;
        }
        Log("std::out_of_range: " + message);
        exit_code_ = 1;
        Stop(Dynarmic::HaltReason::UserDefined3);
        return 0;
    }
    // --- std::stringstream ---
    if (name == "__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode") {
        return Arg(0);
    }
    if (name == "__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev") {
        return 0;
    }
    // --- std::ios_base::clear ---
    if (name == "__ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate") {
        return 0;
    }
    // --- std::ostream insert / istream extract ---
    if (name == "__ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i") {
        return Arg(0);
    }
    if (name == "__ZNSirsIjEERSiRT_") {
        return Arg(0);
    }
    // --- std::string _M_destroy ---
    if (name == "__ZNSs4_Rep10_M_destroyERKSaIcE") {
        return 0;
    }
    return 0;
}


void Emulator::HandlePrintfLike(const std::string& name) {
    std::string format;
    std::function<u32(std::size_t)> read_arg;

    if (name == "_printf") {
        format = ReadGuestCString(Arg(0));
        read_arg = [this](const std::size_t index) {
            return Arg(index);
        };
    } else if (name == "_vprintf") {
        format = ReadGuestCString(Arg(0));
        const u32 va_list = Arg(1);
        read_arg = [this, va_list](const std::size_t index) {
            const std::size_t word_index = index == 0 ? 0 : (index - 1);
            return memory_.Read32(va_list + static_cast<u32>(word_index * 4));
        };
    } else if (name == "_vfprintf") {
        format = ReadGuestCString(Arg(1));
        const u32 va_list = Arg(2);
        read_arg = [this, va_list](const std::size_t index) {
            return memory_.Read32(va_list + static_cast<u32>(index * 4));
        };
    } else if (name == "_snprintf") {
        format = ReadGuestCString(Arg(2));
        read_arg = [this](const std::size_t index) {
            return Arg(index);
        };
    } else if (name == "_vsnprintf") {
        format = ReadGuestCString(Arg(2));
        const u32 va_list = Arg(3);
        read_arg = [this, va_list](const std::size_t index) {
            const std::size_t word_index = index >= 4 ? (index - 4) : 0;
            return memory_.Read32(va_list + static_cast<u32>(word_index * 4));
        };
    } else if (name == "___snprintf_chk") {
        format = ReadGuestCString(Arg(4));
        read_arg = [this](const std::size_t index) {
            return Arg(index);
        };
    } else if (name == "___sprintf_chk") {
        format = ReadGuestCString(Arg(3));
        read_arg = [this](const std::size_t index) {
            return Arg(index);
        };
    } else if (name == "_fprintf") {
        format = ReadGuestCString(Arg(1));
        read_arg = [this](const std::size_t index) {
            return Arg(index);
        };
    } else if (name == "_sprintf") {
        format = ReadGuestCString(Arg(1));
        read_arg = [this](const std::size_t index) {
            return Arg(index);
        };
    } else {

        format = ReadGuestCString(Arg(0));
        read_arg = [this](const std::size_t index) {
            return Arg(index);
        };
    }

    const std::size_t first_arg =
        name == "_printf" ? 1 :
        name == "_vprintf" ? 1 :
        name == "_vfprintf" ? 0 :
        name == "_snprintf" ? 3 :
        name == "_vsnprintf" ? 4 :
        name == "___snprintf_chk" ? 5 :
        name == "___sprintf_chk" ? 4 :
        name == "_fprintf" ? 2 :
        name == "_sprintf" ? 2 : 1;

    const std::string rendered = FormatVarArgsString(format, first_arg, read_arg);
    const bool is_string_buffer_write = name == "_snprintf"
        || name == "_vsnprintf"
        || name == "___snprintf_chk"
        || name == "___sprintf_chk"
        || name == "_sprintf";
    if (!is_string_buffer_write && rendered.rfind("[error]", 0) == 0) {
        last_guest_error_ = rendered;
    }

    if (is_string_buffer_write) {
        const u32 buffer = Arg(0);
        const u32 limit = name == "_sprintf" ? static_cast<u32>(rendered.size() + 1)
            : (name == "_snprintf" ? Arg(1)
            : (name == "_vsnprintf" ? Arg(1)
            : (name == "___snprintf_chk" ? Arg(1) : Arg(2))));
        if (limit != 0) {
            const std::size_t bytes = std::min<std::size_t>(rendered.size(), limit - 1);
            memory_.WriteBuffer(buffer, std::span<const u8>(reinterpret_cast<const u8*>(rendered.data()), bytes));
            memory_.Write8(buffer + static_cast<u32>(bytes), 0);
        }
    } else if (name == "_vfprintf" || name == "_fprintf") {
        if (std::FILE* const file = libc_abi_->LookupHostFile(Arg(0)); file != nullptr) {
            std::fwrite(rendered.data(), 1, rendered.size(), file);
        } else {
            Log(rendered);
        }
    } else {
        Log(rendered);
    }
    SetReturnU32(static_cast<u32>(rendered.size()));
}

int Emulator::HandleScanfLike(const std::string& /*name*/) {
    const std::string input = ReadGuestCString(Arg(0));
    const std::string format = ReadGuestCString(Arg(1));
    std::size_t in = 0;
    std::size_t arg_index = 2;
    int assigned = 0;

    auto skip_input_space = [&]() {
        while (in < input.size() && std::isspace(static_cast<unsigned char>(input[in]))) {
            ++in;
        }
    };
    auto read_assignment_ptr = [&]() {
        return Arg(arg_index++);
    };
    auto bounded_view = [&](const std::size_t width) {
        const std::size_t available = input.size() - in;
        return input.substr(in, width == 0 ? available : std::min(width, available));
    };

    for (std::size_t fmt = 0; fmt < format.size();) {
        if (std::isspace(static_cast<unsigned char>(format[fmt]))) {
            while (fmt < format.size() && std::isspace(static_cast<unsigned char>(format[fmt]))) {
                ++fmt;
            }
            skip_input_space();
            continue;
        }

        if (format[fmt] != '%') {
            if (in >= input.size() || input[in] != format[fmt]) {
                return assigned;
            }
            ++fmt;
            ++in;
            continue;
        }

        ++fmt;
        if (fmt < format.size() && format[fmt] == '%') {
            if (in >= input.size() || input[in] != '%') {
                return assigned;
            }
            ++fmt;
            ++in;
            continue;
        }

        bool suppress = false;
        if (fmt < format.size() && format[fmt] == '*') {
            suppress = true;
            ++fmt;
        }

        std::size_t width = 0;
        while (fmt < format.size() && std::isdigit(static_cast<unsigned char>(format[fmt]))) {
            width = width * 10 + static_cast<std::size_t>(format[fmt++] - '0');
        }

        bool long_modifier = false;
        if (fmt < format.size() && (format[fmt] == 'h' || format[fmt] == 'l' || format[fmt] == 'L'
                || format[fmt] == 'z' || format[fmt] == 't' || format[fmt] == 'j')) {
            long_modifier = format[fmt] == 'l' || format[fmt] == 'L';
            ++fmt;
            if (fmt < format.size() && ((format[fmt - 1] == 'h' && format[fmt] == 'h') || (format[fmt - 1] == 'l' && format[fmt] == 'l'))) {
                ++fmt;
            }
        }

        if (fmt >= format.size()) {
            return assigned;
        }

        const char conv = format[fmt++];
        if (conv != 'c' && conv != '[' && conv != 'n') {
            skip_input_space();
        }

        if (conv == 'n') {
            if (!suppress) {
                memory_.Write32(read_assignment_ptr(), static_cast<u32>(in));
            }
            continue;
        }

        if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
            const std::string token = bounded_view(width);
            char* end = nullptr;
            const int base = conv == 'i' ? 0 : (conv == 'x' || conv == 'X' ? 16 : (conv == 'o' ? 8 : 10));
            errno = 0;
            const long value = (conv == 'u')
                ? static_cast<long>(std::strtoul(token.c_str(), &end, base))
                : std::strtol(token.c_str(), &end, base);
            const std::size_t consumed = static_cast<std::size_t>(end - token.c_str());
            if (consumed == 0) {
                return assigned;
            }
            in += consumed;
            if (!suppress) {
                memory_.Write32(read_assignment_ptr(), static_cast<u32>(value));
                ++assigned;
            }
            continue;
        }

        if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
            const std::string token = bounded_view(width);
            char* end = nullptr;
            errno = 0;
            const double value = std::strtod(token.c_str(), &end);
            const std::size_t consumed = static_cast<std::size_t>(end - token.c_str());
            if (consumed == 0) {
                return assigned;
            }
            in += consumed;
            if (!suppress) {
                const u32 out = read_assignment_ptr();
                if (long_modifier) {
                    memory_.Write64(out, BitCastToU64(value));
                } else {
                    memory_.Write32(out, FloatToBits(static_cast<float>(value)));
                }
                ++assigned;
            }
            continue;
        }

        if (conv == 's') {
            const std::size_t start = in;
            while (in < input.size()
                && !std::isspace(static_cast<unsigned char>(input[in]))
                && (width == 0 || in - start < width)) {
                ++in;
            }
            if (in == start) {
                return assigned;
            }
            if (!suppress) {
                const std::string token = input.substr(start, in - start);
                const u32 out = read_assignment_ptr();
                memory_.WriteBuffer(out, std::span<const u8>(reinterpret_cast<const u8*>(token.data()), token.size()));
                memory_.Write8(out + static_cast<u32>(token.size()), 0);
                ++assigned;
            }
            continue;
        }

        if (conv == 'c') {
            const std::size_t count = width == 0 ? 1 : width;
            if (input.size() - in < count) {
                return assigned;
            }
            if (!suppress) {
                const u32 out = read_assignment_ptr();
                memory_.WriteBuffer(out, std::span<const u8>(reinterpret_cast<const u8*>(input.data() + in), count));
                ++assigned;
            }
            in += count;
            continue;
        }

        if (conv == '[') {
            bool negate = false;
            if (fmt < format.size() && format[fmt] == '^') {
                negate = true;
                ++fmt;
            }

            std::array<bool, 256> accepted{};
            bool first = true;
            while (fmt < format.size()) {
                if (format[fmt] == ']' && !first) {
                    ++fmt;
                    break;
                }
                first = false;
                const unsigned char begin = static_cast<unsigned char>(format[fmt++]);
                if (fmt + 1 < format.size() && format[fmt] == '-' && format[fmt + 1] != ']') {
                    ++fmt;
                    const unsigned char end = static_cast<unsigned char>(format[fmt++]);
                    const unsigned char lo = std::min(begin, end);
                    const unsigned char hi = std::max(begin, end);
                    for (unsigned int c = lo; c <= hi; ++c) {
                        accepted[c] = true;
                    }
                } else {
                    accepted[begin] = true;
                }
            }

            const std::size_t start = in;
            while (in < input.size() && (width == 0 || in - start < width)) {
                const unsigned char c = static_cast<unsigned char>(input[in]);
                if (accepted[c] == negate) {
                    break;
                }
                ++in;
            }
            if (in == start) {
                return assigned;
            }
            if (!suppress) {
                const std::string token = input.substr(start, in - start);
                const u32 out = read_assignment_ptr();
                memory_.WriteBuffer(out, std::span<const u8>(reinterpret_cast<const u8*>(token.data()), token.size()));
                memory_.Write8(out + static_cast<u32>(token.size()), 0);
                ++assigned;
            }
            continue;
        }

        return assigned;
    }

    return assigned;
}

u32 Emulator::Arg(const std::size_t index) const {
    if (index < 4) {
        return cpu_->Regs()[index];
    }
    return memory_.Read32(cpu_->Regs()[13] + static_cast<u32>((index - 4) * 4));
}

void Emulator::SetReturnU32(const u32 value) {
    cpu_->Regs()[0] = value;
}

void Emulator::SetReturnU64(const u64 value) {
    cpu_->Regs()[0] = static_cast<u32>(value);
    cpu_->Regs()[1] = static_cast<u32>(value >> 32);
}

void Emulator::SetReturnDouble(const double value) {
    const u64 bits = BitCastToU64(value);
    SetReturnU64(bits);
}

std::string Emulator::ReadGuestCString(const u32 address) const {
    return memory_.ReadCString(address, kMaxGuestCString);
}

std::filesystem::path Emulator::ResolveGuestPath(const std::string& guest_path) const {
    if (guest_path.empty()) {
        return sandbox_root_;
    }

    const auto try_external = [&](const std::filesystem::path& relative) -> std::optional<std::filesystem::path> {
        if (external_root_.empty()) {
            return std::nullopt;
        }
        const std::array<std::filesystem::path, 2> candidates{
            external_root_ / relative,
            external_root_ / "res" / relative,
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
        }
        return std::nullopt;
    };

    if (StartsWith(guest_path, guest_home_)) {
        const std::filesystem::path relative = std::filesystem::path(guest_path.substr(guest_home_.size())).relative_path();
        const std::string relative_text = relative.generic_string();
        if (StartsWith(relative_text, "res/") && !external_root_.empty()) {
            return external_root_ / relative;
        }
        if (StartsWith(relative_text, "Documents/")) {
            return (sandbox_root_ / "Documents") / relative.lexically_relative("Documents");
        }
        if (StartsWith(relative_text, "Library/")) {
            return (sandbox_root_ / "Library") / relative.lexically_relative("Library");
        }
        if (StartsWith(relative_text, "tmp/")) {
            return (sandbox_root_ / "tmp") / relative.lexically_relative("tmp");
        }
        if (auto external = try_external(relative)) {
            return *external;
        }
        return sandbox_root_ / relative;
    }
    if (StartsWith(guest_path, guest_tmp_)) {
        const auto relative = guest_path.substr(guest_tmp_.size());
        return (sandbox_root_ / "tmp") / std::filesystem::path(relative).relative_path();
    }
    if (auto external = try_external(std::filesystem::path(guest_path).relative_path())) {
        return *external;
    }
    if (StartsWith(guest_path, "res/") && !external_root_.empty()) {
        return external_root_ / std::filesystem::path(guest_path);
    }
    if (!guest_path.empty() && guest_path.front() == '/') {
        return sandbox_root_ / std::filesystem::path(guest_path).relative_path();
    }
    if (auto external = try_external(std::filesystem::path(guest_path))) {
        return *external;
    }
    return sandbox_root_ / guest_path;
}

std::optional<std::string> Emulator::ResolveGuestAssetPath(const std::string& guest_path) const {
    if (!asset_exists_ && !read_asset_) {
        return std::nullopt;
    }

    std::vector<std::string> candidates;
    auto add_candidate = [&](std::string path) {
        path = NormalizeGuestResourceKey(std::move(path), guest_home_);
        const std::string normalized_path = path;
        if (!path.empty() && std::find(candidates.begin(), candidates.end(), path) == candidates.end()) {
            candidates.push_back(std::move(path));
        }
        if (!normalized_path.empty() && !StartsWith(normalized_path, "res/")) {
            std::string with_res = "res/" + normalized_path;
            if (std::find(candidates.begin(), candidates.end(), with_res) == candidates.end()) {
                candidates.push_back(std::move(with_res));
            }
        }
        if (StartsWith(normalized_path, "res/")) {
            std::string without_res = normalized_path.substr(4);
            if (!without_res.empty() && std::find(candidates.begin(), candidates.end(), without_res) == candidates.end()) {
                candidates.push_back(std::move(without_res));
            }
        }
    };

    if (StartsWith(guest_path, guest_home_)) {
        add_candidate(guest_path.substr(guest_home_.size()));
    }
    add_candidate(guest_path);

    for (const auto& candidate : candidates) {
        if (asset_exists_ ? asset_exists_(candidate) : read_asset_(candidate).has_value()) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<u8>> Emulator::ReadGuestFileBytes(const std::string& guest_path) const {
    if (read_asset_) {
        if (const auto asset_path = ResolveGuestAssetPath(guest_path)) {
            return read_asset_(*asset_path);
        }
    }

    const auto host_path = ResolveGuestPath(guest_path);
    std::ifstream input(host_path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    return std::vector<u8>((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

bool Emulator::GuestPathExists(const std::string& guest_path) const {
    if (ResolveGuestAssetPath(guest_path)) {
        return true;
    }
    return std::filesystem::exists(ResolveGuestPath(guest_path));
}

void Emulator::Stop(const Dynarmic::HaltReason reason) {
    cpu_->HaltExecution(reason);
}

void Emulator::Log(const std::string& line) const {
#if defined(__ANDROID__)
    constexpr const char* kAndroidLogTag = "atrasis";
    constexpr const char* kAtrasisPrefix = "[atrasis] ";
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = line.find('\n', begin);
        const std::string_view chunk(line.data() + begin, (end == std::string::npos ? line.size() : end) - begin);
        const std::string prefixed = StartsWith(chunk, "[atrasis]") ? std::string(chunk) : kAtrasisPrefix + std::string(chunk);
        __android_log_print(ANDROID_LOG_INFO, kAndroidLogTag, "%s", prefixed.c_str());
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
#else
    std::fprintf(stderr, "%s\n", line.c_str());
#endif
}

void Emulator::SetErrno(const int value) {
    memory_.Write32(errno_address_, static_cast<u32>(value));
}
