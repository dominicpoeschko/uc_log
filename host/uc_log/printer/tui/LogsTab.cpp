// the Logs tab (the list, export).
// Window members (Window.hpp), one translation unit per tab: an edit of one tab recompiles this file only.
#include "uc_log/printer/tui/Window.hpp"

namespace uc_log::printer { namespace tui {

    auto Window::processLogMessage(std::string_view originalMsg) const -> std::string {
        std::string processedMsg{originalMsg};
        std::size_t pos = 0;

        // Process @METRIC(...) markers
        if(!showMetricString) {
            pos = 0;
            while(auto const marker = nextMetricMarker(processedMsg, pos)) {
                std::string shown{marker->value};
                if(!marker->unit.empty()) {
                    shown += ' ';
                    shown += marker->unit;
                }
                std::size_t const begin = marker->begin;
                processedMsg.replace(begin, marker->end - begin, shown);
                pos = begin + shown.size();
            }
        }

        // Process @TYPENAME(...) markers
        pos = 0;
        while((pos = processedMsg.find("@TYPENAME(", pos)) != std::string::npos) {
            std::size_t const startPos = pos;
            pos += 10;

            std::size_t const endPos = processedMsg.find(')', pos);
            if(endPos == std::string::npos) { break; }

            if(showTypenameString) {
                // Show the typename content
                std::string const typenameContent{processedMsg.substr(pos, endPos - pos)};
                processedMsg.replace(startPos, endPos - startPos + 1, typenameContent);
                pos = startPos + typenameContent.length();
            } else {
                // Hide the entire @TYPENAME(...) marker
                processedMsg.erase(startPos, endPos - startPos + 1);
                pos = startPos;
            }
        }

        return processedMsg;
    }

    auto Window::exportFilteredLogs(std::string const&                       dir,
                                    printer::EntryChunkList::Snapshot const& entries,
                                    bool                                     asPlainText) -> void {
        namespace lf       = uc_log::printer::logformat;
        namespace fs       = std::filesystem;
        auto const    path = fs::path{dir}
                           / fmt::format("filtered_{}.{}",
                                         lf::toIso8601Utc(std::chrono::system_clock::now()),
                                         asPlainText ? "txt" : "rttlog");
        std::ofstream f{path};
        if(!f.is_open()) {
            lastExportPath = path.string();
            lastExportOk   = false;
            messages.error(fmt::format("Export failed — cannot write: {:?}", path));
            return;
        }
        if(!asPlainText) { lf::writeHeader(f); }
        for(auto const& e : entries) {
            auto const& c = *e.common;
            if(asPlainText) {
                f << rowText(e, true) << '\n';
                continue;
            }
            uc_log::printer::LogEntry line{c.channel, {}};
            line.ucTime       = c.ucTime;
            line.fileName     = display.fileNameOf(c.locationKey);
            line.line         = printer::EntryStore::Mirror::lineOf(c.locationKey);
            line.logLevel     = c.logLevel;
            line.functionName = display.functionNameOf(c.functionId);
            line.module       = display.moduleNameOf(c.moduleId);
            line.logMsg       = std::string{e.lineText()};
            line.parsedOk     = c.parsedOk;
            lf::writeEntry(f, c.recvTime, line);
        }
        lastExportPath  = path.string();
        lastExportCount = entries.size();
        lastExportOk    = true;
        messages.status(fmt::format("{} entries saved to {}", entries.size(), path.string()));
    }

    auto Window::getLogComponent() -> ftxui::Component {
        auto scroller = makeScroller(
          [this]() -> printer::EntryChunkList::Snapshot const& { return display.entries; },
          [this](auto const& entry) { return defaultRender(entry); });
        scroller->onYank = [this](std::size_t index) { yankEntryToClipboard(index); };
        logScrollerJump  = [scroller](int index) { scroller->jumpTo(index); };

        searchInput = trackInput(printer::tui::textInput(searchStr, "text or re:pattern", [this]() {
            searchRowShown = true;
            store.setSearchText(searchStr);
        }));

        jumpToInput
          = trackInput(printer::tui::textInput(jumpToStr, "s", {}, [this]() { jumpToUcTime(); })
                       | printer::tui::numericFilter(true));

        auto searchRow = ftxui::Container::Horizontal(
          {ftxui::Renderer([]() { return ftxui::text(" 🔎 [/] "); }),
           searchInput | ftxui::border | ftxui::flex,
           ftxui::Renderer([]() { return ftxui::text("  ⏱ go to "); }),
           jumpToInput | ftxui::border | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 10),
           ftxui::Renderer([]() { return ftxui::text(" s ⏎ "); })});

        // hidden until '/' focuses it (or text is present), so it costs no space while idle
        auto searchRowVisible
          = [this]() { return searchRowShown || !searchStr.empty() || !jumpToStr.empty(); };
        // under the log, not in the row: a signature can run to kilobytes
        auto signaturePanel = ftxui::Renderer([this, scroller]() {
            auto const  index     = static_cast<std::size_t>(std::max(0, scroller->selectedIndex));
            std::string signature = "(no line)";
            if(index < display.entries.size()) {
                signature = display.functionSignatureOf(display.entries[index].common->functionId);
            }
            return ftxui::paragraph(signature) | ftxui::color(Theme::Text::functionName())
                 | ftxui::border | ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, 12);
        });
        auto logsTab
          = ftxui::Container::Vertical({ftxui::Maybe(searchRow, std::move(searchRowVisible)),
                                        ftxui::Component{scroller} | ftxui::flex,
                                        ftxui::Maybe(signaturePanel, &showFullSignature)});

        // Esc leaves the search/jump inputs, hides the row (unless a filter is
        // active) and returns to the log list, so the hotkeys (y, q, ...) work
        // again immediately
        return ftxui::CatchEvent(logsTab, [this, scroller](ftxui::Event const& event) {
            if(event == ftxui::Event::Escape
               && ((searchInput && searchInput->Focused())
                   || (jumpToInput && jumpToInput->Focused())))
            {
                searchRowShown = false;
                scroller->TakeFocus();
                return true;
            }
            return false;
        });
    }

}}   // namespace uc_log::printer::tui
