#pragma once

// What a window asks a value for, for any front end: each question's title, what it explains and what its field
// holds empty, and the choices under the field - the answers given before, the J-Links on USB, the addresses of this
// machine, a directory's entries, a few numbers, the data symbols part of a name matches, the times of the log. A
// window shows them in a dialog, a click puts a choice in (a directory: goes into it) or answers with it; what an
// answer does stays the window's. The directory listing reads the file system (the interfaces: LocalAddresses.hpp):
// the rest is plain data in, rows out.

#include "uc_log/printer/InspectorText.hpp"
#include "uc_log/printer/LocalAddresses.hpp"
#include "uc_log/printer/MetricTable.hpp"
#include "uc_log/printer/TargetInspector.hpp"
#include "uc_log/printer/TargetPort.hpp"
#include "uc_log/printer/TextMatch.hpp"
#include "uc_log/printer/WindowActions.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fmt/format.h>
#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// What the dialog says about a question.
struct Question {
    std::string title;         // its head: what is asked
    std::string about;         // under it: what is valid, what an answer does
    std::string placeholder;   // what the empty field says
    bool        firstMarked{
      false};   // the first choice is marked from the start: part of a name is no answer
};

/// The question for `field` (`index`: a duplex channel's ordinal).
inline Question questionOf(Field       field,
                           std::size_t index = 0) {
    switch(field) {
    case Field::search:
        return {"🔎 Search the log",
                "text, or re: and a pattern: only the lines that hold it are shown, the match "
                "marked; empty: all",
                "text or re:pattern",
                false};
    case Field::jumpToTime:
        return {"🕑 Jump to a target time",
                "seconds, or with ms, us or min: the first line at or after it is marked",
                "e.g. 12.5 or 300ms",
                false};
    case Field::timeFrom:
        return {"🕑 Show from a target time on",
                "seconds, or with ms, us or min: the lines before it are hidden",
                "e.g. 12.5",
                false};
    case Field::timeTo:
        return {"🕑 Show up to a target time",
                "seconds, or with ms, us or min: the lines after it are hidden",
                "e.g. 60",
                false};
    case Field::saveFilter:
        return {"💾 Save the filter",
                "a JSON file: the levels, channels, modules, locations and the target time",
                "filter.json",
                false};
    case Field::loadFilter:
        return {"📂 Load a filter", "a JSON file a save wrote", "filter.json", false};
    case Field::logDir:
        return {"📁 The log file's directory",
                "a new .rttlog is started there; a click on a directory goes into it",
                "a directory",
                false};
    case Field::controlPort:
        return {"🎛 The control socket's TCP port",
                "1024-65535: the control socket listens there (TCP ports transport)",
                "a port",
                false};
    case Field::bindAddress:
        return {"🌐 The address the sockets bind",
                "127.0.0.1: this machine only; 0.0.0.0 or an interface's address: the network "
                "reaches them",
                "an IP address",
                false};
    case Field::duplexBasePort:
        return {"🔁 The duplex channels' base port",
                "1024-65527: channel n listens on the base port + n",
                "a port",
                false};
    case Field::duplexPort:
        return {fmt::format("🔁 Duplex channel {}'s port", index),
                "1024-65535: that channel alone listens there",
                "a port",
                false};
    case Field::probe:
        return {"🔌 The J-Link on USB",
                "its serial number or nickname; empty: the only one on USB - Connect uses it",
                "serial number or nickname",
                false};
    case Field::host:
        return {"🌐 The J-Link over IP",
                "its IP address or host name - Connect uses it",
                "e.g. 192.0.2.7",
                false};
    case Field::noLogTimeout:
        return {"⏱ Reconnect without a log line",
                "seconds without a log line before the printer connects again",
                "seconds",
                false};
    case Field::traceRows:
        return {"🧵 Rows of a ring",
                "how many of each ring's newest rows are shown",
                "rows",
                false};
    case Field::watch:
        return {
          "👁 Watch a data symbol",
          "part of its name (more parts: a space); untyped: 1, 2, 4 or 8 bytes as numbers, else "
          "bytes",
          "part of a name",
          true};
    case Field::metricFilter:
        return {
          "🔎 Narrow the metrics",
          "words of a metric's scope, name or unit: the table shows those that hold every one; a "
          "click on one marks it; empty: all",
          "e.g. adc or [V]",
          false};
    }
    return {};
}

/// A choice under a question's field: a click answers with `value` - or, `into`, puts it into the field and stays
/// (a directory: its entries next).
struct Choice {
    std::string value;
    std::string text;
    std::string detail;   // dimmed after the text
    bool        into{false};
    bool        on{false};   // the value now, a symbol watched: ticked

    bool operator==(Choice const&) const = default;
};

/// The answers given before, the newest first: those holding every word of `query` (all for an empty one).
inline std::vector<Choice> recentChoices(std::vector<std::string> const& recent,
                                         std::string_view                query) {
    std::vector<Choice> out;
    for(auto const& r : recent) {
        if(matchesWords(r, query)) { out.push_back({r, r, "before", false, false}); }
    }
    return out;
}

/// A few numbers to pick (seconds, rows), the one now ticked.
inline std::vector<Choice> numberChoices(std::initializer_list<unsigned> numbers,
                                         unsigned                        now,
                                         std::string_view                unit) {
    std::vector<Choice> out;
    for(auto const n : numbers) {
        out.push_back({std::to_string(n),
                       fmt::format("{} {}", n, unit),
                       n == now ? "now" : "",
                       false,
                       n == now});
    }
    return out;
}

/// What the sockets can bind: this machine only, every interface, then each interface's own address; `now` ticked.
inline std::vector<Choice> addressChoices(std::vector<LocalAddress> const& own,
                                          std::string_view                 now) {
    std::vector<Choice> out{
      {"127.0.0.1", "127.0.0.1",                "this machine only", false, false},
      {  "0.0.0.0",   "0.0.0.0", "every interface: the network too", false, false},
      {      "::1",       "::1",          "this machine only, IPv6", false, false},
      {       "::",        "::",            "every interface, IPv6", false, false}
    };
    for(auto const& a : own) {
        out.push_back({a.address, a.address, fmt::format("{}: the network there", a.interface)});
    }
    for(auto& c : out) { c.on = c.value == now; }
    return out;
}

/// The J-Links on USB by serial number, "" the only one; `now` ticked.
inline std::vector<Choice> probeChoices(std::vector<TargetPort::Probe> const& probes,
                                        std::string_view                      now) {
    std::vector<Choice> out{
      {"", "any", "the only J-Link on USB", false, now.empty()}
    };
    for(auto const& p : probes) {
        if(!p.onUsb) { continue; }
        auto const serial = std::to_string(p.serialNumber);
        out.push_back({serial,
                       p.nickName.empty()
                         ? fmt::format("{} {}", p.product, serial)
                         : fmt::format("{} {} \"{}\"", p.product, serial, p.nickName),
                       "on USB",
                       false,
                       now == serial || (!p.nickName.empty() && now == p.nickName)});
    }
    return out;
}

/// The entries `input` names: a directory (or one ending in '/') lists what is in it, anything else the entries of
/// its directory starting with its last part. Directories (a click goes into them, ".." up) and, with `extension`,
/// the files that end in it (a click answers with them); the hidden ones only for a part starting with '.'. At most
/// `max`, directories first, by name.
inline std::vector<Choice> pathChoices(std::string_view input,
                                       std::string_view extension,
                                       std::size_t      max = 200) {
    namespace fs = std::filesystem;
    std::error_code   ec;
    std::string const text{input};
    bool const        isDir = !text.empty() && (text.ends_with('/') || fs::is_directory(text, ec));
    fs::path const    dir   = isDir ? fs::path{text} : fs::path{text}.parent_path();
    std::string const part  = isDir ? std::string{} : fs::path{text}.filename().string();
    std::string const base
      = isDir
        ? (text.ends_with('/') ? text : text + "/")
        : (dir.empty() ? std::string{} : dir.string() + (dir.string().ends_with('/') ? "" : "/"));
    fs::path const where = dir.empty() ? fs::path{"."} : dir;

    std::vector<Choice> out;
    if(part.empty()) {
        // the directory above, as a path of its own (not "../../..")
        auto here = fs::absolute(where, ec).lexically_normal();
        if(!here.has_filename()) { here = here.parent_path(); }
        if(!ec && here != here.root_path()) {
            auto up = here.parent_path().string();
            if(!up.ends_with('/')) { up += '/'; }
            out.push_back({std::move(up), "📁 ..", "the directory above", true, false});
        }
    }
    std::vector<Choice> dirs;
    std::vector<Choice> files;
    for(fs::directory_iterator it{where, fs::directory_options::skip_permission_denied, ec}, end;
        !ec && it != end;
        it.increment(ec))
    {
        auto const name = it->path().filename().string();
        if(!name.starts_with(part) || (name.starts_with('.') && !part.starts_with('.'))) {
            continue;
        }
        std::error_code entryEc;
        if(it->is_directory(entryEc)) {
            dirs.push_back({base + name + "/", fmt::format("📁 {}/", name), "", true, false});
        } else if(!extension.empty() && name.ends_with(extension)) {
            files.push_back({base + name, fmt::format("📄 {}", name), "", false, false});
        }
    }
    auto const byText = [](Choice const& a, Choice const& b) { return a.text < b.text; };
    std::ranges::sort(dirs, byText);
    std::ranges::sort(files, byText);
    out.insert(out.end(), dirs.begin(), dirs.end());
    out.insert(out.end(), files.begin(), files.end());
    if(out.size() > max) { out.resize(max); }
    return out;
}

/// The data symbols part of a name matched, the likeliest first; a watched one ticked (a click un-watches it). A
/// name longer than `room` loses its nested template arguments, then its front: its end tells two apart.
inline std::vector<Choice> symbolChoices(TargetInspector::Completions const& c,
                                         std::set<std::string> const&        watched,
                                         std::size_t                         room = 100) {
    std::vector<Choice> out;
    out.reserve(c.best.size());
    for(auto const& m : c.best) {
        out.push_back({m.name,
                       shortName(m.name, room),
                       fmt::format("{} B @{:#010x}{}",
                                   m.size,
                                   m.address,
                                   watched.contains(m.name) ? " · watched" : ""),
                       false,
                       watched.contains(m.name)});
    }
    return out;
}

/// The metrics `query` matches (every word of its scope, name or unit), at most `most`: a click marks one; the latest
/// value after it.
inline std::vector<Choice> metricChoices(MetricSummaries const& metrics,
                                         std::string_view       query,
                                         std::size_t            most = 200) {
    std::vector<Choice> out;
    for(auto const& [info, sum] : metrics) {
        if(out.size() == most) { break; }
        auto words = metricWords(info, sum);
        if(!matchesWords(words, query)) { continue; }
        out.push_back({fmt::format("{}::{}", info.scope, info.name),
                       std::move(words),
                       formatMetricValue(sum.latest, *shownUnit(info, sum))});
    }
    return out;
}

/// Times of the log to pick, as (what it is, seconds): the marked line's, the first's, the newest's.
inline std::vector<Choice> timeChoices(std::vector<std::pair<std::string,
                                                             double>> const& times) {
    std::vector<Choice> out;
    for(auto const& [what, seconds] : times) {
        // to the microsecond, without the zeros after the last digit
        auto text = fmt::format("{:.6f}", seconds);
        text.erase(text.find_last_not_of('0') + 1);
        if(text.ends_with('.')) { text.pop_back(); }
        out.push_back({text, fmt::format("{} s", text), what});
    }
    return out;
}

}   // namespace uc_log::printer
