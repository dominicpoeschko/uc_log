#pragma once
// A simulated target's RAM and the RTT control block in it, for the host tests of the printer's RTT reading.
//
//   FakeMemory    little-endian words in a byte store that stands for target memory at `base` (every target the
//                 printer reads is a little-endian Cortex-M); an access outside the store is a FAIL and does nothing
//   FakeRttBlock  a control block as rtt.hpp lays it out, on printer/RttControlBlock.hpp's constants, and the
//                 firmware's side of its rings: produce into an up ring, consume from a down ring, a boot's re-init
//
// The ring arithmetic is rtt_block::space()/filled(): a bug there would not show as a disagreement between this fake
// and the reader. direct_rtt_tests' controlBlockLayout checks the layout and the arithmetic with literal numbers and
// does not use this header: that is the guard.
//
// No locking: the owner holds its bus mutex around every call (the fake transports do).
#include "support/Check.hpp"
#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/RttControlBlock.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace uc_log::test {

class FakeMemory {
public:
    FakeMemory(std::span<std::byte> bytes UC_LOG_LIFETIMEBOUND,
               std::uint32_t              base)
      : bytes_{bytes}
      , base_{base} {}

    [[nodiscard]] bool contains(std::uint32_t address,
                                std::size_t   n) const {
        return address >= base_ && address - base_ <= bytes_.size()
            && n <= bytes_.size() - (address - base_);
    }

    void put32(std::uint32_t address,
               std::uint32_t value) {
        if(!inside(address, 4)) { return; }
        for(std::size_t i = 0; i != 4; ++i) {
            bytes_[address - base_ + i] = static_cast<std::byte>(value >> (8U * i));
        }
    }

    [[nodiscard]] std::uint32_t get32(std::uint32_t address) const {
        if(!inside(address, 4)) { return 0; }
        std::uint32_t v{};
        for(std::size_t i = 0; i != 4; ++i) {
            v |= std::to_integer<std::uint32_t>(bytes_[address - base_ + i]) << (8U * i);
        }
        return v;
    }

    void putByte(std::uint32_t address,
                 std::byte     value) {
        if(inside(address, 1)) { bytes_[address - base_] = value; }
    }

    [[nodiscard]] std::byte getByte(std::uint32_t address) const {
        return inside(address, 1) ? bytes_[address - base_] : std::byte{};
    }

    void putBytes(std::uint32_t              address,
                  std::span<std::byte const> data) {
        if(!inside(address, data.size())) { return; }
        for(std::size_t i = 0; i != data.size(); ++i) { bytes_[address - base_ + i] = data[i]; }
    }

private:
    [[nodiscard]] bool inside(std::uint32_t address,
                              std::size_t   n) const {
        if(contains(address, n)) { return true; }
        fail(__FILE__, __LINE__, "FakeMemory: an access outside the simulated RAM");
        return false;
    }

    std::span<std::byte> bytes_;
    std::uint32_t        base_;
};

/// One ring of a block: its buffer's address and size, and (optional) the address of its name and the name to put
/// there.
struct RingSpec {
    std::uint32_t    buffer;
    std::uint32_t    size;
    std::uint32_t    name     = 0;
    std::string_view nameText = {};
};

class FakeRttBlock {
public:
    FakeRttBlock(FakeMemory& memory        UC_LOG_LIFETIMEBOUND,
                 std::uint32_t             blockAddress,
                 std::span<RingSpec const> up,
                 std::span<RingSpec const> down = {})
      : memory_{memory}
      , block_{blockAddress}
      , numUp_{up.size()} {
        namespace rb = uc_log::printer::rtt_block;
        writeId();
        memory_.put32(block_ + 16, static_cast<std::uint32_t>(up.size()));
        memory_.put32(block_ + 20, static_cast<std::uint32_t>(down.size()));
        std::size_t index = 0;
        for(auto const rings : {up, down}) {
            for(RingSpec const& ring : rings) {
                auto const at = descriptorAddress(index++);
                memory_.put32(at, ring.name);
                memory_.put32(at + rb::bufferField, ring.buffer);
                memory_.put32(at + 8, ring.size);
                memory_.put32(at + rb::writeOffsetField, 0);
                memory_.put32(at + rb::readOffsetField, 0);
                memory_.put32(at + 20, 0);
                if(!ring.nameText.empty()) {
                    memory_.putBytes(ring.name, std::as_bytes(std::span{ring.nameText}));
                    memory_.putByte(ring.name + static_cast<std::uint32_t>(ring.nameText.size()),
                                    std::byte{});
                }
            }
        }
    }

    /// blockAddress + rtt_block::descriptorAt(index); up rings first, then the down rings.
    [[nodiscard]] std::uint32_t descriptorAddress(std::size_t index) const {
        return block_ + uc_log::printer::rtt_block::descriptorAt(index);
    }

    [[nodiscard]] std::uint32_t upDescriptor(std::size_t up) const { return descriptorAddress(up); }

    [[nodiscard]] std::uint32_t downDescriptor(std::size_t down) const {
        return descriptorAddress(numUp_ + down);
    }

    [[nodiscard]] std::uint32_t writeOffset(std::uint32_t descriptor) const {
        return memory_.get32(descriptor + uc_log::printer::rtt_block::writeOffsetField);
    }

    [[nodiscard]] std::uint32_t readOffset(std::uint32_t descriptor) const {
        return memory_.get32(descriptor + uc_log::printer::rtt_block::readOffsetField);
    }

    /// Bytes waiting in the ring behind `descriptor`.
    [[nodiscard]] std::uint32_t filled(std::uint32_t descriptor) const {
        return uc_log::printer::rtt_block::filled(sizeOf(descriptor),
                                                  writeOffset(descriptor),
                                                  readOffset(descriptor));
    }

    /// The firmware's side of up ring `up` in skip mode: all of `bytes` or nothing; the bytes first, then the write
    /// offset (rtt.hpp's order).
    bool produce(std::size_t                up,
                 std::span<std::byte const> bytes) {
        namespace rb      = uc_log::printer::rtt_block;
        auto const desc   = upDescriptor(up);
        auto const size   = sizeOf(desc);
        auto const wr     = writeOffset(desc);
        auto const buffer = memory_.get32(desc + rb::bufferField);
        if(size == 0 || rb::space(size, wr, readOffset(desc)) < bytes.size()) { return false; }
        auto pos = wr;
        for(auto const b : bytes) {
            memory_.putByte(buffer + pos, b);
            pos = (pos + 1) % size;
        }
        memory_.put32(desc + rb::writeOffsetField, pos);
        return true;
    }

    /// produce() until the record no longer fits; the number of records written.
    std::uint64_t fillWith(std::size_t                up,
                           std::span<std::byte const> record) {
        std::uint64_t n = 0;
        while(produce(up, record)) { ++n; }
        return n;
    }

    /// The firmware's side of down ring `down`: everything in it, the read offset moved past it.
    std::vector<std::byte> consume(std::size_t down) {
        namespace rb                  = uc_log::printer::rtt_block;
        auto const             desc   = downDescriptor(down);
        auto const             size   = sizeOf(desc);
        auto const             wr     = writeOffset(desc);
        auto const             buffer = memory_.get32(desc + rb::bufferField);
        auto                   rd     = readOffset(desc);
        std::vector<std::byte> out;
        while(rd != wr) {
            out.push_back(memory_.getByte(buffer + rd));
            rd = (rd + 1) % size;
        }
        memory_.put32(desc + rb::readOffsetField, rd);
        return out;
    }

    /// A boot's re-init (detail/RttEpoch.hpp): the epoch odd (preMemoryInit), every offset 0 (.data), the epoch even.
    void reinit(std::uint32_t epochAddress) {
        auto const odd = memory_.get32(epochAddress) | 1U;
        memory_.put32(epochAddress, odd);
        resetOffsets();
        memory_.put32(epochAddress, odd + 1);
    }

    /// Every ring's offsets 0, as the block's .data copy leaves them.
    void resetOffsets() {
        namespace rb = uc_log::printer::rtt_block;
        for(std::size_t i = 0; i != numUp_ + numDown(); ++i) {
            memory_.put32(descriptorAddress(i) + rb::writeOffsetField, 0);
            memory_.put32(descriptorAddress(i) + rb::readOffsetField, 0);
        }
    }

    /// The block vanishes (a boot ROM runs, a reset cleared RAM): its id is gone.
    void clearId() {
        for(std::uint32_t i = 0; i != 16; ++i) { memory_.putByte(block_ + i, std::byte{}); }
    }

    /// The id back ("SEGGER RTT" and NULs to 16 bytes).
    void writeId() {
        clearId();
        memory_.putBytes(block_, std::as_bytes(std::span{uc_log::printer::rtt_block::id}));
    }

private:
    [[nodiscard]] std::uint32_t sizeOf(std::uint32_t descriptor) const {
        return memory_.get32(descriptor + 8);
    }

    [[nodiscard]] std::size_t numDown() const { return memory_.get32(block_ + 20); }

    FakeMemory&   memory_;
    std::uint32_t block_;
    std::size_t   numUp_;
};
}   // namespace uc_log::test
