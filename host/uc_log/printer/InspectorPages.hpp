#pragma once

// The inspector's pages of a window, for any front end: health (the sanitizer count, the last halt, the fault and
// panic records, the stacks; read now, clear, raise a panic), the trace rings, the watches (a data symbol picked by
// part of its name, its value read a few times a second) and the profile - as panel rows from the inspector's
// Snapshot, boxes of rows whose buttons carry a WindowAction (a PageForm). `now` says what changed just now: no clock
// is read here.

#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/InspectorText.hpp"
#include "uc_log/printer/LogScreen.hpp"
#include "uc_log/printer/PageForm.hpp"
#include "uc_log/printer/PanelRows.hpp"
#include "uc_log/printer/PrinterPages.hpp"
#include "uc_log/printer/StatusBadges.hpp"
#include "uc_log/printer/TargetInspector.hpp"
#include "uc_log/printer/TargetRecords.hpp"
#include "uc_log/printer/Tone.hpp"
#include "uc_log/printer/WindowActions.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

using Inspection = TargetInspector::Snapshot;

namespace Detail {
    inline PanelRow note(std::string text) { return {label(std::move(text))}; }

    inline PanelRow addressRow(std::string_view   name,
                               std::uint32_t      value,
                               std::string const& where) {
        return {
          label(fmt::format("{:<6}", name)),
          toned(Tone::warn, fmt::format("{:#010x}  ", value)),
          PanelPart{RunKind::function, 0, {}, shortName(where)}
        };
    }

    inline PanelRow mapRow(Inspection const& s) {
        if(!s.mapError.empty()) { return {toned(Tone::bad, "map: " + s.mapError)}; }
        return note(fmt::format("symbols from the linker map: {} ({}){}",
                                s.symbolCount,
                                s.lldMap ? "lld" : "GNU ld",
                                s.lldMap ? "" : " - GNU ld maps lack sizes for some symbols"));
    }
}   // namespace Detail

/// The health page's choices: the panic cause to raise, and whether the first click armed it.
struct HealthChoice {
    std::uint32_t panicCause{};
    bool          panicArmed{};
};

inline Page healthForm(Inspection const&   s,
                       bool                haltedNow,
                       HealthChoice const& choice) {
    using namespace Detail;
    Page  p;
    auto& records = p.box("🩺 Records", true);
    records.push_back(mapRow(s));
    if(!s.recordsCleared.empty()) {
        records.push_back(note("clear the records: " + s.recordsCleared));
    }
    records.push_back(note("a record no boot line takes keeps PANIC / FAULT in the status line"));
    p.tool("🔄 Read now", WindowAction{action::ReadRecords{}});
    p.tool("🧹 Clear the records", WindowAction{action::ClearRecords{}}, ButtonLook::danger);

    auto& ub = p.box("🧯 Sanitizer reports");
    if(!s.haveUbsan) {
        ub.push_back(note("no Kvasir::Ubsan::ubsanReports: not a sanitize build"));
    } else if(!s.ubsan) {
        ub.push_back(note("not read yet"));
    } else if(s.ubsan->count == 0) {
        ub.push_back({toned(Tone::ok, "0 since boot")});
    } else {
        ub.push_back({toned(Tone::bad, fmt::format("{} since boot", s.ubsan->count))});
        ub.push_back(addressRow("last", s.ubsan->lastReturnAddress, s.ubsanWhere));
    }

    auto& halt   = p.box("⏸ The last halt");
    auto  wayOut = [&] {
        if(!haltedNow) { return; }
        halt.push_back(note(
          "to leave it: r resets the target (the next boot line reports the panic or fault);"));
        halt.push_back(note(
          "go on the debugger page continues, but a panic's or fault's halt is a breakpoint loop"));
    };
    if(!s.halt) {
        halt.push_back(
          note(haltedNow ? "halted, no capture yet" : "none seen since the printer started"));
        wayOut();
    } else {
        auto const& h = *s.halt;
        halt.push_back({toned(haltedNow ? Tone::bad : Tone::warn,
                              fmt::format("{:%H:%M:%S} in {}{}",
                                          std::chrono::floor<std::chrono::seconds>(h.time),
                                          records::exceptionName(h.capture.exception()),
                                          haltedNow ? " - still halted" : " - running again"))});
        halt.push_back(addressRow("pc", h.capture.pc, h.pcWhere));
        halt.push_back(addressRow("lr", h.capture.lr, h.lrWhere));
        std::string others;
        for(auto const& [n, v] : h.capture.registers()) {
            if(n == "pc" || n == "lr") { continue; }
            others += fmt::format("{}={:#x} ", n, v);
        }
        halt.push_back(note(others));
        if(!h.codeWords.empty()) {
            halt.push_back(note("words on the stack that point into code:"));
            for(auto const& w : h.codeWords) {
                halt.push_back(addressRow(fmt::format("sp+{}", w.offset), w.value, w.where));
            }
        }
        wayOut();
    }

    auto& fault = p.box("📕 Fault record");
    if(!s.haveFault) {
        fault.push_back(note("not in this image (no Fault::HandlerFor)"));
    } else if(!s.fault) {
        fault.push_back({toned(Tone::ok, "empty: no fault since a boot line reported one")});
    } else {
        auto const& f = *s.fault;
        fault.push_back({toned(Tone::bad,
                               fmt::format("{} fault(s), the first in {} on core {}",
                                           f.count,
                                           records::exceptionName(f.exception()),
                                           f.core()))});
        if(f.hasFrame()) {
            fault.push_back(addressRow("pc", f.pc, s.faultPc));
            fault.push_back(addressRow("lr", f.lr, s.faultLr));
            fault.push_back(
              note(fmt::format("r0={:#x} r1={:#x} r2={:#x} r3={:#x} r12={:#x} sp={:#010x} "
                               "xpsr={:#x} exc_return={:#x}",
                               f.r[0],
                               f.r[1],
                               f.r[2],
                               f.r[3],
                               f.r12,
                               f.sp,
                               f.xpsr,
                               f.excReturn)));
        } else {
            fault.push_back(
              note(fmt::format("no stacked frame (not pushed, or outside RAM): exc_return={:#x} "
                               "msp={:#010x} psp={:#010x}",
                               f.excReturn,
                               f.msp,
                               f.psp)));
        }
        fault.push_back(
          note(fmt::format("cfsr={:#010x} ({}) hfsr={:#010x} mmfar={:#010x} bfar={:#010x}",
                           f.cfsr,
                           records::cfsrText(f.cfsr),
                           f.hfsr,
                           f.mmfar,
                           f.bfar)));
    }

    auto& panic = p.box("🚨 Panic record");
    if(!s.havePanic) {
        panic.push_back(note("not in this image"));
    } else if(!s.panic) {
        panic.push_back({toned(Tone::ok, "empty: no panic since a boot line reported one")});
    } else {
        auto const& r = *s.panic;
        std::string extra;
        if(r.causeName() == "unhandled interrupt") {
            extra
              = fmt::format(", exception {} = IRQ {}", r.detail, static_cast<int>(r.detail) - 16);
        } else if(r.detail != 0) {
            extra = fmt::format(", detail {}", r.detail);
        }
        if(r.timeUs() != 0) { extra += fmt::format(", at {} us", r.timeUs()); }
        panic.push_back({toned(Tone::bad,
                               fmt::format("{} panic(s), the first: {}{} on core {}",
                                           r.count,
                                           r.causeName(),
                                           extra,
                                           r.core))});
        if(auto const site = r.site()) {
            panic.push_back(addressRow("site", *site, s.panicSite));
        } else {
            panic.push_back(note("no program site"));
        }
    }
    if(!s.panicRaise.empty()) { panic.push_back(note("raise: " + s.panicRaise)); }

    auto& stacks = p.box("📚 Stacks", true);
    if(s.stacks.empty()) { stacks.push_back(note("no _LINKER_stack_start_/_end_ in the map")); }
    for(auto const& st : s.stacks) {
        auto const size = st.high - st.low;
        if(!st.use) {
            stacks.push_back(
              note(fmt::format("{:<13} {:#010x}..{:#010x} {:>7} B  {}",
                               st.name,
                               st.low,
                               st.high,
                               size,
                               st.error.empty() ? std::string{"not measured yet"} : st.error)));
            continue;
        }
        auto const ratio
          = static_cast<double>(st.use->used) / static_cast<double>(std::max(size, 1U));
        auto const tone = ratio > 0.9 ? Tone::bad : ratio > 0.7 ? Tone::warn : Tone::ok;
        stacks.push_back({label(fmt::format("{:<13} ", st.name)),
                          toned(tone, gaugeText(ratio, 20)),
                          label(fmt::format(" {:5.1f} %  {} of {} B used, {} B never touched",
                                            100.0 * ratio,
                                            st.use->used,
                                            size,
                                            st.use->never))});
    }

    // raising a panic: the cause, then a first click that arms and a second that raises
    auto&    raise = p.box("🚨 Raise a panic", true);
    PanelRow causes;
    for(std::size_t j = 0; j < records::panicCauses.size(); ++j) {
        causes.push_back(p.option(records::panicCauses[j],
                                  choice.panicCause == j,
                                  WindowAction{action::PanicCause{static_cast<std::uint32_t>(j)}}));
    }
    raise.push_back(std::move(causes));
    if(choice.panicArmed) {
        raise.push_back(
          {p.button("🚨 Raise it now",
                    WindowAction{action::RaisePanic{choice.panicCause}},
                    ButtonLook::danger),
           toned(Tone::bad, " the core calls Kvasir::Panic::raise() - it resets or halts")});
    } else {
        raise.push_back(
          {p.button("🚨 Raise a panic …", WindowAction{action::ArmPanic{}}, ButtonLook::danger)});
    }
    return p;
}

/// The trace page's choices.
struct TraceChoice {
    bool        delta{false};   // a column with the first field's step from the row before
    bool        hex{false};
    std::size_t rows{64};   // a ring's last rows shown
};

inline Page traceForm(Inspection const&  s,
                      TraceChoice const& choice) {
    using namespace Detail;
    Page  p;
    auto& how = p.box("🧵 Trace rings", true);
    how.push_back({p.check("Δ the first field", choice.delta, WindowAction{action::TraceDelta{}}),
                   p.check("hex", choice.hex, WindowAction{action::TraceHex{}}),
                   p.button("✎ Rows …", WindowAction{action::Edit{Field::traceRows}}),
                   label(fmt::format(" the last {} rows of a ring", choice.rows))});
    how.push_back(mapRow(s));
    if(s.rings.empty()) {
        how.push_back(note("no Kvasir::Trace::Ring in this firmware (kvasir/Util/Trace.hpp)"));
        return p;
    }
    for(auto const& r : s.rings) {
        if(!r.error.empty()) {
            auto& ring = p.box(fmt::format("ring at {:#010x}", r.address), true);
            ring.push_back({toned(Tone::bad, r.error)});
            continue;
        }
        if(r.capacity == 0) {
            auto& ring = p.box(shortName(r.name, 60), true);
            ring.push_back(note("not read yet"));
            continue;
        }
        auto const shown = std::min(r.rows.size(), choice.rows);
        auto&      ring
          = p.box(fmt::format("{}: {} written, capacity {}, the last {}{}",
                              shortName(r.name, 60),
                              r.count,
                              r.capacity,
                              shown,
                              r.writtenWhileReading != 0
                                ? fmt::format(" ({} more while reading)", r.writtenWhileReading)
                                : std::string{}),
                  true);
        std::vector<std::vector<std::string>> cells;
        std::vector<std::string>              head{"#"};
        for(auto const& f : r.fields) { head.push_back(f); }
        if(choice.delta) {
            head.push_back("Δ " + (r.fields.empty() ? std::string{} : r.fields.front()));
        }
        cells.push_back(std::move(head));
        auto const                   first = r.rows.size() - shown;
        std::optional<std::uint32_t> previous;
        if(first > 0) {
            previous = r.rows[first - 1].values.empty() ? 0 : r.rows[first - 1].values[0];
        }
        for(std::size_t i = first; i != r.rows.size(); ++i) {
            auto const&              row = r.rows[i];
            std::vector<std::string> line{std::to_string(row.index)};
            for(auto v : row.values) {
                line.push_back(choice.hex ? fmt::format("{:#x}", v) : std::to_string(v));
            }
            if(choice.delta) {
                line.push_back(previous && !row.values.empty()
                                 ? fmt::format("+{}", row.values[0] - *previous)
                                 : std::string{});
            }
            if(!row.values.empty()) { previous = row.values[0]; }
            cells.push_back(std::move(line));
        }
        std::vector<std::size_t> widths(cells.front().size(), 0);
        for(auto const& line : cells) {
            for(std::size_t k = 0; k != line.size() && k != widths.size(); ++k) {
                widths[k] = std::max(widths[k], line[k].size());
            }
        }
        for(std::size_t li = 0; li != cells.size(); ++li) {
            std::string text;
            for(std::size_t k = 0; k != cells[li].size() && k != widths.size(); ++k) {
                text += fmt::format("{:>{}}  ", cells[li][k], widths[k]);
            }
            ring.push_back({
              li == 0 ? toned(Tone::warn, std::move(text))
                      : PanelPart{RunKind::message, 0, {}, std::move(text)}
            });
        }
    }
    return p;
}

/// The watch page: the button that asks for a symbol (its matches are the dialog's choices: symbolChoices), the
/// watches and their values.
inline Page watchForm(Inspection const& s,
                      Clock::time_point now) {
    using namespace Detail;
    Page p;
    p.tool("➕ Watch …", WindowAction{action::Edit{Field::watch}}, ButtonLook::go);
    auto& watches = p.box(fmt::format("📌 Watches · {}", s.watches.size()), true);
    watches.push_back(
      note("a data symbol by part of its name; untyped: 1, 2, 4 or 8 bytes as numbers, else bytes "
           "(the first 64), read 5x a second"));
    if(s.watches.empty()) {
        watches.push_back(
          note("none yet: ➕ Watch … above, part of a symbol's name, then one of the matches"));
    }
    for(auto const& w : s.watches) {
        bool const recent = w.changes != 0 && now - w.changed < std::chrono::seconds{1};
        watches.push_back({
          p.button("✖", WindowAction{action::Unwatch{w.name}},
           ButtonLook::danger),
          PanelPart{RunKind::function, 0, {}, shortName(w.name, 80)},
          label(fmt::format("  @{:#010x}, {} B{}",
                            w.address,
                            w.symbolSize,
                            w.symbolSize > w.size ? fmt::format(" (the first {} shown)", w.size)
                                                  : std::string{}
           ))
        });
        if(!w.error.empty()) {
            watches.push_back({label("    "), toned(Tone::bad, w.error)});
        } else {
            watches.push_back({label("    "),
                               toned(recent ? Tone::warn : Tone::info, watchValue(w.value)),
                               label(fmt::format("   {} change(s)", w.changes))});
        }
    }
    return p;
}

inline Page profileForm(Inspection const& s,
                        std::size_t       maxRows) {
    using namespace Detail;
    Page       p;
    auto const total = s.profileSamples;
    p.tool(s.profiling ? "■ Stop" : "▶ Start",
           WindowAction{action::Profile{}},
           s.profiling ? ButtonLook::danger : ButtonLook::go);
    p.tool("🗑 Clear", WindowAction{action::ClearProfile{}});
    auto& head = p.box("⏱ Profile", true);
    head.push_back(
      note("self time by function of the map (DWT_PCSR); inline frames and source lines: "
           "kvasir_bench.py profile"));
    head.push_back({toned(
      s.profiling ? Tone::ok : Tone::plain,
      fmt::format(
        "{} {} samples over {:.1f} s ({:.0f}/s){}{}",
        s.profiling ? (total == 0 ? "● waiting for the target," : "● sampling,") : "○ stopped,",
        total,
        s.profileSeconds,
        s.profileSeconds > 0 ? static_cast<double>(total) / s.profileSeconds : 0.0,
        s.profileNone != 0 ? fmt::format(", {} without a PC (halted / Secure)", s.profileNone)
                           : std::string{},
        s.profileOutside != 0 ? fmt::format(", {} in no function of the map", s.profileOutside)
                              : std::string{}))});
    if(!s.profileError.empty()) { head.push_back({toned(Tone::bad, s.profileError)}); }
    if(s.profile.empty()) { return p; }
    auto& top = p.box("🔥 Where the time goes", true);
    for(std::size_t i = 0; i != std::min(s.profile.size(), maxRows); ++i) {
        auto const& r = s.profile[i];
        auto const  pct
          = total != 0 ? 100.0 * static_cast<double>(r.samples) / static_cast<double>(total) : 0.0;
        top.push_back({
          toned(Tone::info, fmt::format("{:6.1f} % ", pct)),
          toned(Tone::info, gaugeText(pct / 100.0, 12)),
          label("  "),
          PanelPart{RunKind::function, 0, {}, shortName(r.name)}
        });
    }
    return p;
}

}   // namespace uc_log::printer
