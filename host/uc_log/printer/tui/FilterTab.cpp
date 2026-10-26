// the Filter tab (levels, channels, locations, noisy locations, modules, saved filters).
// Window members (Window.hpp), one translation unit per tab: an edit of one tab recompiles this file only.
#include "uc_log/printer/tui/Window.hpp"

namespace uc_log::printer { namespace tui {

    auto Window::getLogLevelFilterComponent() -> ftxui::Component {
        static constexpr std::array levels{uc_log::LogLevel::trace,
                                           uc_log::LogLevel::debug,
                                           uc_log::LogLevel::info,
                                           uc_log::LogLevel::warn,
                                           uc_log::LogLevel::error,
                                           uc_log::LogLevel::crit};
        return printer::tui::toggleSetFilter<uc_log::LogLevel>(
          "📋 Enable All Levels",
          printer::tui::Style::reset,
          editedFilterState.enabledLogLevels,
          levels,
          [](uc_log::LogLevel level) { return std::string{enchantum::to_string(level)}; },
          [this] { updateCurrentFilter(); });
    }

    auto Window::getChannelFilterComponent() -> ftxui::Component {
        return printer::tui::toggleSetFilter<std::size_t>(
          "📡 Enable All Channels",
          printer::tui::Style::build,
          editedFilterState.enabledChannels,
          std::ranges::iota_view{std::size_t{0}, GUI_Constants::maxChannels},
          [](std::size_t channel) { return fmt::format("Channel {}", channel); },
          [this] { updateCurrentFilter(); });
    }

    auto Window::getLocationFilterComponent() -> ftxui::Component {
        auto addIncludeEntry = [this](printer::SourceLocation const& sourceLocation) {
            if(!editedFilterState.includedLocations.contains(sourceLocation)) {
                editedFilterState.includedLocations.insert(sourceLocation);
                updateCurrentFilter();
            }
        };

        auto addExcludeEntry = [this](printer::SourceLocation const& sourceLocation) {
            if(!editedFilterState.excludedLocations.contains(sourceLocation)) {
                editedFilterState.excludedLocations.insert(sourceLocation);
                updateCurrentFilter();
            }
        };

        auto stringToSourceLocation
          = [](std::string const& input) -> std::optional<printer::SourceLocation> {
            auto colonPosition = std::ranges::find(input, ':');
            if(colonPosition == input.end()) {
                if(input.empty()) { return std::nullopt; }
                return printer::SourceLocation{input, 0};
            }
            std::size_t line{};
            auto parseResult = std::from_chars(&(*(colonPosition + 1)), &(*input.end()), line);
            if(parseResult.ec == std::errc{} && parseResult.ptr == &(*input.end())) {
                return printer::SourceLocation{
                  std::string_view{input.begin(), colonPosition},
                  line
                };
            }
            return std::nullopt;
        };

        std::vector<ftxui::Component> manualInputComponents;
        manualInputComponents.push_back(ftxui::Renderer([] {
            return ftxui::text("📝 Manual:") | ftxui::bold | ftxui::color(Theme::Header::accent());
        }));
        manualLocationInput
          = trackInput(printer::tui::textInput(locationFilterInput, "filename:line") | ftxui::flex);
        manualInputComponents.push_back(manualLocationInput);
        manualInputComponents.push_back(
          ftxui::Maybe(printer::tui::button("🟢 Include",
                                            printer::tui::Style::positive,
                                            [this, addIncludeEntry, stringToSourceLocation]() {
                                                auto sourceLocation
                                                  = stringToSourceLocation(locationFilterInput);
                                                if(sourceLocation) {
                                                    addIncludeEntry(*sourceLocation);
                                                    locationFilterInput.clear();
                                                }
                                            }),
                       [this, stringToSourceLocation]() {
                           return stringToSourceLocation(locationFilterInput).has_value();
                       }));
        manualInputComponents.push_back(
          ftxui::Maybe(printer::tui::button("🔴 Exclude",
                                            printer::tui::Style::destructive,
                                            [this, addExcludeEntry, stringToSourceLocation]() {
                                                auto sourceLocation
                                                  = stringToSourceLocation(locationFilterInput);
                                                if(sourceLocation) {
                                                    addExcludeEntry(*sourceLocation);
                                                    locationFilterInput.clear();
                                                }
                                            }),
                       [this, stringToSourceLocation]() {
                           return stringToSourceLocation(locationFilterInput).has_value();
                       }));

        auto manualInputComponent = ftxui::Container::Horizontal(manualInputComponents);

        std::vector<ftxui::Component> dropdownComponents;
        dropdownComponents.push_back(ftxui::Renderer([] {
            return ftxui::text("📋 Known:") | ftxui::bold | ftxui::color(Theme::Status::info());
        }));

        ftxui::DropdownOption dropdownOptions;
        dropdownOptions.radiobox.entries
          = std::make_unique<StringVectorAdapter>(display.locationLabels);
        dropdownOptions.radiobox.selected  = &selectedLocationIndex;
        dropdownOptions.radiobox.on_change = [this]() {
            auto const index = static_cast<std::size_t>(selectedLocationIndex);
            if(index < display.locationList.size()) {
                selectedSourceLocation = display.locationList[index].first;
            }
        };

        dropdownOptions.radiobox.transform
          = [this](ftxui::EntryState const& state) -> ftxui::Element {
            auto const index = static_cast<std::size_t>(state.index);
            if(index >= display.locationList.size()) { return ftxui::text(state.label); }
            printer::SourceLocation const& location = display.locationList[index].first;

            bool const isIncluded = editedFilterState.includedLocations.contains(location);
            bool const isExcluded = editedFilterState.excludedLocations.contains(location);

            auto element = ftxui::text(state.label);

            if(state.active) { element |= ftxui::bold; }
            if(state.focused) { element |= ftxui::inverted; }

            if(isIncluded) {
                element = ftxui::hbox(
                  {ftxui::text("🟢 ") | ftxui::color(Theme::Status::success()), element});
            } else if(isExcluded) {
                element = ftxui::hbox(
                  {ftxui::text("🔴 ") | ftxui::color(Theme::Status::error()), element});
            } else {
                element = ftxui::hbox(
                  {ftxui::text("⚪ ") | ftxui::color(Theme::Status::inactive()), element});
            }

            return element;
        };

        dropdownComponents.push_back(ftxui::Dropdown(dropdownOptions));

        auto getSelectedLocation = [this]() -> printer::SourceLocation {
            auto const index = static_cast<std::size_t>(selectedLocationIndex);
            if(index < display.locationList.size()) { return display.locationList[index].first; }
            return {};
        };

        auto hasSelectedLocation
          = [getSelectedLocation]() { return !getSelectedLocation().first.empty(); };

        auto includeLineButton
          = ftxui::Maybe(printer::tui::button("🟢 Include Line",
                                              printer::tui::Style::positive,
                                              [addIncludeEntry, getSelectedLocation]() {
                                                  addIncludeEntry(getSelectedLocation());
                                              }),
                         hasSelectedLocation);
        auto excludeLineButton
          = ftxui::Maybe(printer::tui::button("🔴 Exclude Line",
                                              printer::tui::Style::destructive,
                                              [addExcludeEntry, getSelectedLocation]() {
                                                  addExcludeEntry(getSelectedLocation());
                                              }),
                         hasSelectedLocation);

        auto includeFileButton = ftxui::Maybe(
          printer::tui::button("📁🟢 Include File",
                               printer::tui::Style::settings,
                               [addIncludeEntry, getSelectedLocation]() {
                                   addIncludeEntry(
                                     printer::SourceLocation{getSelectedLocation().first, 0});
                               }),
          hasSelectedLocation);
        auto excludeFileButton = ftxui::Maybe(
          printer::tui::button("📁🔴 Exclude File",
                               printer::tui::Style::danger,
                               [addExcludeEntry, getSelectedLocation]() {
                                   addExcludeEntry(
                                     printer::SourceLocation{getSelectedLocation().first, 0});
                               }),
          hasSelectedLocation);

        auto lineButtons = ftxui::Container::Horizontal({includeLineButton, excludeLineButton});
        auto fileButtons = ftxui::Container::Horizontal({includeFileButton, excludeFileButton});

        auto buttonRows
          = ftxui::Container::Vertical({lineButtons, fileButtons})
          | ftxui::Renderer([lineButtons, fileButtons](ftxui::Element const&) {
                return ftxui::vbox({lineButtons->Render(),
                                    ftxui::separator() | ftxui::color(Theme::UI::separator()),
                                    fileButtons->Render()});
            });

        dropdownComponents.push_back(buttonRows);

        auto dropdownComponent = ftxui::Container::Horizontal(dropdownComponents);

        std::vector<ftxui::Component> inputSectionComponents;
        inputSectionComponents.push_back(ftxui::Renderer([] {
            return ftxui::text("📍 Location Filters") | ftxui::bold
                 | ftxui::color(Theme::Header::accent()) | ftxui::center;
        }));
        inputSectionComponents.push_back(manualInputComponent);
        inputSectionComponents.push_back(dropdownComponent);

        auto inputComponent = ftxui::Container::Vertical(inputSectionComponents) | ftxui::border;

        auto includedContainerWithBorder
          = printer::tui::locationList(editedFilterState.includedLocations,
                                       "✅ Included Locations",
                                       Theme::Status::success(),
                                       "🟢",
                                       [this] { updateCurrentFilter(); });
        auto excludedContainerWithBorder
          = printer::tui::locationList(editedFilterState.excludedLocations,
                                       "❌ Excluded Locations",
                                       Theme::Status::error(),
                                       "🔴",
                                       [this] { updateCurrentFilter(); });

        auto locationsContainer = ftxui::Container::Horizontal(
          {includedContainerWithBorder | ftxui::flex, excludedContainerWithBorder | ftxui::flex});

        std::vector<ftxui::Component> finalComponents;
        finalComponents.push_back(inputComponent);
        finalComponents.push_back(locationsContainer);

        return ftxui::Container::Vertical(finalComponents);
    }

    auto Window::getFilterComponent() -> ftxui::Component {
        exportDirInputComponent = trackInput(printer::tui::textInput(exportDirInput, "directory"));

        auto exportBtn
          = printer::tui::button(" Export Filtered ", printer::tui::Style::positive, [this]() {
                if(exportDirInput.empty()) { return; }
                // the display snapshot is a cheap copy of chunk pointers and stays
                // valid regardless of concurrent appends/trims
                pendingActions.emplace_back([this,
                                             dir       = exportDirInput,
                                             entries   = display.entries,
                                             plainText = exportFormatSelection == 1]() {
                    exportFilteredLogs(dir, entries, plainText);
                });
            });

        auto exportFormatToggle
          = ftxui::Toggle(std::vector<std::string>{" .rttlog ", " .txt "}, &exportFormatSelection);

        auto exportSection
          = ftxui::Container::Horizontal(
              {ftxui::Renderer([]() { return ftxui::text(" Export dir: "); }),
               exportDirInputComponent | ftxui::border | ftxui::flex,
               ftxui::Renderer([]() { return ftxui::text(" as "); }),
               exportFormatToggle,
               printer::tui::spacer(1),
               exportBtn})
          | ftxui::Renderer([this, exportBtn](ftxui::Element inner) {
                auto resultEl
                  = lastExportPath.empty()
                    ? ftxui::text("")
                    : ftxui::text(
                        lastExportOk
                          ? fmt::format(" {} entries saved to {}", lastExportCount, lastExportPath)
                          : fmt::format(" Export failed: {}", lastExportPath))
                        | ftxui::color(lastExportOk ? Theme::Status::success()
                                                    : Theme::Status::error());
                return ftxui::vbox({ftxui::text("💾 Export Filtered Logs") | ftxui::bold
                                      | ftxui::color(Theme::Header::accent()) | ftxui::center,
                                    ftxui::separator(),
                                    std::move(inner),
                                    std::move(resultEl)})
                     | ftxui::border;
            });

        auto clearButton
          = printer::tui::button("🗑️ Clear All Filters", printer::tui::Style::destructive, [this]() {
                editedFilterState   = printer::FilterState{};
                ucTimeLiveWindowStr = "10";
                minUcTimeStr.clear();
                maxUcTimeStr.clear();
                store.updateUcTime([](printer::EntryStore::UcTimeWindow& w, double /*dataMax*/) {
                    w.enabled        = false;
                    w.live           = false;
                    w.liveWindowSecs = 10.0;
                    w.minSec         = 0.0;
                    w.maxSec         = std::numeric_limits<double>::infinity();
                    return true;
                });
                updateCurrentFilter();
            });

        // file I/O runs as a pending action after the UI mutex is released, so a slow
        // filesystem cannot stall input handling or the log producer
        auto saveButton = printer::tui::button("💾 Save", printer::tui::Style::positive, [this]() {
            pendingActions.emplace_back(
              [this, path = filterConfigPath, filterState = editedFilterState]() {
                  saveFilterConfig(path, filterState);
              });
        });

        auto loadButton = printer::tui::button("📂 Load", printer::tui::Style::settings, [this]() {
            pendingActions.emplace_back(
              [this, path = filterConfigPath]() { loadFilterConfig(path, editedFilterState); });
        });

        filterConfigInput = trackInput(printer::tui::textInput(filterConfigPath, "filter.json"));

        // File row: label + input + status
        auto fileRow
          = ftxui::Container::Horizontal({filterConfigInput})
          | ftxui::Renderer([this](ftxui::Element inner) {
                ftxui::Element statusEl = ftxui::text("");
                if(!filterConfigStatus.empty()) {
                    statusEl = ftxui::text("  " + filterConfigStatus)
                             | ftxui::color(filterConfigFailed ? Theme::Status::error()
                                                               : Theme::Status::success());
                }
                return ftxui::hbox({ftxui::text(" 📁 File: ") | ftxui::color(Theme::Status::info()),
                                    std::move(inner) | ftxui::flex,
                                    std::move(statusEl)});
            });

        // Buttons row: indented to align under the input, with gap between buttons
        auto buttonsRow = ftxui::Container::Horizontal({saveButton, loadButton})
                        | ftxui::Renderer([saveButton, loadButton](ftxui::Element const&) {
                              return ftxui::hbox({ftxui::text(" 📁       "),
                                                  saveButton->Render(),
                                                  ftxui::text("  "),
                                                  loadButton->Render()});
                          });

        // Info hint
        auto infoBox = ftxui::Renderer([] {
            return ftxui::vbox(
              {ftxui::separator(),
               ftxui::hbox(
                 {ftxui::text(" ℹ ") | ftxui::bold | ftxui::color(Theme::Status::info()),
                  ftxui::text("Filter File") | ftxui::bold | ftxui::color(Theme::Status::info())}),
               ftxui::text("   Saves/loads filter state as JSON. Relative "
                           "paths use the working directory.")
                 | ftxui::color(Theme::Status::inactive())});
        });

        auto saveLoadSection
          = ftxui::Container::Vertical({fileRow, buttonsRow})
          | ftxui::Renderer([infoBox](ftxui::Element inner) {
                return ftxui::vbox({ftxui::text("💾 Filter Configuration") | ftxui::bold
                                      | ftxui::color(Theme::Header::secondary()) | ftxui::center,
                                    ftxui::separator(),
                                    std::move(inner),
                                    infoBox->Render()})
                     | ftxui::border;
            });

        std::vector<ftxui::Component> mainComponents;

        std::vector<ftxui::Component> levelComponents;
        levelComponents.push_back(ftxui::Renderer([] {
            return ftxui::text("📊 Log Levels") | ftxui::bold
                 | ftxui::color(Theme::Header::primary()) | ftxui::center;
        }));
        levelComponents.push_back(getLogLevelFilterComponent());

        std::vector<ftxui::Component> channelComponents;
        channelComponents.push_back(ftxui::Renderer([] {
            return ftxui::text("📡 Channels") | ftxui::bold | ftxui::color(Theme::Header::warning())
                 | ftxui::center;
        }));
        channelComponents.push_back(getChannelFilterComponent());

        std::vector<ftxui::Component> moduleComponents;
        moduleComponents.push_back(ftxui::Renderer([] {
            return ftxui::text("🧩 Modules") | ftxui::bold | ftxui::color(Theme::Header::accent())
                 | ftxui::center;
        }));
        moduleComponents.push_back(getModuleFilterComponent());

        std::vector<ftxui::Component> horizontalComponents;
        horizontalComponents.push_back(ftxui::Container::Vertical(levelComponents) | ftxui::flex);
        horizontalComponents.push_back(ftxui::Container::Vertical(channelComponents) | ftxui::flex);
        horizontalComponents.push_back(ftxui::Container::Vertical(moduleComponents) | ftxui::flex);

        mainComponents.push_back(ftxui::Container::Horizontal(horizontalComponents));
        mainComponents.push_back(getLocationFilterComponent());

        // --- Noisy-exclude section ---
        // Method selector toggle
        std::vector<std::string> methodLabels{"Statistical", "Top Percent", "Count Limit"};
        auto methodToggle = ftxui::Toggle(std::move(methodLabels), &selectedOutlierMethod);
        methodToggle      = ftxui::CatchEvent(methodToggle, [this](ftxui::Event const&) -> bool {
            outlierMethod = static_cast<OutlierMethod>(selectedOutlierMethod);
            noiseExcludeStatus.clear();
            return false;
        });

        // Statistical (IQR/Tukey) sensitivity param row (float text input, shown only for method 0)
        iqrInput
          = trackInput(printer::tui::numberInput<double>(iqrMultiplierStr, "1.5", [this](double v) {
                if(v > 0.0) { iqrMultiplier = v; }
            }));
        auto iqrParamRow = ftxui::Maybe(
          ftxui::Container::Horizontal({iqrInput}) | ftxui::Renderer([](ftxui::Element inner) {
              return ftxui::hbox(
                {ftxui::text(" sensitivity: ") | ftxui::color(Theme::Status::info()),
                 std::move(inner) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 8)});
          }),
          [this] { return selectedOutlierMethod == 0; });

        // Top Percent param row (float text input, shown only for method 1)
        topNInput
          = trackInput(printer::tui::numberInput<double>(topNPercentStr, "10", [this](double v) {
                if(v > 0.0 && v < 100.0) { topNPercent = v; }
            }));
        auto topNParamRow = ftxui::Maybe(
          ftxui::Container::Horizontal({topNInput}) | ftxui::Renderer([](ftxui::Element inner) {
              return ftxui::hbox({ftxui::text(" top %: ") | ftxui::color(Theme::Status::info()),
                                  std::move(inner) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 8)});
          }),
          [this] { return selectedOutlierMethod == 1; });

        // Count Limit param row (integer text input, shown only for method 2)
        absInput = trackInput(
          printer::tui::numberInput<std::size_t>(absoluteThresholdStr,
                                                 "100",
                                                 [this](std::size_t v) { absoluteThreshold = v; }));
        auto absParamRow = ftxui::Maybe(
          ftxui::Container::Horizontal({absInput}) | ftxui::Renderer([](ftxui::Element inner) {
              return ftxui::hbox({ftxui::text(" count > ") | ftxui::color(Theme::Status::info()),
                                  std::move(inner) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 10)});
          }),
          [this] { return selectedOutlierMethod == 2; });

        // Enhanced preview: delegates to the memoized computeOutliers — no duplication
        auto previewRenderer = ftxui::Renderer([this] {
            auto const& r = currentOutliers();
            if(!r.valid) {
                return ftxui::text("  (need ≥ 3 known locations to preview)")
                     | ftxui::color(Theme::Status::inactive());
            }
            auto const n = display.locationList.size();
            auto const w = r.wouldExclude.size();
            return ftxui::vbox(
              {ftxui::text(
                 fmt::format("  → {} of {} location{} would be excluded", w, n, n == 1 ? "" : "s"))
                 | ftxui::color(w > 0 ? Theme::Status::warning() : Theme::Status::inactive()),
               ftxui::text(fmt::format("    cutoff: {}", r.cutoff))
                 | ftxui::color(Theme::Status::inactive())});
        });

        auto excludeButton = printer::tui::button("🎯 Exclude Outliers",
                                                  printer::tui::Style::debug,
                                                  [this]() { autoExcludeNoisyLocations(); });

        auto noiseButtonRow
          = ftxui::Container::Horizontal({excludeButton})
          | ftxui::Renderer([this, excludeButton](ftxui::Element const&) {
                ftxui::Element statusEl = ftxui::text("");
                if(!noiseExcludeStatus.empty()) {
                    bool isError = noiseExcludeStatus.starts_with("Need")
                                || noiseExcludeStatus.starts_with("No outliers");
                    statusEl     = ftxui::text("  " + noiseExcludeStatus)
                                 | ftxui::color(isError ? Theme::Status::inactive()
                                                        : Theme::Status::success());
                }
                return ftxui::hbox({ftxui::text(" ") | ftxui::color(ftxui::Color::Default),
                                    excludeButton->Render(),
                                    std::move(statusEl)});
            });

        auto noiseInfoBox = ftxui::Renderer([] {
            return ftxui::vbox(
              {ftxui::separator(),
               ftxui::hbox({ftxui::text(" ℹ ") | ftxui::bold | ftxui::color(Theme::Status::info()),
                            ftxui::text("Noisy Locations") | ftxui::bold
                              | ftxui::color(Theme::Status::info())}),
               ftxui::text("   Statistical: auto-detects outliers using inter-quartile range")
                 | ftxui::color(Theme::Status::inactive()),
               ftxui::text("   Top Percent: exclude the N% most frequent locations")
                 | ftxui::color(Theme::Status::inactive()),
               ftxui::text("   Count Limit: exclude locations seen more than N times")
                 | ftxui::color(Theme::Status::inactive())});
        });

        auto noisyExcludeSection
          = ftxui::Container::Vertical({methodToggle,
                                        iqrParamRow,
                                        topNParamRow,
                                        absParamRow,
                                        previewRenderer,
                                        noiseButtonRow})
          | ftxui::Renderer([noiseInfoBox](ftxui::Element inner) {
                return ftxui::vbox({ftxui::text("🎯 Auto-Exclude Noisy Locations") | ftxui::bold
                                      | ftxui::color(Theme::Header::accent()) | ftxui::center,
                                    ftxui::separator(),
                                    std::move(inner),
                                    noiseInfoBox->Render()})
                     | ftxui::border;
            });

        // ── Time Filter section ──────────────────────────────────────────────
        // an empty bound is none: 0 below, infinity above
        ucTimeMinInput        = trackInput(printer::tui::numberInput<double>(
          minUcTimeStr,
          "0.0",
          [this](double value) {
              store.updateUcTime([value](printer::EntryStore::UcTimeWindow& w, double /*dataMax*/) {
                  w.enabled = true;
                  w.live    = false;
                  w.minSec  = value;
                  return false;
              });
              store.scheduleRefilterDebounced();
          },
          0.0));
        ucTimeMaxInput        = trackInput(printer::tui::numberInput<double>(
          maxUcTimeStr,
          "∞",
          [this](double value) {
              store.updateUcTime([value](printer::EntryStore::UcTimeWindow& w, double /*dataMax*/) {
                  w.enabled = true;
                  w.live    = false;
                  w.maxSec  = value;
                  return false;
              });
              store.scheduleRefilterDebounced();
          },
          std::numeric_limits<double>::infinity()));
        ucTimeLiveWindowInput = trackInput(
          printer::tui::numberInput<double>(ucTimeLiveWindowStr, "10", [this](double value) {
              store.updateUcTime([value](printer::EntryStore::UcTimeWindow& w, double /*dataMax*/) {
                  w.liveWindowSecs = value;
                  return false;
              });
          }));

        auto ucTimeCheckbox = functionCheckbox(
          "Enable",
          [this]() { return display.ucTimeFilterEnabled; },
          [this]() {
              store.updateUcTime([](printer::EntryStore::UcTimeWindow& w, double /*dataMax*/) {
                  w.enabled = !w.enabled;
                  if(!w.enabled) { w.live = false; }
                  return true;
              });
          });

        auto makeStaticPreset = [this](char const* label, double from, double to) {
            return printer::tui::button(label, printer::tui::Style::build, [this, from, to]() {
                minUcTimeStr = fmt::format("{:.1f}", from);
                maxUcTimeStr = std::isinf(to) ? "" : fmt::format("{:.1f}", to);
                store.updateUcTime(
                  [from, to](printer::EntryStore::UcTimeWindow& w, double /*dataMax*/) {
                      w.live    = false;
                      w.enabled = true;
                      w.minSec  = from;
                      w.maxSec  = to;
                      return true;
                  });
            });
        };
        auto makeLivePreset = [this](char const* label, double secs) {
            return printer::tui::button(label, printer::tui::Style::settings, [this, secs]() {
                ucTimeLiveWindowStr = fmt::format("{:.0f}", secs);
                maxUcTimeStr.clear();
                store.updateUcTime(
                  [this, secs](printer::EntryStore::UcTimeWindow& w, double dataMax) {
                      w.liveWindowSecs = secs;
                      w.live           = true;
                      w.enabled        = true;
                      w.minSec         = std::max(0.0, dataMax - secs);
                      w.maxSec         = std::numeric_limits<double>::infinity();
                      minUcTimeStr     = fmt::format("{:.1f}", w.minSec);
                      return true;
                  });
            });
        };

        auto resetUcTimeBtn
          = printer::tui::button(" ✕ Reset ", printer::tui::Style::destructive, [this]() {
                minUcTimeStr.clear();
                maxUcTimeStr.clear();
                store.updateUcTime([](printer::EntryStore::UcTimeWindow& w, double /*dataMax*/) {
                    w.enabled = false;
                    w.live    = false;
                    w.minSec  = 0.0;
                    w.maxSec  = std::numeric_limits<double>::infinity();
                    return true;
                });
            });

        auto inputWidth     = ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 8);
        auto liveInputWidth = ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 6);

        auto ucTimeSection
          = ftxui::Container::Vertical(
              {ftxui::Container::Horizontal({ucTimeCheckbox, resetUcTimeBtn}),
               ftxui::Container::Horizontal(
                 {ftxui::Renderer([]() { return ftxui::text(" From: "); }),
                  ucTimeMinInput | inputWidth,
                  ftxui::Renderer([]() { return ftxui::text(" s   To: "); }),
                  ucTimeMaxInput | inputWidth,
                  ftxui::Renderer([]() { return ftxui::text(" s"); })}),
               ftxui::Container::Horizontal({makeStaticPreset(" First 10 s ", 0.0, 10.0),
                                             makeStaticPreset(" First 30 s ", 0.0, 30.0),
                                             makeStaticPreset(" First 1 m ", 0.0, 60.0)}),
               ftxui::Container::Horizontal(
                 {makeLivePreset(" ⟳ 10 s ", 10.0),
                  makeLivePreset(" ⟳ 30 s ", 30.0),
                  makeLivePreset(" ⟳ 1 m ", 60.0),
                  ftxui::Renderer([]() { return ftxui::text("  Custom: "); }),
                  ucTimeLiveWindowInput | liveInputWidth,
                  ftxui::Renderer([]() { return ftxui::text(" s"); })})})
          | ftxui::Renderer([this](ftxui::Element inner) {
                auto dataStr = std::isinf(display.ucTimeDataMin)
                               ? std::string{"--"}
                               : fmt::format("{:.1f} – {:.1f} s",
                                             display.ucTimeDataMin,
                                             display.ucTimeDataMax);
                auto liveStr = display.ucTimeLiveMode
                               ? fmt::format(" ⟳ live: last {:.0f} s", display.ucTimeLiveWindowSecs)
                               : std::string{};
                return ftxui::vbox(
                         {ftxui::text("⏱ UC Time Filter") | ftxui::bold
                            | ftxui::color(Theme::Header::primary()) | ftxui::center,
                          ftxui::separator(),
                          std::move(inner),
                          ftxui::hbox(
                            {ftxui::text(" Data: ") | ftxui::color(Theme::Text::normal()),
                             ftxui::text(dataStr) | ftxui::color(Theme::Header::accent()),
                             ftxui::text(liveStr) | ftxui::color(Theme::Text::normal())})})
                     | ftxui::border;
            });

        auto timeFilterSection = ucTimeSection;

        auto clearLogButton = printer::tui::button("❌ Clear All Log Entries",
                                                   printer::tui::Style::destructive,
                                                   [this]() { store.clearAll(); });

        auto clearBootButton = printer::tui::button("🔄 Clear Log Entries Before Last Boot",
                                                    printer::tui::Style::destructive,
                                                    [this]() { store.clearBeforeLastBoot(); });

        return ftxui::Container::Vertical(
          {ftxui::Container::Horizontal({clearButton | ftxui::flex,
                                         clearLogButton | ftxui::flex,
                                         clearBootButton | ftxui::flex}),
           printer::tui::divider(),
           timeFilterSection,
           printer::tui::divider(),
           ftxui::Container::Vertical(mainComponents) | ftxui::Renderer([](ftxui::Element inner) {
               return ftxui::vbox({ftxui::text("🔍 Filter Settings") | ftxui::bold
                                     | ftxui::color(Theme::Header::primary()) | ftxui::center,
                                   ftxui::separator(),
                                   std::move(inner)});
           }),
           ftxui::Container::Horizontal({saveLoadSection, noisyExcludeSection})
             | ftxui::Renderer([saveLoadSection, noisyExcludeSection](ftxui::Element const&) {
                   return ftxui::hbox({saveLoadSection->Render() | ftxui::flex,
                                       noisyExcludeSection->Render() | ftxui::flex});
               }),
           exportSection});
    }

}}   // namespace uc_log::printer::tui
