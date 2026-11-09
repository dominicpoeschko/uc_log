#include "uc_log/printer/rtx/Gui.hpp"

#include "uc_log/printer/EntryStore.hpp"
#include "uc_log/printer/Environment.hpp"
#include "uc_log/printer/FramePacer.hpp"
#include "uc_log/printer/LogFormat.hpp"
#include "uc_log/printer/PanelText.hpp"
#include "uc_log/printer/StatusBadges.hpp"
#include "uc_log/printer/TargetInspector.hpp"
#include "uc_log/printer/WindowActions.hpp"
#include "uc_log/printer/rtx/App.hpp"
#include "uc_log/printer/rtx/PrinterActs.hpp"
#include "uc_log/printer/rtx/Worker.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace uc_log::printer::rtx {

namespace {
    double threadCpuSeconds() {
        timespec ts{};
        ::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
    }

}   // namespace

class Window {
public:
    explicit Window(PrinterModel& model UC_LOG_LIFETIMEBOUND)
      : model_{model}
      , messages_{model.messages()}
      , builds_{model.builds()} {
        // UC_LOG_UI_FPS=<frames per second>: another pace (1..1000)
        if(char const* const fps = environment("UC_LOG_UI_FPS")) {
            std::string_view const text{fps};
            unsigned               n{};
            auto const* const      last = std::to_address(text.end());
            auto const [end, ec]        = std::from_chars(std::to_address(text.begin()), last, n);
            if(ec == std::errc{} && end == last && n > 0) {
                pacer_.setInterval(std::chrono::microseconds{1'000'000 / std::min(n, 1000U)});
            }
        }
        messages_.addObserver([this](MessageLog::Said const&) { requestFrame(); });
        store_.setRedrawRequest([this] { requestFrame(); });
        builds_.requestRedraw = [this] { requestFrame(); };
    }

    ~Window() {
        builds_.stopAndJoin();
        builds_.requestRedraw = {};
        messages_.clearObservers();
    }

    Window(Window const&)            = delete;
    Window& operator=(Window const&) = delete;

    void add(std::chrono::system_clock::time_point recvTime,
             LogEntry const&                       entry) {
        store_.addEntry(recvTime, entry);
    }

    void setCompiledFilter(uc_log::Detail::FilterTable const& table) { compiledFilter_ = table; }

    void setMapSource(Gui::MapSource source) { mapSource_ = std::move(source); }

    /// A frame is wanted (any thread): at most one per pacer interval is drawn.
    void requestFrame() {
        if(!running_.load(std::memory_order_acquire)) { return; }
        if(pacer_.request()) {
            // for when the frame is due: one wake-up of the UI thread a frame, not one that only finds it early
            auto const delay = pacer_.delay(FramePacer::Clock::now());
            if(delay > FramePacer::Duration::zero()) {
                ::rtxui::PostDelayedTask([this] { wake(); },
                                         std::chrono::ceil<std::chrono::milliseconds>(delay));
            } else {
                ::rtxui::PostTask([this] { wake(); });
            }
        }
    }

    int run(TargetPort&        target,
            std::string const& buildCommand,
            std::string const& initialHost,
            std::string const& initialProbe) {
        builds_.initialize(buildCommand);
        target_ = &target;
        worker_.emplace();
        inspector_ = std::make_unique<TargetInspector>(TargetInspector::Hooks{
          .target  = &target,
          .map     = mapSource_,
          .changed = [this] { requestFrame(); },
          .note    = [this](std::string_view said) { messages_.status(said); }});
        acts_.emplace(PrinterActs::Parts{
          .model     = model_,
          .store     = store_,
          .messages  = messages_,
          .inspector = inspector_.get(),
          .halted    = [this] { return target_ != nullptr && target_->isHalted(); },
          .onTarget  = [this](std::function<void(TargetPort&)> job) { onTarget(std::move(job)); },
          .notify =
            [this](std::string text) {
                if(app_) { app_->notify(std::move(text)); }
            }});

        app_ = ::rtxui::Ref<App>::New(
          mirror_,
          App::Hooks{
            .quit =
              [this] {
                  if(screen_ != nullptr) { screen_->Exit(); }
              },
            .reset =
              [this] {
                  model_.noteResetRequest();
                  target_->resetTarget();
              },
            .flash =
              [this] {
                  model_.noteFlashRequest();
                  target_->flash();
              },
            .build = [this](bool andFlash) { static_cast<void>(model_.startBuild(andFlash)); },
            .search =
              [this](std::string const& text) {
                  store_.setSearchText(text);
                  requestFrame();
              },
            .exportText    = [this] { exportText(); },
            .clearMessages = [this] { model_.clearCounts(); },
            .setFilter     = [this](FilterState const& f) { store_.setFilterState(f); },
            .setTime =
              [this](EntryStore::UcTimeWindow const& w) {
                  store_.updateUcTime([w](EntryStore::UcTimeWindow& current, double dataMax) {
                      current = w;
                      if(current.live) {
                          current.minSec = std::max(0.0, dataMax - current.liveWindowSecs);
                          current.maxSec = std::numeric_limits<double>::infinity();
                      }
                      return true;
                  });
                  requestFrame();
              },
            .later = [](std::chrono::milliseconds delay,
                        std::function<void()> f) { ::rtxui::PostDelayedTask(std::move(f), delay); },
            .act   = [this](WindowAction const& a) { act(a); },
            .complete =
              [this](std::string const& query, std::size_t max) {
                  return inspector_ ? inspector_->completions(query, max)
                                    : TargetInspector::Completions{};
              }});
        app_->setCompiledFilter(&compiledFilter_);
        app_->setMetricSource(&store_);
        app_->setConnection(initialHost, initialProbe);
        checkSize();
        frame();

        ::rtxui::Screen screen{app_};
        screen.SetInline(false);
        screen_ = &screen;
        running_.store(true, std::memory_order_release);

        // what changes without anybody saying so (the link's byte counts, a target that went away) is looked at once
        // a second; a frame that changed nothing draws nothing
        heartbeat_ = [this] {
            requestFrame();
            ::rtxui::PostDelayedTask(heartbeat_, std::chrono::milliseconds{1000});
        };
        ::rtxui::PostDelayedTask(heartbeat_, std::chrono::milliseconds{1000});
        // RTXUI (0.1.4) lays a resized terminal out anew but sends no Resized event: the window asks the terminal. One
        // chain of it (started in the heartbeat, a new one each second piled up: t of them after t seconds)
        sizePoll_ = [this] {
            checkSize();
            ::rtxui::PostDelayedTask(sizePoll_, std::chrono::milliseconds{200});
        };
        ::rtxui::PostDelayedTask(sizePoll_, std::chrono::milliseconds{200});

        auto const started    = FramePacer::Clock::now();
        auto const startedCpu = threadCpuSeconds();
        screen.Loop();
        if(environment("UC_LOG_UI_STATS") != nullptr) {
            auto const secs
              = std::chrono::duration<double>(FramePacer::Clock::now() - started).count();
            auto const cpu    = threadCpuSeconds() - startedCpu;
            auto const digest = app_->digestStats();
            std::fprintf(
              stderr,
              "rtxui window: %.1f s, UI thread %.0f %% of a core; %zu wakes, %zu frames (%.1f/s), "
              "%.2f ms per frame in frame(); %zu digests (%zu changed) %.2f ms each; %zu rows "
              "formatted\n",
              secs,
              cpu * 100.0 / secs,
              wakes_,
              frames_,
              static_cast<double>(frames_) / secs,
              frames_ == 0 ? 0.0 : frameSeconds_ * 1000.0 / static_cast<double>(frames_),
              digest.calls,
              digest.changed,
              digest.calls == 0 ? 0.0 : digest.seconds * 1000.0 / static_cast<double>(digest.calls),
              app_->log->formattedRows());
        }

        running_.store(false, std::memory_order_release);
        screen_ = nullptr;
        builds_.stopAndJoin();
        // the reader outlives run(): the inspector's reads and the worker's jobs end first
        acts_.reset();
        worker_.reset();
        inspector_.reset();
        app_    = ::rtxui::Ref<App>{};
        target_ = nullptr;
        return 0;
    }

private:
    PrinterModel&         model_;
    MessageLog&           messages_;
    BuildRunner&          builds_;
    EntryStore            store_;
    EntryStore::Mirror    mirror_;
    FramePacer            pacer_;
    std::atomic<bool>     running_{false};
    ::rtxui::Ref<App>     app_;
    ::rtxui::Screen*      screen_{nullptr};
    TargetPort*           target_{nullptr};
    std::function<void()> heartbeat_;
    std::function<void()> sizePoll_;
    int                   columns_{0};   // the terminal's size as last told to the App
    int                   lines_{0};
    bool                  wasFlashing_{false};
    std::size_t           messageClears_{0};
    std::uint64_t         buildSeen_{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<bool>     exporting_{false};
    // UC_LOG_UI_STATS: what the window cost, said at the end
    std::size_t                      wakes_{0};
    std::size_t                      frames_{0};
    double                           frameSeconds_{0.0};
    uc_log::Detail::FilterTable      compiledFilter_{};
    Gui::MapSource                   mapSource_;
    std::unique_ptr<TargetInspector> inspector_;   // while run() runs
    std::uint64_t                    inspectionSeen_{0};
    // the probes' version the page has: none yet, whatever the port's first version is
    std::uint64_t              probesSeen_{std::numeric_limits<std::uint64_t>::max()};
    std::optional<Worker>      worker_;     // while run() runs
    std::optional<PrinterActs> acts_;       // while run() runs
    std::jthread               exporter_;   // last: joined before the members it reads go

    // the terminal's size (TIOCGWINSZ): a change goes to the App and draws a frame
    void checkSize() {
        winsize ws{};
        if(::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0 || ws.ws_row == 0) {
            return;
        }
        if(ws.ws_col == columns_ && ws.ws_row == lines_) { return; }
        columns_ = ws.ws_col;
        lines_   = ws.ws_row;
        app_->setSize(columns_, lines_);
        requestFrame();
    }

    // a target job on the worker; its failure a message
    void onTarget(std::function<void(TargetPort&)> job) {
        if(!worker_ || target_ == nullptr) { return; }
        worker_->post([this, run = std::move(job)] { run(*target_); });
    }

    // what a page's button asks the printer to do
    void act(WindowAction const& a) {
        if(acts_) { acts_->run(a); }
        requestFrame();
    }

    // the shown lines (the filter's) as text into the log directory, on a thread of its own: an export of millions
    // of lines does not stop the window; the result is a message and a notice
    void exportText() {
        if(exporting_.exchange(true)) {
            app_->notify("an export runs already");
            return;
        }
        if(exporter_.joinable()) { exporter_.join(); }
        auto names = std::make_shared<EntryStore::Mirror const>(
          mirror_);   // the lines: shared chunks, no copy
        std::filesystem::path dir
          = std::filesystem::path{model_.snapshot().logFilePath}.parent_path();
        if(dir.empty()) { dir = std::filesystem::current_path(); }
        auto const path
          = dir
          / fmt::format("shown_{}.txt", logformat::toIso8601Utc(std::chrono::system_clock::now()));
        auto const text = app_->log->columns().text;
        app_->notify(fmt::format("exporting {} lines ...", names->entries.size()));
        exporter_ = std::jthread{[this, names, path, text] {
            std::ofstream out{path};
            std::size_t   written = 0;
            for(auto const& line : names->entries) {
                if(!out) { break; }
                out << plainText(line, *names, text, true) << '\n';
                ++written;
            }
            out.flush();
            auto const said = out ? fmt::format("{} lines exported to {}", written, path.string())
                                  : fmt::format("export failed - cannot write {}", path.string());
            if(out) {
                messages_.status(said);
            } else {
                messages_.error(said);
            }
            exporting_.store(false);
            ::rtxui::PostTask([this, said] {
                if(app_) { app_->notify(said); }
            });
        }};
    }

    // the posted wake-up: a frame now, or once the interval since the last one has passed
    void wake() {
        ++wakes_;
        auto const now   = FramePacer::Clock::now();
        auto const delay = pacer_.delay(now);
        if(delay > FramePacer::Duration::zero()) {
            ::rtxui::PostDelayedTask([this] { wake(); },
                                     std::chrono::ceil<std::chrono::milliseconds>(delay));
            return;
        }
        pacer_.begin(now);
        frame();
        ++frames_;
        frameSeconds_ += std::chrono::duration<double>(FramePacer::Clock::now() - now).count();
    }

    // one paced frame: everything the window shows, pulled once
    void frame() {
        store_.pollRefilterDeadline();
        store_.refreshMirror(mirror_);
        auto const link = target_->linkStatus();
        model_.sampleLink(link);
        auto const model  = model_.snapshot();
        auto const counts = messages_.counts();

        builds_.joinIfFinished();
        if(builds_.triggerFlashNow.exchange(false)) {
            model_.noteFlashRequest();
            target_->flash();
        }
        bool const flashing = target_->isFlashing();
        // a flash that ended: its format errors belong to the image before it
        if(wasFlashing_ && !flashing) { messages_.resetFormatErrors(); }
        wasFlashing_ = flashing;

        // the panels' sources: only what is new is copied
        messages_.copyNew(app_->messageEntries, messageClears_);
        builds_.snapshotOutput(app_->buildEntries, buildSeen_);
        app_->buildStatus = builds_.getStatus();

        // the printer's pages: the facts are cheap, the probes and the inspection copied when they changed
        auto& facts_      = app_->printer;
        facts_.model      = model;
        facts_.duplex     = model_.duplexInfo();
        facts_.statistics = model_.statistics();
        facts_.link       = link;
        facts_.halted     = target_->isHalted();
        facts_.flashing   = flashing;
        if(auto const v = target_->getProbesVersion(); v != probesSeen_) {
            probesSeen_   = v;
            facts_.probes = target_->getProbes();
        }
        if(inspector_) {
            inspector_->setPage(app_->inspectorPage());
            if(auto const v = inspector_->version(); v != inspectionSeen_ || !app_->inspection) {
                inspectionSeen_ = v;
                app_->inspection
                  = std::make_shared<TargetInspector::Snapshot const>(inspector_->snapshot());
            }
        }

        auto const  refilter = store_.refilterProgress();
        StatusFacts facts;
        facts.link         = link;
        facts.flashing     = flashing;
        facts.halted       = target_->isHalted();
        facts.build        = builds_.getStatus();
        facts.logs         = mirror_.originalLogCount;
        facts.shownLogs    = mirror_.filteredOriginalLogCount;
        facts.trimmedLogs  = mirror_.trimmedLogCount;
        facts.filterActive = mirror_.filterActive;
        facts.search       = mirror_.searchText;
        if(refilter.inProgress) {
            facts.refilterPercent
              = refilter.total == 0
                ? 100U
                : static_cast<unsigned>(refilter.scanned * 100 / refilter.total);
        }
        facts.formatErrors       = counts.formatErrors;
        facts.errors             = counts.errors;
        facts.targetLost         = model.targetLost;
        facts.firmware           = model.firmware;
        facts.firmwareMismatches = model.firmwareMismatches;
        app_->show(statusBadges(facts));
    }
};

Gui::Gui(PrinterModel& model UC_LOG_LIFETIMEBOUND) : window_{std::make_unique<Window>(model)} {}

Gui::~Gui() = default;

void Gui::add(std::chrono::system_clock::time_point recvTime,
              printer::LogEntry const&              entry) {
    window_->add(recvTime, entry);
}

void Gui::setCompiledFilter(uc_log::Detail::FilterTable const& table) {
    window_->setCompiledFilter(table);
}

void Gui::setMapSource(MapSource source) { window_->setMapSource(std::move(source)); }

void Gui::triggerRedraw() { window_->requestFrame(); }

int Gui::run(TargetPort&        target,
             std::string const& buildCommand,
             std::string const& initialHost,
             std::string const& initialProbe) {
    return window_->run(target, buildCommand, initialHost, initialProbe);
}

}   // namespace uc_log::printer::rtx
