#pragma once

#include "uc_log/detail/LogEntry.hpp"

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

#include <algorithm>
#include <array>
#include <charconv>
#include <fmt/format.h>
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

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
                bool const is_selected = (hoveredEntryIndex == i);
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
            return vbox(std::move(elements), hoveredEntryIndex) | reflect(renderBox);
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
                int const previousHoveredIndex = hoveredEntryIndex;
                if(event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k')) {
                    (hoveredEntryIndex)--;
                }
                if(event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j')) {
                    (hoveredEntryIndex)++;
                }
                if(event == ftxui::Event::PageUp) {
                    (hoveredEntryIndex) -= renderBox.y_max - renderBox.y_min;
                }
                if(event == ftxui::Event::PageDown) {
                    (hoveredEntryIndex) += renderBox.y_max - renderBox.y_min;
                }
                if(event == ftxui::Event::Home) {
                    (hoveredEntryIndex) = 0;
                }
                if(event == ftxui::Event::End) {
                    (hoveredEntryIndex) = size() - 1;
                }
                if(event == ftxui::Event::Tab && size()) {
                    hoveredEntryIndex = (hoveredEntryIndex + 1) % size();
                }
                if(event == ftxui::Event::TabReverse && size()) {
                    hoveredEntryIndex = (hoveredEntryIndex + size() - 1) % size();
                }

                hoveredEntryIndex = util::clamp(hoveredEntryIndex, 0, size() - 1);

                if(hoveredEntryIndex != previousHoveredIndex) {
                    focused_entry() = hoveredEntryIndex;
                    on_change();
                    return true;
                }
            }

            if(event == ftxui::Event::Character(' ') || event == ftxui::Event::Return) {
                selected() = hoveredEntryIndex;
                on_change();
                on_click(hoveredEntryIndex);
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

            int const previousHoveredIndex = hoveredEntryIndex;

            if(event.mouse().button == ftxui::Mouse::WheelUp) {
                (hoveredEntryIndex)--;
            }
            if(event.mouse().button == ftxui::Mouse::WheelDown) {
                (hoveredEntryIndex)++;
            }

            hoveredEntryIndex = util::clamp(hoveredEntryIndex, 0, size() - 1);

            if(hoveredEntryIndex != previousHoveredIndex) {
                on_change();
            }

            return true;
        }

        void Clamp() {
            boxes_.resize(static_cast<std::size_t>(size()));
            selected()        = util::clamp(selected(), 0, size() - 1);
            focused_entry()   = util::clamp(focused_entry(), 0, size() - 1);
            hoveredEntryIndex = util::clamp(hoveredEntryIndex, 0, size() - 1);
        }

        bool Focusable() const final { return entries.size(); }

        int size() const { return int(entries.size()); }

        int                     hoveredEntryIndex = selected();
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
              is_focused || isHovered,
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

            isHovered = false;
            if(event == ftxui::Event::Character(' ') || event == ftxui::Event::Return) {
                on_change();
                TakeFocus();
                return true;
            }
            return false;
        }

        bool OnMouseEvent(ftxui::Event event) {
            isHovered = renderBox.Contain(event.mouse().x, event.mouse().y);

            if(!CaptureMouse(event)) {
                return false;
            }

            if(!isHovered) {
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

        bool       isHovered = false;
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
                    int hiddenBeforeAdjusted
                      = selectedIndex - (ySpace / 2) + (ySpace % 2 == 0 ? 1 : 0);
                    hiddenBefore = hiddenBeforeAdjusted >= 0 ? hiddenBeforeAdjusted : 0;
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

            int displayIndex = 0;
            for(auto const& e : container | std::views::drop(hiddenBefore)) {
                bool const isCurrentItem = displayIndex + hiddenBefore == selectedIndex;

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

                ++displayIndex;
                if(displayIndex == ySpace) {
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
            ftxui::Elements textElements;

            for(bool first = true; auto line : std::views::split(text, '\n')) {
                if(!first) {
                    textElements.push_back(ftxui::text("↩") | ftxui::color(ftxui::Color::Red));
                }
                first = false;

                auto element = ftxui::text(std::string{line.begin(), line.end()});

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
                textElements.push_back(element);
            }
            return ftxui::hbox(textElements);
        };

        auto parseAnsiCode = [&](std::string const& codeStr) {
            if(codeStr.empty()) {
                return;
            }

            if(codeStr.front() == ']') {
                if(codeStr.size() >= 3 && codeStr.substr(0, 3) == "]8;") {
                    std::string params = codeStr.substr(3);
                    if(params == ";") {
                        currentHyperlink.clear();
                    } else if(params.size() > 1 && params.front() == ';') {
                        currentHyperlink = params.substr(1);
                    }
                }
                return;
            }

            if(codeStr.front() == '[') {
                std::string params = codeStr.substr(1);
                if(params.empty()) {
                    return;
                }

                char command = params.back();
                params.pop_back();

                if(command == 'm') {
                    if(params.empty()) {
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
                        case 0:
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
                        case 30: currentFgColor = ftxui::Color::Black; break;
                        case 31: currentFgColor = ftxui::Color::Red; break;
                        case 32: currentFgColor = ftxui::Color::Green; break;
                        case 33: currentFgColor = ftxui::Color::Yellow; break;
                        case 34: currentFgColor = ftxui::Color::Blue; break;
                        case 35: currentFgColor = ftxui::Color::Magenta; break;
                        case 36: currentFgColor = ftxui::Color::Cyan; break;
                        case 37: currentFgColor = ftxui::Color::White; break;
                        case 38:
                            if(i + 2 < codes.size() && codes[i + 1] == 5) {
                                currentFgColor = ftxui::Color::Palette256(codes[i + 2]);
                                i += 2;
                            }
                            break;
                        case 39: currentFgColor = ftxui::Color::Default; break;
                        case 40: currentBgColor = ftxui::Color::Black; break;
                        case 41: currentBgColor = ftxui::Color::Red; break;
                        case 42: currentBgColor = ftxui::Color::Green; break;
                        case 43: currentBgColor = ftxui::Color::Yellow; break;
                        case 44: currentBgColor = ftxui::Color::Blue; break;
                        case 45: currentBgColor = ftxui::Color::Magenta; break;
                        case 46: currentBgColor = ftxui::Color::Cyan; break;
                        case 47: currentBgColor = ftxui::Color::White; break;
                        case 48:
                            if(i + 2 < codes.size() && codes[i + 1] == 5) {
                                currentBgColor = ftxui::Color::Palette256(codes[i + 2]);
                                i += 2;
                            }
                            break;
                        case 49:  currentBgColor = ftxui::Color::Default; break;
                        case 90:  currentFgColor = ftxui::Color::GrayDark; break;
                        case 91:  currentFgColor = ftxui::Color::RedLight; break;
                        case 92:  currentFgColor = ftxui::Color::GreenLight; break;
                        case 93:  currentFgColor = ftxui::Color::YellowLight; break;
                        case 94:  currentFgColor = ftxui::Color::BlueLight; break;
                        case 95:  currentFgColor = ftxui::Color::MagentaLight; break;
                        case 96:  currentFgColor = ftxui::Color::CyanLight; break;
                        case 97:  currentFgColor = ftxui::Color::GrayLight; break;
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

                bool isComplete = false;

                if(!currentEscape.empty() && currentEscape.front() == ']') {
                    if(c == '\\' && currentEscape.size() >= 2
                       && currentEscape[currentEscape.size() - 2] == escape)
                    {
                        currentEscape.pop_back();
                        currentEscape.pop_back();
                        isComplete = true;
                    } else if(c == '\x07') {
                        currentEscape.pop_back();
                        isComplete = true;
                    }
                } else if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
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

    static ftxui::ButtonOption createButtonStyle(ftxui::Color bgColor,
                                                 ftxui::Color textColor = ftxui::Color::Black) {
        auto option      = ftxui::ButtonOption::Simple();
        option.transform = [bgColor, textColor](ftxui::EntryState const& s) {
            auto element = ftxui::text(s.label) | ftxui::center;
            if(s.focused) {
                element |= ftxui::bold | ftxui::color(textColor) | ftxui::bgcolor(bgColor)
                         | ftxui::inverted;
            } else if(s.active) {
                element |= ftxui::bold | ftxui::color(textColor) | ftxui::bgcolor(bgColor);
            } else {
                element |= ftxui::color(textColor) | ftxui::bgcolor(bgColor);
            }
            return element;
        };
        return option;
    }

}}   // namespace uc_log::FTXUIGui
