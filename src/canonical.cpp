#include "drc/canonical.hpp"

namespace drc::canonical {
namespace {

void append_le(std::vector<std::uint8_t>& buffer, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        buffer.push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu));
    }
}

}  // namespace

void Writer::fail(ErrorCode code, std::string message) {
    if (status_.ok()) {
        status_ = Status{code, std::move(message)};
    }
}

void Writer::u8(std::uint8_t value) {
    if (failed()) {
        return;
    }
    buffer_.push_back(value);
}

void Writer::u16(std::uint16_t value) {
    if (failed()) {
        return;
    }
    append_le(buffer_, value, 2);
}

void Writer::u32(std::uint32_t value) {
    if (failed()) {
        return;
    }
    append_le(buffer_, value, 4);
}

void Writer::u64(std::uint64_t value) {
    if (failed()) {
        return;
    }
    append_le(buffer_, value, 8);
}

void Writer::i64(std::int64_t value) {
    u64(static_cast<std::uint64_t>(value));
}

void Writer::boolean(bool value) {
    u8(value ? 1u : 0u);
}

void Writer::text(std::string_view value) {
    if (failed()) {
        return;
    }
    if (value.size() > kMaxTextBytes) {
        fail(ErrorCode::OutOfRange, "text exceeds the canonical maximum");
        return;
    }
    u32(static_cast<std::uint32_t>(value.size()));
    buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void Writer::blob(std::span<const std::uint8_t> value) {
    if (failed()) {
        return;
    }
    if (value.size() > kMaxBlobBytes) {
        fail(ErrorCode::OutOfRange, "blob exceeds the canonical maximum");
        return;
    }
    u32(static_cast<std::uint32_t>(value.size()));
    buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void Writer::sequence_count(std::uint32_t count) {
    if (count > kMaxSequenceEntries) {
        fail(ErrorCode::OutOfRange, "sequence exceeds the canonical maximum");
        return;
    }
    u32(count);
}

void Reader::fail(ErrorCode code, std::string message) {
    if (status_.ok()) {
        status_ = Status{code, std::move(message)};
    }
}

bool Reader::require(std::size_t count) {
    if (failed()) {
        return false;
    }
    if (count > remaining()) {
        fail(ErrorCode::Invalid, "canonical read past the end of the record");
        return false;
    }
    return true;
}

std::uint8_t Reader::u8() {
    if (!require(1)) {
        return 0;
    }
    return bytes_[offset_++];
}

std::uint16_t Reader::u16() {
    if (!require(2)) {
        return 0;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 2; ++i) {
        value |= static_cast<std::uint32_t>(bytes_[offset_ + i]) << (8u * i);
    }
    offset_ += 2;
    return static_cast<std::uint16_t>(value);
}

std::uint32_t Reader::u32() {
    if (!require(4)) {
        return 0;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes_[offset_ + i]) << (8u * i);
    }
    offset_ += 4;
    return value;
}

std::uint64_t Reader::u64() {
    if (!require(8)) {
        return 0;
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes_[offset_ + i]) << (8u * i);
    }
    offset_ += 8;
    return value;
}

std::int64_t Reader::i64() {
    return static_cast<std::int64_t>(u64());
}

bool Reader::boolean() {
    const std::uint8_t raw = u8();
    if (failed()) {
        return false;
    }
    if (raw > 1u) {
        fail(ErrorCode::Invalid, "canonical boolean is not 0 or 1");
        return false;
    }
    return raw == 1u;
}

std::string_view Reader::text() {
    const std::uint32_t length = u32();
    if (failed()) {
        return {};
    }
    if (length > kMaxTextBytes) {
        fail(ErrorCode::OutOfRange, "text length exceeds the canonical maximum");
        return {};
    }
    if (!require(length)) {
        return {};
    }
    const char* data = reinterpret_cast<const char*>(bytes_.data() + offset_);
    offset_ += length;
    return std::string_view{data, length};
}

std::span<const std::uint8_t> Reader::blob() {
    const std::uint32_t length = u32();
    if (failed()) {
        return {};
    }
    if (length > kMaxBlobBytes) {
        fail(ErrorCode::OutOfRange, "blob length exceeds the canonical maximum");
        return {};
    }
    if (!require(length)) {
        return {};
    }
    const std::span<const std::uint8_t> out{bytes_.data() + offset_, length};
    offset_ += length;
    return out;
}

std::uint32_t Reader::sequence_count(std::uint32_t max_entries) {
    const std::uint32_t count = u32();
    if (failed()) {
        return 0;
    }
    if (count > max_entries || count > kMaxSequenceEntries) {
        fail(ErrorCode::OutOfRange, "sequence count exceeds the accepted maximum");
        return 0;
    }
    return count;
}

}  // namespace drc::canonical
