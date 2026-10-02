#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "drc/digest.hpp"
#include "drc/error.hpp"
#include "drc/id.hpp"
#include "drc/time.hpp"

namespace drc::checkpoint {

// A snapshot is a second durable artifact beside the journal. It exists so that
// recovery time stays bounded, and it is only ever read together with the
// journal records that follow the sequence it covers.
inline constexpr std::uint32_t kMagic = 0x43524444u;  // 'DDRC' little-endian
inline constexpr std::uint16_t kFormatVersion = 1;
inline constexpr std::size_t kHeaderBytes = 112;

struct Header {
    std::uint16_t format_version = kFormatVersion;
    Epoch epoch;
    Sequence covered_through;
    std::uint64_t payload_bytes = 0;
    UnixNanos written_at = 0;
    Digest state_digest;
    Digest payload_digest;
};

[[nodiscard]] std::vector<std::uint8_t> encode_header(const Header& header);
[[nodiscard]] Result<Header> decode_header(std::span<const std::uint8_t> bytes);

struct Contents {
    Header header;
    std::vector<std::uint8_t> payload;
};

// Writes the snapshot durably: temporary file, flush, verify by re-reading,
// atomic replace, then a directory sync. A failure at any point leaves the
// previous snapshot intact.
[[nodiscard]] Status write_file(const std::string& path,
                                const Header& header,
                                std::span<const std::uint8_t> payload);

[[nodiscard]] Result<Contents> read_file(const std::string& path, std::uint64_t max_bytes);

}  // namespace drc::checkpoint
