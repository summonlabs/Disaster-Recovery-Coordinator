#include <array>
#include <string>
#include <vector>

#include "drc/canonical.hpp"
#include "drc/digest.hpp"
#include "drc/error.hpp"
#include "drc/fileio.hpp"
#include "drc/id.hpp"
#include "drc/model.hpp"
#include "drc/time.hpp"
#include "test.hpp"

using namespace drc;

DRC_TEST(unit_sha256_matches_published_vectors) {
    const auto empty = Sha256::hash(std::string_view{});
    DRC_REQUIRE_EQ(to_hex(empty),
                   std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
    const auto abc = Sha256::hash(std::string_view{"abc"});
    DRC_REQUIRE_EQ(to_hex(abc),
                   std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
    const auto multi = Sha256::hash(std::string_view{
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"});
    DRC_REQUIRE_EQ(to_hex(multi),
                   std::string{"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"});
    // One million 'a' characters: the long-message vector. It also exercises the
    // block buffering that an earlier implementation got wrong.
    const std::string million(1000000, 'a');
    const auto big = Sha256::hash(std::string_view{million});
    DRC_REQUIRE_EQ(to_hex(big),
                   std::string{"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"});
    Sha256 incremental;
    incremental.update(Bytes{reinterpret_cast<const std::uint8_t*>(million.data()), 3});
    incremental.update(Bytes{reinterpret_cast<const std::uint8_t*>(million.data()) + 3,
                             million.size() - 3});
    DRC_REQUIRE_EQ(to_hex(incremental.finish()), to_hex(big));
}

DRC_TEST(unit_crc32c_matches_the_reference_vector) {
    const std::string check = "123456789";
    const auto bytes = Bytes{reinterpret_cast<const std::uint8_t*>(check.data()), check.size()};
    DRC_REQUIRE_EQ(crc32c(bytes), 0xE3069283u);
    DRC_REQUIRE_EQ(crc32c(Bytes{}), 0u);
    const std::uint32_t first = crc32c_extend(0, Bytes{bytes.data(), 4});
    DRC_REQUIRE_EQ(crc32c_extend(first, Bytes{bytes.data() + 4, 5}), 0xE3069283u);
}

DRC_TEST(unit_digest_detects_a_single_bit_change) {
    const std::string a = "coordinator state";
    std::string b = a;
    b[3] = static_cast<char>(b[3] ^ 0x01);
    const Digest left = Digest::of(Bytes{reinterpret_cast<const std::uint8_t*>(a.data()), a.size()});
    const Digest right = Digest::of(Bytes{reinterpret_cast<const std::uint8_t*>(b.data()), b.size()});
    DRC_REQUIRE(left != right);
    DRC_REQUIRE(!(left < left));
    DRC_REQUIRE(Digest{}.is_zero());
}

DRC_TEST(unit_canonical_round_trip_is_positional) {
    canonical::Writer writer;
    writer.u8(0x12);
    writer.u16(0x1234);
    writer.u32(0x12345678);
    writer.u64(0x123456789ABCDEF0ull);
    writer.i64(-42);
    writer.boolean(true);
    writer.text("hello");
    writer.sequence_count(2);
    writer.u64(7);
    writer.u64(8);
    DRC_REQUIRE(writer.status().ok());

    canonical::Reader reader(writer.bytes());
    DRC_REQUIRE_EQ(reader.u8(), 0x12);
    DRC_REQUIRE_EQ(reader.u16(), 0x1234);
    DRC_REQUIRE_EQ(reader.u32(), 0x12345678u);
    DRC_REQUIRE_EQ(reader.u64(), 0x123456789ABCDEF0ull);
    DRC_REQUIRE_EQ(reader.i64(), -42);
    DRC_REQUIRE_EQ(reader.boolean(), true);
    DRC_REQUIRE_EQ(std::string{reader.text()}, std::string{"hello"});
    DRC_REQUIRE_EQ(reader.sequence_count(4), 2u);
    DRC_REQUIRE_EQ(reader.u64(), 7ull);
    DRC_REQUIRE_EQ(reader.u64(), 8ull);
    DRC_REQUIRE(reader.consumed_all());
}

DRC_TEST(unit_canonical_reads_are_bounds_checked) {
    // A length prefix that claims more bytes than the record holds: the reader
    // must refuse rather than read past the end of its buffer.
    canonical::Writer writer;
    writer.u32(64);
    writer.u8('a');
    writer.u8('b');
    DRC_REQUIRE(writer.status().ok());
    canonical::Reader reader(writer.bytes());
    const std::string_view text = reader.text();
    DRC_REQUIRE(reader.failed());
    DRC_REQUIRE_EQ(reader.status().code(), ErrorCode::Invalid);
    DRC_REQUIRE(text.empty());
    DRC_REQUIRE(!reader.consumed_all());

    canonical::Writer exact;
    exact.u32(3);
    exact.u8('a');
    exact.u8('b');
    exact.u8('c');
    canonical::Reader good(exact.bytes());
    DRC_REQUIRE_EQ(std::string{good.text()}, std::string{"abc"});
    DRC_REQUIRE(good.consumed_all());
}

DRC_TEST(unit_canonical_rejects_oversized_text_and_sequences) {
    canonical::Writer writer;
    const std::string oversized(canonical::kMaxTextBytes + 1, 'a');
    writer.text(oversized);
    DRC_REQUIRE(!writer.status().ok());
    DRC_REQUIRE_EQ(writer.status().code(), ErrorCode::OutOfRange);

    canonical::Writer counted;
    counted.sequence_count(canonical::kMaxSequenceEntries + 1);
    DRC_REQUIRE(!counted.status().ok());

    canonical::Writer payload;
    payload.u32(1000);
    canonical::Reader reader(payload.bytes());
    DRC_REQUIRE_EQ(reader.sequence_count(10), 0u);
    DRC_REQUIRE(reader.failed());
}

DRC_TEST(unit_canonical_rejects_a_non_boolean_byte) {
    canonical::Writer writer;
    writer.u8(2);
    canonical::Reader reader(writer.bytes());
    DRC_REQUIRE_EQ(reader.boolean(), false);
    DRC_REQUIRE(reader.failed());
}

DRC_TEST(unit_utf8_validation_rejects_hostile_sequences) {
    DRC_REQUIRE(is_valid_utf8("plain ascii"));
    DRC_REQUIRE(is_valid_utf8("caf\xC3\xA9"));
    DRC_REQUIRE(is_valid_utf8("\xE2\x82\xAC"));
    DRC_REQUIRE(is_valid_utf8("\xF0\x9F\x9A\x80"));
    DRC_REQUIRE(!is_valid_utf8("\xC0\xAF"));
    DRC_REQUIRE(!is_valid_utf8("\xED\xA0\x80"));
    DRC_REQUIRE(!is_valid_utf8("\xF5\x80\x80\x80"));
    DRC_REQUIRE(!is_valid_utf8("\xE2\x82"));
    DRC_REQUIRE(!is_valid_utf8("\xFF"));
    DRC_REQUIRE(!is_valid_text("bell\x07", true));
    DRC_REQUIRE(!is_valid_text("", false));
    DRC_REQUIRE(is_valid_text("", true));
    DRC_REQUIRE(is_valid_text("line\nbreak", true));
}

DRC_TEST(unit_leaf_name_validation_rejects_traversal_and_devices) {
    DRC_REQUIRE(fileio::validate_leaf_name("coordinator.journal").ok());
    DRC_REQUIRE(fileio::validate_leaf_name("journal-2026.tmp").ok());
    DRC_REQUIRE_ERR(fileio::validate_leaf_name(""), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name("."), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name(".."), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name("../escape"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name("sub/dir"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name("sub\\dir"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name("stream:ads"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name("CON"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name("con.txt"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name("LPT9.log"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name("trailing."), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name("trailing "), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name(std::string{"bell\x07"}), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(fileio::validate_leaf_name(std::string{"\xFF\xFE"}), ErrorCode::Invalid);
    DRC_REQUIRE(fileio::validate_leaf_name("COM0").ok());
    DRC_REQUIRE(fileio::validate_leaf_name("console").ok());
}

DRC_TEST(unit_counters_never_wrap_silently) {
    const Counter<SequenceTag> small{7};
    DRC_REQUIRE_OK(small.next());
    DRC_REQUIRE_EQ(small.next().value().value(), 8ull);
    const Counter<SequenceTag> top{UINT64_MAX};
    DRC_REQUIRE_ERR(top.next(), ErrorCode::Overflow);
    DRC_REQUIRE_ERR(Sequence::parse("0"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(Sequence::parse("12x"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(SiteId::parse(""), ErrorCode::Invalid);
    DRC_REQUIRE_OK(SiteId::parse("9"));
    DRC_REQUIRE_EQ(SiteId::parse("9").value().value(), 9ull);
}

DRC_TEST(unit_capacity_arithmetic_is_checked) {
    DRC_REQUIRE_EQ(checked_add_capacity(2, 3).value(), 5ull);
    DRC_REQUIRE_ERR(checked_add_capacity(UINT64_MAX, 1), ErrorCode::Overflow);
    DRC_REQUIRE_EQ(checked_sub_capacity(5, 3).value(), 2ull);
    DRC_REQUIRE_ERR(checked_sub_capacity(1, 2), ErrorCode::Overflow);
}

DRC_TEST(unit_timestamp_formatting_round_trips) {
    const UnixNanos value = 1767225600LL * kNanosPerSecond + 123456789LL;
    const std::string text = format_timestamp_utc(value);
    DRC_REQUIRE_EQ(text, std::string{"2026-01-01T00:00:00.123456789Z"});
    const Result<UnixNanos> parsed = parse_timestamp_utc(text);
    DRC_REQUIRE_OK(parsed);
    DRC_REQUIRE_EQ(parsed.value(), value);
    DRC_REQUIRE_ERR(parse_timestamp_utc("2026-01-01T00:00:00"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(parse_timestamp_utc("2026-13-01T00:00:00.000000000Z"), ErrorCode::Invalid);
    DRC_REQUIRE_ERR(parse_timestamp_utc("nonsense"), ErrorCode::Invalid);
}

DRC_TEST(unit_manual_clock_saturates_instead_of_wrapping) {
    ManualClock clock{10};
    clock.advance(-20);
    DRC_REQUIRE_EQ(clock.now_nanos(), -10);
    clock.set(INT64_MIN + 5);
    clock.advance(-10);
    DRC_REQUIRE_EQ(clock.now_nanos(), INT64_MIN);
    clock.set(INT64_MAX - 5);
    clock.advance(10);
    DRC_REQUIRE_EQ(clock.now_nanos(), INT64_MAX);
    DRC_REQUIRE_ERR(checked_add_nanos(INT64_MAX, 1), ErrorCode::Overflow);
    DRC_REQUIRE_ERR(checked_add_nanos(INT64_MIN, -1), ErrorCode::Overflow);
}

DRC_TEST(unit_freshness_distinguishes_every_case) {
    DRC_REQUIRE_EQ(evaluate_freshness(100, 0, 1, 1, 10), Freshness::Unknown);
    DRC_REQUIRE_EQ(evaluate_freshness(100, 90, 0, 1, 10), Freshness::Unknown);
    DRC_REQUIRE_EQ(evaluate_freshness(100, 95, 1, 1, 10), Freshness::Fresh);
    DRC_REQUIRE_EQ(evaluate_freshness(100, 89, 1, 1, 10), Freshness::Expired);
    DRC_REQUIRE_EQ(evaluate_freshness(100, 101, 1, 1, 10), Freshness::FutureDated);
    DRC_REQUIRE_EQ(evaluate_freshness(100, 99, 4, 5, 10), Freshness::ForeignEpoch);
    DRC_REQUIRE_EQ(evaluate_freshness(100, 99, 5, 5, -1), Freshness::Expired);
    DRC_REQUIRE(is_fresh(Freshness::Fresh));
    DRC_REQUIRE(!is_fresh(Freshness::ForeignEpoch));
}

DRC_TEST(unit_error_codes_have_stable_names) {
    DRC_REQUIRE_EQ(to_string(ErrorCode::Ok), std::string_view{"ok"});
    DRC_REQUIRE_EQ(to_string(ErrorCode::Stale), std::string_view{"stale"});
    DRC_REQUIRE_EQ(to_string(ErrorCode::Corrupt), std::string_view{"corrupt"});
    DRC_REQUIRE_EQ(to_string(ErrorCode::TornTail), std::string_view{"torn_tail"});
    DRC_REQUIRE_EQ(to_string(ErrorCode::Shutdown), std::string_view{"shutdown"});
    DRC_REQUIRE(is_corruption(ErrorCode::Corrupt));
    DRC_REQUIRE(is_corruption(ErrorCode::TornTail));
    DRC_REQUIRE(!is_corruption(ErrorCode::Stale));
    const Status status{ErrorCode::Conflict, "two claims disagree"};
    DRC_REQUIRE_EQ(status.to_string(), std::string{"conflict: two claims disagree"});
    DRC_REQUIRE(!status.ok());
    DRC_REQUIRE_EQ(status.code(), ErrorCode::Conflict);
}

DRC_TEST(unit_recovery_class_order_and_parsing) {
    const RecoveryClass safety = RecoveryClass::SafetyCritical;
    const RecoveryClass protected_class = RecoveryClass::Protected;
    const RecoveryClass essential = RecoveryClass::Essential;
    DRC_REQUIRE(safety < protected_class);
    DRC_REQUIRE(protected_class < essential);
    DRC_REQUIRE(is_protected_class(RecoveryClass::SafetyCritical));
    DRC_REQUIRE(is_protected_class(RecoveryClass::Protected));
    DRC_REQUIRE(!is_protected_class(RecoveryClass::Standard));
    DRC_REQUIRE_EQ(parse_recovery_class("safety_critical").value(), RecoveryClass::SafetyCritical);
    DRC_REQUIRE_ERR(parse_recovery_class("urgent"), ErrorCode::Invalid);
}

DRC_TEST(unit_policy_equality_covers_every_field) {
    RecoveryPolicy left;
    RecoveryPolicy right;
    DRC_REQUIRE(left == right);
    right.require_federation_evidence = true;
    DRC_REQUIRE(!(left == right));
    right.require_federation_evidence = false;
    right.required_readiness_checks.push_back("power");
    DRC_REQUIRE(!(left == right));
    right.required_readiness_checks.clear();
    right.evidence_freshness_window += 1;
    DRC_REQUIRE(!(left == right));
    right = left;
    right.max_step_attempts += 1;
    DRC_REQUIRE(!(left == right));
}
