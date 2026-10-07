#pragma once

#include "uc_log/detail/TargetInspector.hpp"
#include "uc_log/theme.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <ftxui/dom/elements.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The TUI's views of TargetInspector::Snapshot: pure functions from the snapshot to elements,
/// the components around them (buttons, inputs) live in FTXUIGui.hpp.
namespace uc_log::FTXUIGui::inspector {

using Snapshot = uc_log::detail::TargetInspector::Snapshot;

/// Template arguments nested deeper than one level become <…> once a name is longer than
/// `max` - a trace ring's or a driver's type runs to kilobytes.
inline std::string shortName(std::string_view name,
                             std::size_t      max = 100) {
    if(name.size() <= max) { return std::string{name}; }
    std::string out;
    int         depth = 0;
    for(char c : name) {
        if(c == '<') {
            ++depth;
            if(depth == 2) {
                out += "<…>";
                continue;
            }
        } else if(c == '>') {
            --depth;
            if(depth == 1) { continue; }
        }
        if(depth < 2) { out += c; }
    }
    if(out.size() > max + 20) {
        // the last component says which variable or function it is: the cut goes before it
        auto const leaf = std::string{out.substr(uc_log::detail::symbolLeafStart(out))};
        bool const keep = leaf.size() != out.size() && leaf.size() + 12 <= max;
        out.resize(keep ? max - leaf.size() - 2 : max);
        // not through the middle of a "…" put in above
        while(!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80U) != 0) {
            out.pop_back();
        }
        out += keep ? "…::" + leaf : std::string{"…"};
    }
    return out;
}

inline ftxui::Element header(std::string_view title) {
    return ftxui::text(std::string{title}) | ftxui::bold | ftxui::color(Theme::Header::accent());
}

inline ftxui::Element dim(std::string const& s) {
    return ftxui::text(s) | ftxui::color(Theme::Text::metadata());
}

inline ftxui::Element addressLine(std::string_view   label,
                                  std::uint32_t      value,
                                  std::string const& where) {
    return ftxui::hbox(
      {ftxui::text(fmt::format("  {:<6}", label)) | ftxui::bold,
       ftxui::text(fmt::format("{:#010x}  ", value)) | ftxui::color(Theme::Data::value()),
       ftxui::text(shortName(where)) | ftxui::color(Theme::Text::functionName())});
}

inline ftxui::Element mapLine(Snapshot const& s) {
    if(!s.mapError.empty()) {
        return ftxui::text("map: " + s.mapError) | ftxui::color(Theme::Status::error());
    }
    return dim(fmt::format("symbols from the linker map: {} ({}){}",
                           s.symbolCount,
                           s.lldMap ? "lld" : "GNU ld",
                           s.lldMap ? "" : " - GNU ld maps lack sizes for some symbols"));
}

inline ftxui::Element ubsanSection(Snapshot const& s) {
    std::vector<ftxui::Element> rows{header("🧯 Sanitizer reports (kvasir_bench.py ub)")};
    if(!s.haveUbsan) {
        rows.push_back(dim("  no Kvasir::Ubsan::ubsanReports: not a sanitize build"));
    } else if(!s.ubsan) {
        rows.push_back(dim("  not read yet"));
    } else if(s.ubsan->count == 0) {
        rows.push_back(ftxui::text("  0 since boot") | ftxui::color(Theme::Status::success()));
    } else {
        rows.push_back(ftxui::text(fmt::format("  {} since boot", s.ubsan->count)) | ftxui::bold
                       | ftxui::color(Theme::Status::error()));
        rows.push_back(addressLine("last", s.ubsan->lastReturnAddress, s.ubsanWhere));
    }
    return ftxui::vbox(std::move(rows));
}

inline ftxui::Element haltSection(Snapshot const& s,
                                  bool            haltedNow) {
    std::vector<ftxui::Element> rows{header("⏸ Last halt (kvasir_bench.py crash)")};
    // Kvasir::Panic::halt and the fault handler end in `while(true) bkpt`: Go stops on the next one
    auto const wayOut = [&] {
        if(!haltedNow) { return; }
        rows.push_back(
          dim("  to leave the halt: [r] resets the target (the next boot line reports "
              "the panic or fault);"));
        rows.push_back(
          dim("  Go on the Debugger tab (5) continues, but a panic's or fault's halt is "
              "a breakpoint loop"));
        rows.push_back(dim("  and stops again at once"));
    };
    if(!s.halt) {
        rows.push_back(
          dim(haltedNow ? "  halted, no capture yet" : "  none seen since the printer started"));
        wayOut();
        return ftxui::vbox(std::move(rows));
    }
    auto const& h = *s.halt;
    rows.push_back(ftxui::text(fmt::format("  {:%H:%M:%S} in {}{}",
                                           std::chrono::floor<std::chrono::seconds>(h.time),
                                           uc_log::detail::records::exceptionName(h.exception),
                                           haltedNow ? " - still halted" : " - running again"))
                   | ftxui::color(haltedNow ? Theme::Status::error() : Theme::Status::warning()));
    rows.push_back(
      addressLine("pc", uc_log::detail::TargetInspector::registerOf(h, "pc"), h.pcWhere));
    rows.push_back(
      addressLine("lr", uc_log::detail::TargetInspector::registerOf(h, "lr"), h.lrWhere));
    std::string others;
    for(auto const& [n, v] : h.registers) {
        if(n == "pc" || n == "lr") { continue; }
        others += fmt::format("{}={:#x} ", n, v);
    }
    rows.push_back(dim("  " + others));
    if(!h.codeWords.empty()) {
        rows.push_back(dim("  words on the stack that point into code:"));
        for(auto const& w : h.codeWords) {
            rows.push_back(addressLine(fmt::format("sp+{}", w.offset), w.value, w.where));
        }
    }
    wayOut();
    return ftxui::vbox(std::move(rows));
}

inline ftxui::Element faultSection(Snapshot const& s) {
    std::vector<ftxui::Element> rows{header("📕 Fault record (Kvasir::Fault::lastFault)")};
    if(!s.haveFault) {
        rows.push_back(dim("  not in this image (no Fault::Handler)"));
    } else if(!s.fault) {
        rows.push_back(ftxui::text("  empty: no fault since a boot line reported one")
                       | ftxui::color(Theme::Status::success()));
    } else {
        auto const& f = *s.fault;
        rows.push_back(
          ftxui::text(fmt::format("  {} fault(s), the first in {}",
                                  f.count,
                                  uc_log::detail::records::exceptionName(f.exception())))
          | ftxui::bold | ftxui::color(Theme::Status::error()));
        rows.push_back(addressLine("pc", f.pc, s.faultPc));
        rows.push_back(addressLine("lr", f.lr, s.faultLr));
        rows.push_back(dim(
          fmt::format("  r0={:#x} r1={:#x} r2={:#x} r3={:#x} r12={:#x} xpsr={:#x} exc_return={:#x}",
                      f.r[0],
                      f.r[1],
                      f.r[2],
                      f.r[3],
                      f.r12,
                      f.xpsr,
                      f.excReturn)));
    }
    return ftxui::vbox(std::move(rows));
}

inline ftxui::Element panicSection(Snapshot const& s) {
    std::vector<ftxui::Element> rows{header("🚨 Panic record (Kvasir::Panic::lastPanic)")};
    if(!s.havePanic) {
        rows.push_back(dim("  not in this image"));
    } else if(!s.panic) {
        rows.push_back(ftxui::text("  empty: no panic since a boot line reported one")
                       | ftxui::color(Theme::Status::success()));
    } else {
        auto const& p = *s.panic;
        std::string extra;
        if(p.detail && p.causeName() == "unhandled interrupt") {
            extra
              = fmt::format(", exception {} = IRQ {}", *p.detail, static_cast<int>(*p.detail) - 16);
        } else if(p.detail && *p.detail != 0) {
            extra = fmt::format(", detail {}", *p.detail);
        }
        rows.push_back(
          ftxui::text(fmt::format("  {} panic(s), the first: {}{}", p.count, p.causeName(), extra))
          | ftxui::bold | ftxui::color(Theme::Status::error()));
        if(auto const site = p.site()) {
            rows.push_back(addressLine("site", *site, s.panicSite));
        } else {
            rows.push_back(dim("  no program site"));
        }
    }
    if(!s.panicRaise.empty()) { rows.push_back(dim("  raise: " + s.panicRaise)); }
    return ftxui::vbox(std::move(rows));
}

inline ftxui::Element stacksSection(Snapshot const& s) {
    std::vector<ftxui::Element> rows{header("📚 Stacks (kvasir_bench.py stack)")};
    if(s.stacks.empty()) { rows.push_back(dim("  no _LINKER_stack_start_/_end_ in the map")); }
    for(auto const& st : s.stacks) {
        auto const size = st.high - st.low;
        if(!st.use) {
            rows.push_back(ftxui::text(fmt::format("  {:<13} {:#010x}..{:#010x} {:>7} B  ",
                                                   st.name,
                                                   st.low,
                                                   st.high,
                                                   size))
                           | ftxui::color(Theme::Text::normal()));
            rows.push_back(
              dim("    " + (st.error.empty() ? std::string{"not measured yet"} : st.error)));
            continue;
        }
        auto const ratio
          = static_cast<float>(st.use->used) / static_cast<float>(std::max(size, 1U));
        auto const color = ratio > 0.9F ? Theme::Status::error()
                         : ratio > 0.7F ? Theme::Status::warning()
                                        : Theme::Status::success();
        rows.push_back(ftxui::hbox(
          {ftxui::text(fmt::format("  {:<13} ", st.name)),
           ftxui::gauge(ratio) | ftxui::color(color) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 20),
           ftxui::text(fmt::format(" {:5.1f} %  {} of {} B used, {} B never touched",
                                   100.0F * ratio,
                                   st.use->used,
                                   size,
                                   st.use->never))}));
    }
    return ftxui::vbox(std::move(rows));
}

inline ftxui::Element healthElement(Snapshot const& s,
                                    bool            haltedNow) {
    return ftxui::vbox(
      {mapLine(s),
       s.recordsCleared.empty() ? ftxui::text("") : dim("clear records: " + s.recordsCleared),
       ubsanSection(s),
       ftxui::text(""),
       haltSection(s, haltedNow),
       ftxui::text(""),
       faultSection(s),
       ftxui::text(""),
       panicSection(s),
       ftxui::text(""),
       stacksSection(s)});
}

inline ftxui::Element ringsElement(Snapshot const& s,
                                   bool            delta,
                                   bool            hex,
                                   std::size_t     maxRows) {
    if(s.rings.empty()) {
        return ftxui::vbox(
          {mapLine(s), dim("no Kvasir::Trace::Ring in this firmware (kvasir/Util/Trace.hpp)")});
    }
    std::vector<ftxui::Element> out{mapLine(s)};
    for(auto const& r : s.rings) {
        out.push_back(ftxui::text(""));
        if(!r.error.empty()) {
            out.push_back(header(fmt::format("🧵 ring at {:#010x}", r.address)));
            out.push_back(ftxui::text("  " + r.error) | ftxui::color(Theme::Status::error()));
            continue;
        }
        if(r.capacity == 0) {
            out.push_back(header("🧵 " + shortName(r.name)));
            out.push_back(dim("  not read yet"));
            continue;
        }
        auto const shown = std::min(r.rows.size(), maxRows);
        out.push_back(
          header(fmt::format("🧵 '{}': {} written, capacity {}, last {} shown{}",
                             r.name,
                             r.count,
                             r.capacity,
                             shown,
                             r.writtenWhileReading != 0
                               ? fmt::format(" ({} more while reading)", r.writtenWhileReading)
                               : std::string{})));
        std::vector<std::vector<std::string>> cells;
        std::vector<std::string>              head{"#"};
        for(auto const& f : r.fields) { head.push_back(f); }
        if(delta) { head.push_back("Δ " + (r.fields.empty() ? std::string{} : r.fields.front())); }
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
                line.push_back(hex ? fmt::format("{:#x}", v) : std::to_string(v));
            }
            if(delta) {
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
            std::string text = "  ";
            for(std::size_t k = 0; k != cells[li].size() && k != widths.size(); ++k) {
                text += fmt::format("{:>{}}  ", cells[li][k], widths[k]);
            }
            auto e = ftxui::text(text);
            out.push_back(li == 0 ? (e | ftxui::bold) : e);
        }
    }
    return ftxui::vbox(std::move(out));
}

/// A watched value as numbers: by its size, little-endian.
inline std::string watchValue(std::vector<std::byte> const& v) {
    if(v.empty()) { return "-"; }
    std::uint64_t u{};
    for(std::size_t i = 0; i != std::min<std::size_t>(v.size(), 8); ++i) {
        u |= std::to_integer<std::uint64_t>(v[i]) << (8 * i);
    }
    switch(v.size()) {
    case 1: return fmt::format("{} / {} / {:#04x}", u, static_cast<std::int8_t>(u), u);
    case 2: return fmt::format("{} / {} / {:#06x}", u, static_cast<std::int16_t>(u), u);
    case 4:
        {
            float f{};
            auto  w = static_cast<std::uint32_t>(u);
            std::memcpy(&f, &w, 4);
            return fmt::format("{} / {} / {:#010x} / {:g}f", u, static_cast<std::int32_t>(u), u, f);
        }
    case 8:  return fmt::format("{} / {} / {:#018x}", u, static_cast<std::int64_t>(u), u);
    default: break;
    }
    std::string hex;
    for(auto b : v) { hex += fmt::format("{:02x} ", std::to_integer<unsigned>(b)); }
    return hex;
}

/// A symbol's name with the typed terms marked in it: the scope in one colour, the last
/// component in another. `plain` leaves the colours out (a row drawn inverted).
inline ftxui::Element matchedName(std::string_view                name,
                                  std::vector<std::string> const& lowerTerms,
                                  bool                            plain,
                                  std::size_t                     max = 100) {
    auto const        shown = shortName(name, max);
    auto const        lower = uc_log::detail::asciiLower(shown);
    std::vector<bool> hit(shown.size(), false);
    for(auto const& term : lowerTerms) {
        if(term.empty()) { continue; }
        for(auto pos = lower.find(term); pos != std::string::npos;
            pos      = lower.find(term, pos + term.size()))
        {
            std::fill_n(hit.begin() + static_cast<std::ptrdiff_t>(pos), term.size(), true);
        }
    }
    auto const                  leaf = uc_log::detail::symbolLeafStart(shown);
    std::vector<ftxui::Element> parts;
    for(std::size_t i = 0; i != shown.size();) {
        std::size_t j = i;
        while(j != shown.size() && hit[j] == hit[i] && (j >= leaf) == (i >= leaf)) { ++j; }
        auto part = ftxui::text(shown.substr(i, j - i));
        if(!plain) {
            part = part | ftxui::color(i >= leaf ? Theme::Data::name() : Theme::Data::scope());
        }
        if(hit[i]) { part = part | ftxui::bold | ftxui::underlined; }
        parts.push_back(std::move(part));
        i = j;
    }
    return ftxui::hbox(std::move(parts));
}

/// One line of the watch input's pick list: a data symbol the typed text matches.
inline ftxui::Element completionRow(uc_log::detail::TargetInspector::Completion const& c,
                                    std::vector<std::string> const&                    lowerTerms,
                                    bool                                               watched,
                                    bool                                               selected) {
    auto const meta
      = [&](std::string const& text) { return selected ? ftxui::text(text) : dim(text); };
    auto mark = ftxui::text(watched ? "✓ " : "  ");
    if(!selected) { mark = mark | ftxui::color(Theme::Status::success()); }
    auto row = ftxui::hbox({ftxui::text(selected ? "▶ " : "  "),
                            std::move(mark),
                            meta(fmt::format("{:>6} B  ", c.size)),
                            matchedName(c.name, lowerTerms, selected),
                            meta(fmt::format("  @{:#010x}", c.address)),
                            ftxui::filler()});
    return selected ? row | ftxui::inverted : row;
}

/// Rows of watchRow(): the name and the value (or the error).
inline constexpr int WatchRows = 2;
/// A click in the first columns of a watch's name row removes it: the ✖ drawn there.
inline constexpr int WatchRemoveColumns = 3;

inline ftxui::Element watchRow(Snapshot::Watch const& w,
                               bool                   selected,
                               bool                   hovered,
                               bool                   focused) {
    auto const now    = std::chrono::steady_clock::now();
    bool const recent = w.changes != 0 && now - w.changed < std::chrono::seconds{1};
    auto       name   = ftxui::text(shortName(w.name, 80)) | ftxui::color(Theme::Data::name());
    if(selected && focused) { name = name | ftxui::inverted; }
    if(hovered) { name = name | ftxui::underlined; }
    auto head = ftxui::hbox(
      {ftxui::text(selected || hovered ? " ✖ " : "   ") | ftxui::color(Theme::UI::remove()),
       ftxui::text(selected ? "▶ " : "  "),
       std::move(name),
       dim(fmt::format("  @{:#010x}, {} B{}",
                       w.address,
                       w.symbolSize,
                       w.symbolSize > w.size ? fmt::format(" (first {} shown)", w.size)
                                             : std::string{}))});
    auto value
      = !w.error.empty()
        ? ftxui::text("       " + w.error) | ftxui::color(Theme::Status::error())
        : ftxui::hbox({ftxui::text("       " + watchValue(w.value)) | ftxui::bold
                         | ftxui::color(recent ? Theme::Status::warning() : Theme::Data::value()),
                       dim(fmt::format("   {} change(s)", w.changes))});
    return ftxui::vbox({std::move(head), std::move(value)});
}

inline ftxui::Element profileElement(Snapshot const& s,
                                     std::size_t     maxRows) {
    std::vector<ftxui::Element> out;
    auto const                  total = s.profileSamples;
    out.push_back(
      ftxui::text(fmt::format(
        "{} {} samples over {:.1f} s ({:.0f}/s){}{}",
        s.profiling ? (total == 0 ? "● waiting for the target," : "● sampling,") : "○ stopped,",
        total,
        s.profileSeconds,
        s.profileSeconds > 0 ? static_cast<double>(total) / s.profileSeconds : 0.0,
        s.profileNone != 0 ? fmt::format(", {} without a PC (halted / Secure)", s.profileNone)
                           : std::string{},
        s.profileOutside != 0 ? fmt::format(", {} in no function of the map", s.profileOutside)
                              : std::string{}))
      | ftxui::color(s.profiling ? Theme::Status::active() : Theme::Text::normal()));
    if(!s.profileError.empty()) {
        out.push_back(ftxui::text(s.profileError) | ftxui::color(Theme::Status::error()));
    }
    out.push_back(
      dim("self time by function of the map (DWT_PCSR); inline frames and source lines: "
          "kvasir_bench.py profile"));
    out.push_back(ftxui::text(""));
    for(std::size_t i = 0; i != std::min(s.profile.size(), maxRows); ++i) {
        auto const& r = s.profile[i];
        auto const  p
          = total != 0 ? 100.0 * static_cast<double>(r.samples) / static_cast<double>(total) : 0.0;
        out.push_back(ftxui::hbox(
          {ftxui::text(fmt::format("{:6.1f} % ", p)) | ftxui::color(Theme::Data::value()),
           ftxui::gauge(static_cast<float>(p / 100.0))
             | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 12),
           ftxui::text("  " + shortName(r.name)) | ftxui::color(Theme::Text::functionName())}));
    }
    return ftxui::vbox(std::move(out));
}

}   // namespace uc_log::FTXUIGui::inspector
