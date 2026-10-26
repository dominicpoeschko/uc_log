#pragma once

// Two words a reset keeps, for the printer: the RTT epoch and the count of boots that logged.
//
// The epoch (word 0) changes twice at every boot, around the (re)initialisation of the RTT control block: odd before
// it (Kvasir Startup calls the backend's preMemoryInit() ahead of the .data copy that writes the block), even after it
// (postMemoryInit()). Why: the printer reads an up ring's offsets, then its bytes, then writes the read offset back. A
// reset nobody announced (SYSRESETREQ, a watchdog) keeps RAM; if the new boot re-initialises the block between the
// printer's read and its write, the old read offset lands in the NEW block - and the printer then reads from there
// through the old boot's bytes still in the ring: the previous run's lines a second time. With the epoch read after
// each pass, the printer knows when a re-init fell inside it (printer/DirectRtt.hpp): it drops what it read, puts the
// read offset back to 0 (where the new block starts) and begins a new session. And while the epoch is odd the block in
// RAM is still the ended boot's: the printer opens no session then - one opened there took the moved epoch for a new
// block and read on from the old block's read offset, the middle of a record ("corrupted data removed").
//
// The logged count (word 1) moves once per boot that wrote a record: bits 31:16 the count, bits 15:0 the epoch's low
// half of the boot that last moved it - which is how a boot sees it was counted already. The printer counts the boots
// nobody read with it (printer/BootLedger.hpp); the epoch cannot do that, it also moves for a start that never logs (a
// debugger parking the core before main).
//
// Both sit in a section of their own, first in .noInitLowRam - the start of RAM in every image built for a chip, so
// another image of the same board has them at the same address and the count carries over a reflash. The printer
// finds them by name in the map (Symbol); an image without them gets neither protection.

#include <cstdint>
#include <optional>
#include <string_view>

extern "C" {
// gnu::used: nothing in the image reads it but this; .noInitLowRam: a reset keeps it, so each boot moves it on
// [0] the epoch, [1] the logged count with its mark, [2] the complement of [1]: RAM a reset keeps is not always kept -
// on the RP2350 a watchdog reboot that resets the SRAM stages (PSM.WDSEL) flips bits all over it, and J-Link's flash
// loader runs from the start of RAM - a count that does not match its complement starts again at 0, never read as one
[[gnu::used,
  gnu::section(
    ".noInitLowRam.uc_log_boot_words")]] inline std::uint32_t volatile uc_log_boot_words[3];
}

namespace uc_log::detail {
inline constexpr std::string_view rttEpochSymbol{"uc_log_boot_words"};
inline constexpr std::uint32_t    loggedBootsOffset{
  4};   // the logged count's word, after the epoch, then its complement

// Before .data is initialised: odd, and another value than before (also after a boot that died while it was odd).
// Plain code: no static of the image is set yet.
inline void epochBlockStale() { uc_log_boot_words[0] = (uc_log_boot_words[0] + 1U) | 1U; }

// After .data is initialised: even - the control block is this boot's.
inline void epochBlockReady() { uc_log_boot_words[0] = uc_log_boot_words[0] + 1U; }

/// Whether an epoch says the block in RAM is a finished boot's (even) or one being re-initialised (odd).
constexpr bool epochBlockIsReady(std::uint32_t const epoch) { return (epoch & 1U) == 0; }

// The logged word a boot of epoch `epoch` leaves when its first record finds `logged`: counted once per boot.
//
// Only the epoch's low 16 bits mark the boot (its even value: the first record comes after postMemoryInit). Why the
// wrap of that mark is harmless: the epoch moves by two a boot (odd, then even; more after a boot that died in its
// init), so the mark is wrong only when it moved by a multiple of 2^16 since the boot that last logged - 32 768 starts
// in a row that never wrote a record (a debugger parking the core before main every time) - and then the one boot
// after them that logs goes uncounted: one boot short, never a boot too many.
// A power-on, or a download over RAM, leaves both words undefined (a 1 in 32 768 chance that the first boot finds its
// mark), and the printer counts across neither (BootLedger: unknown / restart).
constexpr std::uint32_t countedBoot(std::uint32_t const logged,
                                    std::uint32_t const epoch) {
    auto const mark = epoch & 0xFFFFU;
    if((logged & 0xFFFFU) == mark) { return logged; }   // this boot counted itself already
    return ((logged >> 16U) + 1U) << 16U
         | mark;   // the count wraps modulo 2^16, as the printer reads it
}

// The check word is the logged word's complement, with bit 0 flipped (restartMark) by the boot that started the count
// again: the printer learns from it that the count restarted here (a count of 1 after a 1 is no repeat then).
inline constexpr std::uint32_t restartMark = 1U;

// The logged word as it may be counted on: itself if the check word matches it (plain or marked), else nothing - the
// RAM was not kept (on the RP2350 a watchdog reboot loses it); the count then starts again.
constexpr std::optional<std::uint32_t> keptLogged(std::uint32_t const logged,
                                                  std::uint32_t const check) {
    if(check == ~logged || check == (~logged ^ restartMark)) { return logged; }
    return std::nullopt;
}

// Cold: once per boot. Both cores of a multicore image may get here for the same boot: each writes the same values
// (computed from the same words), and one that reads the other's write sees its mark and writes them again. A reset
// between the two stores leaves them unmatched: the next boot starts the count again (said, not miscounted).
[[gnu::noinline,
  gnu::cold]] inline void
countLoggedBoot() {
    auto const kept      = keptLogged(uc_log_boot_words[1], uc_log_boot_words[2]);
    auto const counted   = countedBoot(kept.value_or(0U), uc_log_boot_words[0]);
    uc_log_boot_words[1] = counted;
    uc_log_boot_words[2] = kept ? ~counted : ~counted ^ restartMark;
}

// The first record of a boot counts it; every later one is a load and a branch. In .bss: zero at every boot.
inline bool bootLogged{};

inline void noteRecord() {
    if(!bootLogged) [[unlikely]] {
        bootLogged = true;
        countLoggedBoot();
    }
}

// The count of boots that logged, bits 31:16 of the logged word: what uc_log::bootsThatLogged() (Runtime.hpp) reads
// through the RTT backend.
inline std::uint16_t loggedBootCount() {
    return static_cast<std::uint16_t>(uc_log_boot_words[1] >> 16U);
}
}   // namespace uc_log::detail
