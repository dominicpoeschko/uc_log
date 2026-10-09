#pragma once

#include "uc_log/detail/MapSymbols.hpp"
#include "uc_log/detail/SymbolMatch.hpp"
#include "uc_log/detail/TargetRecords.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

/// The TUI's view into the running target, the parts of kvasir_bench.py a printer can do alone:
/// the sanitizer counter (`ub`), the fault and panic records and the last halt (`crash`), the
/// painted stacks (`stack`), the trace rings (`trace`), watched variables (`peek --watch`), a
/// PC-sampling profile (`profile`) and raising a panic (`panic`). Symbols come from the linker map
/// (MapSymbols), memory through the reader's accessMemory - from this object's own thread, never
/// the UI's, and in pieces small enough that the RTT reads go on between them.
namespace uc_log::detail {

struct InspectorAccess {
    std::uint32_t                address{};
    std::uint32_t                size{};
    std::optional<std::uint32_t> write{};   // written first, then 4 bytes read back
};

using InspectorResult = std::expected<std::vector<std::vector<std::byte>>, std::string>;

class TargetInspector {
public:
    struct Hooks {
        std::function<InspectorResult(std::span<InspectorAccess const>, std::chrono::milliseconds)>
                                              memory;
        std::function<bool()>                 connected;   // a session runs and nothing is flashed
        std::function<std::uint64_t()>        sessions;    // a new session: records and map again
        std::function<std::string()>          mapFile;
        std::function<void()>                 changed;   // something to redraw
        std::function<void(std::string_view)> note{};    // a line for the status log
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
            std::chrono::system_clock::time_point              time;
            std::vector<std::pair<std::string, std::uint32_t>> registers;
            std::uint32_t                                      exception{};
            std::string                                        pcWhere;
            std::string                                        lrWhere;
            std::vector<HaltWord>                              codeWords;   // on the stack
        };

        std::optional<Halt> halt;

        struct Stack {
            std::string                           name;
            std::uint32_t                         low{};
            std::uint32_t                         high{};
            std::optional<records::StackUse>      use;
            std::string                           error;
            std::chrono::steady_clock::time_point measured{};
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
            std::string                           name;
            std::uint32_t                         address{};
            std::uint32_t                         size{};   // read
            std::uint32_t                         symbolSize{};
            std::vector<std::byte>                value;
            std::chrono::steady_clock::time_point changed{};   // the last change seen
            std::uint64_t                         changes{};
            std::string                           error;
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

    static constexpr std::uint32_t MaxWatchBytes = 64;
    static constexpr std::uint32_t PieceBytes    = 4096;    // a control-socket read's limit
    static constexpr std::uint32_t CallBytes     = 16384;   // per accessMemory call
    static constexpr auto          CallTimeout   = std::chrono::milliseconds{1000};

    explicit TargetInspector(Hooks hooks)
      : hooks_{std::move(hooks)}
      , thread_{[this](std::stop_token st) { run(st); }} {}

    ~TargetInspector() {
        thread_.request_stop();
        wake_.notify_all();
    }

    TargetInspector(TargetInspector const&)            = delete;
    TargetInspector& operator=(TargetInspector const&) = delete;

    /// Bumped whenever the snapshot changed: copy it only then.
    std::uint64_t version() const { return version_.load(std::memory_order_acquire); }

    Snapshot snapshot() const {
        std::lock_guard<std::mutex> const lock{mutex_};
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

    /// The reader's "core halted: pc=... stack: ..." message.
    void noteHalt(std::string_view text) {
        auto halt = parseHalt(text);
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            halt.pcWhere = describeLocked(registerOf(halt, "pc"));
            halt.lrWhere = describeLocked(registerOf(halt, "lr"));
            for(auto& w : halt.codeWords) { w.where = describeLocked(w.value); }
            std::erase_if(halt.codeWords, [](auto const& w) { return w.where.empty(); });
            state_.halt = std::move(halt);
        }
        recordsDue_ = true;
        wake();
        changed();
    }

    std::string describe(std::uint32_t address) const {
        std::lock_guard<std::mutex> const lock{mutex_};
        return describeLocked(address);
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
        std::lock_guard<std::mutex> const lock{mutex_};

        struct Hit {
            unsigned         rank;
            MapSymbol const* symbol;
        };

        std::vector<Hit> hits;
        for(auto const& s : symbols_.all()) {
            if(s.code || s.size == 0 || isSiteTag(s.name)) { continue; }
            auto const rank = q.rank(s.name);
            // a name that repeats (static locals of inlined functions) is its first symbol
            if(rank && symbols_.find(s.name) == &s) { hits.push_back({*rank, &s}); }
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
            std::lock_guard<std::mutex> const lock{mutex_};
            auto const*                       s = symbols_.find(name);
            if(s == nullptr) {
                SymbolQuery const q{name};
                auto const        all = symbols_.findIf([&](MapSymbol const& m) {
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
            w.size       = std::min(std::max(s->size, 1U), MaxWatchBytes);
            state_.watches.push_back(std::move(w));
        }
        wake();
        changed();
        return {};
    }

    void removeWatch(std::size_t index) {
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            if(index < state_.watches.size()) {
                state_.watches.erase(state_.watches.begin() + static_cast<std::ptrdiff_t>(index));
            }
        }
        changed();
    }

    /// By name: an index taken from a snapshot may be a frame old.
    void removeWatch(std::string const& name) {
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            std::erase_if(state_.watches, [&](auto const& w) { return w.name == name; });
        }
        changed();
    }

    void startProfile() {
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            state_.profiling = true;
            state_.profileError.clear();
        }
        wake();
        changed();
    }

    void stopProfile() {
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            state_.profiling = false;
            rankProfileLocked();
        }
        changed();
    }

    void clearProfile() {
        std::lock_guard<std::mutex> const lock{mutex_};
        pcCounts_.clear();
        state_.profile.clear();
        state_.profileSamples = 0;
        state_.profileNone    = 0;
        state_.profileOutside = 0;
        state_.profileSeconds = 0;
        state_.profileError.clear();
        changedLater_ = true;
    }

    /// Make the core call Kvasir::Panic::raise(cause), as `kvasir_bench.py panic`: halt, R0 =
    /// cause, xPSR without IT/ICI, PC = raise, run. On the worker; the outcome lands in
    /// Snapshot::panicRaise.
    void raisePanic(std::uint32_t cause) {
        panicCause_ = static_cast<std::int64_t>(cause);
        wake();
    }

    /// Empty the fault and panic records in the target's RAM the way the firmware's own boot
    /// report does (magic = 0), for a record no boot line ever takes: the status line's FAULT /
    /// PANIC then stays for good. The last halt's capture goes too (host side only). Refused
    /// while the core is halted: on the panic's breakpoint the reset that follows makes that
    /// bkpt fault on the way out, and without the panic record the fault handler records it -
    /// a FAULT in place of the PANIC just cleared (seen on an RP2350). On the worker;
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
            std::lock_guard<std::mutex> const lock{mutex_};
            state_.halt.reset();
        }
        clearDue_ = true;
        wake();
        changed();
    }

    // ---- parts the host tests call directly ----------------------------------------------

    static Snapshot::Halt parseHalt(std::string_view text) {
        Snapshot::Halt halt;
        halt.time                 = std::chrono::system_clock::now();
        auto const        stackAt = text.find(" stack:");
        auto const        head    = text.substr(0, stackAt);
        std::regex const  kv{R"((\w+)=(0x[0-9a-fA-F]+|\d+))"};
        std::string const headText{head};
        for(std::sregex_iterator it{headText.begin(), headText.end(), kv}, end; it != end; ++it) {
            auto const name  = (*it)[1].str();
            auto const value = static_cast<std::uint32_t>(std::stoul((*it)[2].str(), nullptr, 0));
            if(name == "exception") {
                halt.exception = value;
            } else {
                halt.registers.emplace_back(name, value);
            }
        }
        if(stackAt != std::string_view::npos) {
            auto          rest   = text.substr(stackAt + 7);
            std::uint32_t offset = 0;
            while(!rest.empty()) {
                auto const start = rest.find_first_not_of(' ');
                if(start == std::string_view::npos) { break; }
                rest.remove_prefix(start);
                auto const end   = std::min(rest.find(' '), rest.size());
                auto const token = rest.substr(0, end);
                rest.remove_prefix(end);
                std::uint32_t v{};
                auto const [ptr, ec]
                  = std::from_chars(token.data(), std::to_address(token.end()), v, 16);
                if(ec == std::errc{} && ptr == std::to_address(token.end())) {
                    halt.codeWords.push_back({offset, v, {}});
                }
                offset += 4;
            }
        }
        return halt;
    }

    static std::uint32_t registerOf(Snapshot::Halt const& halt,
                                    std::string_view      name) {
        for(auto const& [n, v] : halt.registers) {
            if(n == name) { return v; }
        }
        return 0;
    }

    /// The register transfers of raisePanic(), kvasir_bench.py's panic_register_writes. LR is
    /// raise()'s return address, which the record keeps as the panic's place: set to "called
    /// from where the core was halted" (`pc`), or the record names whatever LR happened to hold.
    static std::vector<std::vector<std::pair<std::uint32_t,
                                             std::uint32_t>>>
    panicRegisterWrites(std::uint32_t raiseAddress,
                        std::uint32_t cause,
                        std::uint32_t xpsr,
                        std::uint32_t pc) {
        return {
          {             {Dcrdr, cause},   {Dcrsr, RegWnR | RegR0}},
          {    {Dcrdr, (pc + 2U) | 1U},   {Dcrsr, RegWnR | RegLr}},
          { {Dcrdr, xpsr & ~EpsrItIci}, {Dcrsr, RegWnR | RegXpsr}},
          {{Dcrdr, raiseAddress & ~1U},   {Dcrsr, RegWnR | RegPc}}
        };
    }

    static constexpr std::string_view PanicRaise{"Kvasir::Panic::raise(Kvasir::Panic::Cause)"};

private:
    // Armv6-M/v7-M/v8-M debug registers (DDI0553B.y D1.2.33 DCRDR, D1.2.34 DCRSR, D1.2.39
    // DHCSR; DDI0419E C1.6): a DCRSR write while the core is not halted is ignored.
    static constexpr std::uint32_t Dhcsr    = 0xE000'EDF0U;
    static constexpr std::uint32_t Dcrsr    = 0xE000'EDF4U;
    static constexpr std::uint32_t Dcrdr    = 0xE000'EDF8U;
    static constexpr std::uint32_t DbgKey   = 0xA05F'0000U;
    static constexpr std::uint32_t CDebugEn = 1U << 0;
    static constexpr std::uint32_t CHalt    = 1U << 1;
    static constexpr std::uint32_t SRegRdy  = 1U << 16;
    static constexpr std::uint32_t SHalt    = 1U << 17;
    static constexpr std::uint32_t RegWnR   = 1U << 16;
    static constexpr std::uint32_t RegR0    = 0;
    static constexpr std::uint32_t RegLr    = 14;
    static constexpr std::uint32_t RegPc    = 15;
    static constexpr std::uint32_t RegXpsr  = 16;
    // EPSR.IT/ICI, xPSR[26:25] and [15:10] (B3.5, rule RQLRN)
    static constexpr std::uint32_t EpsrItIci = (0b11U << 25) | (0x3FU << 10);
    // DEMCR.TRCENA (bit 24) turns the DWT on; DWT_PCSR (D1.2.68 / TRM 100230 C3.2)
    static constexpr std::uint32_t Demcr     = 0xE000'EDFCU;
    static constexpr std::uint32_t DwtPcsr   = 0xE000'101CU;
    static constexpr std::uint32_t PcsrNone  = 0xFFFF'FFFFU;
    static constexpr std::size_t   PcsrReads = 64;

    using Clock = std::chrono::steady_clock;

    // remote_fmt's call-site tags (catalog.hpp): one per log line, named after the whole format
    // string, in an INFO section at address 0 - symbols of the map that are not in the target
    static bool isSiteTag(std::string_view name) {
        return name.ends_with("::REMOTE_FMT_SITE_TAG") || name == "remote_fmt::detail::siteAnchor";
    }

    std::string describeLocked(std::uint32_t address) const { return symbols_.describe(address); }

    void wake() {
        {
            std::lock_guard<std::mutex> const lock{wakeMutex_};
            woken_ = true;
        }
        wake_.notify_all();
    }

    void changed() {
        version_.fetch_add(1, std::memory_order_release);
        if(hooks_.changed) { hooks_.changed(); }
    }

    bool connected() const { return !hooks_.connected || hooks_.connected(); }

    InspectorResult access(std::vector<InspectorAccess> const& accesses) {
        if(!hooks_.memory) { return std::unexpected{std::string{"no target access"}}; }
        return hooks_.memory(accesses, CallTimeout);
    }

    /// `size` bytes from `address`, in pieces and calls small enough to keep RTT flowing.
    std::expected<std::vector<std::byte>,
                  std::string>
    readRange(std::uint32_t          address,
              std::uint32_t          size,
              std::stop_token const& st) {
        std::vector<std::byte> out;
        out.reserve(size);
        while(out.size() < size) {
            if(st.stop_requested()) { return std::unexpected{std::string{"stopping"}}; }
            std::vector<InspectorAccess> pieces;
            std::uint32_t                inCall = 0;
            auto                         at     = address + static_cast<std::uint32_t>(out.size());
            auto                         left   = size - static_cast<std::uint32_t>(out.size());
            while(left != 0 && inCall < CallBytes) {
                auto const n = std::min({left, PieceBytes, CallBytes - inCall});
                pieces.push_back({at, n, {}});
                at += n;
                left -= n;
                inCall += n;
            }
            auto r = access(pieces);
            if(!r) { return std::unexpected{r.error()}; }
            for(auto const& p : *r) { out.insert(out.end(), p.begin(), p.end()); }
        }
        return out;
    }

    std::optional<std::uint32_t> readWord(std::uint32_t address,
                                          std::string&  error) {
        auto r = access({
          InspectorAccess{address, 4, {}}
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
          InspectorAccess{address, 4, value}
        });
        if(!r) {
            error = r.error();
            return std::nullopt;
        }
        return records::word((*r)[0], 0);
    }

    // ---- map ------------------------------------------------------------------------------

    void loadMapIfChanged(bool force) {
        if(!hooks_.mapFile) { return; }
        auto const      path = hooks_.mapFile();
        std::error_code ec;
        auto const      stamp = std::filesystem::last_write_time(path, ec);
        if(!force && !ec && loaded_ && stamp == mapStamp_ && path == mapPath_) { return; }
        auto                              parsed = MapSymbols::fromFile(path);
        std::lock_guard<std::mutex> const lock{mutex_};
        mapPath_  = path;
        mapStamp_ = stamp;
        loaded_   = true;
        if(!parsed) {
            state_.mapError = parsed.error();
            symbols_        = {};
        } else {
            state_.mapError.clear();
            symbols_ = std::move(*parsed);
        }
        state_.lldMap      = symbols_.fromLld();
        state_.symbolCount = symbols_.size();
        ++state_.mapGeneration;
        auto const* ub    = symbols_.find(records::UbsanSymbol);
        auto const* fault = symbols_.find(records::FaultSymbol);
        auto const* panic = symbols_.find(records::PanicSymbol);
        ubsan_  = ub ? std::optional{std::pair{ub->address, std::min(ub->size, 8U)}} : std::nullopt;
        fault_ = fault ? std::optional{fault->address} : std::nullopt;
        panic_  = panic ? std::optional{std::pair{panic->address, panic->size >= 20 ? 20U : 16U}}
                        : std::nullopt;
        raise_ = [&]() -> std::optional<std::uint32_t> {
            if(auto const* r = symbols_.find(PanicRaise)) { return r->address; }
            return std::nullopt;
        }();
        state_.haveUbsan = ub != nullptr;
        state_.haveFault = fault != nullptr;
        state_.havePanic = panic != nullptr;
        if(!state_.haveUbsan) { state_.ubsan.reset(); }
        if(!state_.haveFault) { state_.fault.reset(); }
        if(!state_.havePanic) { state_.panic.reset(); }

        state_.stacks.clear();
        auto const stack = [&](std::string      name,
                               std::string_view lowName,
                               std::string_view lowAlt,
                               std::string_view highName,
                               std::string_view highAlt) {
            auto const low
              = symbols_.value(lowName).or_else([&] { return symbols_.value(lowAlt); });
            auto const high
              = symbols_.value(highName).or_else([&] { return symbols_.value(highAlt); });
            if(low && high && *high > *low) {
                state_.stacks.push_back({std::move(name), *low, *high, std::nullopt, {}, {}});
            }
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

        state_.rings.clear();
        for(auto const& s : symbols_.all()) {
            if(!s.code && s.name.ends_with(">::storage") && s.name.contains("Trace::Ring<")
               && s.size >= records::TraceHeaderSize)
            {
                Snapshot::Ring ring;
                ring.address          = s.address;
                ring.name             = s.name;   // until the layout string is read
                ringSizes_[s.address] = s.size;
                state_.rings.push_back(std::move(ring));
            }
        }
        // watches follow their symbols to a new address, or say they are gone
        for(auto& w : state_.watches) {
            if(auto const* s = symbols_.find(w.name)) {
                w.address    = s->address;
                w.symbolSize = s->size;
                w.size       = std::min(std::max(s->size, 1U), MaxWatchBytes);
                w.error.clear();
            } else {
                w.error = "not in the map any more";
            }
        }
        layouts_.clear();
        pcCounts_.clear();
        state_.profile.clear();
        recordsDue_ = true;
        stacksDue_  = true;
        changed();
    }

    // ---- periodic reads -------------------------------------------------------------------

    void readUbsan() {
        std::optional<std::pair<std::uint32_t, std::uint32_t>> at;
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            at = ubsan_;
        }
        if(!at) { return; }
        auto r = access({
          InspectorAccess{at->first, 8, {}}
        });
        if(!r) { return; }
        auto const                        ub = records::decodeUbsan((*r)[0]);
        std::lock_guard<std::mutex> const lock{mutex_};
        bool const news = !state_.ubsan || !ub || state_.ubsan->count != ub->count;
        state_.ubsan    = ub;
        state_.ubsanWhere
          = ub && ub->count != 0 ? describeLocked(ub->lastReturnAddress) : std::string{};
        if(news) { changedLater_ = true; }
    }

    void readRecords() {
        std::optional<std::uint32_t>                           fault;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> panic;
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            fault = fault_;
            panic = panic_;
        }
        std::vector<InspectorAccess> reads;
        if(fault) { reads.push_back({*fault, records::FaultSize, {}}); }
        if(panic) { reads.push_back({panic->first, panic->second, {}}); }
        if(reads.empty()) { return; }
        auto r = access(reads);
        if(!r) { return; }
        std::lock_guard<std::mutex> const lock{mutex_};
        std::size_t                       i = 0;
        if(fault) {
            state_.fault   = records::decodeFault((*r)[i++]);
            state_.faultPc = state_.fault ? describeLocked(state_.fault->pc) : std::string{};
            state_.faultLr = state_.fault ? describeLocked(state_.fault->lr) : std::string{};
        }
        if(panic) {
            state_.panic     = records::decodePanic((*r)[i]);
            auto const site  = state_.panic ? state_.panic->site() : std::nullopt;
            state_.panicSite = site ? describeLocked(*site) : std::string{};
        }
        changedLater_ = true;
    }

    void readStacks(std::stop_token const& st) {
        std::vector<Snapshot::Stack> stacks;
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            stacks = state_.stacks;
        }
        for(auto& s : stacks) {
            auto raw = readRange(s.low, s.high - s.low, st);
            if(!raw) {
                s.error = raw.error();
                s.use.reset();
                continue;
            }
            s.error.clear();
            s.use      = records::stackUse(*raw);
            s.measured = Clock::now();
            if(!s.use) { s.error = "not painted (no Kvasir::StackUsage for it)"; }
        }
        std::lock_guard<std::mutex> const lock{mutex_};
        // the map may have changed meanwhile: only take what still matches
        for(auto& s : state_.stacks) {
            for(auto const& m : stacks) {
                if(m.low == s.low && m.high == s.high) {
                    s.use      = m.use;
                    s.error    = m.error;
                    s.measured = m.measured;
                }
            }
        }
        changedLater_ = true;
    }

    void readRings(std::stop_token const& st) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> rings;
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            for(auto const& r : state_.rings) {
                rings.emplace_back(r.address, ringSizes_[r.address]);
            }
        }
        std::vector<Snapshot::Ring> out;
        for(auto const& [address, size] : rings) {
            Snapshot::Ring ring;
            ring.address = address;
            auto raw     = readRange(address, size, st);
            if(!raw) {
                ring.error = raw.error();
                out.push_back(std::move(ring));
                continue;
            }
            auto const h = records::decodeRingHeader(*raw);
            if(!h || h->storageSize() > raw->size()) {
                ring.error = "no valid header (does the board run this build?)";
                out.push_back(std::move(ring));
                continue;
            }
            std::string error;
            auto const  again  = readWord(address + 8, error);
            auto        layout = layoutAt(h->layout);
            ring.name          = layout.name;
            ring.fields        = layout.fields;
            ring.fields.resize(h->fields);
            ring.count               = h->count;
            ring.capacity            = h->capacity;
            ring.writtenWhileReading = again ? *again - h->count : 0;
            ring.rows                = records::decodeRingRows(*raw, *h, again.value_or(h->count));
            out.push_back(std::move(ring));
        }
        std::lock_guard<std::mutex> const lock{mutex_};
        for(auto& r : state_.rings) {
            for(auto& m : out) {
                if(m.address == r.address) { r = std::move(m); }
            }
        }
        changedLater_ = true;
    }

    records::RingLayout layoutAt(std::uint32_t address) {
        if(auto const it = layouts_.find(address); it != layouts_.end()) { return it->second; }
        auto r = access({
          InspectorAccess{address, 256, {}}
        });
        if(!r) { return {}; }
        auto layout = records::decodeRingLayout((*r)[0]);
        layouts_.emplace(address, layout);
        return layout;
    }

    void readWatches() {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> wanted;
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            for(auto const& w : state_.watches) { wanted.emplace_back(w.address, w.size); }
        }
        if(wanted.empty()) { return; }
        std::vector<InspectorAccess> reads;
        for(auto const& [a, n] : wanted) { reads.push_back({a, n, {}}); }
        auto                              r = access(reads);
        std::lock_guard<std::mutex> const lock{mutex_};
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
            auto const  demcr = readWord(Demcr, error);
            if(!demcr) {
                profileFailed(error);
                return;
            }
            if((*demcr & (1U << 24)) == 0) {
                profileFailed("DEMCR.TRCENA is clear: the DWT is off, DWT_PCSR reads nothing");
                return;
            }
            profileChecked_ = true;
        }
        std::vector<InspectorAccess> const reads(PcsrReads, InspectorAccess{DwtPcsr, 4, {}});
        auto                               r = access(reads);
        if(!r) {
            profileFailed(r.error());
            return;
        }
        std::lock_guard<std::mutex> const lock{mutex_};
        for(auto const& d : *r) {
            auto const pc = records::word(d, 0);
            if(pc == PcsrNone) {
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
        std::lock_guard<std::mutex> const lock{mutex_};
        state_.profileError = why;
        state_.profiling    = false;
        profileChecked_     = false;
        changedLater_       = true;
    }

    void rankProfileLocked() {
        std::unordered_map<MapSymbol const*, std::uint64_t> bySymbol;
        std::uint64_t                                       outside = 0;
        bool                                                allZero = !pcCounts_.empty();
        for(auto const& [pc, n] : pcCounts_) {
            if(pc != 0) { allZero = false; }
            if(auto const* s = symbols_.containing(pc)) {
                bySymbol[s] += n;
            } else {
                outside += n;
            }
        }
        state_.profileOutside = outside;
        if(allZero) {
            state_.profileError = "DWT_PCSR reads 0: this core has no DWT_PCSR (Armv6-M)";
        }
        state_.profile.clear();
        for(auto const& [s, n] : bySymbol) { state_.profile.push_back({s->name, n}); }
        std::ranges::sort(state_.profile, std::greater{}, &Snapshot::ProfileRow::samples);
        if(state_.profile.size() > 200) { state_.profile.resize(200); }
    }

    void doRaisePanic(std::uint32_t cause) {
        std::optional<std::uint32_t> raise;
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            raise = raise_;
        }
        auto const done = [&](std::string text) {
            {
                std::lock_guard<std::mutex> const lock{mutex_};
                state_.panicRaise = std::move(text);
            }
            changed();
        };
        if(!raise) {
            done("Kvasir::Panic::raise is not in the map: a firmware without Kvasir::Panic?");
            return;
        }
        std::string error;
        auto const  resume  = [&] { writeWord(Dhcsr, DbgKey | CDebugEn, error); };
        auto const  waitFor = [&](std::uint32_t bit) {
            auto const end = Clock::now() + std::chrono::seconds{1};
            while(Clock::now() < end) {
                auto const v = readWord(Dhcsr, error);
                if(!v) { return false; }
                if((*v & bit) != 0) { return true; }
            }
            return false;
        };
        if(!writeWord(Dhcsr, DbgKey | CDebugEn | CHalt, error) || !waitFor(SHalt)) {
            resume();
            done("the core did not halt (DHCSR.S_HALT) " + error);
            return;
        }
        // a DCRSR write while the core runs is ignored: S_HALT first (D1.2.34)
        if(!writeWord(Dcrsr, RegXpsr, error) || !waitFor(SRegRdy)) {
            resume();
            done("the core did not hand out xPSR (DHCSR.S_REGRDY) " + error);
            return;
        }
        auto const xpsr = readWord(Dcrdr, error);
        if(!xpsr) {
            resume();
            done("reading xPSR: " + error);
            return;
        }
        if(!writeWord(Dcrsr, RegPc, error) || !waitFor(SRegRdy)) {
            resume();
            done("the core did not hand out the PC (DHCSR.S_REGRDY) " + error);
            return;
        }
        auto const pc = readWord(Dcrdr, error);
        if(!pc) {
            resume();
            done("reading the PC: " + error);
            return;
        }
        for(auto const& step : panicRegisterWrites(*raise, cause, *xpsr, *pc)) {
            for(auto const& [address, value] : step) {
                if(!writeWord(address, value, error)) {
                    resume();
                    done("register write: " + error);
                    return;
                }
            }
            if(!waitFor(SRegRdy)) {
                resume();
                done("the core did not take a register write (DHCSR.S_REGRDY)");
                return;
            }
        }
        resume();
        done("raise("
             + std::string{cause < records::PanicCauses.size() ? records::PanicCauses[cause] : "?"}
             + ") called; the next boot line names it");
        recordsDue_ = true;
    }

    void clearDone(std::string text) {
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            state_.recordsCleared = std::move(text);
        }
        changed();
    }

    void doClearRecords() {
        std::optional<std::uint32_t>                           fault;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> panic;
        {
            std::lock_guard<std::mutex> const lock{mutex_};
            fault = fault_;
            panic = panic_;
        }
        // Read first: only a word that holds a record's magic is written - with the map of
        // another image the address is anything - and what goes is said before it is gone.
        std::vector<InspectorAccess> reads;
        if(fault) { reads.push_back({*fault, records::FaultSize, {}}); }
        if(panic) { reads.push_back({panic->first, panic->second, {}}); }
        if(reads.empty()) {
            clearDone("no fault or panic record in this image");
            return;
        }
        auto r = access(reads);
        if(!r) {
            clearDone("not cleared: " + r.error());
            return;
        }
        std::vector<InspectorAccess> writes;
        std::vector<std::string>     gone;
        std::size_t                  i = 0;
        if(fault) {
            if(auto const f = records::decodeFault((*r)[i++])) {
                writes.push_back({*fault, 4, 0U});
                std::lock_guard<std::mutex> const lock{mutex_};
                gone.push_back(std::format(
                  "fault record cleared from the TUI: {} fault(s), the first in {}, pc={:#010x} "
                  "{} lr={:#010x} {} xpsr={:#x} exc_return={:#x}",
                  f->count,
                  records::exceptionName(f->exception()),
                  f->pc,
                  describeLocked(f->pc),
                  f->lr,
                  describeLocked(f->lr),
                  f->xpsr,
                  f->excReturn));
            }
        }
        if(panic) {
            if(auto const p = records::decodePanic((*r)[i])) {
                writes.push_back({panic->first, 4, 0U});
                auto const                        site = p->site();
                std::lock_guard<std::mutex> const lock{mutex_};
                gone.push_back(
                  std::format("panic record cleared from the TUI: {} panic(s), the first: {}, {} "
                              "detail={}",
                              p->count,
                              p->causeName(),
                              site ? std::format("site={:#010x} {}", *site, describeLocked(*site))
                                   : std::string{"no program site"},
                              p->detail.value_or(0)));
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
                clearDone("not cleared: the record's magic read back unchanged");
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

    void run(std::stop_token st) {
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
                loadMapIfChanged(false);
            }
            if(hooks_.sessions) {
                auto const s = hooks_.sessions();
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
                    std::lock_guard<std::mutex> const lock{mutex_};
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

    mutable std::mutex              mutex_;   // guards everything below up to the atomics
    Snapshot                        state_;
    MapSymbols                      symbols_;
    std::string                     mapPath_;
    std::filesystem::file_time_type mapStamp_{};
    bool                            loaded_{};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> ubsan_;
    std::optional<std::uint32_t>                           fault_;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> panic_;
    std::optional<std::uint32_t>                           raise_;
    std::map<std::uint32_t, std::uint32_t>                 ringSizes_;
    std::map<std::uint32_t, std::uint64_t>                 pcCounts_;
    Clock::time_point                                      lastRank_{};

    // the worker's own
    std::map<std::uint32_t, records::RingLayout> layouts_;

    std::atomic<Page>          page_{Page::none};
    std::atomic<bool>          refreshAll_{true};
    std::atomic<bool>          recordsDue_{true};
    std::atomic<bool>          stacksDue_{true};
    std::atomic<bool>          changedLater_{false};
    std::atomic<std::int64_t>  panicCause_{-1};
    std::atomic<bool>          clearDue_{false};
    std::atomic<bool>          profileChecked_{false};
    std::atomic<std::uint64_t> version_{0};

    std::mutex                  wakeMutex_;
    std::condition_variable_any wake_;
    bool                        woken_{};

    std::jthread thread_;   // last: started after every member above exists
};

}   // namespace uc_log::detail
