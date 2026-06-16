#pragma once

#include "common.h"

namespace CryptoCompat {

std::vector<u8> Digest(std::string_view algorithm, std::span<const u8> data);
std::vector<u8> Hmac(u32 common_crypto_algorithm, std::span<const u8> key, std::span<const u8> data);

}
