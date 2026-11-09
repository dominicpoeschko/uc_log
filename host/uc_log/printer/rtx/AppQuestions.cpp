// The App: a question in the dialog (printer/Questions.hpp) - the value it starts with, the choices under its
// field, what an answer does (checked first: a wrong one stays in the dialog, said).

#include "uc_log/printer/rtx/App.hpp"

#include "uc_log/printer/FilterFile.hpp"
#include "uc_log/printer/TargetTime.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace uc_log::printer::rtx {

void App::ask(Field       field,
              std::size_t index) {
    auto const q = questionOf(field, index);
    auto&      d = dialogState_;
    d.modal      = Modal::question;
    d.asked      = field;
    d.askedIndex = index;
    d.askedValue = valueOf(field, index);
    d.symbols    = {};
    d.shownFor   = DialogState::nothingShown;
    if(field == Field::bindAddress) { d.addresses = localAddresses(); }
    dialog->open        = true;
    dialog->title       = q.title;
    dialog->about       = q.about;
    dialog->placeholder = q.placeholder;
    dialog->query       = d.askedValue;
    dialog->error       = {};
    dialog->buttons     = {
      {  "", "",                                    "Cancel"},
      {"go", "", field == Field::watch ? "✓ Watch" : "✓ Use"}
    };
    fillDialog(true);
}

std::string App::valueOf(Field       field,
                         std::size_t index) const {
    auto const& m = printer.model;
    switch(field) {
    case Field::search:       return mirror_.searchText;
    case Field::jumpToTime:
    case Field::timeFrom:
    case Field::timeTo:
    case Field::watch:        return {};
    case Field::metricFilter: return pages_.metricFilter;
    case Field::saveFilter:
    case Field::loadFilter:   return filterPath_;
    case Field::logDir:
        {
            // the log file's, or where the printer runs
            auto dir = std::filesystem::path{m.logFilePath}.parent_path();
            if(dir.empty()) {
                std::error_code ec;
                dir = std::filesystem::current_path(ec);
            }
            return dir.string();
        }
    case Field::controlPort:    return std::to_string(static_cast<unsigned>(m.controlPort));
    case Field::bindAddress:    return m.bindAddress;
    case Field::duplexBasePort: return std::to_string(static_cast<unsigned>(m.duplexBasePort));
    case Field::duplexPort:
        for(auto const& d : printer.duplex) {
            if(d.ordinal == index) { return std::to_string(static_cast<unsigned>(d.port)); }
        }
        return {};
    case Field::probe:        return pages_.debugger.probe;
    case Field::host:         return pages_.debugger.host;
    case Field::noLogTimeout: return std::to_string(pages_.debugger.noLogTimeout);
    case Field::traceRows:    return std::to_string(pages_.trace.rows);
    }
    return {};
}

std::vector<Choice> App::choicesFor(std::string const& text) {
    // what it started with lists every choice; typed, the answers before that hold it
    auto const          query = text == dialogState_.askedValue ? std::string{} : text;
    std::vector<Choice> out;
    switch(dialogState_.asked) {
    case Field::jumpToTime:
    case Field::timeFrom:
    case Field::timeTo:
        {
            std::vector<std::pair<std::string, double>> times;
            auto const                                  seconds = [this](std::size_t i) {
                return std::chrono::duration<double>(mirror_.entries[i].common->ucTime.time)
                  .count();
            };
            if(auto const marked = log->current(mirror_)) {
                times.emplace_back(fmt::format("the marked line, {}", *marked + 1),
                                   seconds(*marked));
            }
            if(auto const n = mirror_.entries.size(); n != 0) {
                times.emplace_back("the first line shown", seconds(0));
                times.emplace_back("the newest line", seconds(n - 1));
            }
            out = timeChoices(times);
            break;
        }
    case Field::saveFilter:
    case Field::loadFilter: return pathChoices(text, ".json");
    case Field::logDir:     return pathChoices(text, "");
    case Field::bindAddress:
        out = addressChoices(dialogState_.addresses, printer.model.bindAddress);
        break;
    case Field::probe: out = probeChoices(printer.probes, pages_.debugger.probe); break;
    case Field::noLogTimeout:
        out = numberChoices({1, 2, 5, 10, 30, 60}, pages_.debugger.noLogTimeout, "s");
        break;
    case Field::traceRows:
        out
          = numberChoices({10, 20, 50, 100, 500},
                          static_cast<unsigned>(std::min<std::size_t>(pages_.trace.rows, 100'000)),
                          "rows");
        break;
    case Field::watch:
        {
            dialogState_.symbols = hooks_.complete && !text.empty()
                                   ? hooks_.complete(text, 200)
                                   : TargetInspector::Completions{};
            std::set<std::string> watched;
            if(inspection) {
                for(auto const& w : inspection->watches) { watched.insert(w.name); }
            }
            // the name gets what the box leaves beside the tick and the size and address
            auto const room
              = static_cast<std::size_t>(std::max(20, std::clamp(width_ - 4, 20, 100) - 36));
            return symbolChoices(dialogState_.symbols, watched, room);
        }
    case Field::metricFilter:
        if(metrics_ != nullptr) { out = metricChoices(metrics_->metricSummaries(), text); }
        break;
    case Field::search:
    case Field::controlPort:
    case Field::duplexBasePort:
    case Field::duplexPort:
    case Field::host:           break;
    }
    // the answers given before, those not listed already
    if(auto const it = recent_.find(dialogState_.asked); it != recent_.end()) {
        for(auto& r : recentChoices(it->second, query)) {
            if(std::ranges::none_of(out, [&](Choice const& c) { return c.value == r.value; })) {
                out.push_back(std::move(r));
            }
        }
    }
    return out;
}

namespace {
    std::optional<std::uint32_t> positive(std::string const& text) {
        std::uint32_t     v{};
        auto const* const end = std::to_address(text.end());
        auto const [at, ec]   = std::from_chars(std::to_address(text.begin()), end, v);
        if(ec != std::errc{} || at != end || v == 0) { return std::nullopt; }
        return v;
    }
}   // namespace

bool App::answer(std::string const& text) {
    auto const forward = [this](WindowAction const& x) {
        if(hooks_.act) { hooks_.act(x); }
    };
    auto const wrong = [this](std::string why) {
        dialog->error = std::move(why);
        return false;
    };
    switch(dialogState_.asked) {
    case Field::search:
        if(hooks_.search) { hooks_.search(text); }
        break;
    case Field::jumpToTime:
        {
            auto const time = parseTargetTime(text);
            if(!time) { return wrong(fmt::format("not a time: {:?}", text)); }
            auto const index = firstAtOrAfter(mirror_.entries, *time);
            if(!index) { return wrong(fmt::format("no line at or after {}", text)); }
            log->viewport().show(SnapshotLines{&mirror_.entries}, logBody(), *index);
            notify(fmt::format("line {}: the first at or after {}", *index + 1, text));
            break;
        }
    case Field::timeFrom:
    case Field::timeTo:
        {
            auto const time = parseTargetTime(text);
            if(!time) { return wrong(fmt::format("not a time: {:?}", text)); }
            auto       w       = timeWindow();
            auto const seconds = std::chrono::duration<double>(*time).count();
            if(!w.enabled || w.live) {
                w.minSec = 0.0;
                w.maxSec = std::numeric_limits<double>::infinity();
            }
            w.enabled                                                     = true;
            w.live                                                        = false;
            (dialogState_.asked == Field::timeFrom ? w.minSec : w.maxSec) = seconds;
            if(hooks_.setTime) { hooks_.setTime(w); }
            notify(fmt::format("showing {}", filterSummary(filter_, w)));
            break;
        }
    case Field::saveFilter:
        {
            if(text.empty()) { return wrong("a file to save it to"); }
            auto const saved = saveFilter(text, filter_);
            if(!saved) { return wrong(saved.error()); }
            filterPath_ = text;
            notify(fmt::format("filter saved to {}", text));
            break;
        }
    case Field::loadFilter:
        {
            auto loaded = loadFilter(text);
            if(!loaded) { return wrong(loaded.error()); }
            filterPath_ = text;
            applyFilter(std::move(*loaded), fmt::format("filter loaded from {}", text));
            break;
        }
    case Field::logDir:
        if(text.empty()) { return wrong("a directory"); }
        forward(WindowAction{action::LogDir{text}});
        break;
    case Field::controlPort:
        if(!parsePort(text)) { return wrong(fmt::format("not a port (1024-65535): {:?}", text)); }
        forward(WindowAction{action::ControlPort{text}});
        break;
    case Field::bindAddress:
        if(!isIpAddress(text)) {
            return wrong(
              fmt::format("not an IP address: {:?} - 127.0.0.1, 0.0.0.0, ::1, ...", text));
        }
        forward(WindowAction{action::BindAddress{text}});
        break;
    case Field::duplexBasePort:
        if(!parsePort(text, 65535 - PrinterModel::maxDuplexChannels)) {
            return wrong(fmt::format("not a base port (1024-65527): {:?}", text));
        }
        forward(WindowAction{action::DuplexBasePort{text}});
        break;
    case Field::duplexPort:
        if(!parsePort(text)) { return wrong(fmt::format("not a port (1024-65535): {:?}", text)); }
        forward(WindowAction{
          action::DuplexPort{dialogState_.askedIndex, text}
        });
        break;
    case Field::probe:
        pages_.debugger.probe = text;
        pages_.debugger.usb   = true;
        notify("connect to use it");
        break;
    case Field::host:
        if(text.empty()) { return wrong("an IP address or a host name"); }
        pages_.debugger.host = text;
        pages_.debugger.usb  = false;
        notify("connect to use it");
        break;
    case Field::noLogTimeout:
        {
            auto const v = positive(text);
            if(!v) { return wrong(fmt::format("not a number of seconds: {:?}", text)); }
            pages_.debugger.noLogTimeout = *v;
            forward(WindowAction{action::NoLogTimeout{*v}});
            break;
        }
    case Field::traceRows:
        {
            auto const v = positive(text);
            if(!v) { return wrong(fmt::format("not a number of rows: {:?}", text)); }
            pages_.trace.rows = std::min<std::size_t>(*v, 100'000);
            break;
        }
    case Field::watch:
        {
            // a symbol of the map: one of the matches (a watched one goes)
            auto const it = std::ranges::find_if(dialogState_.choices,
                                                 [&](Choice const& c) { return c.value == text; });
            if(it == dialogState_.choices.end()) {
                return wrong(text.empty() ? std::string{"part of a data symbol's name"}
                                          : fmt::format("no data symbol is called {:?}", text));
            }
            forward(it->on ? WindowAction{action::Unwatch{text}}
                           : WindowAction{action::Watch{text}});
            break;
        }
    case Field::metricFilter:
        {
            // a click on a metric marks it, the table narrowed by what is typed; Enter: by the text
            auto const picked   = std::ranges::find_if(dialogState_.choices, [&](Choice const& c) {
                return c.value == text;
            });
            pages_.metricFilter = picked != dialogState_.choices.end() && text != dialog->query
                                  ? dialog->query
                                  : text;
            if(picked != dialogState_.choices.end() && metrics_ != nullptr) {
                for(auto const& info : metrics_->metricInfos()) {
                    if(fmt::format("{}::{}", info.scope, info.name) == text) {
                        pages_.metric = info;
                        break;
                    }
                }
            }
            pages_.metricTop    = 0;
            pages_.metricFollow = true;
            break;
        }
    }
    // remembered: the dialog lists it the next time
    if(!text.empty() && dialogState_.asked != Field::watch) {
        auto& recent = recent_[dialogState_.asked];
        std::erase(recent, text);
        recent.insert(recent.begin(), text);
        if(recent.size() > 20) { recent.resize(20); }
    }
    closeDialog();
    return true;
}

}   // namespace uc_log::printer::rtx
