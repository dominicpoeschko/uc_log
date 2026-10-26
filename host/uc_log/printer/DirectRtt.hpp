#pragma once

// RTT read by the printer itself, through plain memory accesses, instead of the J-Link DLL's own RTT.
//
// Why: the DLL's RTT polls the target in a thread of its own, copies each up ring into a host buffer and moves the
// target's read offset past what it copied - before the printer has the bytes. When a session ends (the printer
// stops, resets or flashes the target) SEGGER's stop drops that host buffer: bytes gone from the target, never
// decoded, no line saying so. A stress test found it: 5 of 5 printer restarts during a
// flood lost records without an announcement, up to a whole 4 KiB ring a time, and the bytes left in the target's
// RAM were overwritten before they could be read back.
//
// Here the read offset moves only past bytes the printer holds: read the ring's offsets, copy what is there, write
// the read offset back. A session that ends leaves every byte it did not take in the target's ring for the next one.
// Down buffers (the duplex channels): bytes written into the ring, then the write offset. The block's layout:
// printer/RttControlBlock.hpp.

#include "uc_log/detail/RttEpoch.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/RttControlBlock.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace uc_log::printer {

template<typename Transport>
class DirectRtt {
public:
    using Status     = typename Transport::Status;
    using BufferDesc = typename Transport::BufferDesc;

    struct Ring {
        std::uint32_t descriptor;   // its 24-byte descriptor
        std::uint32_t buffer;
        std::uint32_t size;
        std::uint32_t namePointer;
        std::uint32_t flags;
    };

    // The control block at `address` (the map file's), read once; nullopt if no "SEGGER RTT" block with sane
    // descriptors is there - the caller then uses the DLL's RTT.
    static std::optional<DirectRtt> open(Transport&                   jlink,
                                         std::uint32_t                address,
                                         std::uint32_t                totalBuffers,
                                         std::optional<std::uint32_t> epochAddress = std::nullopt) {
        if(address == 0 || totalBuffers == 0 || totalBuffers > maxBuffers) { return std::nullopt; }
        std::vector<std::byte> raw(rtt_block::bytesFor(totalBuffers));
        try {
            jlink.readMemory(address, raw);
        } catch(...) { return std::nullopt; }
        rtt_block::View const block{raw};
        auto const            numUp   = block.numUp();
        auto const            numDown = block.numDown();
        if(!block.hasId() || numUp + numDown != totalBuffers) { return std::nullopt; }
        DirectRtt rtt{address};
        rtt.status_.isRunning      = 1;
        rtt.status_.numUpBuffers   = static_cast<int>(numUp);
        rtt.status_.numDownBuffers = static_cast<int>(numDown);
        if(epochAddress) {
            try {
                rtt.epochAddress_ = epochAddress;
                rtt.sessionEpoch_ = rtt.settledEpoch(jlink, *epochAddress);
            } catch(...) { rtt.epochAddress_.reset(); }
        }
        for(std::uint32_t b = 0; b != totalBuffers; ++b) {
            auto const d = block.descriptor(b);
            if(!d.offsetsInRange()) { return std::nullopt; }
            (b < numUp ? rtt.up_ : rtt.down_)
              .push_back(
                Ring{address + rtt_block::descriptorAt(b), d.buffer, d.size, d.name, d.flags});
        }
        if(rtt.epochAddress_ && !uc_log::detail::epochBlockIsReady(rtt.sessionEpoch_)) {
            try {
                rtt.staleSkipped_ = rtt.skipUnread(jlink);
            } catch(...) { return std::nullopt; }
        }
        return rtt;
    }

    [[nodiscard]] Status started() const {
        Status s{};
        s.isRunning      = 1;
        s.numUpBuffers   = static_cast<int>(up_.size());
        s.numDownBuffers = static_cast<int>(down_.size());
        return s;
    }

    // How much of a ring one pass of readAll() takes. halfWhenShared: with more than one ring holding data, at most half
    // of each - a full big ring does not keep the probe from a small one for the whole time its bytes take (16 KiB
    // ~23 ms at 700 KB/s, while a 4 KiB ring flooded at that rate fills in ~6 ms); one ring alone is read whole. none:
    // every ring whole, up to its write offset, so each read ends on a record boundary - the drain at a session's end.
    enum class Cap { halfWhenShared, none };

    // One pass over the up rings: the header and all up descriptors in one read, then per ring with data its bytes,
    // its read offset and the RTT epoch. A boot since the session began drops the pass's bytes (old ones may be
    // torn, new ones belong to the next session), puts every read offset it wrote back to 0 and ends the pass. A pass
    // that reads nothing reads no epoch (the reader sees such a boot through DHCSR.S_RESET_ST). `outs[i]` receives
    // ring i's bytes; also refreshes status().
    //
    // The epoch after each offset write, not once a pass: a boot before an offset write leaves the old boot's offset
    // in the new block until the epoch is read, and a target logging then may wrap over its own unread first bytes.
    // Per ring that window is one probe round trip; a target that wraps inside it loses its first bytes unnoticed.

    std::vector<std::span<std::byte>> readAll(Transport&                            jlink,
                                              std::span<std::span<std::byte> const> outs,
                                              Cap cap = Cap::halfWhenShared) {
        std::vector<std::span<std::byte>> got(outs.size());
        for(std::size_t i = 0; i != got.size(); ++i) { got[i] = outs[i].first(0); }
        std::vector<std::byte> raw(rtt_block::bytesFor(static_cast<std::uint32_t>(up_.size())));
        jlink.readMemory(address_, raw);
        rtt_block::View const block{raw};
        bool const            there = block.hasId();
        status_.isRunning           = there ? 1 : 0;
        status_.numUpBuffers        = there ? static_cast<int>(block.numUp()) : 0;
        status_.numDownBuffers      = there ? static_cast<int>(block.numDown()) : 0;
        if(!there) { return got; }
        std::size_t const          rings = std::min(up_.size(), outs.size());
        std::vector<std::uint32_t> avails(rings);
        std::size_t                withData = 0;
        for(std::size_t i = 0; i != rings; ++i) {
            auto const& ring = up_[i];
            auto const  d    = block.descriptor(i);
            if(d.writeOffset >= ring.size || d.readOffset >= ring.size) {
                throw std::runtime_error{"RTT up ring " + std::to_string(i)
                                         + ": offsets out of range"};
            }
            avails[i] = rtt_block::filled(ring.size, d.writeOffset, d.readOffset);
            if(avails[i] != 0 && !outs[i].empty()) { ++withData; }
        }
        std::vector<std::size_t> written;
        for(std::size_t i = 0; i != rings; ++i) {
            auto const&         ring  = up_[i];
            auto const          rd    = block.descriptor(i).readOffset;
            std::uint32_t const avail = avails[i];
            std::uint32_t const most  = cap == Cap::halfWhenShared && withData > 1
                                        ? std::max<std::uint32_t>(ring.size / 2, 512)
                                        : ring.size;
            std::uint32_t const n
              = std::min({avail, most, static_cast<std::uint32_t>(outs[i].size())});
            if(n == 0) { continue; }
            std::uint32_t const first = std::min(n, ring.size - rd);
            try {
                jlink.readMemory(ring.buffer + rd, outs[i].first(first));
                if(first < n) { jlink.readMemory(ring.buffer, outs[i].subspan(first, n - first)); }
                jlink.writeWord(ring.descriptor + rtt_block::readOffsetField, (rd + n) % ring.size);
            } catch(...) {
                // The probe failed after earlier rings were read and their read offsets moved in the target: those
                // bytes exist only here now. They are returned; the failure waits in failure_ for the caller, which
                // hands them to the decoder first (takeFailure()). Nothing read yet: thrown as it is.
                if(written.empty()) { throw; }
                failure_ = std::current_exception();
                try {
                    dropIfReinit(jlink, written, got, outs);   // where the probe still answers
                } catch(...) { /* ignored: the first failure is the one kept (failure_) */
                }
                return got;
            }
            written.push_back(i);
            transferred_ += n;
            got[i] = outs[i].first(n);
            if(dropIfReinit(jlink, written, got, outs)) { return got; }
        }
        return got;
    }

    // what the last readAll() saw of the block (no access of its own)
    [[nodiscard]] Status status() const {
        Status s              = status_;
        s.numBytesTransferred = transferred_;
        s.numBytesRead        = transferred_;
        return s;
    }

    [[nodiscard]] std::size_t numUp() const { return up_.size(); }

    // the size of up ring `index` (0 for none)
    [[nodiscard]] std::uint32_t upSize(std::size_t index) const {
        return index < up_.size() ? up_[index].size : 0;
    }

    // A boot happened since the session started (detail/RttEpoch.hpp): the caller ends the session.
    [[nodiscard]] bool reinitSeen() const { return reinitSeen_; }

    // a probe failure readAll() kept after handing out what it had read (null: none); taken once
    std::exception_ptr takeFailure() { return std::exchange(failure_, nullptr); }

    // the RTT epoch this session began with (0 without an epoch symbol), and whether the image has one
    [[nodiscard]] std::uint32_t sessionEpoch() const { return sessionEpoch_; }

    /// The epoch stayed odd at open (settledEpoch): the bytes skipped then (skipUnread); nullopt: it settled.
    [[nodiscard]] std::optional<std::uint32_t> staleSkipped() const { return staleSkipped_; }

    [[nodiscard]] bool epochKnown() const { return epochAddress_.has_value(); }

private:
    // The epoch check after an offset write: a boot since the session began drops the pass's bytes and puts
    // its read offsets to 0; true then.
    bool dropIfReinit(Transport&                            jlink,
                      std::span<std::size_t const>          written,
                      std::span<std::span<std::byte>>       got,
                      std::span<std::span<std::byte> const> outs) {
        if(!epochAddress_ || written.empty() || epoch(jlink, *epochAddress_) == sessionEpoch_) {
            return false;
        }
        reinitSeen_ = true;
        for(auto const i : written) { got[i] = outs[i].first(0); }
        for(auto const i : written) {
            jlink.writeWord(up_[i].descriptor + rtt_block::readOffsetField, 0);
        }
        return true;
    }

    // The epoch once no boot is re-initialising the block (even, RttEpoch.hpp): a boot's .data copy takes microseconds
    // to milliseconds. Odd after epochSettle: a boot that died in its init - what the block holds is not known to be
    // this boot's (skipUnread).
    std::uint32_t settledEpoch(Transport&    jlink,
                               std::uint32_t address) {
        auto const deadline = Clock::now() + epochSettle;
        auto       e        = epoch(jlink, address);
        while(!uc_log::detail::epochBlockIsReady(e) && Clock::now() < deadline) {
            Clock::sleep_for(std::chrono::milliseconds{1});
            e = epoch(jlink, address);
        }
        return e;
    }

    // Every up ring's unread bytes skipped (read offset := write offset), never read: from the middle of a record they
    // would decode as damage. Their count.
    std::uint32_t skipUnread(Transport& jlink) {
        std::uint32_t skipped = 0;
        for(auto const& ring : up_) {
            std::array<std::byte, 8> offsets{};
            jlink.readMemory(ring.descriptor + rtt_block::writeOffsetField, offsets);
            auto const wr = le32(offsets, 0);
            auto const rd = le32(offsets, 4);
            if(wr >= ring.size || rd >= ring.size) { continue; }
            skipped += (wr + ring.size - rd) % ring.size;
            jlink.writeWord(ring.descriptor + rtt_block::readOffsetField, wr);
        }
        return skipped;
    }

    static std::uint32_t epoch(Transport&    jlink,
                               std::uint32_t address) {
        std::array<std::byte, 4> raw{};
        jlink.readMemory(address, raw);
        return le32(raw);
    }

public:
    // Into down ring `index`, as much as fits; returns the bytes taken.
    std::size_t write(Transport&                 jlink,
                      std::uint32_t              index,
                      std::span<std::byte const> data) {
        if(index >= down_.size() || data.empty()) { return 0; }
        auto const&              ring = down_[index];
        std::array<std::byte, 8> offsets{};
        jlink.readMemory(ring.descriptor + rtt_block::writeOffsetField, offsets);
        auto const wr = le32(offsets, 0);   // the host's
        auto const rd = le32(offsets, 4);   // the target's
        if(wr >= ring.size || rd >= ring.size) { return 0; }
        std::uint32_t const n = std::min<std::uint32_t>(rtt_block::space(ring.size, wr, rd),
                                                        static_cast<std::uint32_t>(data.size()));
        if(n == 0) { return 0; }
        std::uint32_t const first = std::min(n, ring.size - wr);
        jlink.writeMemory(ring.buffer + wr, data.first(first));
        if(first < n) { jlink.writeMemory(ring.buffer, data.subspan(first, n - first)); }
        jlink.writeWord(ring.descriptor + rtt_block::writeOffsetField, (wr + n) % ring.size);
        return n;
    }

    // The buffer's name from the target (a pointer in the descriptor, a NUL-terminated string).
    // `nameRead`: how the name is read - it usually lies in flash (the image's .rodata), and a read there through XIP
    // must wait while a boot ROM drives the flash (the reader's flashRead); empty: plainly
    std::optional<BufferDesc> bufferDesc(Transport&                                       jlink,
                                         bool                                             down,
                                         std::uint32_t                                    index,
                                         std::function<bool(std::uint32_t,
                                                            std::span<std::byte>)> const& nameRead
                                         = {}) {
        auto const& rings = down ? down_ : up_;
        if(index >= rings.size()) { return std::nullopt; }
        auto const& ring = rings[index];
        std::string name;
        if(ring.namePointer != 0) {
            std::array<std::byte, 32> raw{};
            if(nameRead) {
                if(!nameRead(ring.namePointer, raw)) { return std::nullopt; }
            } else {
                try {
                    jlink.readMemory(ring.namePointer, raw);
                } catch(...) { return std::nullopt; }
            }
            for(auto const b : raw) {
                if(b == std::byte{}) { break; }
                name += static_cast<char>(b);
            }
        }
        BufferDesc d{};
        d.name = std::move(name);
        if constexpr(requires { d.index; }) { d.index = index; }
        if constexpr(requires { d.isDown; }) { d.isDown = down; }
        if constexpr(requires { d.size; }) { d.size = ring.size; }
        if constexpr(requires { d.flags; }) { d.flags = ring.flags; }
        return d;
    }

private:
    static constexpr std::uint32_t maxBuffers = 32;

    explicit DirectRtt(std::uint32_t address) : address_{address} {}

    std::uint32_t                address_;
    std::vector<Ring>            up_;
    std::vector<Ring>            down_;
    std::uint32_t                transferred_{};
    static constexpr auto        epochSettle = std::chrono::milliseconds{200};
    std::optional<std::uint32_t> epochAddress_;
    std::uint32_t                sessionEpoch_{};
    std::optional<std::uint32_t> staleSkipped_;
    bool                         reinitSeen_{};
    std::exception_ptr           failure_{};
    Status                       status_{};
};

}   // namespace uc_log::printer
