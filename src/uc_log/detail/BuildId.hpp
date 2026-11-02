#pragma once

// The running image's build id in RAM, for the printer's firmware check.
//
// The linker writes a hash of the image into it as a GNU build-id note (Kvasir_SDK: --build-id=sha1, the note at
// `_LINKER_build_id_` in flash). At every boot the RTT backend's postMemoryInit() copies the hash's first 8 bytes into
// the boot words (RttEpoch.hpp, [3] and [4]) right before the epoch turns even, each word XOR that even epoch. At a
// session start the printer reads the boot words and compares the copy, XOR the epoch it reads, with the note's bytes
// in the hex file it decodes with: equal, and the board runs that image - without a read of the flash, which on an RP
// chip goes through XIP and can meet a boot ROM driving the flash. Different (another image, a boot not that far yet,
// RAM a flash loader overwrote): the printer compares the flash as before.
//
// Why in the boot words, bound to the epoch: RAM survives a reflash. An id at an address of its own stayed there when
// the next image did not use that address (a gcc build of another example on the same board), and the printer took
// it for a match and decoded the earlier image's RTT block with the wrong strings. The boot words are at the start of RAM
// in every image of a chip and every uc_log boot moves the epoch, so an id left by an earlier boot or another image
// no longer matches the epoch read with it.

#include "RttEpoch.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uc_log::Detail {
// The note's words: name size, hash size, type, name, then the hash (its first 8 bytes here).
struct BuildIdNote {
    std::uint32_t nameSize;
    std::uint32_t hashSize;
    std::uint32_t type;
    std::uint32_t name;
    std::uint32_t hash[2];
};
}   // namespace uc_log::Detail

extern "C" {
// the note (Kvasir_SDK linker/common_text_body.inc.ld) under the linker's name; weak: an image linked without the
// symbol has a null address
[[gnu::weak]] extern uc_log::Detail::BuildIdNote const
  uc_log_build_id_note __asm__("_LINKER_build_id_");
}

namespace uc_log::Detail {
// where the copy starts in the boot words (RttEpoch.hpp)
inline constexpr std::uint32_t    buildIdWordsOffset{12};
inline constexpr std::string_view buildIdNoteSymbol{"_LINKER_build_id_"};

// A GNU build-id note: name size 4, hash size, type 3 (NT_GNU_BUILD_ID), "GNU\0", the hash from word 4 on
// (BuildIdNote).
inline constexpr std::uint32_t buildIdNoteType{3};
inline constexpr std::uint32_t buildIdNoteName{0x0055'4e47};   // "GNU\0", little endian
inline constexpr std::uint32_t buildIdHashAt{16};
inline constexpr std::uint32_t buildIdBytes{8};
static_assert(offsetof(BuildIdNote,
                       hash)
                == buildIdHashAt
              && sizeof(BuildIdNote::hash) == buildIdBytes);

// Whether the words at a note's start (name size, hash size, type, name) are a build-id note with a hash of at least
// buildIdBytes: the printer's check of the hex file's bytes there.
constexpr bool isBuildIdNote(std::uint32_t const nameSize,
                             std::uint32_t const hashSize,
                             std::uint32_t const type,
                             std::uint32_t const name) {
    return nameSize == 4 && hashSize >= buildIdBytes && type == buildIdNoteType
        && name == buildIdNoteName;
}

// Startup, after .data and .bss: the note's hash into RAM. Not checked here that a note is there: the printer checks
// the hex file's bytes at the same address, and without a note it compares the flash whatever RAM holds.
inline void copyBuildId() {
    if(&uc_log_build_id_note == nullptr) { return; }
    auto const ready = uc_log_boot_words[0] + 1U;   // the even value epochBlockReady() writes next
    uc_log_boot_words[3] = uc_log_build_id_note.hash[0] ^ ready;
    uc_log_boot_words[4] = uc_log_build_id_note.hash[1] ^ ready;
}
}   // namespace uc_log::Detail
