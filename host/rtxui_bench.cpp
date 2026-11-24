// uc_log_rtxui_bench [--demo_lines] [--metrics] [width height lines-per-frame frames]: what a frame of the RTXUI window
// costs, without a terminal - the real App over a real EntryStore on RTXUI's mock terminal device, the digest (bound
// state compared, templates rendered) and the draw (layout, paint, the diff written) timed apart. Medians of the
// frames; wall-clock numbers for a person to read, never a test's verdict. --demo_lines: the demo's lines (a metric in
// three of four); --metrics: with the metrics panel open (its list and plot made every frame).

#include "rtxui/terminal/screen_internal.hpp"
#include "rtxui/terminal/terminal_device.hpp"
#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/ProgramArgs.hpp"
#include "uc_log/printer/StatusBadges.hpp"
#include "uc_log/printer/rtx/App.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

double median(std::vector<double> v) {
    std::ranges::sort(v);
    return v.empty() ? 0.0 : v[v.size() / 2];
}

// the demo's mix (host/gui_test.cpp): six channels, modules, metric markers, a 160-character line
uc_log::printer::LogEntry demoEntry(std::size_t i) {
    static constexpr std::string_view messages[]
      = {"Hallo", "oh noooooo", "all bad", "messed up", ""};
    static constexpr std::string_view modules[]
      = {"", "i2c", "i2c.bus", "usb.device", "myapp.regulator"};
    uc_log::printer::LogEntry e{i % 6, {}};
    e.logLevel = static_cast<uc_log::LogLevel>((i * 7) % 6);
    e.fileName = i % 4 == 0 ? "main.cpp" : (i % 4 == 1 ? "foo.hpp" : "bar.c");
    e.line     = 20 + (i % 4) * 30;
    // the fifth: a line of 160 cells, cut at the edge
    e.logMsg = i % std::size(messages) == 4 ? std::string(160, 'x')
                                            : std::string{messages[i % std::size(messages)]};
    if(i % 4 != 0) { e.logMsg += " @METRIC(foo::sin[V]=0.25)"; }
    e.functionName = "main";
    e.module       = std::string{modules[i % std::size(modules)]};
    e.ucTime.time  = std::chrono::milliseconds{i * 100};
    e.parsedOk     = true;
    return e;
}

uc_log::printer::LogEntry entry(std::size_t i,
                                bool        demoLines) {
    if(demoLines) { return demoEntry(i); }
    static constexpr std::string_view words[] = {"i2c",
                                                 "transfer",
                                                 "done",
                                                 "after",
                                                 "retry",
                                                 "sensor",
                                                 "value",
                                                 "queue",
                                                 "spi",
                                                 "dma",
                                                 "irq",
                                                 "timeout"};
    uc_log::printer::LogEntry         e{i % 3, {}};
    e.logLevel = static_cast<uc_log::LogLevel>(i % 6);
    e.fileName = i % 2 == 0 ? "src/I2CQueued.hpp" : "main.cpp";
    e.line     = 10 + i % 400;
    std::string text;
    for(std::size_t w = 0; w < 6 + i % 9; ++w) {
        text += words[(i * 7 + w * 3) % std::size(words)];
        text += ' ';
    }
    e.logMsg       = std::move(text);
    e.functionName = "fn";
    e.ucTime.time  = std::chrono::microseconds{i * 1234};
    e.parsedOk     = true;
    return e;
}

constexpr std::array<std::string_view, 2> flagOptions{"demo_lines", "metrics"};

std::size_t argOr(std::vector<std::string> const& positional,
                  std::size_t                     i,
                  std::size_t                     fallback) {
    return i < positional.size()
           ? static_cast<std::size_t>(std::strtoull(positional[i].c_str(), nullptr, 10))
           : fallback;
}

}   // namespace

int main(int    argc,
         char** argv) {
    auto const parsed = uc_log::printer::parseProgramArgs(argc, argv, {}, flagOptions);
    if(!parsed) {
        std::fprintf(stderr, "uc_log_rtxui_bench: %s\n", parsed.error().c_str());
        return 2;
    }
    auto const& positional = parsed->positional;
    bool const  demoLines  = parsed->flag("demo_lines");
    auto const  width      = static_cast<int>(argOr(positional, 0, 200));
    auto const  height     = static_cast<int>(argOr(positional, 1, 60));
    auto const  perFrame   = argOr(positional, 2, 1000);
    auto const  frames     = argOr(positional, 3, 200);

    uc_log::printer::EntryStore         store;
    uc_log::printer::EntryStore::Mirror mirror;
    std::size_t                         n = 0;
    for(; n < 100'000; ++n) {
        store.addEntry(std::chrono::system_clock::now(), entry(n, demoLines));
    }
    store.refreshMirror(mirror);

    auto app
      = ::rtxui::Ref<uc_log::printer::rtx::App>::New(mirror, uc_log::printer::rtx::App::Hooks{});
    app->setSize(width, height);
    if(parsed->flag("metrics")) {
        app->setMetricSource(&store);
        app->run(uc_log::printer::Command::metrics);
    }
    auto device = std::make_shared<::rtxui::MockTerminalDevice>();
    device->TriggerResize(width, height);
    ::rtxui::Screen screen = ::rtxui::ScreenInternals::Make(app, device);

    uc_log::printer::StatusFacts facts;
    std::vector<double>          digest;
    std::vector<double>          draw;
    std::vector<double>          ours;
    std::size_t                  bytes = 0;
    for(std::size_t f = 0; f < frames + 10; ++f) {
        for(std::size_t k = 0; k < perFrame; ++k, ++n) {
            store.addEntry(std::chrono::system_clock::now(), entry(n, demoLines));
        }
        auto const t0 = Clock::now();
        store.refreshMirror(mirror);
        facts.logs = mirror.originalLogCount;
        app->show(uc_log::printer::statusBadges(facts));
        auto const t1 = Clock::now();
        app->Digest();
        auto const t2 = Clock::now();
        screen.Draw();
        auto const t3 = Clock::now();
        if(f >= 10) {
            ours.push_back(ms(t1 - t0));
            digest.push_back(ms(t2 - t1));
            draw.push_back(ms(t3 - t2));
            bytes += device->GetOutput().size();
        }
        device->ClearOutput();
    }
    std::printf(
      "%dx%d, %zu new lines per frame, %zu frames, %zu runs in the log: mirror+format %.2f ms, "
      "digest %.2f "
      "ms, draw %.2f ms, %.1f KB written per frame\n",
      width,
      height,
      perFrame,
      frames,
      app->log->lines->spans.size(),
      median(ours),
      median(digest),
      median(draw),
      static_cast<double>(bytes) / static_cast<double>(frames) / 1024.0);
    return 0;
}
