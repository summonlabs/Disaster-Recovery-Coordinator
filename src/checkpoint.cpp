#include "drc/checkpoint.hpp"

#include <cstring>
#include <string>

#include "drc/fileio.hpp"

namespace drc::checkpoint {
namespace {

constexpr std::size_t kOffsetMagic = 0;
constexpr std::size_t kOffsetVersion = 4;
constexpr std::size_t kOffsetReserved = 6;
constexpr std::size_t kOffsetHeaderSize = 8;
constexpr std::size_t kOffsetHeaderCrc = 12;
constexpr std::size_t kOffsetEpoch = 16;
constexpr std::size_t kOffsetCoveredThrough = 24;
constexpr std::size_t kOffsetPayloadBytes = 32;
constexpr std::size_t kOffsetWrittenAt = 40;
constexpr std::size_t kOffsetStateDigest = 48;
constexpr std::size_t kOffsetPayloadDigest = 80;

void put_u16(std::uint8_t* out, std::uint16_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFu);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put_u32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value & 0xFFu);
    out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
    out[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
    out[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void put_u64(std::uint8_t* out, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu);
    }
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                      (static_cast<std::uint16_t>(in[1]) << 8));
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
}

[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* in) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(in[i]) << (8u * i);
    }
    return value;
}

}  // namespace

std::vector<std::uint8_t> encode_header(const Header& header) {
    std::vector<std::uint8_t> bytes(kHeaderBytes, 0);
    put_u32(bytes.data() + kOffsetMagic, kMagic);
    put_u16(bytes.data() + kOffsetVersion, header.format_version);
    put_u16(bytes.data() + kOffsetReserved, 0);
    put_u32(bytes.data() + kOffsetHeaderSize, static_cast<std::uint32_t>(kHeaderBytes));
    put_u32(bytes.data() + kOffsetHeaderCrc, 0);
    put_u64(bytes.data() + kOffsetEpoch, header.epoch.value());
    put_u64(bytes.data() + kOffsetCoveredThrough, header.covered_through.value());
    put_u64(bytes.data() + kOffsetPayloadBytes, header.payload_bytes);
    put_u64(bytes.data() + kOffsetWrittenAt, static_cast<std::uint64_t>(header.written_at));
    std::memcpy(bytes.data() + kOffsetStateDigest, header.state_digest.bytes().data(),
                header.state_digest.bytes().size());
    std::memcpy(bytes.data() + kOffsetPayloadDigest, header.payload_digest.bytes().data(),
                header.payload_digest.bytes().size());
    put_u32(bytes.data() + kOffsetHeaderCrc,
            crc32c(Bytes{bytes.data(), bytes.size()}));
    return bytes;
}

Result<Header> decode_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != kHeaderBytes) {
        return Status{ErrorCode::Invalid, "snapshot header has the wrong size"};
    }
    const std::uint8_t* header = bytes.data();
    if (get_u32(header + kOffsetMagic) != kMagic) {
        return Status{ErrorCode::Invalid, "snapshot magic mismatch"};
    }
    if (get_u16(header + kOffsetVersion) != kFormatVersion) {
        return Status{ErrorCode::FormatUnsupported, "snapshot format version is not supported"};
    }
    if (get_u16(header + kOffsetReserved) != 0) {
        return Status{ErrorCode::Invalid, "snapshot reserved field is not zero"};
    }
    if (get_u32(header + kOffsetHeaderSize) != kHeaderBytes) {
        return Status{ErrorCode::Invalid, "snapshot header size mismatch"};
    }
    std::vector<std::uint8_t> copy(bytes.begin(), bytes.end());
    const std::uint32_t stored_crc = get_u32(header + kOffsetHeaderCrc);
    put_u32(copy.data() + kOffsetHeaderCrc, 0);
    if (crc32c(Bytes{copy.data(), copy.size()}) != stored_crc) {
        return Status{ErrorCode::Corrupt, "snapshot header checksum mismatch"};
    }
    Header out;
    out.format_version = get_u16(header + kOffsetVersion);
    out.epoch = Epoch{get_u64(header + kOffsetEpoch)};
    out.covered_through = Sequence{get_u64(header + kOffsetCoveredThrough)};
    out.payload_bytes = get_u64(header + kOffsetPayloadBytes);
    out.written_at = static_cast<UnixNanos>(get_u64(header + kOffsetWrittenAt));
    Sha256::DigestBytes state{};
    Sha256::DigestBytes payload{};
    std::memcpy(state.data(), header + kOffsetStateDigest, state.size());
    std::memcpy(payload.data(), header + kOffsetPayloadDigest, payload.size());
    out.state_digest = Digest{state};
    out.payload_digest = Digest{payload};
    return out;
}

Status write_file(const std::string& path,
                  const Header& header,
                  std::span<const std::uint8_t> payload) {
    Header effective = header;
    effective.payload_bytes = payload.size();
    effective.payload_digest = Digest{Sha256::hash(payload)};
    const std::vector<std::uint8_t> encoded_header = encode_header(effective);
    std::vector<std::uint8_t> file;
    file.reserve(encoded_header.size() + payload.size());
    file.insert(file.end(), encoded_header.begin(), encoded_header.end());
    file.insert(file.end(), payload.begin(), payload.end());

    const std::string temporary = path + ".tmp";
    Status status = fileio::write_file_durable(temporary, std::span<const std::uint8_t>(file));
    if (!status.ok()) {
        (void)fileio::remove_file(temporary);
        return status;
    }
    // Verify by reading back before the snapshot is allowed to replace the
    // previous one: a snapshot that cannot be decoded is worse than an old one.
    Result<std::vector<std::uint8_t>> check = fileio::read_file(temporary, file.size() + 1);
    if (!check.ok()) {
        (void)fileio::remove_file(temporary);
        return check.status();
    }
    if (check.value() != file) {
        (void)fileio::remove_file(temporary);
        return Status{ErrorCode::Io, "snapshot read-back did not match what was written"};
    }
    status = fileio::atomic_replace(temporary, path);
    if (!status.ok()) {
        (void)fileio::remove_file(temporary);
        return status;
    }
    const std::size_t separator = path.find_last_of("/\\");
    const std::string directory =
        separator == std::string::npos ? std::string{"."} : path.substr(0, separator);
    return fileio::sync_directory(directory);
}

Result<Contents> read_file(const std::string& path, std::uint64_t max_bytes) {
    Result<std::vector<std::uint8_t>> bytes = fileio::read_file(path, max_bytes);
    if (!bytes.ok()) {
        return bytes.status();
    }
    if (bytes.value().size() < kHeaderBytes) {
        return Status{ErrorCode::Invalid, "snapshot is smaller than its header"};
    }
    const std::span<const std::uint8_t> all{bytes.value().data(), bytes.value().size()};
    Result<Header> header = decode_header(all.subspan(0, kHeaderBytes));
    if (!header.ok()) {
        return header.status();
    }
    if (header.value().payload_bytes != all.size() - kHeaderBytes) {
        return Status{ErrorCode::Corrupt, "snapshot payload length does not match its header"};
    }
    const std::span<const std::uint8_t> payload = all.subspan(kHeaderBytes);
    if (Digest{Sha256::hash(payload)} != header.value().payload_digest) {
        return Status{ErrorCode::Corrupt, "snapshot payload digest mismatch"};
    }
    Contents contents;
    contents.header = header.value();
    contents.payload.assign(payload.begin(), payload.end());
    return contents;
}

}  // namespace drc::checkpoint
