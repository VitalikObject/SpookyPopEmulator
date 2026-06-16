#include "crypto_compat.h"

#include <algorithm>
#include <cstring>

#if defined(__APPLE__) && !defined(CRYPTO_COMPAT_FORCE_PORTABLE)
#define CRYPTO_COMPAT_USE_COMMONCRYPTO 1
#else
#define CRYPTO_COMPAT_USE_COMMONCRYPTO 0
#endif

#if CRYPTO_COMPAT_USE_COMMONCRYPTO
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#endif

namespace {

#if !CRYPTO_COMPAT_USE_COMMONCRYPTO

u32 LoadLe32(const u8* p) {
    return static_cast<u32>(p[0])
        | (static_cast<u32>(p[1]) << 8)
        | (static_cast<u32>(p[2]) << 16)
        | (static_cast<u32>(p[3]) << 24);
}

u32 LoadBe32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24)
        | (static_cast<u32>(p[1]) << 16)
        | (static_cast<u32>(p[2]) << 8)
        | static_cast<u32>(p[3]);
}

void StoreLe32(std::vector<u8>& out, const u32 value) {
    out.push_back(static_cast<u8>(value & 0xFF));
    out.push_back(static_cast<u8>((value >> 8) & 0xFF));
    out.push_back(static_cast<u8>((value >> 16) & 0xFF));
    out.push_back(static_cast<u8>((value >> 24) & 0xFF));
}

void StoreBe32(std::vector<u8>& out, const u32 value) {
    out.push_back(static_cast<u8>((value >> 24) & 0xFF));
    out.push_back(static_cast<u8>((value >> 16) & 0xFF));
    out.push_back(static_cast<u8>((value >> 8) & 0xFF));
    out.push_back(static_cast<u8>(value & 0xFF));
}

std::vector<u8> Md5(std::span<const u8> input) {
    static constexpr std::array<u32, 64> k = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static constexpr std::array<u32, 64> s = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };

    std::vector<u8> data(input.begin(), input.end());
    const u64 bit_len = static_cast<u64>(data.size()) * 8;
    data.push_back(0x80);
    while ((data.size() % 64) != 56) {
        data.push_back(0);
    }
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<u8>((bit_len >> (i * 8)) & 0xFF));
    }

    u32 a0 = 0x67452301;
    u32 b0 = 0xefcdab89;
    u32 c0 = 0x98badcfe;
    u32 d0 = 0x10325476;

    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::array<u32, 16> m{};
        for (std::size_t i = 0; i < m.size(); ++i) {
            m[i] = LoadLe32(&data[offset + i * 4]);
        }

        u32 a = a0;
        u32 b = b0;
        u32 c = c0;
        u32 d = d0;
        for (u32 i = 0; i < 64; ++i) {
            u32 f = 0;
            u32 g = 0;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5 * i + 1) & 15;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) & 15;
            } else {
                f = c ^ (b | (~d));
                g = (7 * i) & 15;
            }
            const u32 temp = d;
            d = c;
            c = b;
            b += std::rotl(a + f + k[i] + m[g], s[i]);
            a = temp;
        }

        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    std::vector<u8> out;
    out.reserve(16);
    StoreLe32(out, a0);
    StoreLe32(out, b0);
    StoreLe32(out, c0);
    StoreLe32(out, d0);
    return out;
}

std::vector<u8> Sha1(std::span<const u8> input) {
    std::vector<u8> data(input.begin(), input.end());
    const u64 bit_len = static_cast<u64>(data.size()) * 8;
    data.push_back(0x80);
    while ((data.size() % 64) != 56) {
        data.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<u8>((bit_len >> (i * 8)) & 0xFF));
    }

    u32 h0 = 0x67452301;
    u32 h1 = 0xefcdab89;
    u32 h2 = 0x98badcfe;
    u32 h3 = 0x10325476;
    u32 h4 = 0xc3d2e1f0;

    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::array<u32, 80> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = LoadBe32(&data[offset + i * 4]);
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = std::rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        u32 a = h0;
        u32 b = h1;
        u32 c = h2;
        u32 d = h3;
        u32 e = h4;
        for (std::size_t i = 0; i < 80; ++i) {
            u32 f = 0;
            u32 k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const u32 temp = std::rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = std::rotl(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::vector<u8> out;
    out.reserve(20);
    StoreBe32(out, h0);
    StoreBe32(out, h1);
    StoreBe32(out, h2);
    StoreBe32(out, h3);
    StoreBe32(out, h4);
    return out;
}

std::vector<u8> Sha256(std::span<const u8> input) {
    static constexpr std::array<u32, 64> k = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    std::vector<u8> data(input.begin(), input.end());
    const u64 bit_len = static_cast<u64>(data.size()) * 8;
    data.push_back(0x80);
    while ((data.size() % 64) != 56) {
        data.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        data.push_back(static_cast<u8>((bit_len >> (i * 8)) & 0xFF));
    }

    u32 h0 = 0x6a09e667;
    u32 h1 = 0xbb67ae85;
    u32 h2 = 0x3c6ef372;
    u32 h3 = 0xa54ff53a;
    u32 h4 = 0x510e527f;
    u32 h5 = 0x9b05688c;
    u32 h6 = 0x1f83d9ab;
    u32 h7 = 0x5be0cd19;

    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::array<u32, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = LoadBe32(&data[offset + i * 4]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const u32 s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        u32 a = h0;
        u32 b = h1;
        u32 c = h2;
        u32 d = h3;
        u32 e = h4;
        u32 f = h5;
        u32 g = h6;
        u32 h = h7;
        for (std::size_t i = 0; i < 64; ++i) {
            const u32 s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const u32 ch = (e & f) ^ ((~e) & g);
            const u32 temp1 = h + s1 + ch + k[i] + w[i];
            const u32 s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            const u32 temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }

    std::vector<u8> out;
    out.reserve(32);
    StoreBe32(out, h0);
    StoreBe32(out, h1);
    StoreBe32(out, h2);
    StoreBe32(out, h3);
    StoreBe32(out, h4);
    StoreBe32(out, h5);
    StoreBe32(out, h6);
    StoreBe32(out, h7);
    return out;
}

#endif

#if !CRYPTO_COMPAT_USE_COMMONCRYPTO
std::string_view HmacDigestName(const u32 common_crypto_algorithm) {
    switch (common_crypto_algorithm) {
    case 1:
        return "MD5";
    case 2:
        return "SHA256";
    default:
        return "SHA1";
    }
}
#endif

} // namespace

namespace CryptoCompat {

std::vector<u8> Digest(const std::string_view algorithm, const std::span<const u8> data) {
#if CRYPTO_COMPAT_USE_COMMONCRYPTO
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (algorithm == "MD5") {
        std::vector<u8> out(CC_MD5_DIGEST_LENGTH);
        CC_MD5(data.data(), static_cast<CC_LONG>(data.size()), out.data());
        return out;
    }
    if (algorithm == "SHA1") {
        std::vector<u8> out(CC_SHA1_DIGEST_LENGTH);
        CC_SHA1(data.data(), static_cast<CC_LONG>(data.size()), out.data());
        return out;
    }
    std::vector<u8> out(CC_SHA256_DIGEST_LENGTH);
    CC_SHA256(data.data(), static_cast<CC_LONG>(data.size()), out.data());
    return out;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#else
    if (algorithm == "MD5") {
        return Md5(data);
    }
    if (algorithm == "SHA1") {
        return Sha1(data);
    }
    return Sha256(data);
#endif
}

std::vector<u8> Hmac(const u32 common_crypto_algorithm, const std::span<const u8> key, const std::span<const u8> data) {
#if CRYPTO_COMPAT_USE_COMMONCRYPTO
    CCHmacAlgorithm host_algorithm = kCCHmacAlgSHA1;
    std::size_t size = CC_SHA1_DIGEST_LENGTH;
    switch (common_crypto_algorithm) {
    case 1:
        host_algorithm = kCCHmacAlgMD5;
        size = CC_MD5_DIGEST_LENGTH;
        break;
    case 2:
        host_algorithm = kCCHmacAlgSHA256;
        size = CC_SHA256_DIGEST_LENGTH;
        break;
    default:
        host_algorithm = kCCHmacAlgSHA1;
        size = CC_SHA1_DIGEST_LENGTH;
        break;
    }
    std::vector<u8> out(size);
    CCHmac(host_algorithm, key.data(), key.size(), data.data(), data.size(), out.data());
    return out;
#else
    constexpr std::size_t kBlockSize = 64;
    const std::string_view digest_name = HmacDigestName(common_crypto_algorithm);
    std::vector<u8> normalized_key(key.begin(), key.end());
    if (normalized_key.size() > kBlockSize) {
        normalized_key = Digest(digest_name, normalized_key);
    }
    normalized_key.resize(kBlockSize, 0);

    std::vector<u8> inner(kBlockSize + data.size());
    std::vector<u8> outer(kBlockSize);
    for (std::size_t i = 0; i < kBlockSize; ++i) {
        inner[i] = normalized_key[i] ^ 0x36;
        outer[i] = normalized_key[i] ^ 0x5c;
    }
    std::copy(data.begin(), data.end(), inner.begin() + kBlockSize);

    const std::vector<u8> inner_digest = Digest(digest_name, inner);
    outer.insert(outer.end(), inner_digest.begin(), inner_digest.end());
    return Digest(digest_name, outer);
#endif
}

} // namespace CryptoCompat
