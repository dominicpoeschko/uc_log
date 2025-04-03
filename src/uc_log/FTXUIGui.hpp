#pragma once

#include "uc_log/detail/LogEntry.hpp"

#include <chrono>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace uc_log { namespace FTXUIGui {

    template<typename ContainerGetter, typename Transform>
    class ScrollerBase : public ftxui::ComponentBase {
    public:
        ContainerGetter containerGetter_;
        Transform       transform_;

        ScrollerBase(ContainerGetter&& c, Transform&& tf) : containerGetter_{c}, transform_{tf} {}

    private:
        ftxui::Element OnRender() {
            auto const& container = containerGetter_();
            size_                 = container.size();
            int const ySpace      = (box_.y_max - box_.y_min) + 1;
            selected_             = std::max(0, std::min(size_ - 1, selected_));
            if(stick) {
                selected_ = size_ - 1;
            }

            int hiddenBefore{};
            int hiddenBehind{};

            if(size_ > ySpace) {
                int const toMany = size_ - ySpace;

                if(stick) {
                    hiddenBefore = toMany;
                    hiddenBehind = 0;
                } else {
                    int foo      = selected_ - (ySpace / 2) + (ySpace % 2 == 0 ? 1 : 0);
                    hiddenBefore = foo >= 0 ? foo : 0;
                    hiddenBehind = toMany - hiddenBefore;
                    if(hiddenBehind < 0) {
                        hiddenBefore = hiddenBefore + hiddenBehind;
                        hiddenBehind = 0;
                    }
                }
            }
            ftxui::Elements elements;
            elements.reserve(static_cast<std::size_t>(ySpace + 2));

            elements.push_back(
              ftxui::text("")
              | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, std::min(hiddenBefore, 8192)));

            int x = 0;
            for(auto const& e : container | std::views::drop(hiddenBefore)) {
                bool const isSlected = x + hiddenBefore == selected_;

                if(isSlected) {
                    auto const selectedStyle
                      = Focused() && !stick ? ftxui::inverted : ftxui::nothing;
                    auto const selectedFocus = Focused() ? ftxui::focus : ftxui::select;
                    elements.push_back(transform_(*e) | selectedStyle | selectedFocus);
                } else {
                    elements.push_back(transform_(*e));
                }

                ++x;
                if(x == ySpace) {
                    break;
                }
            }

            elements.push_back(
              ftxui::text("") | size(ftxui::HEIGHT, ftxui::EQUAL, std::min(hiddenBehind, 8192)));

            ftxui::Element background = ftxui::vbox(elements);
            background->ComputeRequirement();

            return std::move(background) | ftxui::vscroll_indicator | ftxui::yframe | ftxui::yflex
                 | ftxui::reflect(box_);
        }

        bool OnEvent(ftxui::Event event) final {
            if(event.is_mouse() && box_.Contain(event.mouse().x, event.mouse().y)) {
                TakeFocus();
            }

            auto selected_old = selected_;

            if(
              event == ftxui::Event::Character('k')
              || (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelUp))
            {
                stick = false;
                selected_--;
            }
            if((event == ftxui::Event::Character('j')
                || (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelDown)))
            {
                selected_++;
            }
            if(event == ftxui::Event::PageDown) {
                selected_ += box_.y_max - box_.y_min;
            }
            if(event == ftxui::Event::PageUp) {
                stick = false;
                selected_ -= box_.y_max - box_.y_min;
            }
            if(event == ftxui::Event::Home) {
                stick     = false;
                selected_ = 0;
            }
            if(event == ftxui::Event::End) {
                stick     = true;
                selected_ = size_ - 1;
            }

            if(selected_ >= size_ - 1) {
                stick = true;
            }
            selected_ = std::max(0, std::min(size_ - 1, selected_));

            return selected_old != selected_;
        }

        bool Focusable() const final { return true; }

        bool       stick     = true;
        int        selected_ = 0;
        int        size_     = 0;
        ftxui::Box box_;
    };

    template<typename ContainerGetter, typename Transform>
    ftxui::Component Scroller(ContainerGetter&& c, Transform&& tf) {
        return ftxui::Make<ScrollerBase<ContainerGetter, Transform>>(
          std::forward<ContainerGetter>(c),
          std::forward<Transform>(tf));
    }

    inline ftxui::Element toElement(uc_log::detail::LogEntry::Channel const& c) {
        static std::array<ftxui::Color, 6> const Colors{
          {ftxui::Color::Black,
           ftxui::Color::Red,
           ftxui::Color::Blue,
           ftxui::Color::Magenta,
           ftxui::Color::White,
           ftxui::Color::Yellow}
        };

        return ftxui::text(fmt::format("{}", c.channel))
             | ftxui::color(Colors[c.channel % Colors.size()])
             | ((c.channel == 0) ? ftxui::bgcolor(ftxui::Color::Green) : ftxui::nothing);
    }

    inline ftxui::Element toElement(uc_log::LogLevel const& l) {
        static std::array<std::pair<ftxui::Color, std::string_view>, 6> const LCS{
          {{ftxui::Color::Yellow, "trace"},
           {ftxui::Color::Green, "debug"},
           {ftxui::Color::BlueLight, "info"},
           {ftxui::Color::Magenta, "warn"},
           {ftxui::Color::Red, "error"},
           {ftxui::Color::White, "crit"}}
        };
        static std::size_t const MaxLength
          = std::max_element(LCS.begin(), LCS.end(), [](auto rhs, auto lhs) {
                return rhs.second.size() < lhs.second.size();
            })->second.size();

        auto index = static_cast<std::size_t>(l);

        if(index >= LCS.size()) {
            index = 0;
        }

        return ftxui::text(std::string{LCS[index].second})
             | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, MaxLength) | ftxui::color(LCS[index].first)
             | ((index == 5) ? ftxui::bgcolor(ftxui::Color::RedLight) : ftxui::nothing);
    }

    struct FTXUIGui {
        struct GuiLogEntry {
            std::chrono::system_clock::time_point recv_time;
            uc_log::detail::LogEntry              logEntry;
        };

        std::mutex mutex;

        std::function<ftxui::ScreenInteractive&(void)> getScreen;

        std::vector<std::shared_ptr<GuiLogEntry const>> allLogEntrys;
        std::vector<std::shared_ptr<GuiLogEntry const>> filteredLogEntrys;

        std::function<bool(GuiLogEntry const&)> currentFilter
          = [](GuiLogEntry const&) { return true; };

        std::function<ftxui::Element(GuiLogEntry const&)> currentRenderFunction
          = [this](GuiLogEntry const& e) { return defaultRender(e); };

        bool showSysTime{true};
        bool showFunctionName{false};
        bool showUcTime{true};
        bool showLocation{true};
        bool showChannel{true};
        bool showLogLevel{true};

        ftxui::Element defaultRender(GuiLogEntry const& e) {
            ftxui::Elements elements;
            elements.reserve(12);

            if(showSysTime) {
                elements.push_back(
                  ftxui::text(detail::to_time_string_with_milliseconds(e.recv_time))
                  | ftxui::color(ftxui::Color::Cyan));
            }

            if(showChannel) {
                elements.push_back(toElement(e.logEntry.channel));
            }

            if(showUcTime) {
                elements.push_back(
                  ftxui::text(fmt::format("{}", e.logEntry.ucTime))
                  | ftxui::color(ftxui::Color::Magenta));
            }

            if(showSysTime || showChannel || showUcTime) {
                elements.push_back(ftxui::text(" ") | ftxui::color(ftxui::Color::Default));
            }

            if(showLogLevel) {
                elements.push_back(toElement(e.logEntry.logLevel));
            }

            if(showSysTime || showChannel || showUcTime || showLogLevel) {
                elements.push_back(ftxui::text(": ") | ftxui::color(ftxui::Color::Default));
            }

            elements.push_back(
              ftxui::text(e.logEntry.logMsg) | ftxui::color(ftxui::Color::Default) | ftxui::flex);

            if(showFunctionName) {
                elements.push_back(ftxui::text(" ") | ftxui::color(ftxui::Color::Default));

                elements.push_back(
                  ftxui::text(e.logEntry.functionName) | ftxui::color(ftxui::Color::RedLight));

                if(showLocation || showChannel) {
                    elements.push_back(ftxui::text(" ") | ftxui::color(ftxui::Color::Default));
                }
            }

            if(showLocation) {
                elements.push_back(
                  ftxui::text(fmt::format("({}:{})", e.logEntry.fileName, e.logEntry.line))
                  | ftxui::color(ftxui::Color::BlueLight));
            }

            if(showChannel) {
                elements.push_back(toElement(e.logEntry.channel));
            }
            return ftxui::hbox(elements);
        }

        void
        add(std::chrono::system_clock::time_point recv_time, uc_log::detail::LogEntry const& e) {
            {
                std::lock_guard<std::mutex> lock{mutex};
                auto entry = std::make_shared<GuiLogEntry const>(recv_time, e);
                allLogEntrys.push_back(entry);
                if(currentFilter(*entry)) {
                    filteredLogEntrys.push_back(entry);
                }
            }
            if(getScreen) {
                getScreen().PostEvent(ftxui::Event::Custom);
            }
        }

        void fatalError(std::string_view msg) { std::ignore = msg; }

        void statusMessage(std::string_view msg) { std::ignore = msg; }

        void errorMessage(std::string_view msg) { std::ignore = msg; }

        template<typename Reader>
        int run(Reader& rttReader, std::string const& buildCommand) {
            std::ignore = rttReader;
            std::ignore = buildCommand;

            auto component = Scroller(
              [&]() -> std::vector<std::shared_ptr<GuiLogEntry const>> const& {
                  return filteredLogEntrys;
              },
              [&](auto const& e) { return currentRenderFunction(e); });

            auto screen = ftxui::ScreenInteractive::Fullscreen();

            getScreen = [&screen]() -> ftxui::ScreenInteractive& { return screen; };

            ftxui::Loop loop(&screen, component);

            while(!loop.HasQuitted()) {
                {
                    std::lock_guard<std::mutex> lock{mutex};
                    loop.RunOnce();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            return 0;
        }
    };
}}   // namespace uc_log::FTXUIGui
