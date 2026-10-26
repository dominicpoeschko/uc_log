// the Health and Inspect tabs (records, stacks, rings, watches, profile).
// Window members (Window.hpp), one translation unit per tab: an edit of one tab recompiles this file only.
#include "uc_log/printer/tui/Window.hpp"

namespace uc_log::printer { namespace tui {

    auto Window::refreshInspectorView(bool halted) -> void {
        targetHalted = halted;
        std::scoped_lock<std::mutex> const lock{inspectorMutex};
        if(!inspector) { return; }
        using Page = uc_log::printer::TargetInspector::Page;
        inspector->setPage(selectedTab == healthTabIndex    ? Page::health
                           : selectedTab != inspectTabIndex ? Page::none
                           : selectedInspectTab == 0        ? Page::trace
                           : selectedInspectTab == 1        ? Page::watch
                                                            : Page::profile);
        if(auto const v = inspector->version(); v != inspectorViewVersion) {
            inspectorViewVersion = v;
            inspectorView        = inspector->snapshot();
            watchNames.clear();
            for(auto const& w : inspectorView.watches) { watchNames.push_back(w.name); }
            if(auto const it = std::ranges::find(watchNames, watchSelectPending);
               it != watchNames.end())
            {
                watchSelection = static_cast<int>(it - watchNames.begin());
                watchSelectPending.clear();
            }
            if(watchSelection >= static_cast<int>(watchNames.size())) {
                watchSelection = std::max(0, static_cast<int>(watchNames.size()) - 1);
            }
        }
        // the frames between versions keep the "changed just now" colour honest
        if(selectedTab == inspectTabIndex && selectedInspectTab == 1) {
            redrawPending.store(true, std::memory_order_relaxed);
        }
    }

    auto Window::getHealthComponent() -> ftxui::Component {
        for(auto const& c : uc_log::printer::records::panicCauses) {
            panicCauseEntries.emplace_back(c);
        }
        auto refreshBtn = printer::tui::button("🔄 Read now", printer::tui::Style::reset, [this]() {
            withInspector([](auto& i) { i.refresh(); });
        });
        // a record no boot line takes would keep FAULT / PANIC in the status line for good
        auto clearBtn
          = printer::tui::button("🧹 Clear records", printer::tui::Style::destructive, [this]() {
                bool const halted = targetHalted;
                withInspector([halted](auto& i) { i.clearRecords(halted); });
            });
        auto causeDropdown = ftxui::Dropdown(&panicCauseEntries, &panicCauseSelection);
        auto raiseBtn
          = printer::tui::button("🚨 Raise panic", printer::tui::Style::destructive, [this]() {
                if(!panicArmed) {
                    panicArmed = true;
                    return;
                }
                panicArmed       = false;
                auto const cause = static_cast<std::uint32_t>(panicCauseSelection);
                withInspector([cause](auto& i) { i.raisePanic(cause); });
            });
        // the first click arms, the second raises; a row of its own: beside the buttons it
        // squeezed them ("Raise pani") and was cut off itself
        auto armedRenderer = ftxui::Renderer([this]() {
            return panicArmed ? ftxui::text(
                                  " click \"Raise panic\" again to make the core call "
                                  "Kvasir::Panic::raise() - it resets or halts")
                                  | ftxui::color(Theme::Status::error()) | ftxui::bold
                              : ftxui::emptyElement();
        });
        auto controls      = ftxui::Container::Horizontal(
          {refreshBtn,
           printer::tui::spacer(1),
           clearBtn,
           ftxui::Renderer([]() { return ftxui::text("   panic cause "); }),
           causeDropdown,
           printer::tui::spacer(1),
           raiseBtn});
        auto view
          = scrollView([this]() { return inspector::healthElement(inspectorView, targetHalted); });
        auto left = ftxui::Container::Vertical(
          {controls, armedRenderer, printer::tui::divider(), view | ftxui::flex});
        return ftxui::Container::Horizontal(
          {left | ftxui::flex,
           printer::tui::divider(),
           getStatisticsComponent() | ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, 60)});
    }

    auto Window::getWatchComponent() -> ftxui::Component {
        auto input
          = trackInput(printer::tui::textInput(watchInput,
                                               "part of a symbol's name (more parts: space)",
                                               {},
                                               [this]() { addWatchFromInput(); }));
        // The focus stays in the input while the list under it is walked: Tab / Shift+Tab
        // go round it, the arrows leave it at its ends (to the tabs above, the watches
        // below), Esc empties the input.
        input          = ftxui::CatchEvent(input, [this](ftxui::Event const& e) {
            if(e == ftxui::Event::Escape) {
                if(watchInput.empty()) { return false; }
                watchInput.clear();
                watchStatus.clear();
                return true;
            }
            if(e != ftxui::Event::Tab && e != ftxui::Event::TabReverse
               && e != ftxui::Event::ArrowDown && e != ftxui::Event::ArrowUp
               && e != ftxui::Event::PageDown && e != ftxui::Event::PageUp)
            {
                return false;
            }
            refreshWatchCompletions();
            int const n = static_cast<int>(watchCompletions.best.size());
            if(n == 0) { return false; }
            int& s = watchCompletionSelection;
            if(e == ftxui::Event::Tab) {
                s = (s + 1) % n;
            } else if(e == ftxui::Event::TabReverse) {
                s = (s + n - 1) % n;
            } else if(e == ftxui::Event::ArrowDown) {
                if(s >= n - 1) { return false; }
                ++s;
            } else if(e == ftxui::Event::ArrowUp) {
                if(s <= 0) { return false; }
                --s;
            } else if(e == ftxui::Event::PageDown) {
                s = std::min(n - 1, s + watchCompletionRows - 1);
            } else {
                s = std::max(0, s - (watchCompletionRows - 1));
            }
            return true;
        });
        auto addBtn    = printer::tui::button("➕ watch", printer::tui::Style::positive, [this]() {
            addWatchFromInput();
        });
        auto removeBtn = printer::tui::button("✖ remove selected",
                                              printer::tui::Style::destructive,
                                              [this]() { removeWatchAt(watchSelection); });
        auto controls  = ftxui::Container::Horizontal(
          {input | ftxui::border | ftxui::flex, addBtn, printer::tui::spacer(1), removeBtn});
        auto matches = ftxui::Renderer([this]() {
            refreshWatchCompletions();
            std::vector<ftxui::Element> rows;
            if(!watchStatus.empty()) {
                rows.push_back(ftxui::text(watchStatus) | ftxui::color(Theme::Status::info()));
            }
            auto const listed = watchCompletions.best.size();
            if(watchInput.empty()) { return ftxui::vbox(std::move(rows)); }
            if(listed == 0) {
                rows.push_back(ftxui::text("no data symbol of the map matches")
                               | ftxui::color(Theme::Status::warning()));
            } else {
                rows.push_back(
                  ftxui::text(fmt::format("{} match{}{}: Tab / ⇧Tab / ↑↓ choose, Enter watches, a "
                                          "click watches or un-watches, Esc clears",
                                          watchCompletions.total,
                                          watchCompletions.total == 1 ? "" : "es",
                                          watchCompletions.total > listed
                                            ? fmt::format(", the best {} listed", listed)
                                            : std::string{}))
                  | ftxui::color(Theme::Text::metadata()));
            }
            return ftxui::vbox(std::move(rows));
        });

        PickListOption pick;
        pick.count = [this]() {
            refreshWatchCompletions();
            return static_cast<int>(watchCompletions.best.size());
        };
        pick.row = [this](int index, PickListOption::RowState state) {
            auto const& c = watchCompletions.best[static_cast<std::size_t>(index)];
            return inspector::completionRow(c,
                                            watchCompletionTerms,
                                            isWatched(c.name),
                                            state.selected);
        };
        pick.selected       = &watchCompletionSelection;
        pick.focusable      = false;
        pick.hoverSelects   = true;
        pick.onClick        = [this](int index, int, int) { pickWatchCompletion(index, true); };
        watchCompletionList = pickList(std::move(pick));

        PickListOption list;
        list.count = [this]() { return static_cast<int>(inspectorView.watches.size()); };
        list.row   = [this](int index, PickListOption::RowState state) {
            return inspector::watchRow(inspectorView.watches[static_cast<std::size_t>(index)],
                                       state.selected,
                                       state.hovered,
                                       state.focused);
        };
        list.empty = []() {
            return inspector::dim(
              "no watches: type a part of a symbol's name above and pick one of the matches");
        };
        list.rowsPerItem = inspector::watchRows;
        list.selected    = &watchSelection;
        list.onClick     = [this](int index, int line, int column) {
            if(line == 0 && column < inspector::watchRemoveColumns) { removeWatchAt(index); }
        };
        list.onDelete            = [this](int index) { removeWatchAt(index); };
        ftxui::Component watches = pickList(std::move(list));

        return ftxui::Container::Vertical(
          {controls,
           matches,
           ftxui::Component{watchCompletionList}
             | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, watchCompletionRows),
           ftxui::Renderer([]() {
               return ftxui::text(
                        "untyped: 1, 2, 4 or 8 bytes as numbers, else bytes (first "
                        "64), read 5x a second; ↑↓ or a click select, Del or ✖ "
                        "removes")
                    | ftxui::color(Theme::Text::metadata());
           }),
           printer::tui::divider(),
           watches | ftxui::flex});
    }

    auto Window::getProfileComponent() -> ftxui::Component {
        auto startBtn
          = printer::tui::button("▶ start / ■ stop", printer::tui::Style::positive, [this]() {
                bool const running = inspectorView.profiling;
                withInspector([running](auto& i) {
                    if(running) {
                        i.stopProfile();
                    } else {
                        i.startProfile();
                    }
                });
            });
        auto clearBtn = printer::tui::button("🗑 clear", printer::tui::Style::reset, [this]() {
            withInspector([](auto& i) { i.clearProfile(); });
        });
        auto view = scrollView([this]() { return inspector::profileElement(inspectorView, 100); });
        return ftxui::Container::Vertical(
          {ftxui::Container::Horizontal({startBtn, printer::tui::spacer(1), clearBtn}),
           printer::tui::divider(),
           view | ftxui::flex});
    }

}}   // namespace uc_log::printer::tui
