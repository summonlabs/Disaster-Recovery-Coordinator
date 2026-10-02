#include "drc/digest.hpp"

#include <array>

namespace drc {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t amount) noexcept {
    return (value >> amount) | (value << (32u - amount));
}

[[nodiscard]] std::uint32_t load_be32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0x82F63B78u;
            } else {
                crc >>= 1;
            }
        }
        table[i] = crc;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

}  // namespace

void Sha256::reset() noexcept {
    state_ = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    buffer_.fill(0);
    buffered_ = 0;
    total_bytes_ = 0;
}

void Sha256::compress(const std::uint8_t* block) noexcept {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = load_be32(block + (i * 4));
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ ((~e) & g);
        const std::uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(Bytes data) noexcept {
    total_bytes_ += data.size();
    std::size_t offset = 0;
    // Copy in whole blocks. The earlier form could leave the buffer at 64
    // bytes without compressing, which is an out-of-bounds write on the next
    // byte; the loop below can never do that.
    while (offset < data.size()) {
        const std::size_t room = kBlockBytes - buffered_;
        const std::size_t left = data.size() - offset;
        const std::size_t take = room < left ? room : left;
        for (std::size_t i = 0; i < take; ++i) {
            buffer_[buffered_ + i] = data[offset + i];
        }
        buffered_ += take;
        offset += take;
        if (buffered_ == kBlockBytes) {
            compress(buffer_.data());
            buffered_ = 0;
        }
    }
}

Sha256::DigestBytes Sha256::finish() noexcept {
    const std::uint64_t bits = total_bytes_ * 8u;
    const std::uint8_t pad = 0x80;
    update(Bytes{&pad, 1});
    const std::uint8_t zero = 0x00;
    while (buffered_ != 56) {
        update(Bytes{&zero, 1});
    }
    std::array<std::uint8_t, 8> length{};
    for (std::size_t i = 0; i < 8; ++i) {
        length[7 - i] = static_cast<std::uint8_t>((bits >> (8u * i)) & 0xFFu);
    }
    update(Bytes{length.data(), length.size()});

    DigestBytes out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>((state_[i] >> 24) & 0xFFu);
        out[i * 4 + 1] = static_cast<std::uint8_t>((state_[i] >> 16) & 0xFFu);
        out[i * 4 + 2] = static_cast<std::uint8_t>((state_[i] >> 8) & 0xFFu);
        out[i * 4 + 3] = static_cast<std::uint8_t>(state_[i] & 0xFFu);
    }
    return out;
}

Sha256::DigestBytes Sha256::hash(Bytes data) noexcept {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish();
}

Sha256::DigestBytes Sha256::hash(std::string_view text) noexcept {
    return hash(Bytes{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

std::uint32_t crc32c_extend(std::uint32_t seed, Bytes data) noexcept {
    std::uint32_t crc = ~seed;
    for (const std::uint8_t byte : data) {
        crc = kCrc32cTable[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
    }
    return ~crc;
}

std::uint32_t crc32c(Bytes data) noexcept {
    return crc32c_extend(0, data);
}

std::string to_hex(Bytes bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[(byte >> 4) & 0x0Fu]);
        out.push_back(kDigits[byte & 0x0Fu]);
    }
    return out;
}

std::string to_hex(const std::array<std::uint8_t, 32>& bytes) {
    return to_hex(Bytes{bytes.data(), bytes.size()});
}

bool Digest::is_zero() const noexcept {
    for (const std::uint8_t byte : bytes_) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace drc
