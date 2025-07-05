#pragma once

#include "uc_log/detail/LogEntry.hpp"

#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wredundant-decls"
    #pragma GCC diagnostic ignored "-Woverloaded-virtual"
    #pragma GCC diagnostic ignored "-Wsign-conversion"
    #pragma GCC diagnostic ignored "-Wshadow"
#endif

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wsign-conversion"
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
    #pragma clang diagnostic ignored "-Wreserved-macro-identifier"
    #pragma clang diagnostic ignored "-Wsuggest-override"
    #pragma clang diagnostic ignored "-Wdeprecated-redundant-constexpr-static-def"
    #pragma clang diagnostic ignored "-Wmissing-noreturn"
    #pragma clang diagnostic ignored "-Wzero-as-null-pointer-constant"
    #pragma clang diagnostic ignored "-Wglobal-constructors"
    #pragma clang diagnostic ignored "-Wdocumentation"
    #pragma clang diagnostic ignored "-Wsuggest-destructor-override"
    #pragma clang diagnostic ignored "-Wshorten-64-to-32"
    #pragma clang diagnostic ignored "-Wswitch-default"
    #pragma clang diagnostic ignored "-Wdocumentation-unknown-command"
    #pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
    #pragma clang diagnostic ignored "-Wold-style-cast"
    #pragma clang diagnostic ignored "-Wcovered-switch-default"
    #pragma clang diagnostic ignored "-Wswitch-enum"
    #pragma clang diagnostic ignored "-Wimplicit-fallthrough"
    #pragma clang diagnostic ignored "-Wexit-time-destructors"
    #pragma clang diagnostic ignored "-Wextra-semi"
    #pragma clang diagnostic ignored "-Wextra-semi-stmt"
    #pragma clang diagnostic ignored "-Wreserved-identifier"
    #pragma clang diagnostic ignored "-Wnewline-eof"
    #pragma clang diagnostic ignored "-Wredundant-parens"
#endif

#include <boost/asio.hpp>
#include <boost/process.hpp>
#include <boost/process/v1/search_path.hpp>

#ifdef __GNUC__
    #pragma GCC diagnostic pop
#endif
#ifdef __clang__
    #pragma clang diagnostic pop
#endif

#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wextra-semi"
#endif

#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

#include <magic_enum/magic_enum.hpp>

#ifdef __GNUC__
    #pragma GCC diagnostic pop
#endif

#ifdef __clang__
    #pragma clang diagnostic pop
#endif

#include <chrono>
#include <csignal>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/loop.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>

namespace uc_log { namespace FTXUIGui {

    namespace GUI_Constants {
        static constexpr std::size_t MaxScrollLines = 8192;
        static constexpr std::size_t MaxChannels    = 6;
        static constexpr auto        UpdateInterval = std::chrono::milliseconds{10};
        static constexpr std::size_t MaxLogEntries  = 10'000'000;
    }   // namespace GUI_Constants

    namespace util {
        template<class T>
        constexpr T const& clamp(T const& v,
                                 T const& lo,
                                 T const& hi) {
            return v < lo ? lo : hi < v ? hi : v;
        }
    }   // namespace util

    struct RadioboxOption {
        static RadioboxOption Simple() {
            auto option      = RadioboxOption();
            option.transform = [](ftxui::EntryState const& s) {
                auto t = ftxui::text(s.label);
                if(s.active) {
                    t |= ftxui::bold;
                }
                if(s.focused) {
                    t |= ftxui::inverted;
                }
                return ftxui::hbox({t});
            };
            return option;
        }

        ftxui::ConstStringListRef entries;
        ftxui::Ref<int>           selected = 0;

        std::function<ftxui::Element(ftxui::EntryState const&)> transform;

        std::function<void()>    on_change     = [] {};
        std::function<void(int)> on_click      = [](int) {};
        ftxui::Ref<int>          focused_entry = 0;
    };

    class RadioboxBase
      : public ftxui::ComponentBase
      , public RadioboxOption {
    public:
        explicit RadioboxBase(RadioboxOption const& option) : RadioboxOption(option) {}

    private:
        ftxui::Element OnRender() override {
            Clamp();
            ftxui::Elements elements;
            bool const      is_menu_focused = Focused();
            elements.reserve(static_cast<std::size_t>(size()));
            for(int i = 0; i < size(); ++i) {
                bool const is_focused  = (focused_entry() == i) && is_menu_focused;
                bool const is_selected = (hoveredIndex == i);
                auto       state       = ftxui::EntryState{
                  entries[static_cast<std::size_t>(i)],
                  selected() == i,
                  is_selected,
                  is_focused,
                  i,
                };
                auto element = (transform ? transform : RadioboxOption::Simple().transform)(state);
                if(is_selected) {
                    element |= ftxui::focus;
                }
                elements.push_back(element | reflect(boxes_[static_cast<std::size_t>(i)]));
            }
            return vbox(std::move(elements), hoveredIndex) | reflect(renderBox);
        }

        bool OnEvent(ftxui::Event event) override {
            Clamp();
            if(!CaptureMouse(event)) {
                return false;
            }

            if(event.is_mouse()) {
                return OnMouseEvent(event);
            }

            if(Focused()) {
                int const old_hovered = hoveredIndex;
                if(event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k')) {
                    (hoveredIndex)--;
                }
                if(event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j')) {
                    (hoveredIndex)++;
                }
                if(event == ftxui::Event::PageUp) {
                    (hoveredIndex) -= renderBox.y_max - renderBox.y_min;
                }
                if(event == ftxui::Event::PageDown) {
                    (hoveredIndex) += renderBox.y_max - renderBox.y_min;
                }
                if(event == ftxui::Event::Home) {
                    (hoveredIndex) = 0;
                }
                if(event == ftxui::Event::End) {
                    (hoveredIndex) = size() - 1;
                }
                if(event == ftxui::Event::Tab && size()) {
                    hoveredIndex = (hoveredIndex + 1) % size();
                }
                if(event == ftxui::Event::TabReverse && size()) {
                    hoveredIndex = (hoveredIndex + size() - 1) % size();
                }

                hoveredIndex = util::clamp(hoveredIndex, 0, size() - 1);

                if(hoveredIndex != old_hovered) {
                    focused_entry() = hoveredIndex;
                    on_change();
                    return true;
                }
            }

            if(event == ftxui::Event::Character(' ') || event == ftxui::Event::Return) {
                selected() = hoveredIndex;
                on_change();
                on_click(hoveredIndex);
                return true;
            }

            return false;
        }

        bool OnMouseEvent(ftxui::Event event) {
            if(event.mouse().button == ftxui::Mouse::WheelDown
               || event.mouse().button == ftxui::Mouse::WheelUp)
            {
                return OnMouseWheel(event);
            }

            for(int i = 0; i < size(); ++i) {
                if(!boxes_[static_cast<std::size_t>(i)].Contain(event.mouse().x, event.mouse().y)) {
                    continue;
                }

                TakeFocus();
                focused_entry() = i;
                if(event.mouse().button == ftxui::Mouse::Left
                   && event.mouse().motion == ftxui::Mouse::Pressed)
                {
                    if(selected() != i) {
                        selected() = i;
                        on_change();
                    }
                    on_click(i);

                    return true;
                }
            }
            return false;
        }

        bool OnMouseWheel(ftxui::Event event) {
            if(!renderBox.Contain(event.mouse().x, event.mouse().y)) {
                return false;
            }

            int const old_hovered = hoveredIndex;

            if(event.mouse().button == ftxui::Mouse::WheelUp) {
                (hoveredIndex)--;
            }
            if(event.mouse().button == ftxui::Mouse::WheelDown) {
                (hoveredIndex)++;
            }

            hoveredIndex = util::clamp(hoveredIndex, 0, size() - 1);

            if(hoveredIndex != old_hovered) {
                on_change();
            }

            return true;
        }

        void Clamp() {
            boxes_.resize(static_cast<std::size_t>(size()));
            selected()      = util::clamp(selected(), 0, size() - 1);
            focused_entry() = util::clamp(focused_entry(), 0, size() - 1);
            hoveredIndex    = util::clamp(hoveredIndex, 0, size() - 1);
        }

        bool Focusable() const final { return entries.size(); }

        int size() const { return int(entries.size()); }

        int                     hoveredIndex = selected();
        std::vector<ftxui::Box> boxes_;
        ftxui::Box              renderBox;
    };

    static ftxui::Component Radiobox(RadioboxOption option) {
        return ftxui::Make<RadioboxBase>(std::move(option));
    }

    struct CheckboxOption {
        static CheckboxOption Simple() {
            auto option      = CheckboxOption();
            option.transform = [](ftxui::EntryState const& s) {
                auto t = ftxui::text(s.label);
                if(s.active) {
                    t |= ftxui::bold;
                }
                if(s.focused) {
                    t |= ftxui::inverted;
                }
                return ftxui::hbox(
                  {t | ftxui::color(s.state ? ftxui::Color::Green : ftxui::Color::Red)});
            };
            return option;
        }

        ftxui::ConstStringRef label = "Checkbox";

        std::function<bool()> is_checked = [] { return false; };

        std::function<ftxui::Element(ftxui::EntryState const&)> transform;

        std::function<void()> on_change = [] {};
    };

    class CheckboxBase
      : public ftxui::ComponentBase
      , public CheckboxOption {
    public:
        explicit CheckboxBase(CheckboxOption option) : CheckboxOption(std::move(option)) {}

    private:
        ftxui::Element OnRender() override {
            bool const is_focused  = Focused();
            bool const is_active   = Active();
            auto       entry_state = ftxui::EntryState{
              *label,
              is_checked(),
              is_active,
              is_focused || hoveredIndex,
              -1,
            };
            auto element
              = (transform ? transform : CheckboxOption::Simple().transform)(entry_state);
            element |= ftxui::focus;
            element |= reflect(renderBox);
            return element;
        }

        bool OnEvent(ftxui::Event event) override {
            if(!CaptureMouse(event)) {
                return false;
            }

            if(event.is_mouse()) {
                return OnMouseEvent(event);
            }

            hoveredIndex = false;
            if(event == ftxui::Event::Character(' ') || event == ftxui::Event::Return) {
                on_change();
                TakeFocus();
                return true;
            }
            return false;
        }

        bool OnMouseEvent(ftxui::Event event) {
            hoveredIndex = renderBox.Contain(event.mouse().x, event.mouse().y);

            if(!CaptureMouse(event)) {
                return false;
            }

            if(!hoveredIndex) {
                return false;
            }

            if(event.mouse().button == ftxui::Mouse::Left
               && event.mouse().motion == ftxui::Mouse::Pressed)
            {
                on_change();
                return true;
            }

            return false;
        }

        bool Focusable() const final { return true; }

        bool       hoveredIndex = false;
        ftxui::Box renderBox;
    };

    template<typename GetState,
             typename SetState>
    ftxui::Component FunctionCheckbox(ftxui::ConstStringRef label,
                                      GetState&&            get,
                                      SetState&&            set) {
        auto options       = CheckboxOption::Simple();
        options.label      = label;
        options.is_checked = std::forward<GetState>(get);
        options.on_change  = std::forward<SetState>(set);

        return ftxui::Make<CheckboxBase>(options);
    }

    class ScrollableWithMetadata : public ftxui::Node {
    public:
        ScrollableWithMetadata(ftxui::Element scrollable,
                               ftxui::Element metadata)
          : scrollableContent_(std::move(scrollable))
          , metadataContent_(std::move(metadata)) {}

        ftxui::Element getScrollableContent() const { return scrollableContent_; }

        ftxui::Element getMetadataContent() const { return metadataContent_; }

    private:
        ftxui::Element scrollableContent_;
        ftxui::Element metadataContent_;
    };

    template<typename ContainerGetter, typename Transform>
    class ScrollerBase : public ftxui::ComponentBase {
    public:
        ContainerGetter containerGetter_;
        Transform       transform_;

        ScrollerBase(ContainerGetter&& c,
                     Transform&&       tf)
          : containerGetter_{c}
          , transform_{tf} {}

    private:
        ftxui::Element OnRender() final {
            auto const& container = containerGetter_();
            containerSize         = static_cast<int>(container.size());
            int const ySpace      = (renderBox.y_max - renderBox.y_min) + 2;
            selectedIndex         = std::max(0, std::min(containerSize - 1, selectedIndex));
            if(stick) {
                selectedIndex = containerSize - 1;
            }

            int hiddenBefore{};
            int hiddenBehind{};

            if(containerSize > ySpace) {
                int const tooMany = containerSize - ySpace;

                if(stick) {
                    hiddenBefore = tooMany;
                    hiddenBehind = 0;
                } else {
                    int temporaryHiddenBefore
                      = selectedIndex - (ySpace / 2) + (ySpace % 2 == 0 ? 1 : 0);
                    hiddenBefore = temporaryHiddenBefore >= 0 ? temporaryHiddenBefore : 0;
                    hiddenBehind = tooMany - hiddenBefore;
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
              | ftxui::size(ftxui::HEIGHT,
                            ftxui::EQUAL,
                            static_cast<int>(std::min(static_cast<std::size_t>(hiddenBefore),
                                                      GUI_Constants::MaxScrollLines))));

            ftxui::Elements metadataElements;
            metadataElements.reserve(static_cast<std::size_t>(ySpace + 2));

            metadataElements.push_back(
              ftxui::text("")
              | ftxui::size(ftxui::HEIGHT,
                            ftxui::EQUAL,
                            static_cast<int>(std::min(static_cast<std::size_t>(hiddenBefore),
                                                      GUI_Constants::MaxScrollLines))));

            int currentIndex = 0;
            for(auto const& e : container | std::views::drop(hiddenBefore)) {
                bool const isCurrentItem = currentIndex + hiddenBefore == selectedIndex;

                auto transformedElement = transform_(e);

                ftxui::Element scrollableContent;
                ftxui::Element metadataForThisRow;
                if constexpr(!std::is_same_v<decltype(transformedElement), ftxui::Element>) {
                    scrollableContent  = transformedElement->getScrollableContent();
                    metadataForThisRow = transformedElement->getMetadataContent();

                } else {
                    scrollableContent  = transformedElement;
                    metadataForThisRow = ftxui::text("");
                }

                if(isCurrentItem) {
                    auto const selectedStyle
                      = Focused() && !stick ? ftxui::inverted : ftxui::nothing;
                    auto const selectedFocus = Focused() ? ftxui::focus : ftxui::select;
                    elements.push_back(scrollableContent | selectedStyle | selectedFocus
                                       | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1));
                    metadataElements.push_back(metadataForThisRow | selectedStyle
                                               | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1));
                } else {
                    elements.push_back(scrollableContent
                                       | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1));
                    metadataElements.push_back(metadataForThisRow
                                               | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1));
                }

                ++currentIndex;
                if(currentIndex == ySpace) {
                    break;
                }
            }

            metadataElements.push_back(
              ftxui::text("")
              | size(ftxui::HEIGHT,
                     ftxui::EQUAL,
                     static_cast<int>(std::min(static_cast<std::size_t>(hiddenBehind),
                                               GUI_Constants::MaxScrollLines))));

            elements.push_back(
              ftxui::text("")
              | size(ftxui::HEIGHT,
                     ftxui::EQUAL,
                     static_cast<int>(std::min(static_cast<std::size_t>(hiddenBehind),
                                               GUI_Constants::MaxScrollLines))));

            ftxui::Element background = ftxui::vbox(elements);
            background->ComputeRequirement();

            ftxui::Element metadataOverlay = ftxui::vbox(metadataElements);
            metadataOverlay->ComputeRequirement();

            struct HorizontallyScrolledRenderer : public ftxui::Node {
            public:
                HorizontallyScrolledRenderer(ftxui::Element child,
                                             int            offset,
                                             ftxui::Element fixedOverlay,
                                             int            overlayWidth)
                  : child_(std::move(child))
                  , offset_(offset)
                  , fixedOverlay_(std::move(fixedOverlay))
                  , overlayWidth_(overlayWidth) {}

                void ComputeRequirement() override {
                    child_->ComputeRequirement();
                    requirement_ = child_->requirement();
                    fixedOverlay_->ComputeRequirement();
                    auto fixReq = fixedOverlay_->requirement();
                    requirement_.min_x += fixReq.min_x;
                }

                void SetBox(ftxui::Box box) override {
                    box_                 = box;
                    ftxui::Box child_box = box;
                    child_box.x_min -= offset_;
                    child_box.x_max += 10000;
                    child_->SetBox(child_box);
                    ftxui::Box overlay_box = box;
                    overlay_box.x_min      = box.x_max - overlayWidth_ + 1;
                    overlay_box.x_max      = box.x_max;
                    fixedOverlay_->SetBox(overlay_box);
                }

                void Render(ftxui::Screen& screen) override {
                    auto oldStencil = screen.stencil;

                    screen.stencil.x_max -= overlayWidth_;
                    child_->Render(screen);
                    screen.stencil = oldStencil;
                    fixedOverlay_->Render(screen);
                }

            private:
                ftxui::Element child_;
                int            offset_;
                ftxui::Element fixedOverlay_;
                int            overlayWidth_;
            };

            int metadataWidth = metadataOverlay->requirement().min_x;

            background = std::make_shared<HorizontallyScrolledRenderer>(std::move(background),
                                                                        horizontalOffset,
                                                                        std::move(metadataOverlay),
                                                                        metadataWidth);

            return std::move(background) | ftxui::vscroll_indicator | ftxui::yframe | ftxui::yflex
                 | ftxui::reflect(renderBox);
        }

        bool OnEvent(ftxui::Event event) final {
            if(event.is_mouse() && renderBox.Contain(event.mouse().x, event.mouse().y)) {
                TakeFocus();
            }

            auto previousSelected         = selectedIndex;
            auto previousHorizontalOffset = horizontalOffset;

            if(event == ftxui::Event::ArrowLeft || event == ftxui::Event::Character('h')
               || (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelLeft))
            {
                horizontalOffset = std::max(0, horizontalOffset - 4);
            }
            if(event == ftxui::Event::ArrowRight || event == ftxui::Event::Character('l')
               || (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelRight))
            {
                horizontalOffset += 4;
            }
            if(event == ftxui::Event::Home) {
                stick            = false;
                selectedIndex    = 0;
                horizontalOffset = 0;
            }

            if(event == ftxui::Event::Character('k')
               || (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelUp))
            {
                stick = false;
                selectedIndex--;
            }
            if(event == ftxui::Event::Character('j')
               || (event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelDown))
            {
                selectedIndex++;
            }
            if(event == ftxui::Event::PageDown) {
                selectedIndex += renderBox.y_max - renderBox.y_min;
            }
            if(event == ftxui::Event::PageUp) {
                stick = false;
                selectedIndex -= renderBox.y_max - renderBox.y_min;
            }
            if(event == ftxui::Event::End) {
                stick            = true;
                selectedIndex    = containerSize - 1;
                horizontalOffset = 0;
            }

            if(selectedIndex >= containerSize - 1) {
                stick = true;
            }
            selectedIndex = std::max(0, std::min(containerSize - 1, selectedIndex));

            return previousSelected != selectedIndex
                || previousHorizontalOffset != horizontalOffset;
        }

        bool Focusable() const final { return true; }

        bool       stick            = true;
        int        selectedIndex    = 0;
        int        containerSize    = 0;
        int        horizontalOffset = 0;
        ftxui::Box renderBox;
    };

    template<typename ContainerGetter,
             typename Transform>
    ftxui::Component Scroller(ContainerGetter&& c,
                              Transform&&       tf) {
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
        static constexpr std::size_t MaxLength = 5;   // max of "trace", "debug", "error", ...

        auto index = static_cast<std::size_t>(l);

        if(index >= LCS.size()) {
            index = 0;
        }

        auto const& [color, text] = LCS[index];
        return ftxui::text(std::string{text}) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, MaxLength)
             | ftxui::color(color)
             | ((index == 5) ? ftxui::bgcolor(ftxui::Color::RedLight) : ftxui::nothing);
    }

    using SourceLocation = std::pair<std::string, std::size_t>;

    struct SourceLocationAdapter : ftxui::ConstStringListRef::Adapter {
        SourceLocationAdapter(std::map<SourceLocation,
                                       std::size_t>& container_)
          : container{container_} {}

        std::size_t size() const override { return container.size(); }

        std::string operator[](std::size_t i) const override {
            auto it = container.begin();
            while(i != 0) {
                --i;
                ++it;
            }

            auto const& [sourceLocation, count] = *it;
            auto const& [fileName, lineNumber]  = sourceLocation;
            return fmt::format("{}:{} -> {}", fileName, lineNumber, count);
        }

        std::map<SourceLocation, std::size_t>& container;
    };

    struct EnabledLocationAdapter : ftxui::ConstStringListRef::Adapter {
        EnabledLocationAdapter(std::set<SourceLocation>& container_) : container{container_} {}

        std::size_t size() const override { return container.size(); }

        std::string operator[](std::size_t i) const override {
            auto it = container.begin();
            while(i != 0) {
                --i;
                ++it;
            }
            auto const& [fileName, lineNumber] = *it;
            if(lineNumber == 0) {
                return fmt::format("{}:all", fileName);
            } else {
                return fmt::format("{}:{}", fileName, lineNumber);
            }
        }

        std::set<SourceLocation>& container;
    };

    static ftxui::Element ansiColoredTextToFtxui(std::string_view sv) {
        static constexpr char escape = '\x1B';

        ftxui::Elements elements;
        std::string     currentText;
        std::string     currentEscape;
        bool            inEscape = false;

        ftxui::Color currentFgColor = ftxui::Color::Default;
        ftxui::Color currentBgColor = ftxui::Color::Default;
        bool         bold           = false;
        bool         dim            = false;
        bool         italic         = false;
        bool         underline      = false;
        bool         blink          = false;
        bool         reverse        = false;
        bool         strikethrough  = false;

        std::string currentHyperlink;

        auto applyStyles = [&](std::string const& text) {
            auto element = ftxui::text(text);

            if(currentFgColor != ftxui::Color::Default) {
                element |= ftxui::color(currentFgColor);
            }
            if(currentBgColor != ftxui::Color::Default) {
                element |= ftxui::bgcolor(currentBgColor);
            }
            if(bold) {
                element |= ftxui::bold;
            }
            if(dim) {
                element |= ftxui::dim;
            }
            if(italic) {
                element |= ftxui::italic;
            }
            if(underline) {
                element |= ftxui::underlined;
            }
            if(blink) {
                element |= ftxui::blink;
            }
            if(reverse) {
                element |= ftxui::inverted;
            }
            if(strikethrough) {
                element |= ftxui::strikethrough;
            }

            if(!currentHyperlink.empty()) {
                element = ftxui::hyperlink(currentHyperlink, element);
            }

            return element;
        };

        auto parseAnsiCode = [&](std::string const& codeStr) {
            if(codeStr.empty()) {
                return;
            }

            // Handle OSC sequences (hyperlinks)
            if(codeStr.front() == ']') {
                // OSC 8 hyperlink format: ]8;;URL or ]8;;
                if(codeStr.size() >= 3 && codeStr.substr(0, 3) == "]8;") {
                    std::string params = codeStr.substr(3);   // Skip "]8;"
                    if(params == ";") {
                        // End hyperlink
                        currentHyperlink.clear();
                    } else if(params.size() > 1 && params.front() == ';') {
                        // Start hyperlink: ]8;;URL
                        currentHyperlink = params.substr(1);   // Skip the semicolon
                    }
                }
                return;
            }

            // Handle CSI sequences (most common)
            if(codeStr.front() == '[') {
                std::string params = codeStr.substr(1);
                if(params.empty()) {
                    return;
                }

                char command = params.back();
                params.pop_back();

                if(command == 'm') {   // SGR (Select Graphic Rendition)
                    if(params.empty()) {
                        // Reset all
                        currentFgColor = ftxui::Color::Default;
                        currentBgColor = ftxui::Color::Default;
                        bold = dim = italic = underline = blink = reverse = strikethrough = false;
                        return;
                    }

                    std::vector<int> codes;
                    std::string      current;
                    for(char c : params) {
                        if(c == ';') {
                            if(!current.empty()) {
                                codes.push_back(std::stoi(current));
                                current.clear();
                            }
                        } else {
                            current.push_back(c);
                        }
                    }
                    if(!current.empty()) {
                        codes.push_back(std::stoi(current));
                    }

                    for(std::size_t i = 0; i < codes.size(); ++i) {
                        int code = codes[i];
                        switch(code) {
                        case 0:   // Reset
                            currentFgColor = ftxui::Color::Default;
                            currentBgColor = ftxui::Color::Default;
                            bold = dim = italic = underline = blink = reverse = strikethrough
                              = false;
                            break;
                        case 1:  bold = true; break;
                        case 2:  dim = true; break;
                        case 3:  italic = true; break;
                        case 4:  underline = true; break;
                        case 5:  blink = true; break;
                        case 7:  reverse = true; break;
                        case 9:  strikethrough = true; break;
                        case 22: bold = dim = false; break;
                        case 23: italic = false; break;
                        case 24: underline = false; break;
                        case 25: blink = false; break;
                        case 27: reverse = false; break;
                        case 29: strikethrough = false; break;
                        // Foreground colors
                        case 30: currentFgColor = ftxui::Color::Black; break;
                        case 31: currentFgColor = ftxui::Color::Red; break;
                        case 32: currentFgColor = ftxui::Color::Green; break;
                        case 33: currentFgColor = ftxui::Color::Yellow; break;
                        case 34: currentFgColor = ftxui::Color::Blue; break;
                        case 35: currentFgColor = ftxui::Color::Magenta; break;
                        case 36: currentFgColor = ftxui::Color::Cyan; break;
                        case 37: currentFgColor = ftxui::Color::White; break;
                        case 38:   // 256-color foreground
                            if(i + 2 < codes.size() && codes[i + 1] == 5) {
                                currentFgColor = ftxui::Color::Palette256(codes[i + 2]);
                                i += 2;   // Skip the next two codes
                            }
                            break;
                        case 39: currentFgColor = ftxui::Color::Default; break;
                        // Background colors
                        case 40: currentBgColor = ftxui::Color::Black; break;
                        case 41: currentBgColor = ftxui::Color::Red; break;
                        case 42: currentBgColor = ftxui::Color::Green; break;
                        case 43: currentBgColor = ftxui::Color::Yellow; break;
                        case 44: currentBgColor = ftxui::Color::Blue; break;
                        case 45: currentBgColor = ftxui::Color::Magenta; break;
                        case 46: currentBgColor = ftxui::Color::Cyan; break;
                        case 47: currentBgColor = ftxui::Color::White; break;
                        case 48:   // 256-color background
                            if(i + 2 < codes.size() && codes[i + 1] == 5) {
                                currentBgColor = ftxui::Color::Palette256(codes[i + 2]);
                                i += 2;   // Skip the next two codes
                            }
                            break;
                        case 49: currentBgColor = ftxui::Color::Default; break;
                        // Bright foreground colors
                        case 90: currentFgColor = ftxui::Color::GrayDark; break;
                        case 91: currentFgColor = ftxui::Color::RedLight; break;
                        case 92: currentFgColor = ftxui::Color::GreenLight; break;
                        case 93: currentFgColor = ftxui::Color::YellowLight; break;
                        case 94: currentFgColor = ftxui::Color::BlueLight; break;
                        case 95: currentFgColor = ftxui::Color::MagentaLight; break;
                        case 96: currentFgColor = ftxui::Color::CyanLight; break;
                        case 97: currentFgColor = ftxui::Color::GrayLight; break;
                        // Bright background colors
                        case 100: currentBgColor = ftxui::Color::GrayDark; break;
                        case 101: currentBgColor = ftxui::Color::RedLight; break;
                        case 102: currentBgColor = ftxui::Color::GreenLight; break;
                        case 103: currentBgColor = ftxui::Color::YellowLight; break;
                        case 104: currentBgColor = ftxui::Color::BlueLight; break;
                        case 105: currentBgColor = ftxui::Color::MagentaLight; break;
                        case 106: currentBgColor = ftxui::Color::CyanLight; break;
                        case 107: currentBgColor = ftxui::Color::GrayLight; break;
                        }
                    }
                }
            }
        };

        for(auto c : sv) {
            if(c == escape) {
                if(!currentText.empty()) {
                    elements.push_back(applyStyles(currentText));
                    currentText.clear();
                }
                inEscape = true;
                currentEscape.clear();
                continue;
            }

            if(inEscape) {
                currentEscape.push_back(c);

                // Check for different escape sequence endings
                bool isComplete = false;

                // OSC sequences end with ESC\ or BEL (0x07)
                if(!currentEscape.empty() && currentEscape.front() == ']') {
                    if(c == '\\' && currentEscape.size() >= 2
                       && currentEscape[currentEscape.size() - 2] == escape)
                    {
                        // Remove the ESC from the sequence before parsing
                        currentEscape.pop_back();   // Remove '\'
                        currentEscape.pop_back();   // Remove ESC
                        isComplete = true;
                    } else if(c == '\x07') {        // BEL character
                        currentEscape.pop_back();   // Remove BEL
                        isComplete = true;
                    }
                }
                // CSI sequences end with letters or ~
                else if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~')
                {
                    isComplete = true;
                }

                if(isComplete) {
                    parseAnsiCode(currentEscape);
                    inEscape = false;
                    currentEscape.clear();
                }
            } else {
                currentText.push_back(c);
            }
        }

        if(!currentText.empty()) {
            elements.push_back(applyStyles(currentText));
        }

        return elements.empty() ? ftxui::text("") : ftxui::hbox(elements);
    }

    struct FTXUIGui {
        struct GuiLogEntry {
            std::chrono::system_clock::time_point recv_time;
            uc_log::detail::LogEntry              logEntry;
        };

        FTXUIGui()                            = default;
        FTXUIGui(FTXUIGui const&)             = delete;
        FTXUIGui& operator==(FTXUIGui const&) = delete;

        FTXUIGui(FTXUIGui&&)             = delete;
        FTXUIGui& operator==(FTXUIGui&&) = delete;

        ~FTXUIGui() {
            if(buildIoContext) {
                buildIoContext->stop();
            }
            if(buildThread.joinable()) {
                buildThread.join();
            }
        }

        struct FilterState {
            std::set<uc_log::LogLevel> enabledLogLevels;
            std::set<std::size_t>      enabledChannels;
            std::set<SourceLocation>   enabledLocations;

            bool operator==(FilterState const&) const = default;
        };

        std::mutex mutex;

        ftxui::ScreenInteractive* screen_ptr = nullptr;

        std::map<SourceLocation, std::size_t>           allSourceLocations;
        std::vector<std::shared_ptr<GuiLogEntry const>> allLogEntries;
        std::vector<std::shared_ptr<GuiLogEntry const>> filteredLogEntries;

        FilterState activeFilterState;
        FilterState editedFilterState;

        static constexpr auto NoFilter = [](GuiLogEntry const&) { return true; };

        std::function<bool(GuiLogEntry const&)> currentFilter = NoFilter;

        bool showSysTime{true};
        bool showFunctionName{false};
        bool showUcTime{true};
        bool showLocation{true};
        bool showChannel{true};
        bool showLogLevel{true};

        struct MessageEntry {
            enum class Level { Fatal, Error, Status };

            Level                                 level;
            std::chrono::system_clock::time_point time;
            std::string                           message;
        };

        std::vector<MessageEntry> messages;

        struct BuildEntry {
            std::chrono::system_clock::time_point time;
            std::string                           line;
            bool                                  fromTool;
            bool                                  isError;
        };

        enum class BuildStatus { Idle, Running, Success, Failed };

        std::vector<BuildEntry> buildOutput;
        BuildStatus             buildStatus = BuildStatus::Idle;

        std::vector<std::string> parsedBuildArgs;
        boost::filesystem::path  buildExecutablePath;

        std::string                                  stdoutBuffer;
        std::string                                  stderrBuffer;
        std::jthread                                 buildThread;
        std::unique_ptr<boost::asio::io_context>     buildIoContext{};
        std::unique_ptr<boost::asio::readable_pipe>  stdoutPipe{};
        std::unique_ptr<boost::asio::readable_pipe>  stderrPipe{};
        std::unique_ptr<boost::process::v2::process> buildProcess{};

        void addBuildOutput(std::string const& line,
                            bool               fromTool,
                            bool               isError) {
            std::lock_guard<std::mutex> lock{mutex};
            buildOutput.emplace_back(std::chrono::system_clock::now(), line, fromTool, isError);
            if(screen_ptr) {
                screen_ptr->PostEvent(ftxui::Event::Custom);
            }
        }

        void addBuildOutputGui(std::string const& line,
                               bool               isError) {
            buildOutput.emplace_back(std::chrono::system_clock::now(), line, false, isError);
        }

        void startAsyncReadStdout() {
            boost::asio::async_read_until(
              *stdoutPipe,
              boost::asio::dynamic_buffer(stdoutBuffer),
              '\n',
              [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                  if(!ec && bytes_transferred > 0) {
                      auto pos = stdoutBuffer.find('\n');
                      if(pos != std::string::npos) {
                          std::string line = stdoutBuffer.substr(0, pos);
                          stdoutBuffer.erase(0, pos + 1);
                          addBuildOutput(line, true, false);
                      }
                      startAsyncReadStdout();
                  }
              });
        }

        void startAsyncReadStderr() {
            boost::asio::async_read_until(
              *stderrPipe,
              boost::asio::dynamic_buffer(stderrBuffer),
              '\n',
              [this](boost::system::error_code ec, std::size_t bytes_transferred) {
                  if(!ec && bytes_transferred > 0) {
                      auto pos = stderrBuffer.find('\n');
                      if(pos != std::string::npos) {
                          std::string line = stderrBuffer.substr(0, pos);
                          stderrBuffer.erase(0, pos + 1);
                          addBuildOutput(line, true, true);
                      }
                      startAsyncReadStderr();
                  }
              });
        }

    private:
        bool hasScript() const {
            try {
                auto scriptPath = boost::process::v1::search_path("script");
                return !scriptPath.empty();
            } catch(...) {
                return false;
            }
        }

        std::pair<boost::filesystem::path,
                  std::vector<std::string>>
        createColoredBuildCommand() const {
            if(hasScript()) {
                auto scriptPath = boost::process::v1::search_path("script");
                if(!scriptPath.empty()) {
                    std::vector<std::string> wrappedArgs;
                    wrappedArgs.push_back("-q");
                    wrappedArgs.push_back("-c");

                    std::string commandStr = buildExecutablePath.string();
                    for(auto const& arg : parsedBuildArgs) {
                        commandStr += " ";
                        if(arg.find(' ') != std::string::npos) {
                            commandStr += "\"" + arg + "\"";
                        } else {
                            commandStr += arg;
                        }
                    }
                    wrappedArgs.push_back(commandStr);
                    wrappedArgs.push_back("/dev/null");

                    return {scriptPath, wrappedArgs};
                }
            }

            return {buildExecutablePath, parsedBuildArgs};
        }

        bool initializeBuildCommand(std::string const& buildCommandStr) {
            std::vector<std::string> args;
            std::string              currentArgument;
            bool                     in_quotes   = false;
            bool                     escape_next = false;

            for(char c : buildCommandStr) {
                if(escape_next) {
                    currentArgument += c;
                    escape_next = false;
                } else if(c == '\\') {
                    escape_next = true;
                } else if(c == '"' || c == '\'') {
                    in_quotes = !in_quotes;
                } else if(c == ' ' && !in_quotes) {
                    if(!currentArgument.empty()) {
                        args.push_back(currentArgument);
                        currentArgument.clear();
                    }
                } else {
                    currentArgument += c;
                }
            }

            if(!currentArgument.empty()) {
                args.push_back(currentArgument);
            }

            if(args.empty()) {
                return false;
            }

            std::string executable = args[0];
            args.erase(args.begin());

            auto const ex = boost::process::v1::search_path(executable);
            if(ex.empty()) {
                return false;
            }

            parsedBuildArgs     = std::move(args);
            buildExecutablePath = ex;

            return true;
        }

    public:
        void executeAsyncBuild() {
            if(buildStatus == BuildStatus::Running) {
                return;
            }

            if(buildIoContext) {
                buildIoContext->stop();
            }

            buildOutput.clear();
            buildStatus = BuildStatus::Running;

            if(buildThread.joinable()) {
                buildThread.join();
            }

            try {
                stdoutPipe.reset();
                stderrPipe.reset();
                buildProcess.reset();

                buildIoContext = std::make_unique<boost::asio::io_context>();

                addBuildOutputGui(fmt::format("🚀 Starting process: {} {}",
                                              buildExecutablePath.string(),
                                              parsedBuildArgs),
                                  false);

                buildThread = std::jthread{[this]() {
                    stdoutPipe = std::make_unique<boost::asio::readable_pipe>(*buildIoContext);
                    stderrPipe = std::make_unique<boost::asio::readable_pipe>(*buildIoContext);

                    auto [execPath, execArgs] = createColoredBuildCommand();
                    std::vector<std::string> envVars;

                    auto currentEnv = boost::this_process::environment();
                    for(auto const& envVar : currentEnv) {
                        envVars.push_back(envVar.get_name() + "=" + envVar.to_string());
                    }
                    envVars.push_back("FORCE_COLOR=1");
                    envVars.push_back("CLICOLOR_FORCE=1");
                    envVars.push_back("COLORTERM=truecolor");
                    envVars.push_back("CMAKE_COLOR_DIAGNOSTICS=ON");
                    envVars.push_back("NINJA_STATUS=[%f/%t] ");

                    buildProcess = std::make_unique<boost::process::v2::process>(
                      *buildIoContext,
                      execPath,
                      execArgs,
                      boost::process::v2::process_stdio{nullptr, *stdoutPipe, *stderrPipe},
                      boost::process::v2::process_environment{envVars});

                    startAsyncReadStdout();
                    startAsyncReadStderr();

                    buildProcess->async_wait([this](boost::system::error_code ec, int exitCode) {
                        {
                            std::lock_guard<std::mutex> lock{mutex};
                            buildStatus
                              = (exitCode == 0) ? BuildStatus::Success : BuildStatus::Failed;
                        }

                        if(ec) {
                            addBuildOutput(fmt::format("❌ Process error: {}", ec.message()),
                                           false,
                                           true);
                        } else {
                            addBuildOutput(fmt::format("🏁 Build {} (exit code: {})",
                                                       exitCode == 0 ? "succeeded" : "failed",
                                                       exitCode),
                                           false,
                                           exitCode != 0);
                        }
                    });
                    buildIoContext->run();

                    if(buildProcess->running()) {
                        buildProcess->terminate();
                    }
                }};

            } catch(boost::system::system_error const& e) {
                buildStatus = BuildStatus::Failed;
                addBuildOutputGui(fmt::format("❌ System error: {}", e.what()), true);
            } catch(std::exception const& e) {
                buildStatus = BuildStatus::Failed;
                addBuildOutputGui(fmt::format("❌ Build error: {}", e.what()), true);
            } catch(...) {
                buildStatus = BuildStatus::Failed;
                addBuildOutputGui("❌ Unknown error occurred during build", true);
            }
        }

        void executeBuild() { executeAsyncBuild(); }

        auto defaultRender(GuiLogEntry const& e) {
            ftxui::Elements elements;
            elements.reserve(12);

            if(showSysTime) {
                elements.push_back(
                  ftxui::text(detail::to_time_string_with_milliseconds(e.recv_time))
                  | ftxui::color(ftxui::Color::Cyan));
                elements.push_back(ftxui::text(" "));
            }

            if(showChannel) {
                elements.push_back(toElement(e.logEntry.channel));
                elements.push_back(ftxui::text(" "));
            }

            if(showUcTime) {
                elements.push_back(ftxui::text(fmt::format("{}", e.logEntry.ucTime))
                                   | ftxui::color(ftxui::Color::Magenta));
                elements.push_back(ftxui::text(" "));
            }

            if(showLogLevel) {
                elements.push_back(toElement(e.logEntry.logLevel));
                elements.push_back(ftxui::text("│ ") | ftxui::color(ftxui::Color::GrayDark));
            }

            elements.push_back(ansiColoredTextToFtxui(e.logEntry.logMsg));

            auto scrollableContent = ftxui::hbox(elements) | ftxui::flex;

            ftxui::Elements metadata;
            if(showFunctionName) {
                metadata.push_back(ftxui::text(e.logEntry.functionName)
                                   | ftxui::color(ftxui::Color::Blue));
            }

            if(showLocation) {
                if(!metadata.empty()) {
                    metadata.push_back(ftxui::text(" "));
                }
                metadata.push_back(
                  ftxui::text(fmt::format("{}:{}", e.logEntry.fileName, e.logEntry.line))
                  | ftxui::color(ftxui::Color::GrayDark));
            }

            ftxui::Element metadataElement = nullptr;
            if(!metadata.empty()) {
                metadataElement = ftxui::hbox(metadata);
            }

            return std::make_shared<ScrollableWithMetadata>(std::move(scrollableContent),
                                                            std::move(metadataElement));
        }

        ftxui::Element renderMessage(MessageEntry const& e) {
            ftxui::Elements elements;
            elements.reserve(3);

            elements.push_back(ftxui::text(detail::to_time_string_with_milliseconds(e.time))
                               | ftxui::color(ftxui::Color::Cyan));

            elements.push_back(ftxui::text(" | ") | ftxui::color(ftxui::Color::Default));

            auto messageColor
              = e.level == MessageEntry::Level::Fatal ? ftxui::color(ftxui::Color::Red)
              : e.level == MessageEntry::Level::Error ? ftxui::color(ftxui::Color::Magenta)
                                                      : ftxui::color(ftxui::Color::Green);

            elements.push_back(ftxui::text(e.message) | messageColor | ftxui::flex);

            return ftxui::hbox(elements);
        }

        void updateFilteredLogEntries() {
            filteredLogEntries.clear();
            for(auto& entry : allLogEntries) {
                if(currentFilter(*entry)) {
                    filteredLogEntries.push_back(entry);
                }
            }
        }

        auto createFilter(FilterState const& filterState) {
            return [filterState](GuiLogEntry const& entry) {
                if(!filterState.enabledLogLevels.empty()) {
                    if(!filterState.enabledLogLevels.contains(entry.logEntry.logLevel)) {
                        return false;
                    }
                }
                if(!filterState.enabledChannels.empty()) {
                    if(!filterState.enabledChannels.contains(entry.logEntry.channel.channel)) {
                        return false;
                    }
                }
                if(!filterState.enabledLocations.empty()) {
                    if(!filterState.enabledLocations.contains(
                         SourceLocation{entry.logEntry.fileName, entry.logEntry.line}))
                    {
                        if(!filterState.enabledLocations.contains(
                             SourceLocation{entry.logEntry.fileName, 0}))
                        {
                            return false;
                        }
                    }
                }

                return true;
            };
        }

        void updateCurrentFilter() {
            if(activeFilterState == editedFilterState) {
                return;
            }

            activeFilterState = editedFilterState;

            if(activeFilterState == FilterState{}) {
                currentFilter = NoFilter;
            } else {
                currentFilter = createFilter(activeFilterState);
            }
            updateFilteredLogEntries();
        }

        void add(std::chrono::system_clock::time_point recv_time,
                 uc_log::detail::LogEntry const&       e) {
            {
                std::lock_guard<std::mutex> lock{mutex};
                auto entry = std::make_shared<GuiLogEntry const>(recv_time, e);
                allLogEntries.push_back(entry);
                allSourceLocations[SourceLocation{e.fileName, e.line}]++;
                if(currentFilter(*entry)) {
                    filteredLogEntries.push_back(entry);
                }
            }
            {
                std::lock_guard<std::mutex> lock{mutex};
                if(screen_ptr) {
                    screen_ptr->PostEvent(ftxui::Event::Custom);
                }
            }
        }

        void fatalError(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            messages.emplace_back(MessageEntry::Level::Fatal,
                                  std::chrono::system_clock::now(),
                                  std::string{msg});
            if(screen_ptr) {
                screen_ptr->PostEvent(ftxui::Event::Custom);
            }
        }

        void statusMessage(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            messages.emplace_back(MessageEntry::Level::Status,
                                  std::chrono::system_clock::now(),
                                  std::string{msg});
            if(screen_ptr) {
                screen_ptr->PostEvent(ftxui::Event::Custom);
            }
        }

        void errorMessage(std::string_view msg) {
            std::lock_guard<std::mutex> lock{mutex};
            messages.emplace_back(MessageEntry::Level::Error,
                                  std::chrono::system_clock::now(),
                                  std::string{msg});
            if(screen_ptr) {
                screen_ptr->PostEvent(ftxui::Event::Custom);
            }
        }

        ftxui::Component getLogComponent() {
            return Scroller(
              [&]() -> std::vector<std::shared_ptr<GuiLogEntry const>> const& {
                  return filteredLogEntries;
              },
              [&](auto const& e) { return defaultRender(*e); });
        }

        ftxui::Component getStatusComponent() {
            auto clearButton = ftxui::Button(
              "🗑️ Clear messages",
              [this]() { messages.clear(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Yellow));

            return ftxui::Container::Vertical(
              {clearButton,
               Scroller([&]() -> std::vector<MessageEntry> const& { return messages; },
                        [&](auto const& e) { return renderMessage(e); })});
        }

        ftxui::Element renderBuildEntry(BuildEntry const& entry) {
            ftxui::Elements elements;
            elements.reserve(3);

            elements.push_back(ftxui::text(detail::to_time_string_with_milliseconds(entry.time))
                               | ftxui::color(ftxui::Color::Cyan));

            elements.push_back(ftxui::text(" | ") | ftxui::color(ftxui::Color::Default));

            if(entry.fromTool) {
                elements.push_back(ansiColoredTextToFtxui(entry.line) | ftxui::flex);
            } else {
                auto lineColor = entry.isError ? ftxui::color(ftxui::Color::Red)
                                               : ftxui::color(ftxui::Color::Default);
                elements.push_back(ftxui::text(entry.line) | lineColor | ftxui::flex);
            }
            return ftxui::hbox(elements);
        }

        ftxui::Component getBuildComponent() {
            auto clearButton = ftxui::Button(
              "🗑️ Clear Output",
              [this]() { buildOutput.clear(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Yellow));

            auto stopButton = ftxui::Button(
              "⏸ Stop Build",
              [this]() {
                  try {
                      if(buildIoContext) {
                          boost::asio::post(*buildIoContext, [this]() {
                              if(buildProcess->running()) {
                                  buildProcess->terminate();
                                  addBuildOutputGui("Stopped build process...", false);
                              }
                              buildIoContext->stop();
                          });
                      }

                      if(buildThread.joinable()) {
                          buildThread.join();
                      }
                      buildStatus = BuildStatus::Failed;
                  } catch(std::exception const& e) {
                      addBuildOutputGui(fmt::format("❌ Error stopping build: {}", e.what()), true);
                  }
              },
              ftxui::ButtonOption::Animated(ftxui::Color::Red));

            auto buildButton = ftxui::Button(
              "🔨 Start Build [b]",
              [this]() { executeAsyncBuild(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Green));

            auto statusDisplay = ftxui::Renderer([this]() {
                std::string  statusText;
                ftxui::Color statusColor;

                switch(buildStatus) {
                case BuildStatus::Idle:
                    statusText  = "⚪ Idle";
                    statusColor = ftxui::Color::GrayDark;
                    break;
                case BuildStatus::Running:
                    statusText  = "🟡 Building...";
                    statusColor = ftxui::Color::Yellow;
                    break;
                case BuildStatus::Success:
                    statusText  = "✅ Success";
                    statusColor = ftxui::Color::Green;
                    break;
                case BuildStatus::Failed:
                    statusText  = "❌ Failed";
                    statusColor = ftxui::Color::Red;
                    break;
                }

                return ftxui::vbox({ftxui::text("🔨 Build Status") | ftxui::bold
                                      | ftxui::color(ftxui::Color::Cyan) | ftxui::center,
                                    ftxui::separator(),
                                    ftxui::hbox({ftxui::text("Status: ") | ftxui::bold,
                                                 ftxui::text(statusText) | ftxui::color(statusColor)
                                                   | ftxui::bold}),
                                    ftxui::hbox({ftxui::text("Output Lines: ") | ftxui::bold,
                                                 ftxui::text(fmt::format("{}", buildOutput.size()))
                                                   | ftxui::color(ftxui::Color::Cyan)})})
                     | ftxui::borderRounded;
            });

            auto outputScroller
              = Scroller([&]() -> std::vector<BuildEntry> const& { return buildOutput; },
                         [&](auto const& e) { return renderBuildEntry(e); });

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal(
                 {buildButton | ftxui::flex, stopButton | ftxui::flex, clearButton | ftxui::flex}),
               statusDisplay,
               outputScroller | ftxui::flex});
        }

        ftxui::Component getLogLevelFilterComponent() {
            static constexpr std::array levels{uc_log::LogLevel::trace,
                                               uc_log::LogLevel::debug,
                                               uc_log::LogLevel::info,
                                               uc_log::LogLevel::warn,
                                               uc_log::LogLevel::error,
                                               uc_log::LogLevel::crit};

            std::vector<ftxui::Component> logLevel_components{};

            auto allButton = ftxui::Button(
              "📋 Enable All Levels",
              [this]() {
                  editedFilterState.enabledLogLevels.clear();
                  updateCurrentFilter();
              },
              ftxui::ButtonOption::Animated(ftxui::Color::Cyan));

            logLevel_components.push_back(allButton);

            for(auto level : levels) {
                auto checkbox = FunctionCheckbox(
                  std::string{magic_enum::enum_name(level)},
                  [level, this]() {
                      return editedFilterState.enabledLogLevels.contains(level)
                          || editedFilterState.enabledLogLevels.empty();
                  },
                  [level, this]() {
                      if(editedFilterState.enabledLogLevels.empty()) {
                          editedFilterState.enabledLogLevels.insert(levels.begin(), levels.end());
                          editedFilterState.enabledLogLevels.erase(level);
                      } else {
                          if(editedFilterState.enabledLogLevels.contains(level)) {
                              editedFilterState.enabledLogLevels.erase(level);
                          } else {
                              editedFilterState.enabledLogLevels.insert(level);
                          }
                      }

                      if(editedFilterState.enabledLogLevels.size() == levels.size()) {
                          editedFilterState.enabledLogLevels.clear();
                      }

                      updateCurrentFilter();
                  });

                logLevel_components.push_back(checkbox);
            }

            return ftxui::Container::Vertical(logLevel_components) | ftxui::border;
        }

        ftxui::Component getChannelFilterComponent() {
            static constexpr auto channels
              = std::ranges::iota_view{std::size_t{0}, GUI_Constants::MaxChannels};
            std::vector<ftxui::Component> channel_components{};

            auto allButton = ftxui::Button(
              "📡 Enable All Channels",
              [this]() {
                  editedFilterState.enabledChannels.clear();
                  updateCurrentFilter();
              },
              ftxui::ButtonOption::Animated(ftxui::Color::Yellow));

            channel_components.push_back(allButton);

            for(auto channel : channels) {
                auto checkbox = FunctionCheckbox(
                  fmt::format("Channel {}", channel),
                  [channel, this]() {
                      return editedFilterState.enabledChannels.contains(channel)
                          || editedFilterState.enabledChannels.empty();
                  },
                  [channel, this]() {
                      if(editedFilterState.enabledChannels.empty()) {
                          editedFilterState.enabledChannels.insert(channels.begin(),
                                                                   channels.end());
                          editedFilterState.enabledChannels.erase(channel);
                      } else {
                          if(editedFilterState.enabledChannels.contains(channel)) {
                              editedFilterState.enabledChannels.erase(channel);
                          } else {
                              editedFilterState.enabledChannels.insert(channel);
                          }
                      }

                      if(editedFilterState.enabledChannels.size() == channels.size()) {
                          editedFilterState.enabledChannels.clear();
                      }

                      updateCurrentFilter();
                  });

                channel_components.push_back(checkbox);
            }

            return ftxui::Container::Vertical(channel_components) | ftxui::border;
        }

        int            lastSelectedIndex{};
        SourceLocation lastSelected;
        std::string    locationFilterInput;

        ftxui::Component getLocationFilterComponent() {
            auto addEntry = [this](SourceLocation const& sc) {
                if(!editedFilterState.enabledLocations.contains(sc)) {
                    editedFilterState.enabledLocations.insert(sc);
                    updateCurrentFilter();
                }
            };

            auto stringToSourceLocation
              = [](std::string const& x) -> std::optional<SourceLocation> {
                auto colonPosition = std::find(x.begin(), x.end(), ':');
                if(colonPosition == x.end()) {
                    if(x.empty()) {
                        return std::nullopt;
                    }
                    return SourceLocation{x, 0};
                }
                std::size_t line;
                auto parseResult = std::from_chars(&(*(colonPosition + 1)), &(*x.end()), line);
                if(parseResult.ec == std::errc{} && parseResult.ptr == &(*x.end())) {
                    return SourceLocation{
                      std::string_view{x.begin(), colonPosition},
                      line
                    };
                }
                return std::nullopt;
            };

            std::vector<ftxui::Component> manualInputComponents;
            manualInputComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📝 Manual:") | ftxui::bold
                     | ftxui::color(ftxui::Color::Magenta);
            }));
            manualInputComponents.push_back(ftxui::Input(&locationFilterInput, "filename:line")
                                            | ftxui::flex);
            manualInputComponents.push_back(
              ftxui::Maybe(ftxui::Button(
                             "+ Add",
                             [this, addEntry, stringToSourceLocation]() {
                                 auto sc = stringToSourceLocation(locationFilterInput);
                                 if(sc) {
                                     addEntry(*sc);
                                     locationFilterInput.clear();
                                 }
                             },
                             ftxui::ButtonOption::Animated(ftxui::Color::Green)),
                           [this, stringToSourceLocation]() {
                               return stringToSourceLocation(locationFilterInput).has_value();
                           }));

            auto manualInputComponent = ftxui::Container::Horizontal(manualInputComponents);

            std::vector<ftxui::Component> dropdownComponents;
            dropdownComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📋 Known:") | ftxui::bold | ftxui::color(ftxui::Color::Cyan);
            }));

            ftxui::DropdownOption dropdownOptions;
            dropdownOptions.radiobox.entries
              = std::make_unique<SourceLocationAdapter>(allSourceLocations);
            dropdownOptions.radiobox.selected  = &lastSelectedIndex;
            dropdownOptions.radiobox.on_change = [this]() {
                auto it = allSourceLocations.begin();
                auto i  = lastSelectedIndex;
                while(i != 0) {
                    --i;
                    ++it;
                }
                lastSelected = it->first;
            };

            dropdownComponents.push_back(ftxui::Dropdown(dropdownOptions));
            dropdownComponents.push_back(
              ftxui::Maybe(ftxui::Button(
                             "+ Add",
                             [addEntry, this]() { addEntry(lastSelected); },
                             ftxui::ButtonOption::Animated(ftxui::Color::Green)),
                           [this]() { return !lastSelected.first.empty(); }));

            auto dropdownComponent = ftxui::Container::Horizontal(dropdownComponents);

            std::vector<ftxui::Component> inputSectionComponents;
            inputSectionComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📍 Add Location Filter") | ftxui::bold
                     | ftxui::color(ftxui::Color::Magenta) | ftxui::center;
            }));
            inputSectionComponents.push_back(manualInputComponent);
            inputSectionComponents.push_back(dropdownComponent);

            auto inputComponent
              = ftxui::Container::Vertical(inputSectionComponents) | ftxui::border;

            std::vector<ftxui::Component> location_components{};

            location_components.push_back(ftxui::Renderer([this]() {
                return ftxui::text(fmt::format("📂 Active Filters ({})",
                                               editedFilterState.enabledLocations.size()))
                     | ftxui::bold | ftxui::color(ftxui::Color::Cyan);
            }));

            RadioboxOption radioboxOption = RadioboxOption::Simple();

            radioboxOption.transform = [](ftxui::EntryState const& s) {
                auto t = ftxui::text(s.label);
                if(s.active) {
                    t |= ftxui::bold;
                }
                if(s.focused) {
                    t |= ftxui::inverted;
                }
                return ftxui::hbox({ftxui::text("❌ ") | ftxui::color(ftxui::Color::Red), t});
            };

            radioboxOption.entries
              = std::make_unique<EnabledLocationAdapter>(editedFilterState.enabledLocations);
            radioboxOption.on_click = [this](int i) {
                auto it = editedFilterState.enabledLocations.begin();
                while(i != 0) {
                    --i;
                    ++it;
                }
                editedFilterState.enabledLocations.erase(it);
            };

            location_components.push_back(Radiobox(radioboxOption) | ftxui::frame | ftxui::border
                                          | ftxui::center);
            auto locationsContainer
              = ftxui::Container::Vertical(location_components) | ftxui::border;

            std::vector<ftxui::Component> finalComponents;
            finalComponents.push_back(inputComponent);
            finalComponents.push_back(locationsContainer);

            return ftxui::Container::Vertical(finalComponents);
        }

        ftxui::Component getFilterComponent() {
            auto clearButton = ftxui::Button(
              "🗑️ Clear All Filters",
              [this]() {
                  editedFilterState = FilterState{};
                  updateCurrentFilter();
              },
              ftxui::ButtonOption::Animated(ftxui::Color::Red));

            std::vector<ftxui::Component> mainComponents;

            std::vector<ftxui::Component> levelComponents;
            levelComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📊 Log Levels") | ftxui::bold | ftxui::color(ftxui::Color::Cyan)
                     | ftxui::center;
            }));
            levelComponents.push_back(getLogLevelFilterComponent());

            std::vector<ftxui::Component> channelComponents;
            channelComponents.push_back(ftxui::Renderer([] {
                return ftxui::text("📡 Channels") | ftxui::bold | ftxui::color(ftxui::Color::Yellow)
                     | ftxui::center;
            }));
            channelComponents.push_back(getChannelFilterComponent());

            std::vector<ftxui::Component> horizontalComponents;
            horizontalComponents.push_back(ftxui::Container::Vertical(levelComponents)
                                           | ftxui::flex);
            horizontalComponents.push_back(ftxui::Container::Vertical(channelComponents)
                                           | ftxui::flex);

            mainComponents.push_back(ftxui::Container::Horizontal(horizontalComponents));
            mainComponents.push_back(getLocationFilterComponent());

            return ftxui::Container::Vertical(
              {clearButton,
               ftxui::Container::Vertical(mainComponents)
                 | ftxui::Renderer([](ftxui::Element inner) {
                       return ftxui::vbox({ftxui::text("🔍 Filters") | ftxui::bold
                                             | ftxui::color(ftxui::Color::Cyan) | ftxui::center,
                                           ftxui::separator(),
                                           inner});
                   })
                 | ftxui::borderRounded});
        }

        ftxui::Component getAppearanceSettingsComponent() {
            auto resetButton = ftxui::Button(
              "🔄 Reset to Defaults",
              [this]() {
                  showSysTime      = true;
                  showFunctionName = false;
                  showUcTime       = true;
                  showLocation     = true;
                  showChannel      = true;
                  showLogLevel     = true;
              },
              ftxui::ButtonOption::Animated(ftxui::Color::Blue));

            auto clearButton = ftxui::Button(
              "❌ Clear log entries",
              [this]() {
                  allLogEntries.clear();
                  filteredLogEntries.clear();
              },
              ftxui::ButtonOption::Animated(ftxui::Color::Red));

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal({resetButton | ftxui::flex, clearButton | ftxui::flex}),
               ftxui::Container::Vertical({ftxui::Checkbox("⏰ System Time", &showSysTime),
                                           ftxui::Checkbox("🔍 Function Names", &showFunctionName),
                                           ftxui::Checkbox("🕐 Target Time", &showUcTime),
                                           ftxui::Checkbox("📍 Source Location", &showLocation),
                                           ftxui::Checkbox("📡 Log Channel", &showChannel),
                                           ftxui::Checkbox("📊 Log Level", &showLogLevel)})
                 | ftxui::Renderer([](ftxui::Element inner) {
                       return ftxui::vbox({ftxui::text("🎨 Display Settings") | ftxui::bold
                                             | ftxui::color(ftxui::Color::Magenta) | ftxui::center,
                                           ftxui::separator(),
                                           inner});
                   })
                 | ftxui::borderRounded});
        }

        template<typename Reader>
        ftxui::Component getDebuggerComponent(Reader& rttReader) {
            auto resetTargetBtn = ftxui::Button(
              "🔄 Reset Target [r]",
              [&rttReader]() { rttReader.resetTarget(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Blue));

            auto resetDebuggerBtn = ftxui::Button(
              "🔌 Reset Debugger [d]",
              [&rttReader]() { rttReader.resetJLink(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Magenta));

            auto flashBtn = ftxui::Button(
              "⚡ Flash Target [f]",
              [&rttReader]() { rttReader.flash(); },
              ftxui::ButtonOption::Animated(ftxui::Color::Yellow));

            auto statusDisplay = ftxui::Renderer([&rttReader]() {
                auto const rttStatus = rttReader.getStatus();

                return ftxui::vbox(
                         {ftxui::text("📊 Debugger Status") | ftxui::bold
                            | ftxui::color(ftxui::Color::Cyan) | ftxui::center,
                          ftxui::separator(),

                          ftxui::hbox(
                            {ftxui::text("Connection: ") | ftxui::bold,
                             ftxui::text(rttStatus.isRunning != 0 ? "✓ Active" : "✗ Inactive")
                               | ftxui::color(rttStatus.isRunning != 0 ? ftxui::Color::Green
                                                                       : ftxui::Color::Red)
                               | ftxui::bold}),

                          ftxui::hbox(
                            {ftxui::text("Overflows: ") | ftxui::bold,
                             ftxui::text(fmt::format("{}", rttStatus.hostOverflowCount))
                               | ftxui::color(rttStatus.hostOverflowCount == 0 ? ftxui::Color::Green
                                                                               : ftxui::Color::Red)
                               | ftxui::bold}),

                          ftxui::hbox({ftxui::text("Read: ") | ftxui::bold,
                                       ftxui::text(fmt::format("{} bytes", rttStatus.numBytesRead))
                                         | ftxui::color(ftxui::Color::Cyan)}),

                          ftxui::hbox({ftxui::text("Buffers: ") | ftxui::bold,
                                       ftxui::text(fmt::format("↑{} ↓{}",
                                                               rttStatus.numUpBuffers,
                                                               rttStatus.numDownBuffers))
                                         | ftxui::color(ftxui::Color::Yellow)})})
                     | ftxui::borderRounded;
            });

            return ftxui::Container::Vertical(
              {ftxui::Container::Horizontal({resetTargetBtn | ftxui::flex,
                                             resetDebuggerBtn | ftxui::flex,
                                             flashBtn | ftxui::flex}),
               statusDisplay});
        }

        int tab_selected{};

        ftxui::Component
        generateTabsComponent(std::vector<std::pair<std::string_view,
                                                    ftxui::Component>> const& entries) {
            std::vector<std::string>      tab_values{};
            std::vector<ftxui::Component> tab_components{};

            for(auto const& [name, component] : entries) {
                tab_values.push_back(std::string{name} + " ");
                tab_components.push_back(component | ftxui::border);
            }

            auto toggle
              = ftxui::Toggle(std::move(tab_values), &tab_selected) | ftxui::bold | ftxui::border;

            ftxui::Components vertical_components{
              toggle,
              ftxui::Container::Tab(std::move(tab_components), &tab_selected) | ftxui::flex};

            return ftxui::Container::Vertical(vertical_components);
        }

        template<typename Reader>
        ftxui::Component getTabComponent(Reader& rttReader) {
            auto tabs = generateTabsComponent({
              {   "📄 Logs",                getLogComponent()},
              { "🔍 Filter",             getFilterComponent()},
              {"🎨 Display", getAppearanceSettingsComponent()},
              {  "🔧 Debug",  getDebuggerComponent(rttReader)},
              {  "🔨 Build",              getBuildComponent()},
              { "💬 Status",             getStatusComponent()}
            });

            return ftxui::Container::Vertical(
              {getStatusLineComponent(rttReader), tabs | ftxui::flex});
        }

        template<typename Reader>
        ftxui::Component getStatusLineComponent(Reader& rttReader) {
            return ftxui::Renderer([&rttReader, this]() {
                auto const rttStatus    = rttReader.getStatus();
                auto const logCount     = filteredLogEntries.size();
                auto const totalCount   = allLogEntries.size();
                bool const filterActive = activeFilterState != FilterState{};

                std::string  statusText;
                ftxui::Color statusColor;
                switch(buildStatus) {
                case BuildStatus::Idle:
                    statusText  = "⚪ Idle";
                    statusColor = ftxui::Color::GrayDark;
                    break;
                case BuildStatus::Running:
                    statusText  = "🟡 Building...";
                    statusColor = ftxui::Color::Yellow;
                    break;
                case BuildStatus::Success:
                    statusText  = "✅ Success";
                    statusColor = ftxui::Color::Green;
                    break;
                case BuildStatus::Failed:
                    statusText  = "❌ Failed";
                    statusColor = ftxui::Color::Red;
                    break;
                }

                return ftxui::hbox(
                         {ftxui::text(rttStatus.isRunning != 0 ? "● Connected" : "○ Disconnected")
                            | ftxui::color(rttStatus.isRunning != 0 ? ftxui::Color::Green
                                                                    : ftxui::Color::Red)
                            | ftxui::bold,
                          ftxui::separator(),
                          ftxui::text(fmt::format("Logs: {}/{}", logCount, totalCount))
                            | ftxui::color(ftxui::Color::Cyan),
                          ftxui::separator(),
                          ftxui::text(fmt::format("Overflows: {}", rttStatus.hostOverflowCount))
                            | ftxui::color(rttStatus.hostOverflowCount == 0 ? ftxui::Color::Green
                                                                            : ftxui::Color::Red),
                          ftxui::separator(),
                          ftxui::text(fmt::format("Bytes: {}", rttStatus.numBytesRead))
                            | ftxui::color(ftxui::Color::Yellow),
                          ftxui::separator(),
                          ftxui::text(filterActive ? "✓ Filters Active" : "○ No Filters")
                            | ftxui::color(filterActive ? ftxui::Color::Yellow
                                                        : ftxui::Color::Green),
                          ftxui::separator(),
                          ftxui::text(fmt::format("🔨 {}", statusText)) | ftxui::color(statusColor),
                          ftxui::separator(),
                          ftxui::filler(),
                          ftxui::text("[q]uit [r]eset [f]lash [b]uild [d]ebugger_reset")
                            | ftxui::color(ftxui::Color::GrayDark)})
                     | ftxui::border;
            });
        }

        template<typename Reader>
        int run(Reader&            rttReader,
                std::string const& buildCommand) {
            if(!initializeBuildCommand(buildCommand)) {
                fmt::print(stderr, "❌ Invalid build command: {}\n", buildCommand);
                return 1;
            }

            static std::atomic<bool> gotSignal{};
            {
                struct sigaction sa{};
                sa.sa_handler = [](int) { gotSignal = true; };
                ::sigemptyset(&sa.sa_mask);
                if(::sigaction(SIGINT, &sa, nullptr) == -1) {
                    fmt::print(stderr,
                               "Failed to register signal handler: {}\n",
                               std::strerror(errno));
                    return 1;
                }
            }

            auto             screen = ftxui::ScreenInteractive::Fullscreen();
            ftxui::Component mainComponent;
            {
                std::lock_guard<std::mutex> lock{mutex};
                screen_ptr = &screen;

                mainComponent
                  = ftxui::CatchEvent(getTabComponent(rttReader), [&](ftxui::Event event) {
                        if(tab_selected == 1 && event.is_character()) {
                            return false;
                        }

                        if(event == ftxui::Event::Character('r')) {
                            rttReader.resetTarget();
                            return true;
                        }
                        if(event == ftxui::Event::Character('f')) {
                            rttReader.flash();
                            return true;
                        }
                        if(event == ftxui::Event::Character('d')) {
                            rttReader.resetJLink();
                            return true;
                        }
                        if(event == ftxui::Event::Character('b')) {
                            executeBuild();
                            return true;
                        }
                        if(event == ftxui::Event::Character('q')) {
                            screen_ptr->Exit();
                            return true;
                        }

                        return false;
                    });
            }
            ftxui::Loop loop(screen_ptr, mainComponent);

            while(!loop.HasQuitted() && !gotSignal) {
                {
                    std::lock_guard<std::mutex> lock{mutex};
                    loop.RunOnce();
                }
                std::this_thread::sleep_for(GUI_Constants::UpdateInterval);
            }

            return 0;
        }
    };
}}   // namespace uc_log::FTXUIGui
