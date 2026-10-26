#pragma once

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/MapSymbols.hpp"
#include "uc_log/printer/PanicRaise.hpp"
#include "uc_log/printer/TargetOp.hpp"
#include "uc_log/printer/TargetPort.hpp"
#include "uc_log/printer/TargetRecords.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// What a firmware leaves for a debugger, found in the linker map and read from the target: the crash records (fault,
/// stack snapshot, panic), the health and boot-guard records, the sanitizer counter, the painted stacks and the trace
/// rings - decoded by TargetRecords.hpp. The one place that knows where they are and how much to read: the TUI's
/// inspector and the control socket's `records` request both read through it.
///
/// Sizes come from the record types, never from the map: GNU ld shows no size for a symbol in the merged `.noInit`
/// input section. A fixed record reads its recordSize, lastStack<N> the size its N gives, a ring's storage what its
/// descriptor names (the map's size, where it shows one, must not be smaller). Large reads go in pieces
/// (`readRange`), so the RTT reads go on between them.
namespace uc_log::printer::records {

/// Where an image keeps them, by name in its map: found once per map parse.
struct Places {
    struct Stack {
        std::string   name;   // "core 0 stack", "core 1 stack"
        std::uint32_t low{};
        std::uint32_t high{};
    };

    struct Ring {
        std::string   symbol;   // the storage's name
        std::uint32_t storage{};
        std::uint32_t mapSize{};   // 0: the map shows none
        std::uint32_t descriptor{};
    };

    std::optional<std::uint32_t>                           fault;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> stack;   // lastStack<N>: address, N
    std::optional<std::uint32_t>                           panic;
    std::optional<std::uint32_t>                           starvation;
    std::optional<std::uint32_t>                           recoveries;
    std::optional<std::uint32_t>                           guard;
    std::optional<std::uint32_t>                           ubsan;
    std::optional<std::uint32_t> raise;   // Kvasir::Panic::raise(Kvasir::Panic::Cause)
    std::vector<Stack>           stacks;
    std::vector<Ring>            rings;

    static Places of(MapSymbols const& map) {
        Places     p;
        auto const at = [&](std::string_view name) -> std::optional<std::uint32_t> {
            if(auto const* s = map.find(name)) { return s->address; }
            return std::nullopt;
        };
        p.fault      = at(faultSymbol);
        p.panic      = at(panicSymbol);
        p.starvation = at(starvationSymbol);
        p.recoveries = at(recoveriesSymbol);
        p.guard      = at(guardSymbol);
        p.ubsan      = at(ubsanSymbol);
        p.raise      = at(panic_raise::raiseSymbol);
        // by entries(): GNU ld's demangled names with spaces (lastStack<(unsigned short)256>, a ring's template
        // arguments) are not among its symbols
        for(auto const& s : map.entries()) {
            if(auto const n = stackSnapshotBytes(s.name); n && !s.fromSection) {
                p.stack = std::pair{s.address, *n};
                break;
            }
        }
        // a core's stack: the linker's names, or their _LINKER_INTERN_ originals where the alias is a choice the map
        // gives no value for (core 1's stack may sit in a scratch bank)
        auto const stack = [&](std::string      name,
                               std::string_view lowName,
                               std::string_view lowAlt,
                               std::string_view highName,
                               std::string_view highAlt) {
            auto const low  = map.value(lowName).or_else([&] { return map.value(lowAlt); });
            auto const high = map.value(highName).or_else([&] { return map.value(highAlt); });
            if(low && high && *high > *low) { p.stacks.push_back({std::move(name), *low, *high}); }
        };
        stack("core 0 stack",
              "_LINKER_stack_start_",
              "_LINKER_INTERN_stack_start_",
              "_LINKER_stack_end_",
              "_LINKER_INTERN_stack_end_");
        stack("core 1 stack",
              "_LINKER_stack1_start_",
              "_LINKER_INTERN_stack1_start_",
              "_LINKER_stack1_end_",
              "_LINKER_INTERN_stack1_end_");
        // a ring is its constant `...::descriptor` next to its RAM `...::storage`; a storage alone is no ring
        std::map<std::string_view, std::uint32_t> descriptors;
        for(auto const& s : map.entries()) {
            if(!s.fromSection && s.name.ends_with(">::descriptor")
               && s.name.contains("Trace::Ring<"))
            {
                std::string_view const name{s.name};
                descriptors.emplace(
                  name.substr(0, name.size() - std::string_view{"descriptor"}.size()),
                  s.address);
            }
        }
        for(auto const& s : map.entries()) {
            if(!s.fromSection && s.name.ends_with(">::storage") && s.name.contains("Trace::Ring<"))
            {
                std::string_view const name{s.name};
                auto const             d = descriptors.find(
                  name.substr(0, name.size() - std::string_view{"storage"}.size()));
                if(d != descriptors.end()) {
                    p.rings.push_back({s.name, s.address, s.size, d->second});
                }
            }
        }
        return p;
    }
};

/// What a read asks for.
struct Kinds {
    bool crash{};    // fault, stack snapshot, panic
    bool health{};   // starvation, recoveries, boot guard
    bool ubsan{};
    bool stacks{};
    bool trace{};

    static constexpr Kinds all() { return {true, true, true, true, true}; }
};

/// One record the image has a symbol for.
template<typename T>
struct InImage {
    std::uint32_t address{};
    std::optional<T>
      record{};   // nullopt: its bytes are no record (never stored, taken, torn), or the read failed
    std::string error{};   // the read failed
};

struct StackRead {
    std::string                      name{};
    std::uint32_t                    low{};
    std::uint32_t                    high{};
    std::optional<records::StackUse> use{};
    bool                             read{};   // the bytes came; then no `use` = not painted
    std::string                      error{};
};

struct RingRead {
    std::string                   symbol{};
    std::uint32_t                 address{};   // the storage
    std::string                   name{};      // from its layout string
    std::vector<std::string>      fields{};
    std::uint32_t                 count{};
    std::uint32_t                 capacity{};
    std::uint32_t                 writtenWhileReading{};
    std::vector<records::RingRow> rows{};   // what was written while reading dropped
    std::string                   error{};
};

/// One read: a record absent here is one the image has no symbol for, or one not asked for.
struct Read {
    std::optional<InImage<Fault>>         fault{};
    std::optional<InImage<StackSnapshot>> stack{};
    std::optional<InImage<Panic>>         panic{};
    std::optional<InImage<Starvation>>    starvation{};
    std::optional<InImage<Recoveries>>    recoveries{};
    std::optional<InImage<GuardState>>    guard{};
    std::optional<InImage<Ubsan>>         ubsan{};
    std::vector<StackRead>                stacks{};
    std::vector<RingRead>                 rings{};
};

class RecordReader {
public:
    using Access = std::function<TargetPort::Result(std::vector<TargetOp> const&)>;

    static constexpr std::uint32_t pieceBytes = 4096;    // a control-socket read's limit
    static constexpr std::uint32_t callBytes  = 16384;   // per access

    explicit RecordReader(Access access UC_LOG_LIFETIMEBOUND) : access_{std::move(access)} {}

    /// Through a TargetPort, each access with `timeout`.
    static RecordReader through(TargetPort& port          UC_LOG_LIFETIMEBOUND,
                                std::chrono::milliseconds timeout) {
        return RecordReader{[&port, timeout](std::vector<TargetOp> const& ops) {
            return port.accessTarget(ops, timeout);
        }};
    }

    /// A new map: the descriptors and layouts read once per map are read again.
    void forget() {
        descriptors_.clear();
        layouts_.clear();
    }

    /// `size` bytes from `address`, in pieces and calls small enough to keep RTT flowing.
    std::expected<std::vector<std::byte>,
                  std::string>
    readRange(std::uint32_t          address,
              std::uint32_t          size,
              std::stop_token const& st) const {
        std::vector<std::byte> out;
        out.reserve(size);
        while(out.size() < size) {
            if(st.stop_requested()) { return std::unexpected{std::string{"stopping"}}; }
            std::vector<TargetOp> pieces;
            std::uint32_t         inCall = 0;
            auto                  at     = address + static_cast<std::uint32_t>(out.size());
            auto                  left   = size - static_cast<std::uint32_t>(out.size());
            while(left != 0 && inCall < callBytes) {
                auto const n = std::min({left, pieceBytes, callBytes - inCall});
                pieces.emplace_back(target_op::Read{at, n});
                at += n;
                left -= n;
                inCall += n;
            }
            auto r = access_(pieces);
            if(!r) { return std::unexpected{r.error()}; }
            for(auto const& p : *r) { out.insert(out.end(), p.begin(), p.end()); }
        }
        return out;
    }

    std::expected<std::uint32_t,
                  std::string>
    readWord(std::uint32_t address) const {
        auto r = access_({
          target_op::Read{address, 4}
        });
        if(!r) { return std::unexpected{r.error()}; }
        return word(r->front(), 0);
    }

    Read read(Places const&          places,
              Kinds const&           kinds,
              std::stop_token const& st) {
        Read out;
        readFixed(places, kinds, out);
        if(kinds.crash && places.stack) {
            auto const [address, bytes] = *places.stack;
            auto& slot = out.stack.emplace(InImage<StackSnapshot>{.address = address});
            if(auto raw = readRange(address, stackSnapshotSize(bytes), st)) {
                slot.record = decodeStackSnapshot(*raw);
            } else {
                slot.error = raw.error();
            }
        }
        if(kinds.stacks) {
            for(auto const& s : places.stacks) { out.stacks.push_back(readStack(s, st)); }
        }
        if(kinds.trace) {
            for(auto const& r : places.rings) { out.rings.push_back(readRing(r, st)); }
        }
        return out;
    }

private:
    // the fixed-size records, small: one access
    void readFixed(Places const& places,
                   Kinds const&  kinds,
                   Read&         out) const {
        std::vector<TargetOp>                                        reads;
        std::vector<std::function<void(std::span<std::byte const>)>> decoders;
        std::vector<std::function<void(std::string const&)>>         failures;
        auto const want = [&]<typename T>(std::optional<InImage<T>>&   slot,
                                          std::optional<std::uint32_t> at,
                                          std::uint32_t                size,
                                          auto                         decode) {
            if(!at) { return; }
            slot.emplace(InImage<T>{.address = *at});
            reads.emplace_back(target_op::Read{*at, size});
            decoders.emplace_back(
              [&slot, decode](std::span<std::byte const> raw) { slot->record = decode(raw); });
            failures.emplace_back([&slot](std::string const& why) { slot->error = why; });
        };
        if(kinds.crash) {
            want(out.fault, places.fault, faultSize, decodeFault);
            want(out.panic, places.panic, panicSize, decodePanic);
        }
        if(kinds.health) {
            want(out.starvation, places.starvation, recordSize<Starvation>, decodeStarvation);
            want(out.recoveries, places.recoveries, recordSize<Recoveries>, decodeRecoveries);
            want(out.guard, places.guard, recordSize<GuardState>, decodeGuard);
        }
        if(kinds.ubsan) { want(out.ubsan, places.ubsan, ubsanSize, decodeUbsan); }
        if(reads.empty()) { return; }
        auto r = access_(reads);
        for(std::size_t i = 0; i != reads.size(); ++i) {
            if(r && i < r->size()) {
                decoders[i]((*r)[i]);
            } else {
                failures[i](r ? std::string{"the read came back short"} : r.error());
            }
        }
    }

    [[nodiscard]] StackRead readStack(Places::Stack const&   s,
                                      std::stop_token const& st) const {
        StackRead out{.name = s.name, .low = s.low, .high = s.high};
        auto      raw = readRange(s.low, s.high - s.low, st);
        if(!raw) {
            out.error = raw.error();
            return out;
        }
        out.read = true;
        out.use  = stackUse(*raw);
        if(!out.use) { out.error = "not painted (no Kvasir::StackUsage for it)"; }
        return out;
    }

    RingRead readRing(Places::Ring const&    place,
                      std::stop_token const& st) {
        RingRead ring{.symbol = place.symbol, .address = place.storage};
        auto     descriptor = readDescriptor(place.descriptor, st);
        if(!descriptor) {
            ring.error = descriptor.error();
            return ring;
        }
        auto const size = ringStorageSize(*descriptor);
        if(!size) {
            ring.error = "no valid header (does the board run this build?)";
            return ring;
        }
        if(place.mapSize != 0 && place.mapSize < *size) {
            ring.error = "the map's size of the storage (" + std::to_string(place.mapSize)
                       + " bytes) is smaller than its descriptor's (" + std::to_string(*size) + ")";
            return ring;
        }
        auto raw = readRange(place.storage, *size, st);
        if(!raw) {
            ring.error = raw.error();
            return ring;
        }
        auto const h = decodeRing(*descriptor, *raw);
        if(!h) {
            ring.error = "no valid header (does the board run this build?)";
            return ring;
        }
        auto const again  = readWord(place.storage + traceCountAt);
        auto const layout = layoutAt(h->layout);
        ring.name         = layout.name;
        ring.fields       = layout.fields;
        ring.fields.resize(h->fields);
        ring.count               = h->count;
        ring.capacity            = h->capacity;
        ring.writtenWhileReading = again ? *again - h->count : 0;
        ring.rows                = decodeRingRows(*raw, *h, again.value_or(h->count));
        return ring;
    }

    /// A ring's descriptor is constant (flash): read once per map.
    std::expected<std::vector<std::byte>,
                  std::string>
    readDescriptor(std::uint32_t          address,
                   std::stop_token const& st) {
        if(auto const it = descriptors_.find(address); it != descriptors_.end()) {
            return it->second;
        }
        auto raw = readRange(address, traceDescriptorSize, st);
        if(!raw) { return std::unexpected{raw.error()}; }
        descriptors_.emplace(address, *raw);
        return std::move(*raw);
    }

    RingLayout layoutAt(std::uint32_t address) {
        if(auto const it = layouts_.find(address); it != layouts_.end()) { return it->second; }
        auto r = access_({
          target_op::Read{address, 256}
        });
        if(!r) { return {}; }
        auto layout = decodeRingLayout(r->front());
        layouts_.emplace(address, layout);
        return layout;
    }

    Access                                          access_;
    std::map<std::uint32_t, std::vector<std::byte>> descriptors_;
    std::map<std::uint32_t, RingLayout>             layouts_;
};

}   // namespace uc_log::printer::records
