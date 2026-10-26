#pragma once

// uc_log's stress stream: log records whose every value is a function of (producer, boot, sequence
// number), so a host checker can regenerate each line's exact text and tell a lost, torn, duplicated
// or wrong line from a right one (tests/stress: uc_log_log_stress_check).
//
// One header for both sides. UC_LOG_STRESS_SHAPES(X) lists the record shapes as
// X(shape, format, arguments...); the arguments are expressions of `p` (producer), `b` (boot),
// `s` (sequence number) and `v` (stress::Values of them). The firmware expands X into a UC_LOG_I
// per shape, the checker into fmt::format with the same format string - remote_fmt's round-trip
// tests hold the printer's text to fmt's for every type used here.
//
// What the shapes cover: every integer width and sign, bool, char, float, double, strings (ASCII
// slices of every length 0..71 - most records over the 32 staging bytes are several writes -, UTF-8
// words), ranges of u16 and u8, a reflected enum, optional, tuple, variant, three chrono durations,
// void*, format specs (#x, width, sign, precision, binary, align), a 12-argument record, a metric.

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <variant>

namespace uc_log::stress {

// remote_fmt sends an enum's raw value and the catalog its names (the type entry); the checker names it the same way
enum class Color : std::uint8_t { red, green, blue, cyan, magenta };

inline constexpr std::array<std::string_view, 5> colorNames{"red",
                                                            "green",
                                                            "blue",
                                                            "cyan",
                                                            "magenta"};

// The stress firmware's producers and the RTT up buffer each one writes (RttLog with a TwoCores layout and
// one ISR ring per core: core 0 thread, core 0 ISRs, core 1 thread, core 1 ISR).
inline constexpr unsigned numProducers = 6;

inline constexpr std::array<unsigned, numProducers> channelOf{0, 1, 1, 1, 2, 3};

inline constexpr unsigned numShapes = 12;

// a 32-bit mixer (the finaliser of murmur3's lowbias variant): every input bit moves every output bit
constexpr std::uint32_t mix(std::uint32_t x) {
    x ^= x >> 16U;
    x *= 0x7feb'352dU;
    x ^= x >> 15U;
    x *= 0x846c'a68bU;
    x ^= x >> 16U;
    return x;
}

// ASCII only, no '|' (the checker splits on it), no '\\' or control characters (the .rttlog escapes
// those); a slice may start and end anywhere
inline constexpr std::string_view asciiPool{
  "The quick brown fox, \"quoted\" and 'single', jumps over the lazy dog; 0123456789 "
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz {braces} [brackets] (parens) <>=+-*/%&^~"};

// whole UTF-8 words only: a slice through a code point would be escaped by the .rttlog
inline constexpr std::array<std::string_view, 6> utf8Words{"äöü", "€", "日本語", "✓ ok", "Ω≈π", ""};

inline constexpr std::array<std::uint16_t, 16> u16Table{0,
                                                        1,
                                                        2,
                                                        255,
                                                        256,
                                                        1000,
                                                        4095,
                                                        4096,
                                                        12345,
                                                        32767,
                                                        32768,
                                                        40000,
                                                        50000,
                                                        60000,
                                                        65534,
                                                        65535};

struct Values {
    std::uint32_t r0;
    std::uint32_t r1;
    std::uint32_t r2;
    std::uint32_t r3;

    std::uint8_t  u8;
    std::int8_t   i8;
    std::uint16_t u16;
    std::int16_t  i16;
    std::uint32_t u32;
    std::int32_t  i32;
    std::uint64_t u64;
    std::int64_t  i64;

    bool   flag;
    char   letter;
    float  f;   // k / 8: exact in binary, short in decimal
    double d;   // k / 16

    std::string_view ascii;
    std::string_view utf8;

    std::span<std::uint16_t const> u16s;   // 0..12 elements of u16Table
    std::array<std::uint8_t, 4>    bytes;

    Color                                  color;
    std::optional<std::uint32_t>           maybe;
    std::tuple<std::uint8_t, std::int16_t> pair;
    std::variant<std::uint32_t, float>     either;
    std::array<bool, 3>                    bits;

    std::chrono::microseconds us;
    std::chrono::milliseconds ms;
    std::chrono::nanoseconds  ns;
    std::uintptr_t            address;   // logged as void const*

    std::array<std::uint32_t, 12> twelve;
};

constexpr Values values(unsigned      producer,
                        std::uint32_t boot,
                        std::uint32_t seq) {
    std::uint32_t const seed = mix(seq ^ mix((producer << 24U) ^ boot ^ 0x5bd1'e995U));
    Values              v{};
    v.r0 = seed;
    v.r1 = mix(seed + 1U);
    v.r2 = mix(seed + 2U);
    v.r3 = mix(seed + 3U);

    v.u8  = static_cast<std::uint8_t>(v.r0);
    v.i8  = static_cast<std::int8_t>(static_cast<std::uint8_t>(v.r0 >> 8U));
    v.u16 = static_cast<std::uint16_t>(v.r1);
    v.i16 = static_cast<std::int16_t>(static_cast<std::uint16_t>(v.r1 >> 16U));
    v.u32 = v.r2;
    v.i32 = static_cast<std::int32_t>(v.r3);
    v.u64 = (std::uint64_t{v.r0} << 32U) | v.r1;
    v.i64 = static_cast<std::int64_t>((std::uint64_t{v.r2} << 32U) | v.r3);

    v.flag   = (v.r0 & 1U) != 0U;
    v.letter = static_cast<char>('A' + (v.r1 % 26U));
    v.f      = static_cast<float>(static_cast<std::int32_t>(v.r2 % 200'001U) - 100'000) / 8.0F;
    v.d      = static_cast<double>(static_cast<std::int32_t>(v.r3 % 2'000'001U) - 1'000'000) / 16.0;

    std::size_t const len = v.r0 % 72U;
    std::size_t const off = v.r1 % (asciiPool.size() - len + 1U);
    v.ascii               = asciiPool.substr(off, len);
    v.utf8                = utf8Words[v.r2 % utf8Words.size()];

    std::size_t const n = v.r3 % 13U;
    std::size_t const o = (v.r0 >> 4U) % (u16Table.size() - n + 1U);
    v.u16s              = std::span<std::uint16_t const>{u16Table}.subspan(o, n);
    v.bytes             = {static_cast<std::uint8_t>(v.r1 >> 24U),
                           static_cast<std::uint8_t>(v.r2 >> 24U),
                           static_cast<std::uint8_t>(v.r3 >> 24U),
                           static_cast<std::uint8_t>(v.r0 >> 24U)};

    v.color = static_cast<Color>(v.r2 % colorNames.size());
    if((v.r3 & 2U) != 0U) { v.maybe = v.r1 % 100'000U; }
    v.pair = {static_cast<std::uint8_t>(v.r2 >> 8U), static_cast<std::int16_t>(v.r3 >> 8U)};
    if((v.r0 & 4U) != 0U) {
        v.either = v.r3;
    } else {
        v.either = static_cast<float>(static_cast<std::int32_t>(v.r1 % 4001U) - 2000) / 4.0F;
    }
    v.bits = {(v.r1 & 1U) != 0U, (v.r2 & 1U) != 0U, (v.r3 & 1U) != 0U};

    v.us      = std::chrono::microseconds{static_cast<std::int64_t>(v.r1 % 1'000'000U)};
    v.ms      = std::chrono::milliseconds{-static_cast<std::int64_t>(v.r2 % 100'000U)};
    v.ns      = std::chrono::nanoseconds{static_cast<std::int64_t>(v.u64 >> 4U)};
    v.address = 0x2000'0000U + (v.r3 & 0x7'FFFCU);

    for(std::size_t i = 0; i < v.twelve.size(); ++i) {
        v.twelve[i] = mix(seed + 16U + static_cast<std::uint32_t>(i)) >> (i % 32U);
    }
    return v;
}

// A levels variant: shape k at level k % 6 (uc_log::LogLevel: trace, debug, info, warn,
// error, crit), so a compile-time floor removes whole shapes - the checker then expects exactly the shapes at or above
// the floor, at their level.
constexpr unsigned levelOf(unsigned shape) { return shape % 6U; }

// Records of producer 0 whose number is a multiple of 37 are logged from a deeply templated function (DeepSite in
// the stress firmware): the printer's function column for them is the abbreviated signature of that function, which the
// checker holds to its pinned text - the catalog's long-signature path end to end.
constexpr bool deepSite(unsigned      producer,
                        std::uint32_t seq) {
    return producer == 0 && seq % 37U == 0;
}

// The scale of the metric shape: the printer extracts the number, so keep it small and exact.
constexpr std::uint32_t metricValue(Values const& v) { return v.r0 % 1000U; }

}   // namespace uc_log::stress

// X(shape, format, arguments...). `p`, `b`, `s`, `v` are the producer, boot, sequence number and
// Values. Every format starts "LS|p|b|s|<shape letter>|". The metric shape (10) is not in here: its
// text has the metric's marker in it, so each side writes it out itself.
#define UC_LOG_STRESS_SHAPES(X)                                                 \
    X(0, "LS|{}|{}|{}|a|{}", p, b, s, v.u32)                                    \
    X(1,                                                                        \
      "LS|{}|{}|{}|i|{}|{}|{}|{}|{}|{}|{}|{}",                                  \
      p,                                                                        \
      b,                                                                        \
      s,                                                                        \
      v.u8,                                                                     \
      v.i8,                                                                     \
      v.u16,                                                                    \
      v.i16,                                                                    \
      v.u32,                                                                    \
      v.i32,                                                                    \
      v.u64,                                                                    \
      v.i64)                                                                    \
    X(2, "LS|{}|{}|{}|f|{}|{}|{}|{}", p, b, s, v.flag, v.letter, v.f, v.d)      \
    X(3, "LS|{}|{}|{}|s|{}|{}|end", p, b, s, v.ascii, v.utf8)                   \
    X(4, "LS|{}|{}|{}|r|{}|{}", p, b, s, v.u16s, v.bytes)                       \
    X(5, "LS|{}|{}|{}|e|{}|{}|{}", p, b, s, v.color, v.maybe, v.pair)           \
    X(6, "LS|{}|{}|{}|t|{}|{}|{}", p, b, s, v.us, v.ms, v.ns)                   \
    X(7, "LS|{}|{}|{}|p|{}", p, b, s, reinterpret_cast<void const*>(v.address)) \
    X(8,                                                                        \
      "LS|{}|{}|{}|x|{:#010x}|{:>8}|{:+}|{:.3f}|{:08b}|{:<6}|",                 \
      p,                                                                        \
      b,                                                                        \
      s,                                                                        \
      v.u32,                                                                    \
      v.u16,                                                                    \
      v.i32,                                                                    \
      v.f,                                                                      \
      v.u8,                                                                     \
      v.utf8)                                                                   \
    X(9,                                                                        \
      "LS|{}|{}|{}|l|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",                      \
      p,                                                                        \
      b,                                                                        \
      s,                                                                        \
      v.twelve[0],                                                              \
      v.twelve[1],                                                              \
      v.twelve[2],                                                              \
      v.twelve[3],                                                              \
      v.twelve[4],                                                              \
      v.twelve[5],                                                              \
      v.twelve[6],                                                              \
      v.twelve[7],                                                              \
      v.twelve[8],                                                              \
      v.twelve[9],                                                              \
      v.twelve[10],                                                             \
      v.twelve[11])                                                             \
    X(11, "LS|{}|{}|{}|v|{}|{}", p, b, s, v.either, v.bits)
