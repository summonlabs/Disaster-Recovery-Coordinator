#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace drc {

using Bytes = std::span<const std::uint8_t>;

// SHA-256 (FIPS 180-4), first-party and dependency-free. Used for the journal
// record chain, snapshot integrity, and canonical state digests.
class Sha256 {
public:
    static constexpr std::size_t kDigestBytes = 32;
    static constexpr std::size_t kBlockBytes = 64;
    using DigestBytes = std::array<std::uint8_t, kDigestBytes>;

    Sha256() noexcept { reset(); }
    void reset() noexcept;
    void update(Bytes data) noexcept;
    [[nodiscard]] DigestBytes finish() noexcept;

    [[nodiscard]] static DigestBytes hash(Bytes data) noexcept;
    [[nodiscard]] static DigestBytes hash(std::string_view text) noexcept;

private:
    void compress(const std::uint8_t* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, kBlockBytes> buffer_{};
    std::size_t buffered_ = 0;
    std::uint64_t total_bytes_ = 0;
};

// CRC-32C (Castagnoli). Cheap per-record integrity; it detects accidental
// damage, not tampering. The record chain carries the tamper evidence.
[[nodiscard]] std::uint32_t crc32c(Bytes data) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t seed, Bytes data) noexcept;

[[nodiscard]] std::string to_hex(Bytes bytes);
[[nodiscard]] std::string to_hex(const std::array<std::uint8_t, 32>& bytes);

// A canonical-state digest: the SHA-256 of the canonical encoding of the state.
class Digest {
public:
    Digest() = default;
    explicit Digest(Sha256::DigestBytes bytes) : bytes_(bytes) {}

    [[nodiscard]] static Digest of(Bytes canonical_bytes) {
        return Digest{Sha256::hash(canonical_bytes)};
    }

    [[nodiscard]] const Sha256::DigestBytes& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::string to_hex() const { return drc::to_hex(bytes_); }
    [[nodiscard]] bool is_zero() const noexcept;

    friend bool operator==(const Digest& a, const Digest& b) noexcept {
        return a.bytes_ == b.bytes_;
    }
    friend bool operator!=(const Digest& a, const Digest& b) noexcept {
        return !(a == b);
    }
    friend bool operator<(const Digest& a, const Digest& b) noexcept {
        return a.bytes_ < b.bytes_;
    }

private:
    Sha256::DigestBytes bytes_{};
};

}  // namespace drc
