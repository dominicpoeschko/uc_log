#pragma once

#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/CortexM.hpp"
#include "uc_log/printer/HaltCapture.hpp"
#include "uc_log/printer/MapSymbols.hpp"
#include "uc_log/printer/PanicRaise.hpp"
#include "uc_log/printer/RecordReader.hpp"
#include "uc_log/printer/SymbolMatch.hpp"
#include "uc_log/printer/TargetOp.hpp"
#include "uc_log/printer/TargetPort.hpp"
#include "uc_log/printer/TargetRecords.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

/// The TUI's view into the running target, the parts of kvasir_bench.py a printer can do alone:
/// the sanitizer counter (`ub`), the fault and panic records and the last halt (`crash`), the
/// painted stacks (`stack`), the trace rings (`trace`), watched variables (`peek --watch`), a
/// PC-sampling profile (`profile`) and raising a panic (`panic`). Symbols come from the linker map
/// (MapSymbols), the records through RecordReader, memory through Hooks::target (the reader's TargetPort) - from this
/// object's own thread, never the UI's, and in pieces small enough that the RTT reads go on between them.
namespace uc_log::printer {

class TargetInspector {
public:
    struct Hooks {
        // memory reads and writes, halt / registers / go (the reader's own ops: a DHCSR write behind the J-Link DLL's
        // back hangs it, and the reader refuses one), the link state and the session count; null: no target
        printer::TargetPort* target{};
        // the map's current parse (MapFacts::get()->map): a new pointer is a rebuilt map, its watches resolved again
        std::function<std::shared_ptr<MapSnapshot const>()> map;
        std::function<void()>                               changed;   // something to redraw
        std::function<void(std::string_view)>               note{};    // a line for the status log
    };

    /// What the UI shows; kept up to date by the worker, copied out by snapshot().
    struct Snapshot {
        std::string   mapError;   // empty: the map is read
        bool          lldMap{};
        std::size_t   symbolCount{};
        std::uint64_t mapGeneration{};   // counts the maps read: what was looked up in one is stale

        bool                          haveUbsan{};
        std::optional<records::Ubsan> ubsan;
        std::string                   ubsanWhere;

        bool                          haveFault{};
        std::optional<records::Fault> fault;
        std::string                   faultPc;
        std::string                   faultLr;

        bool                          havePanic{};
        std::optional<records::Panic> panic;
        std::string                   panicSite;

        struct HaltWord {
            std::uint32_t offset{};
            std::uint32_t value{};
            std::string   where;
        };

        struct Halt {
            std::chrono::system_clock::time_point time;
            uc_log::printer::HaltCapture          capture;
            std::string                           pcWhere{};
            std::string                           lrWhere{};
            std::vector<HaltWord> codeWords{};   // the stack words that point into code
        };

        std::optional<Halt> halt;

        struct Stack {
            std::string                        name;
            std::uint32_t                      low{};
            std::uint32_t                      high{};
            std::optional<records::StackUse>   use;
            std::string                        error;
            uc_log::printer::Clock::time_point measured{};
        };

        std::vector<Stack> stacks;

        struct Ring {
            std::string                   name;
            std::uint32_t                 address{};
            std::vector<std::string>      fields;
            std::uint32_t                 count{};
            std::uint32_t                 capacity{};
            std::uint32_t                 writtenWhileReading{};
            std::vector<records::RingRow> rows;
            std::string                   error;
        };

        std::vector<Ring> rings;

        struct Watch {
            std::string                        name;
            std::uint32_t                      address{};
            std::uint32_t                      size{};   // read
            std::uint32_t                      symbolSize{};
            std::vector<std::byte>             value;
            uc_log::printer::Clock::time_point changed{};   // the last change seen
            std::uint64_t                      changes{};
            std::string                        error;
        };

        std::vector<Watch> watches;

        struct ProfileRow {
            std::string   name;
            std::uint64_t samples{};
        };

        bool                    profiling{};
        std::uint64_t           profileSamples{};
        std::uint64_t           profileNone{};      // 0xFFFFFFFF: halted, or Secure code
        std::uint64_t           profileOutside{};   // in no function of the map
        double                  profileSeconds{};
        std::vector<ProfileRow> profile;
        std::string             profileError;

        std::string panicRaise;       // the last raisePanic()'s outcome
        std::string recordsCleared;   // the last clearRecords()'s outcome
    };

    enum class Page : std::uint8_t { none, health, trace, watch, profile };

    static constexpr std::uint32_t maxWatchBytes = 64;
    static constexpr auto          callTimeout   = std::chrono::milliseconds{1000};

    explicit TargetInspector(Hooks hooks)
      : hooks_{std::move(hooks)}
      , thread_{[this](std::stop_token const& st) { run(st); }} {}

    ~TargetInspector() {
        thread_.request_stop();
        wake_.notify_all();
    }

    TargetInspector(TargetInspector const&)            = delete;
    TargetInspector& operator=(TargetInspector const&) = delete;

    /// Bumped whenever the snapshot changed: copy it only then.
    std::uint64_t version() const { return version_.load(std::memory_order_acquire); }

    Snapshot snapshot() const {
        std::scoped_lock<std::mutex> const lock{mutex_};
        return state_;
    }

    void setPage(Page page) {
        if(page_.exchange(page) != page) { wake(); }
    }

    /// Read everything now (the Health tab's refresh button, a new tab shown).
    void refresh() {
        refreshAll_ = true;
        wake();
    }

    /// The reader's captured halt (MessageKind::halt).
    void noteHalt(uc_log::printer::HaltCapture const& capture) {
        Snapshot::Halt halt{.time = std::chrono::system_clock::now(), .capture = capture};
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            halt.pcWhere = describeLocked(capture.pc);
            halt.lrWhere = describeLocked(capture.lr);
            for(std::uint32_t offset = 0; auto const& v : capture.stack) {
                if(v) {
                    if(auto where = describeLocked(*v); !where.empty()) {
                        halt.codeWords.push_back({offset, *v, std::move(where)});
                    }
                }
                offset += 4;
            }
            state_.halt = std::move(halt);
        }
        recordsDue_ = true;
        wake();
        changed();
    }

    struct Completion {
        std::string   name;
        std::uint32_t address{};
        std::uint32_t size{};

        bool operator==(Completion const&) const = default;
    };

    struct Completions {
        std::vector<Completion> best;      // the likeliest first (SymbolQuery::rank)
        std::size_t             total{};   // data symbols that match at all
    };

    /// The data symbols `query` matches (SymbolQuery: terms, plain text or regex), the best
    /// `max` of them: by rank, then the shorter name.
    Completions completions(std::string const& query,
                            std::size_t        max) const {
        Completions       out;
        SymbolQuery const q{query};
        if(q.empty()) { return out; }
        std::scoped_lock<std::mutex> const lock{mutex_};

        struct Hit {
            unsigned         rank;
            MapSymbol const* symbol;
        };

        std::vector<Hit> hits;
        for(auto const& s : symbols_->all()) {
            if(s.code || s.size == 0 || isSiteTag(s.name)) { continue; }
            auto const rank = q.rank(s.name);
            // a name that repeats (static locals of inlined functions) is its first symbol
            if(rank && symbols_->find(s.name) == &s) { hits.push_back({*rank, &s}); }
        }
        out.total       = hits.size();
        auto const less = [](Hit const& a, Hit const& b) {
            if(a.rank != b.rank) { return a.rank < b.rank; }
            auto const& an = a.symbol->name;
            auto const& bn = b.symbol->name;
            return an.size() != bn.size() ? an.size() < bn.size() : an < bn;
        };
        auto const shown = std::min(max, hits.size());
        std::partial_sort(hits.begin(),
                          hits.begin() + static_cast<std::ptrdiff_t>(shown),
                          hits.end(),
                          less);
        for(std::size_t i = 0; i != shown; ++i) {
            auto const& s = *hits[i].symbol;
            out.best.push_back({s.name, s.address, s.size});
        }
        return out;
    }

    /// Watch a data symbol by its exact name (or the only one a query matches). Empty = done.
    std::string addWatch(std::string const& name) {
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            auto const*                        s = symbols_->find(name);
            if(s == nullptr) {
                SymbolQuery const q{name};
                auto const        all = symbols_->findIf([&](MapSymbol const& m) {
                    return !m.code && m.size != 0 && q.rank(m.name).has_value();
                });
                if(all.size() != 1) {
                    return all.empty() ? "no data symbol " + name + " in the map"
                                       : std::to_string(all.size()) + " symbols match " + name;
                }
                s = all.front();
            }
            if(std::ranges::any_of(state_.watches,
                                   [&](auto const& w) { return w.name == s->name; }))
            {
                return {};
            }
            Snapshot::Watch w;
            w.name       = s->name;
            w.address    = s->address;
            w.symbolSize = s->size;
            w.size       = std::min(std::max(s->size, 1U), maxWatchBytes);
            state_.watches.push_back(std::move(w));
        }
        wake();
        changed();
        return {};
    }

    /// By name: an index taken from a snapshot may be a frame old.
    void removeWatch(std::string const& name) {
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            std::erase_if(state_.watches, [&](auto const& w) { return w.name == name; });
        }
        changed();
    }

    void startProfile() {
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            state_.profiling = true;
            state_.profileError.clear();
        }
        wake();
        changed();
    }

    void stopProfile() {
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            state_.profiling = false;
            rankProfileLocked();
        }
        changed();
    }

    void clearProfile() {
        std::scoped_lock<std::mutex> const lock{mutex_};
        pcCounts_.clear();
        state_.profile.clear();
        state_.profileSamples = 0;
        state_.profileNone    = 0;
        state_.profileOutside = 0;
        state_.profileSeconds = 0;
        state_.profileError.clear();
        changedLater_ = true;
    }

    /// Make the core call Kvasir::Panic::raise(cause), as the control socket's `raise_panic` (PanicRaise.hpp) -
    /// through Hooks::target. On the worker; the outcome lands in
    /// Snapshot::panicRaise.
    void raisePanic(std::uint32_t cause) {
        panicCause_ = static_cast<std::int64_t>(cause);
        wake();
    }

    /// Empty the fault and panic records in the target's RAM as the firmware's own boot report
    /// does (the id word to 0: Persistent::load() rejects it), for a record no boot line ever
    /// takes: the status line's FAULT / PANIC then stays for good. The last halt's capture goes
    /// too (host side only). Refused while the core is halted: on the panic's breakpoint the
    /// reset that follows makes that bkpt fault on the way out, and without the panic record the
    /// fault handler records it - a FAULT in place of the PANIC just cleared. On the worker;
    /// the outcome lands in Snapshot::recordsCleared, and what was thrown away is said through
    /// Hooks::note first.
    void clearRecords(bool haltedNow) {
        if(haltedNow) {
            clearDone(
              "not cleared: halted - reset first ([r]), or the reset leaves a fault "
              "record in the panic's place");
            return;
        }
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            state_.halt.reset();
        }
        clearDue_ = true;
        wake();
        changed();
    }

private:
    static constexpr std::uint32_t pcsrNone  = 0xFFFF'FFFFU;
    static constexpr std::size_t   pcsrReads = 64;

    using Clock = uc_log::printer::Clock;

    // remote_fmt's call-site tags (catalog.hpp): one per log line, named after the whole format
    // string, in an INFO section at address 0 - symbols of the map that are not in the target
    static bool isSiteTag(std::string_view name) {
        return name.ends_with("::REMOTE_FMT_SITE_TAG") || name == "remote_fmt::detail::siteAnchor";
    }

    std::string describeLocked(std::uint32_t address) const { return symbols_->describe(address); }

    void wake() {
        {
            std::scoped_lock<std::mutex> const lock{wakeMutex_};
            woken_ = true;
        }
        wake_.notify_all();
    }

    void changed() {
        version_.fetch_add(1, std::memory_order_release);
        if(hooks_.changed) { hooks_.changed(); }
    }

    // a session runs and nothing is flashed
    bool connected() const {
        return hooks_.target != nullptr && hooks_.target->linkStatus().isRunning != 0
            && !hooks_.target->isFlashing();
    }

    printer::TargetPort::Result access(std::vector<TargetOp> const& ops) {
        if(hooks_.target == nullptr) { return std::unexpected{std::string{"no target access"}}; }
        return hooks_.target->accessTarget(ops, callTimeout);
    }

    std::optional<std::uint32_t> readWord(std::uint32_t address,
                                          std::string&  error) {
        auto r = access({
          target_op::Read{address, 4}
        });
        if(!r) {
            error = r.error();
            return std::nullopt;
        }
        return records::word((*r)[0], 0);
    }

    std::optional<std::uint32_t> writeWord(std::uint32_t address,
                                           std::uint32_t value,
                                           std::string&  error) {
        auto r = access({
          target_op::WriteWord{address, value}
        });
        if(!r) {
            error = r.error();
            return std::nullopt;
        }
        return records::word((*r)[0], 0);
    }

    // ---- map ------------------------------------------------------------------------------

    void loadMapIfChanged() {
        if(!hooks_.map) { return; }
        auto map = hooks_.map();
        if(!map || map == map_) { return; }
        std::scoped_lock<std::mutex> const lock{mutex_};
        state_.mapError    = map->error;
        symbols_           = std::shared_ptr<MapSymbols const>{map, &map->symbols};
        map_               = std::move(map);
        state_.lldMap      = symbols_->fromLld();
        state_.symbolCount = symbols_->size();
        ++state_.mapGeneration;
        places_          = records::Places::of(*symbols_);
        state_.haveUbsan = places_.ubsan.has_value();
        state_.haveFault = places_.fault.has_value();
        state_.havePanic = places_.panic.has_value();
        if(!state_.haveUbsan) { state_.ubsan.reset(); }
        if(!state_.haveFault) { state_.fault.reset(); }
        if(!state_.havePanic) { state_.panic.reset(); }

        state_.stacks.clear();
        for(auto const& s : places_.stacks) {
            state_.stacks.push_back({s.name, s.low, s.high, std::nullopt, {}, {}});
        }
        state_.rings.clear();
        for(auto const& r : places_.rings) {
            Snapshot::Ring ring;
            ring.address = r.storage;
            ring.name    = r.symbol;   // until the layout string is read
            state_.rings.push_back(std::move(ring));
        }
        // watches follow their symbols to a new address, or say they are gone
        for(auto& w : state_.watches) {
            if(auto const* s = symbols_->find(w.name)) {
                w.address    = s->address;
                w.symbolSize = s->size;
                w.size       = std::min(std::max(s->size, 1U), maxWatchBytes);
                w.error.clear();
            } else {
                w.error = "not in the map any more";
            }
        }
        reader_.forget();
        pcCounts_.clear();
        state_.profile.clear();
        recordsDue_ = true;
        stacksDue_  = true;
        changed();
    }

    // ---- periodic reads -------------------------------------------------------------------

    records::Places places() const {
        std::scoped_lock<std::mutex> const lock{mutex_};
        return places_;
    }

    void readUbsan() {
        auto const r = reader_.read(places(), {.ubsan = true}, {});
        if(!r.ubsan || !r.ubsan->error.empty()) { return; }
        auto const&                        ub = r.ubsan->record;
        std::scoped_lock<std::mutex> const lock{mutex_};
        bool const news = !state_.ubsan || !ub || state_.ubsan->count != ub->count;
        state_.ubsan    = ub;
        state_.ubsanWhere
          = ub && ub->count != 0 ? describeLocked(ub->lastReturnAddress) : std::string{};
        if(news) { changedLater_ = true; }
    }

    void readRecords() {
        auto places = this->places();
        places.stack.reset();   // the Health tab shows no stack snapshot
        auto const r = reader_.read(places, {.crash = true}, {});
        if((r.fault && !r.fault->error.empty()) || (r.panic && !r.panic->error.empty())) { return; }
        if(!r.fault && !r.panic) { return; }
        std::scoped_lock<std::mutex> const lock{mutex_};
        if(r.fault) {
            state_.fault   = r.fault->record;
            state_.faultPc = state_.fault ? describeLocked(state_.fault->pc) : std::string{};
            state_.faultLr = state_.fault ? describeLocked(state_.fault->lr) : std::string{};
        }
        if(r.panic) {
            state_.panic     = r.panic->record;
            auto const site  = state_.panic ? state_.panic->site() : std::nullopt;
            state_.panicSite = site ? describeLocked(*site) : std::string{};
        }
        changedLater_ = true;
    }

    void readStacks(std::stop_token const& st) {
        auto const                         r = reader_.read(places(), {.stacks = true}, st);
        std::scoped_lock<std::mutex> const lock{mutex_};
        // the map may have changed meanwhile: only take what still matches
        for(auto& s : state_.stacks) {
            for(auto const& m : r.stacks) {
                if(m.low == s.low && m.high == s.high) {
                    s.use   = m.use;
                    s.error = m.error;
                    if(m.read) { s.measured = Clock::now(); }
                }
            }
        }
        changedLater_ = true;
    }

    void readRings(std::stop_token const& st) {
        auto                               r = reader_.read(places(), {.trace = true}, st);
        std::scoped_lock<std::mutex> const lock{mutex_};
        for(auto& ring : state_.rings) {
            for(auto& m : r.rings) {
                if(m.address != ring.address) { continue; }
                ring = Snapshot::Ring{.name                = std::move(m.name),
                                      .address             = m.address,
                                      .fields              = std::move(m.fields),
                                      .count               = m.count,
                                      .capacity            = m.capacity,
                                      .writtenWhileReading = m.writtenWhileReading,
                                      .rows                = std::move(m.rows),
                                      .error               = std::move(m.error)};
            }
        }
        changedLater_ = true;
    }

    void readWatches() {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> wanted;
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            for(auto const& w : state_.watches) { wanted.emplace_back(w.address, w.size); }
        }
        if(wanted.empty()) { return; }
        std::vector<TargetOp> reads;
        reads.reserve(wanted.size());
        for(auto const& [a, n] : wanted) { reads.emplace_back(target_op::Read{a, n}); }
        auto                               r = access(reads);
        std::scoped_lock<std::mutex> const lock{mutex_};
        for(std::size_t i = 0; i != wanted.size(); ++i) {
            for(auto& w : state_.watches) {
                if(w.address != wanted[i].first || w.size != wanted[i].second) { continue; }
                if(!r) {
                    w.error = r.error();
                    continue;
                }
                auto const& v = (*r)[i];
                if(!w.value.empty() && v != w.value) {
                    w.changed = Clock::now();
                    ++w.changes;
                }
                w.value.assign(v.begin(), v.end());
                w.error.clear();
            }
        }
        changedLater_ = true;
    }

    void sampleProfile() {
        auto const start = Clock::now();
        if(!profileChecked_) {
            std::string error;
            auto const  demcr = readWord(cortex_m::demcr, error);
            if(!demcr) {
                profileFailed(error);
                return;
            }
            if((*demcr & cortex_m::demcrTrcena) == 0) {
                profileFailed("DEMCR.TRCENA is clear: the DWT is off, DWT_PCSR reads nothing");
                return;
            }
            profileChecked_ = true;
        }
        std::vector<TargetOp> const reads(pcsrReads,
                                          TargetOp{
                                            target_op::Read{cortex_m::dwtPcsr, 4}
        });
        auto                        r = access(reads);
        if(!r) {
            profileFailed(r.error());
            return;
        }
        std::scoped_lock<std::mutex> const lock{mutex_};
        for(auto const& d : *r) {
            auto const pc = records::word(d, 0);
            if(pc == pcsrNone) {
                ++state_.profileNone;
                continue;
            }
            ++pcCounts_[pc & ~1U];
            ++state_.profileSamples;
        }
        state_.profileSeconds += std::chrono::duration<double>(Clock::now() - start).count();
        if(Clock::now() - lastRank_ > std::chrono::milliseconds{500}) {
            lastRank_ = Clock::now();
            rankProfileLocked();
            changedLater_ = true;
        }
    }

    void profileFailed(std::string const& why) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        state_.profileError = why;
        state_.profiling    = false;
        profileChecked_     = false;
        changedLater_       = true;
    }

    void rankProfileLocked() {
        // by the symbol's address: one row per symbol, in address order before the sort by samples (ties stay
        // in that order, not in pointer order)
        std::map<std::uint32_t, std::pair<MapSymbol const*, std::uint64_t>> bySymbol;
        std::uint64_t                                                       outside = 0;
        bool allZero = !pcCounts_.empty();
        for(auto const& [pc, n] : pcCounts_) {
            if(pc != 0) { allZero = false; }
            if(auto const* s = symbols_->containing(pc)) {
                auto& row = bySymbol[s->address];
                row.first = s;
                row.second += n;
            } else {
                outside += n;
            }
        }
        state_.profileOutside = outside;
        if(allZero) {
            state_.profileError = "DWT_PCSR reads 0: this core has no DWT_PCSR (Armv6-M)";
        }
        state_.profile.clear();
        for(auto const& [address, row] : bySymbol) {
            state_.profile.push_back({row.first->name, row.second});
        }
        std::ranges::stable_sort(state_.profile, std::greater{}, &Snapshot::ProfileRow::samples);
        if(state_.profile.size() > 200) { state_.profile.resize(200); }
    }

    void doRaisePanic(std::uint32_t cause) {
        auto const raise = places().raise;
        auto const done  = [&](std::string text) {
            {
                std::scoped_lock<std::mutex> const lock{mutex_};
                state_.panicRaise = std::move(text);
            }
            changed();
        };
        if(!raise) {
            done("Kvasir::Panic::raise is not in the map: a firmware without Kvasir::Panic?");
            return;
        }
        if(hooks_.target == nullptr) {
            done("this printer cannot halt the core");
            return;
        }
        auto const raised = panic_raise::raise(
          [&](std::vector<TargetOp> const& ops) {
              return hooks_.target->accessTarget(ops, printer::TargetPort::defaultTimeout);
          },
          *raise,
          cause);
        if(!raised) {
            done(raised.error());
            return;
        }
        done("raise("
             + std::string{cause < records::panicCauses.size() ? records::panicCauses[cause] : "?"}
             + ") called; the next boot line names it");
        recordsDue_ = true;
    }

    void clearDone(std::string text) {
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            state_.recordsCleared = std::move(text);
        }
        changed();
    }

    void doClearRecords() {
        auto const at    = places();
        auto const fault = at.fault;
        auto const panic = at.panic;
        // Read first: only a record that decodes (id, header and CRC) is written - with the map of
        // another image the address is anything - and what goes is said before it is gone. Its id
        // word goes to 0, which the firmware's Persistent::load() rejects, as a taken record.
        std::vector<TargetOp> reads;
        if(fault) { reads.emplace_back(target_op::Read{*fault, records::faultSize}); }
        if(panic) { reads.emplace_back(target_op::Read{*panic, records::panicSize}); }
        if(reads.empty()) {
            clearDone("no fault or panic record in this image");
            return;
        }
        auto r = access(reads);
        if(!r) {
            clearDone("not cleared: " + r.error());
            return;
        }
        std::vector<TargetOp>    writes;
        std::vector<std::string> gone;
        std::size_t              i = 0;
        if(fault) {
            if(auto const f = records::decodeFault((*r)[i++])) {
                writes.emplace_back(target_op::WriteWord{*fault, 0U});
                std::scoped_lock<std::mutex> const lock{mutex_};
                gone.push_back(
                  f->hasFrame()
                    ? std::format("fault record cleared from the TUI: {} fault(s), the first in {} "
                                  "on core {}, "
                                  "pc={:#010x} {} lr={:#010x} {} xpsr={:#x} exc_return={:#x} "
                                  "cfsr={:#x} hfsr={:#x}",
                                  f->count,
                                  records::exceptionName(f->exception()),
                                  f->core(),
                                  f->pc,
                                  describeLocked(f->pc),
                                  f->lr,
                                  describeLocked(f->lr),
                                  f->xpsr,
                                  f->excReturn,
                                  f->cfsr,
                                  f->hfsr)
                    : std::format("fault record cleared from the TUI: {} fault(s), the first on "
                                  "core {} with no "
                                  "stacked frame, exc_return={:#x} cfsr={:#x} hfsr={:#x}",
                                  f->count,
                                  f->core(),
                                  f->excReturn,
                                  f->cfsr,
                                  f->hfsr));
            }
        }
        if(panic) {
            if(auto const p = records::decodePanic((*r)[i])) {
                writes.emplace_back(target_op::WriteWord{*panic, 0U});
                auto const                         site = p->site();
                std::scoped_lock<std::mutex> const lock{mutex_};
                gone.push_back(
                  std::format("panic record cleared from the TUI: {} panic(s), the first: {}, {} "
                              "detail={}",
                              p->count,
                              p->causeName(),
                              site ? std::format("site={:#010x} {}", *site, describeLocked(*site))
                                   : std::string{"no program site"},
                              p->detail));
            }
        }
        if(writes.empty()) {
            clearDone("nothing to clear: no fault or panic record in RAM");
            return;
        }
        auto w = access(writes);
        if(!w) {
            clearDone("not cleared: " + w.error());
            return;
        }
        for(auto const& back : *w) {
            if(records::word(back, 0) != 0) {
                clearDone("not cleared: the record's id read back unchanged");
                recordsDue_ = true;
                return;
            }
        }
        if(hooks_.note) {
            for(auto const& line : gone) { hooks_.note(line); }
        }
        clearDone(
          std::format("{} record(s) cleared; what they held is in the Status tab", gone.size()));
        recordsDue_ = true;
    }

    // ---- the worker -----------------------------------------------------------------------

    void run(std::stop_token const& st) {
        auto          lastMapCheck = Clock::time_point{};
        auto          lastUbsan    = Clock::time_point{};
        auto          lastRecords  = Clock::time_point{};
        auto          lastStacks   = Clock::time_point{};
        auto          lastRings    = Clock::time_point{};
        auto          lastWatches  = Clock::time_point{};
        std::uint64_t sessions     = 0;
        while(!st.stop_requested()) {
            auto const now  = Clock::now();
            auto const page = page_.load();
            bool const all  = refreshAll_.exchange(false);
            if(now - lastMapCheck >= std::chrono::seconds{1} || all) {
                lastMapCheck = now;
                loadMapIfChanged();
            }
            if(hooks_.target != nullptr) {
                auto const s = hooks_.target->sessionCount();
                if(s != sessions) {
                    sessions        = s;
                    recordsDue_     = true;
                    stacksDue_      = true;
                    profileChecked_ = false;
                }
            }
            if(connected()) {
                if(auto const cause = panicCause_.exchange(-1); cause >= 0) {
                    doRaisePanic(static_cast<std::uint32_t>(cause));
                }
                if(clearDue_.exchange(false)) { doClearRecords(); }
                if(now - lastUbsan >= std::chrono::seconds{1} || all) {
                    lastUbsan = now;
                    readUbsan();
                }
                if(now - lastRecords >= std::chrono::seconds{2} || recordsDue_.exchange(false)
                   || all)
                {
                    lastRecords = now;
                    readRecords();
                }
                if((page == Page::health && now - lastStacks >= std::chrono::seconds{10})
                   || stacksDue_.exchange(false) || all)
                {
                    lastStacks = now;
                    readStacks(st);
                }
                if(page == Page::trace
                   && (now - lastRings >= std::chrono::milliseconds{500} || all))
                {
                    lastRings = now;
                    readRings(st);
                }
                if(page == Page::watch && now - lastWatches >= std::chrono::milliseconds{200}) {
                    lastWatches = now;
                    readWatches();
                }
                bool profiling = false;
                {
                    std::scoped_lock<std::mutex> const lock{mutex_};
                    profiling = state_.profiling;
                }
                if(profiling) {
                    sampleProfile();
                    if(changedLater_.exchange(false)) { changed(); }
                    continue;   // back to back, as kvasir_bench.py profile
                }
            } else {
                panicCause_ = -1;
                if(clearDue_.exchange(false)) { clearDone("not cleared: no target session"); }
            }
            if(changedLater_.exchange(false)) { changed(); }
            std::unique_lock<std::mutex> lock{wakeMutex_};
            wake_.wait_for(lock, st, std::chrono::milliseconds{100}, [&] { return woken_; });
            woken_ = false;
        }
    }

    Hooks hooks_;

    mutable std::mutex                     mutex_;   // guards everything below up to the atomics
    Snapshot                               state_;
    std::shared_ptr<MapSnapshot const>     map_;   // the parse symbols_ points into
    std::shared_ptr<MapSymbols const>      symbols_{std::make_shared<MapSymbols const>()};
    records::Places                        places_;   // where this map's image keeps its records
    std::map<std::uint32_t, std::uint64_t> pcCounts_;
    Clock::time_point                      lastRank_{};

    // the worker's own: the ring descriptors and layouts it read once per map load
    records::RecordReader reader_{[this](std::vector<TargetOp> const& ops) { return access(ops); }};

    std::atomic<Page>          page_{Page::none};
    std::atomic<bool>          refreshAll_{true};
    std::atomic<bool>          recordsDue_{true};
    std::atomic<bool>          stacksDue_{true};
    std::atomic<bool>          changedLater_{false};
    std::atomic<std::int64_t>  panicCause_{-1};
    std::atomic<bool>          clearDue_{false};
    std::atomic<bool>          profileChecked_{false};
    std::atomic<std::uint64_t> version_{0};

    std::mutex               wakeMutex_;
    Clock::ConditionVariable wake_;
    bool                     woken_{};

    Clock::Thread thread_;   // last: started after every member above exists
};

}   // namespace uc_log::printer
