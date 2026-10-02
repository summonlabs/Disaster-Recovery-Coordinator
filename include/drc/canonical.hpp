#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "drc/error.hpp"

namespace drc::canonical {

// Bounds for decoded metadata. Every length in the format is validated against
// one of these before a single byte is allocated or indexed.
inline constexpr std::uint32_t kMaxTextBytes = 4096;
inline constexpr std::uint32_t kMaxBlobBytes = 1024u * 1024u;
inline constexpr std::uint32_t kMaxRecordBytes = 1024u * 1024u;
inline constexpr std::uint32_t kMaxSequenceEntries = 65536u;

// Little-endian, length-prefixed, self-describing-by-position encoding. It is
// the single encoding used for journal payloads, snapshots, and state digests,
// so two builds on two platforms produce identical bytes for identical state.
//
// Both Writer and Reader carry a sticky failure state. Accessors are total:
// after a failure they return a safe default and never read or index out of
// bounds. Callers must check status() (or failed()) before trusting a decode.
class Writer {
public:
    Writer() = default;

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void i64(std::int64_t value);
    void boolean(bool value);
    void text(std::string_view value);
    void blob(std::span<const std::uint8_t> value);
    void sequence_count(std::uint32_t count);

    [[nodiscard]] bool failed() const noexcept { return !status_.ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(buffer_); }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

private:
    void fail(ErrorCode code, std::string message);

    std::vector<std::uint8_t> buffer_;
    Status status_{};
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8();
    [[nodiscard]] std::uint16_t u16();
    [[nodiscard]] std::uint32_t u32();
    [[nodiscard]] std::uint64_t u64();
    [[nodiscard]] std::int64_t i64();
    [[nodiscard]] bool boolean();
    [[nodiscard]] std::string_view text();
    [[nodiscard]] std::span<const std::uint8_t> blob();
    // Reads a sequence count and rejects anything above max_entries, so a
    // hostile or damaged count can never drive an unbounded allocation.
    [[nodiscard]] std::uint32_t sequence_count(std::uint32_t max_entries);

    [[nodiscard]] bool at_end() const noexcept { return offset_ == bytes_.size(); }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

    [[nodiscard]] bool failed() const noexcept { return !status_.ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    // True only when every byte was consumed by a successful decode.
    [[nodiscard]] bool consumed_all() const noexcept { return status_.ok() && at_end(); }

private:
    void fail(ErrorCode code, std::string message);
    [[nodiscard]] bool require(std::size_t count);

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
    Status status_{};
};

// Canonical helpers used by state digests.
[[nodiscard]] inline std::vector<std::uint8_t> encode_bool(bool value) {
    Writer w;
    w.boolean(value);
    return w.take();
}

}  // namespace drc::canonical
