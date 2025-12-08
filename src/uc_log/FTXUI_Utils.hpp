#pragma once

#include "uc_log/detail/LogEntry.hpp"
#include "uc_log/metric_utils.hpp"
#include "uc_log/theme.hpp"

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
        constexpr T clamp(T const& value,
                          T const& lower,
                          T const& upper) {
            if(value < lower) { return lower; }
            if(upper < value) { return upper; }
            return value;
        }
    }   // namespace util

    struct CheckboxOption {
        static CheckboxOption Simple() {
            auto option      = CheckboxOption();
            option.transform = [](ftxui::EntryState const& state) {
                auto element = ftxui::text(state.label);
                if(state.active) { element |= ftxui::bold; }
                if(state.focused) { element |= ftxui::inverted; }
                return ftxui::hbox(
                  {element | ftxui::color(state.state ? ftxui::Color::Green : ftxui::Color::Red)});
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
                    .label   = *label,
                    .state   = is_checked(),
                    .active  = is_active,
                    .focused = is_focused || isHovered,
                    .index   = -1,
            };
            auto element
              = (transform ? transform : CheckboxOption::Simple().transform)(entry_state);
            element |= ftxui::focus;
            element |= reflect(renderBox);
            return element;
        }

        bool OnEvent(ftxui::Event event) override {
            if(!CaptureMouse(event)) { return false; }

            if(event.is_mouse()) { return OnMouseEvent(event); }

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

            if(!CaptureMouse(event)) { return false; }

            if(!isHovered) { return false; }

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
        options.label      = std::move(label);
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

        ScrollerBase(ContainerGetter&& containerGetter,
                     Transform&&       transformFunc)
          : containerGetter_{containerGetter}
          , transform_{transformFunc} {}

    private:
        ftxui::Element OnRender() final {
            auto const& container = containerGetter_();
            containerSize         = static_cast<int>(container.size());
            int const ySpace      = (renderBox.y_max - renderBox.y_min) + 2;
            selectedIndex         = std::max(0, std::min(containerSize - 1, selectedIndex));
            if(stick) { selectedIndex = containerSize - 1; }

            int hiddenBefore{};
            int hiddenBehind{};

            if(containerSize > ySpace) {
                int const tooMany = containerSize - ySpace;

                if(stick) {
                    hiddenBefore = tooMany;
                    hiddenBehind = 0;
                } else {
                    int const hiddenBeforeAdjusted
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
            elements.reserve(static_cast<std::size_t>(ySpace) + 2U);

            elements.push_back(
              ftxui::text("")
              | ftxui::size(ftxui::HEIGHT,
                            ftxui::EQUAL,
                            static_cast<int>(std::min(static_cast<std::size_t>(hiddenBefore),
                                                      GUI_Constants::MaxScrollLines))));

            ftxui::Elements metadataElements;
            metadataElements.reserve(static_cast<std::size_t>(ySpace) + 2U);

            metadataElements.push_back(
              ftxui::text("")
              | ftxui::size(ftxui::HEIGHT,
                            ftxui::EQUAL,
                            static_cast<int>(std::min(static_cast<std::size_t>(hiddenBefore),
                                                      GUI_Constants::MaxScrollLines))));

            int displayIndex = 0;
            for(auto const& entry : container | std::views::drop(hiddenBefore)) {
                bool const isCurrentItem = displayIndex + hiddenBefore == selectedIndex;

                auto transformedElement = transform_(entry);

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
                if(displayIndex == ySpace) { break; }
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

                    // Calculate where content should be clipped (before metadata and indicator)
                    int const indicatorWidth = 2;   // Space for "→ "
                    int const contentMaxX    = box_.x_max - overlayWidth_ - indicatorWidth;

                    // Apply stencil to clip content
                    screen.stencil.x_max = std::min(screen.stencil.x_max, contentMaxX);
                    child_->Render(screen);
                    screen.stencil = oldStencil;

                    // Position for the truncation indicator
                    int const indicatorX = contentMaxX;

                    // Check if content was actually truncated at the clip edge
                    for(int y = box_.y_min; y <= box_.y_max; ++y) {
                        if(y >= 0 && y < screen.dimy() && indicatorX >= 0
                           && indicatorX < screen.dimx())
                        {
                            // Check last column before indicator for content
                            int const checkX = contentMaxX - 1;
                            if(checkX >= 0 && checkX < screen.dimx()) {
                                auto const& edgePixel = screen.PixelAt(checkX, y);
                                bool const  isTruncated
                                  = !edgePixel.character.empty() && edgePixel.character != " ";

                                if(isTruncated) {
                                    screen.PixelAt(indicatorX, y).character = "→";
                                    screen.PixelAt(indicatorX, y).foreground_color
                                      = ftxui::Color::Red;
                                }
                            }
                        }
                    }

                    fixedOverlay_->Render(screen);
                }

            private:
                ftxui::Element child_;
                int            offset_;
                ftxui::Element fixedOverlay_;
                int            overlayWidth_;
            };

            int const metadataWidth = metadataOverlay->requirement().min_x;

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

            if(selectedIndex >= containerSize - 1) { stick = true; }
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
    ftxui::Component Scroller(ContainerGetter&& containerGetter,
                              Transform&&       transformFunc) {
        return ftxui::Make<ScrollerBase<ContainerGetter, Transform>>(
          std::forward<ContainerGetter>(containerGetter),
          std::forward<Transform>(transformFunc));
    }

    inline ftxui::Element toElement(uc_log::detail::LogEntry::Channel const& channel) {
        static std::array<ftxui::Color, 6> const Colors{
          {ftxui::Color::Black,
           ftxui::Color::Red,
           ftxui::Color::Blue,
           ftxui::Color::Magenta,
           ftxui::Color::White,
           ftxui::Color::Yellow}
        };

        return ftxui::text(fmt::format("{}", channel.channel))
             | ftxui::color(Colors[channel.channel % Colors.size()])
             | ((channel.channel == 0) ? ftxui::bgcolor(ftxui::Color::Green) : ftxui::nothing);
    }

    inline ftxui::Element toElement(uc_log::LogLevel const& level) {
        static std::array<std::pair<ftxui::Color, std::string_view>, 6> const LCS{
          {{ftxui::Color::Yellow, "trace"},
           {ftxui::Color::Green, "debug"},
           {ftxui::Color::BlueLight, "info"},
           {ftxui::Color::Magenta, "warn"},
           {ftxui::Color::Red, "error"},
           {ftxui::Color::White, "crit"}}
        };
        static constexpr std::size_t MaxLength = 5;   // max of "trace", "debug", "error", ...

        auto index = static_cast<std::size_t>(level);

        if(index >= LCS.size()) { index = 0; }

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

        std::string operator[](std::size_t index) const override {
            auto const iter = std::next(container.begin(), static_cast<int>(index));

            auto const& [sourceLocation, count] = *iter;
            auto const& [fileName, lineNumber]  = sourceLocation;
            return fmt::format("{}:{} -> {}", fileName, lineNumber, count);
        }

        std::map<SourceLocation, std::size_t>& container;
    };

    struct EnabledLocationAdapter : ftxui::ConstStringListRef::Adapter {
        EnabledLocationAdapter(std::set<SourceLocation>& container_) : container{container_} {}

        std::size_t size() const override { return container.size(); }

        std::string operator[](std::size_t index) const override {
            auto const iter = std::next(container.begin(), static_cast<int>(index));
            auto const& [fileName, lineNumber] = *iter;
            if(lineNumber == 0) { return fmt::format("{}:all", fileName); }
            return fmt::format("{}:{}", fileName, lineNumber);
        }

        std::set<SourceLocation>& container;
    };

    static ftxui::Element ansiColoredTextToFtxui(std::string_view text) {
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

        auto applyStyles = [&](std::string const& str) {
            ftxui::Elements textElements;

            for(bool first = true; auto line : std::views::split(str, '\n')) {
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
                if(bold) { element |= ftxui::bold; }
                if(dim) { element |= ftxui::dim; }
                if(italic) { element |= ftxui::italic; }
                if(underline) { element |= ftxui::underlined; }
                if(blink) { element |= ftxui::blink; }
                if(reverse) { element |= ftxui::inverted; }
                if(strikethrough) { element |= ftxui::strikethrough; }

                if(!currentHyperlink.empty()) {
                    element = ftxui::hyperlink(currentHyperlink, element);
                }
                textElements.push_back(element);
            }
            return ftxui::hbox(textElements);
        };

        auto parseAnsiCode = [&](std::string const& codeStr) {
            if(codeStr.empty()) { return; }

            if(codeStr.front() == ']') {
                if(codeStr.size() >= 3 && codeStr.starts_with("]8;")) {
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
                if(params.empty()) { return; }

                char const command = params.back();
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
                    for(char const character : params) {
                        if(character == ';') {
                            if(!current.empty()) {
                                codes.push_back(std::stoi(current));
                                current.clear();
                            }
                        } else {
                            current.push_back(character);
                        }
                    }
                    if(!current.empty()) { codes.push_back(std::stoi(current)); }

                    for(std::size_t i = 0; i < codes.size(); ++i) {
                        int const code = codes[i];
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
                        default:  break;
                        }
                    }
                }
            }
        };

        for(auto character : text) {
            if(character == escape) {
                if(!currentText.empty()) {
                    elements.push_back(applyStyles(currentText));
                    currentText.clear();
                }
                inEscape = true;
                currentEscape.clear();
                continue;
            }

            if(inEscape) {
                currentEscape.push_back(character);

                bool isComplete = false;

                if(!currentEscape.empty() && currentEscape.front() == ']') {
                    if(character == '\\' && currentEscape.size() >= 2
                       && currentEscape[currentEscape.size() - 2] == escape)
                    {
                        currentEscape.pop_back();
                        currentEscape.pop_back();
                        isComplete = true;
                    } else if(character == '\x07') {
                        currentEscape.pop_back();
                        isComplete = true;
                    }
                } else if((character >= 'A' && character <= 'Z')
                          || (character >= 'a' && character <= 'z') || character == '~')
                {
                    isComplete = true;
                }

                if(isComplete) {
                    parseAnsiCode(currentEscape);
                    inEscape = false;
                    currentEscape.clear();
                }
            } else {
                currentText.push_back(character);
            }
        }

        if(!currentText.empty()) { elements.push_back(applyStyles(currentText)); }

        return elements.empty() ? ftxui::text("") : ftxui::hbox(elements);
    }

    static ftxui::ButtonOption createButtonStyle(ftxui::Color bgColor,
                                                 ftxui::Color textColor = ftxui::Color::Black) {
        auto option      = ftxui::ButtonOption::Simple();
        option.transform = [bgColor, textColor](ftxui::EntryState const& state) {
            auto element = ftxui::text(state.label) | ftxui::center;
            if(state.focused) {
                element |= ftxui::bold | ftxui::color(textColor) | ftxui::bgcolor(bgColor)
                         | ftxui::inverted;
            } else if(state.active) {
                element |= ftxui::bold | ftxui::color(textColor) | ftxui::bgcolor(bgColor);
            } else {
                element |= ftxui::color(textColor) | ftxui::bgcolor(bgColor);
            }
            return element;
        };
        return option;
    }

    enum class TimeUnit : std::uint8_t { Seconds, Minutes, Hours };

    enum class TimeRangeMode : std::uint8_t { ShowAll, LastPeriod };

    class MetricPlotWidget {
    public:
        struct Config {
            bool   autoFitY     = true;
            double yZoomLevel   = 1.0;
            double yCenterValue = 0.0;

            TimeRangeMode timeRangeMode   = TimeRangeMode::ShowAll;
            int           timePeriodValue = 10;
            TimeUnit      timePeriodUnit  = TimeUnit::Seconds;
            bool          autoScroll      = true;

            int minHeight = 6;
        };

    private:
        Config                    config_;
        std::optional<MetricInfo> selectedMetric_;

        int         timeModeIndex_      = 0;
        int         timeUnitIndex_      = 0;
        std::string timePeriodValueStr_ = "1";

        static std::chrono::seconds timeUnitToSeconds(int      value,
                                                      TimeUnit unit) {
            switch(unit) {
            case TimeUnit::Seconds: return std::chrono::seconds(value);
            case TimeUnit::Minutes: return std::chrono::seconds(value * 60);
            case TimeUnit::Hours:   return std::chrono::seconds(value * 3600);
            }
            return std::chrono::seconds(value);
        }

        static std::string timeUnitToString(TimeUnit unit,
                                            bool     plural = true) {
            switch(unit) {
            case TimeUnit::Seconds: return plural ? "seconds" : "second";
            case TimeUnit::Minutes: return plural ? "minutes" : "minute";
            case TimeUnit::Hours:   return plural ? "hours" : "hour";
            }
            return "seconds";
        }

        static TimeUnit suggestTimeUnit(std::chrono::seconds totalSpan) {
            auto secs = totalSpan.count();
            if(secs <= 300) { return TimeUnit::Seconds; }
            if(secs <= 7200) { return TimeUnit::Minutes; }
            return TimeUnit::Hours;
        }

        std::chrono::seconds analyzeDataTimeSpan(std::vector<MetricEntry> const& values) const {
            if(values.empty()) { return std::chrono::seconds(0); }
            auto oldest = values.front().recv_time;
            auto newest = values.back().recv_time;
            return std::chrono::duration_cast<std::chrono::seconds>(newest - oldest);
        }

        std::string formatMetricValue(double             value,
                                      std::string const& unit) const {
            auto absValue = std::abs(value);

            if(absValue == 0.0) { return unit.empty() ? "0" : fmt::format("0{}", unit); }

            if(absValue >= 1e9) {
                return unit.empty() ? fmt::format("{:.2f}G", value / 1e9)
                                    : fmt::format("{:.2f}G{}", value / 1e9, unit);
            }
            if(absValue >= 1e6) {
                return unit.empty() ? fmt::format("{:.2f}M", value / 1e6)
                                    : fmt::format("{:.2f}M{}", value / 1e6, unit);
            }
            if(absValue >= 1e3) {
                return unit.empty() ? fmt::format("{:.2f}K", value / 1e3)
                                    : fmt::format("{:.2f}K{}", value / 1e3, unit);
            }
            if(absValue >= 1.0) {
                return unit.empty() ? fmt::format("{:.2f}", value)
                                    : fmt::format("{:.2f}{}", value, unit);
            }
            if(absValue >= 1e-3) {
                return unit.empty() ? fmt::format("{:.2f}m", value * 1e3)
                                    : fmt::format("{:.2f}m{}", value * 1e3, unit);
            }
            if(absValue >= 1e-6) {
                return unit.empty() ? fmt::format("{:.2f}µ", value * 1e6)
                                    : fmt::format("{:.2f}µ{}", value * 1e6, unit);
            }
            return unit.empty() ? fmt::format("{:.2e}", value)
                                : fmt::format("{:.2e}{}", value, unit);
        }

        std::string formatTimeLabel(std::chrono::system_clock::time_point current_time,
                                    std::chrono::system_clock::time_point reference_time) const {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(reference_time
                                                                                  - current_time);

            if(duration.count() >= 0) { return "now"; }

            auto absDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
              current_time - reference_time);
            auto absSeconds = std::chrono::duration_cast<std::chrono::seconds>(absDuration).count();

            if(absSeconds < 60) { return fmt::format("-{}s", absSeconds); }
            if(absSeconds < 3600) {
                auto minutes = absSeconds / 60;
                return fmt::format("-{}m", minutes);
            }
            if(absSeconds < 86400) {
                auto hours   = absSeconds / 3600;
                auto minutes = (absSeconds % 3600) / 60;
                return fmt::format("-{}h{}m", hours, minutes);
            }
            auto days  = absSeconds / 86400;
            auto hours = (absSeconds % 86400) / 3600;
            return fmt::format("-{}d{}h", days, hours);
        }

        std::vector<std::string> generateYAxisLabels(double             minVal,
                                                     double             maxVal,
                                                     int                height,
                                                     std::string const& unit) const {
            std::vector<std::string> labels;

            if(height < 3) { return labels; }

            double const range = maxVal - minVal;
            if(range == 0.0) {
                labels.push_back(formatMetricValue(minVal, unit));
                return labels;
            }

            int numTicks = std::min(5, height / 2);
            numTicks     = std::max(numTicks, 2);

            for(int i = 0; i < numTicks; ++i) {
                double const value = maxVal - (static_cast<double>(i) * range / (numTicks - 1));
                labels.push_back(formatMetricValue(value, unit));
            }

            return labels;
        }

        std::vector<std::string> generateXAxisLabels(std::vector<MetricEntry> const& values,
                                                     std::size_t                     startIdx,
                                                     std::size_t visibleDataSize) const {
            std::vector<std::string> labels;

            if(values.empty() || visibleDataSize < 2 || startIdx >= values.size()) {
                return labels;
            }

            auto currentTime = std::chrono::system_clock::now();

            int numTicks = std::min(4, static_cast<int>(visibleDataSize) / 8);
            numTicks     = std::max(numTicks, 2);

            for(int i = 0; i < numTicks; ++i) {
                std::size_t dataIdx = 0;
                if(i == numTicks - 1) {
                    dataIdx = startIdx + visibleDataSize - 1;
                } else {
                    dataIdx = startIdx
                            + (static_cast<std::size_t>(i) * (visibleDataSize - 1))
                                / static_cast<std::size_t>(numTicks - 1);
                }

                if(dataIdx < values.size()) {
                    auto timeLabel = formatTimeLabel(currentTime, values[dataIdx].recv_time);
                    labels.push_back(timeLabel);
                }
            }

            return labels;
        }

        std::pair<double,
                  double>
        calculateYAxisRange(double dataMin,
                            double dataMax) const {
            if(config_.autoFitY) {
                if(std::abs(dataMin - dataMax) < std::numeric_limits<double>::epsilon()
                                                   * std::max(std::abs(dataMin), std::abs(dataMax)))
                {
                    double const center = dataMin;
                    double const span   = std::max(1.0, std::abs(center) * 0.1);
                    return {center - span, center + span};
                }

                double const dataRange = dataMax - dataMin;
                double const padding   = dataRange * 0.05;
                return {dataMin - padding, dataMax + padding};
            }
            double baseRange{};
            double center{};

            if(std::abs(dataMin - dataMax) < std::numeric_limits<double>::epsilon()
                                               * std::max(std::abs(dataMin), std::abs(dataMax)))
            {
                baseRange = std::max(1.0, std::abs(dataMin) * 0.2);
                center    = config_.yCenterValue != 0.0 ? config_.yCenterValue : dataMin;
            } else {
                baseRange = (dataMax - dataMin) / config_.yZoomLevel;
                center
                  = config_.yCenterValue != 0.0 ? config_.yCenterValue : (dataMin + dataMax) / 2.0;
            }

            double const halfRange = baseRange / 2.0;
            return {center - halfRange, center + halfRange};
        }

        std::pair<std::size_t,
                  std::size_t>
        calculateXAxisDataRange(std::vector<MetricEntry> const& values) const {
            if(values.empty()) { return {0, 0}; }

            std::size_t const dataSize = values.size();
            std::size_t       startIdx = 0;
            std::size_t       endIdx{};

            switch(config_.timeRangeMode) {
            case TimeRangeMode::ShowAll:
                startIdx = 0;
                endIdx   = dataSize;
                break;

            case TimeRangeMode::LastPeriod:
                {
                    auto timeWindow
                      = timeUnitToSeconds(config_.timePeriodValue, config_.timePeriodUnit);
                    auto cutoffTime = std::chrono::system_clock::now() - timeWindow;

                    for(std::size_t i = 0; i < dataSize; ++i) {
                        if(values[i].recv_time >= cutoffTime) {
                            startIdx = i;
                            break;
                        }
                    }
                    endIdx = dataSize;
                    break;
                }
            }

            return {startIdx, endIdx};
        }

    public:
        MetricPlotWidget()
          : timeModeIndex_(static_cast<int>(config_.timeRangeMode))
          , timeUnitIndex_(static_cast<int>(config_.timePeriodUnit))
          , timePeriodValueStr_(std::to_string(config_.timePeriodValue)) {}

        void setSelectedMetric(std::optional<MetricInfo> metric) {
            selectedMetric_ = std::move(metric);
        }

        void syncUIState() {
            timeModeIndex_      = static_cast<int>(config_.timeRangeMode);
            timeUnitIndex_      = static_cast<int>(config_.timePeriodUnit);
            timePeriodValueStr_ = std::to_string(config_.timePeriodValue);
        }

        std::optional<MetricInfo> const& getSelectedMetric() const { return selectedMetric_; }

        [[nodiscard]] ftxui::Component createControlsComponent() {
            return createControlsWithData([]() { return std::vector<MetricEntry>{}; });
        }

        template<typename MetricDataProvider>
        [[nodiscard]] ftxui::Component createControlsComponent(MetricDataProvider&& dataProvider) {
            return createControlsWithData(std::forward<MetricDataProvider>(dataProvider));
        }

    private:
        template<typename MetricDataProvider>
        ftxui::Component
        createControlsWithData([[maybe_unused]] MetricDataProvider&& dataProvider) {
            syncUIState();
            auto yAutoFitCheckbox = ftxui::Checkbox("🔧 Auto-fit", &config_.autoFitY);

            auto yZoomOutBtn = ftxui::Button(
              "🔍➖",
              [this]() { config_.yZoomLevel = std::max(0.1, config_.yZoomLevel / 1.5); },
              createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

            auto yZoomInBtn = ftxui::Button(
              "🔍➕",
              [this]() { config_.yZoomLevel = std::min(10.0, config_.yZoomLevel * 1.5); },
              createButtonStyle(Theme::Button::Background::settings(), Theme::Button::text()));

            std::vector<std::string> const timeRangeModeOptions = {"📈 Show All", "⏰ Last Period"};
            auto timeModeToggle = ftxui::Toggle(timeRangeModeOptions, &timeModeIndex_);
            timeModeToggle      = ftxui::CatchEvent(timeModeToggle, [this](ftxui::Event const&) {
                config_.timeRangeMode = static_cast<TimeRangeMode>(timeModeIndex_);
                return false;
            });

            auto timePeriodInput = ftxui::Input(&timePeriodValueStr_, "Enter number...");
            timePeriodInput      = ftxui::CatchEvent(timePeriodInput, [this](ftxui::Event const&) {
                try {
                    int const value = std::stoi(timePeriodValueStr_);
                    if(value > 0 && value <= 999) { config_.timePeriodValue = value; }
                } catch(std::exception const&) {}
                return false;
            });

            std::vector<std::string> const timeUnitOptions = {"sec", "min", "hr"};
            auto timeUnitToggle = ftxui::Toggle(timeUnitOptions, &timeUnitIndex_);
            timeUnitToggle      = ftxui::CatchEvent(timeUnitToggle, [this](ftxui::Event const&) {
                config_.timePeriodUnit = static_cast<TimeUnit>(timeUnitIndex_);
                return false;
            });

            auto yAxisRow = ftxui::Container::Horizontal({
              ftxui::Renderer(
                []() { return ftxui::text("Y-Axis:") | ftxui::color(Theme::Header::primary()); }),
              yAutoFitCheckbox,
              ftxui::Maybe(yZoomOutBtn, [this]() { return !config_.autoFitY; }),
              ftxui::Maybe(yZoomInBtn, [this]() { return !config_.autoFitY; }),
            });

            auto xAxisRow = ftxui::Container::Horizontal({
              ftxui::Renderer(
                []() { return ftxui::text("X-Axis:") | ftxui::color(Theme::Header::primary()); }),
              timeModeToggle,
              ftxui::Maybe(ftxui::Container::Horizontal({
                             ftxui::Renderer([]() { return ftxui::separator(); }),
                             ftxui::Renderer([]() {
                                 return ftxui::text("Show last:")
                                      | ftxui::color(Theme::Text::normal());
                             }),
                             timePeriodInput | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 6),
                             timeUnitToggle,
                           }),
                           [this]() { return config_.timeRangeMode == TimeRangeMode::LastPeriod; }),
            });

            ftxui::Components const allControls = {
              yAxisRow,
              xAxisRow,
            };

            return ftxui::Container::Vertical(allControls);
        }

    public:
        template<typename MetricDataProvider>
        ftxui::Component createGraphComponent(MetricDataProvider&& dataProvider) {
            return ftxui::Renderer([this,
                                    capturedDataProvider = std::forward<MetricDataProvider>(
                                      dataProvider)]() mutable -> ftxui::Element {
                if(!selectedMetric_) {
                    ftxui::Elements const noSelectionElements
                      = {ftxui::text("No metric selected") | ftxui::color(Theme::Status::warning())
                           | ftxui::center,
                         ftxui::text("Select a metric from the overview tab")
                           | ftxui::color(Theme::Text::metadata()) | ftxui::center};
                    return ftxui::vbox(noSelectionElements);
                }

                auto metricData = capturedDataProvider(*selectedMetric_);
                if(!metricData || (*metricData)->empty()) {
                    ftxui::Elements const noDataElements
                      = {ftxui::text("No data available") | ftxui::color(Theme::Status::error())
                           | ftxui::center,
                         ftxui::text("Waiting for metric data...")
                           | ftxui::color(Theme::Text::metadata()) | ftxui::center};
                    return ftxui::vbox(noDataElements);
                }

                auto const& values = **metricData;
                auto const& info   = *selectedMetric_;

                auto [xStartIdx, xEndIdx]         = calculateXAxisDataRange(values);
                std::size_t const visibleDataSize = xEndIdx - xStartIdx;

                if(visibleDataSize == 0) {
                    ftxui::Elements const noRangeElements
                      = {ftxui::text("No data in time range")
                           | ftxui::color(Theme::Status::warning()) | ftxui::center,
                         ftxui::text("Adjust X-axis controls")
                           | ftxui::color(Theme::Text::metadata()) | ftxui::center};
                    return ftxui::vbox(noRangeElements);
                }

                double dataMinVal = values[xStartIdx].value;
                double dataMaxVal = values[xStartIdx].value;
                for(std::size_t i = xStartIdx; i < xEndIdx; ++i) {
                    dataMinVal = std::min(dataMinVal, values[i].value);
                    dataMaxVal = std::max(dataMaxVal, values[i].value);
                }

                auto [yMin, yMax] = calculateYAxisRange(dataMinVal, dataMaxVal);

                auto graphFunc = [values, xStartIdx, xEndIdx, visibleDataSize, yMin, yMax](
                                   int width,
                                   int height) -> std::vector<int> {
                    std::vector<int> output(static_cast<std::size_t>(width), height / 2);

                    if(visibleDataSize == 0 || width <= 0) { return output; }

                    double yRange = yMax - yMin;
                    if(yRange == 0.0) { yRange = 1.0; }

                    for(int xPos = 0; xPos < width; ++xPos) {
                        std::size_t dataIdx{};
                        if(visibleDataSize == 1) {
                            dataIdx = xStartIdx;
                        } else {
                            dataIdx = xStartIdx
                                    + (static_cast<std::size_t>(xPos) * (visibleDataSize - 1))
                                        / static_cast<std::size_t>(width - 1);
                        }

                        if(dataIdx < xEndIdx) {
                            double normalizedValue = (values[dataIdx].value - yMin) / yRange;
                            normalizedValue        = std::clamp(normalizedValue, 0.0, 1.0);
                            int const yPos = static_cast<int>(normalizedValue * (height - 1));
                            output[static_cast<std::size_t>(xPos)]
                              = std::clamp(yPos, 0, height - 1);
                        }
                    }

                    return output;
                };

                auto graph = ftxui::graph(graphFunc) | ftxui::color(Theme::Header::accent());

                if(values.size() < 2) { return graph; }

                auto yLabels = generateYAxisLabels(yMin, yMax, config_.minHeight, info.unit);
                auto xLabels = generateXAxisLabels(values, xStartIdx, visibleDataSize);

                ftxui::Elements yLabelElements;
                for(std::size_t i = 0; i < yLabels.size(); ++i) {
                    yLabelElements.push_back(ftxui::text(yLabels[i])
                                             | ftxui::color(Theme::Text::metadata()));
                    if(i < yLabels.size() - 1) { yLabelElements.push_back(ftxui::filler()); }
                }

                ftxui::Elements xLabelElements;
                for(auto const& label : xLabels) {
                    xLabelElements.push_back(ftxui::text(label)
                                             | ftxui::color(Theme::Text::metadata()));
                }

                auto yAxisColumn = ftxui::vbox(yLabelElements)
                                 | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 8) | ftxui::align_right;

                ftxui::Elements spacedXLabels;
                spacedXLabels.push_back(ftxui::text("")
                                        | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 8));
                for(std::size_t i = 0; i < xLabelElements.size(); ++i) {
                    spacedXLabels.push_back(xLabelElements[i]);
                    if(i < xLabelElements.size() - 1) { spacedXLabels.push_back(ftxui::filler()); }
                }
                auto xAxisRow = ftxui::hbox(spacedXLabels);

                ftxui::Elements const graphRowElements = {yAxisColumn, graph | ftxui::flex};
                ftxui::Elements const graphElements
                  = {ftxui::hbox(graphRowElements) | ftxui::flex,
                     xAxisRow | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 1)};
                return ftxui::vbox(graphElements);
            });
        }

        template<typename MetricDataProvider>
        ftxui::Component createStatsComponent(MetricDataProvider&& dataProvider) {
            return ftxui::Renderer([this,
                                    capturedDataProvider = std::forward<MetricDataProvider>(
                                      dataProvider)]() mutable -> ftxui::Element {
                if(!selectedMetric_) { return ftxui::text(""); }

                auto metricData = capturedDataProvider(*selectedMetric_);
                if(!metricData || (*metricData)->empty()) { return ftxui::text(""); }

                auto const& values = **metricData;
                auto const& info   = *selectedMetric_;

                auto [statsStartIdx, statsEndIdx] = calculateXAxisDataRange(values);
                std::size_t visibleStatsSize      = statsEndIdx - statsStartIdx;

                if(visibleStatsSize == 0) { return ftxui::text(""); }

                double       dataMinVal = values[statsStartIdx].value;
                double       dataMaxVal = values[statsStartIdx].value;
                double const latestVal  = values[statsEndIdx - 1].value;

                for(std::size_t i = statsStartIdx; i < statsEndIdx; ++i) {
                    dataMinVal = std::min(dataMinVal, values[i].value);
                    dataMaxVal = std::max(dataMaxVal, values[i].value);
                }

                auto [yMin, yMax] = calculateYAxisRange(dataMinVal, dataMaxVal);

                ftxui::Elements const titleElements = {
                  ftxui::text("📊 ") | ftxui::color(Theme::Data::icon()),
                  ftxui::text(info.scope) | ftxui::color(Theme::Data::scope()),
                  ftxui::text("::") | ftxui::color(Theme::Text::separator()),
                  ftxui::text(info.name) | ftxui::color(Theme::Data::name()) | ftxui::bold,
                  ftxui::text(info.unit.empty() ? "" : fmt::format(" [{}]", info.unit))
                    | ftxui::color(Theme::Data::unit()),
                };

                ftxui::Elements const statsElements = {
                  ftxui::text("Current: ") | ftxui::bold,
                  ftxui::text(formatMetricValue(latestVal, ""))
                    | ftxui::color(Theme::Data::value()),
                  ftxui::text(" | Range: ") | ftxui::bold,
                  ftxui::text(formatMetricValue(dataMinVal, ""))
                    | ftxui::color(Theme::Status::success()),
                  ftxui::text(" to ") | ftxui::color(Theme::Status::success()),
                  ftxui::text(formatMetricValue(dataMaxVal, ""))
                    | ftxui::color(Theme::Status::success()),
                  ftxui::text(" | Y-Axis: ") | ftxui::bold,
                  ftxui::text(formatMetricValue(yMin, "")) | ftxui::color(Theme::Data::value()),
                  ftxui::text(" to ") | ftxui::color(Theme::Data::value()),
                  ftxui::text(formatMetricValue(yMax, "")) | ftxui::color(Theme::Data::value()),
                  ftxui::text(" | Visible: ") | ftxui::bold,
                  ftxui::text(fmt::format("{}/{}", visibleStatsSize, values.size()))
                    | ftxui::color(Theme::Data::count()),
                };

                ftxui::Elements const allStatsElements
                  = {ftxui::separator(), ftxui::hbox(titleElements), ftxui::hbox(statsElements)};
                return ftxui::vbox(allStatsElements);
            });
        }

        template<typename MetricDataProvider,
                 typename ClearCallback>
        ftxui::Component createComponent(MetricDataProvider& dataProvider,
                                         ClearCallback&&     clearCallback) {
            auto clearButton = ftxui::Button(
              "🗑️ Clear Data",
              std::forward<ClearCallback>(clearCallback),
              createButtonStyle(Theme::Button::Background::danger(), Theme::Button::textOnDark()));

            auto headerRenderer = ftxui::Renderer([]() {
                ftxui::Elements const headerElements
                  = {ftxui::text("📈 Live Metric Plot") | ftxui::bold
                       | ftxui::color(Theme::Header::primary()) | ftxui::center,
                     ftxui::separator()};
                return ftxui::vbox(headerElements);
            });

            auto controls = createControlsComponent();
            auto graph    = createGraphComponent(dataProvider);
            auto stats    = createStatsComponent(dataProvider);

            ftxui::Components const mainContentComponents
              = {headerRenderer, graph | ftxui::flex, stats};
            auto mainContent = ftxui::Container::Vertical(mainContentComponents);

            ftxui::Components const allComponents
              = {clearButton,
                 ftxui::Renderer([]() { return ftxui::separator(); }),
                 controls,
                 ftxui::Renderer([]() { return ftxui::separator(); }),
                 mainContent | ftxui::flex};

            return ftxui::Container::Vertical(allComponents);
        }
    };

    static std::string formatNumber(std::uint32_t num) {
        if(num >= 1000000) { return fmt::format("{}M", num / 1000000); }
        if(num >= 1000) { return fmt::format("{}K", num / 1000); }
        return std::to_string(num);
    }

    static std::string formatBytes(std::uint32_t bytes) {
        if(bytes >= 1073741824) { return fmt::format("{:.1f}GB", bytes / 1073741824.0); }
        if(bytes >= 1048576) { return fmt::format("{:.1f}MB", bytes / 1048576.0); }
        if(bytes >= 1024) { return fmt::format("{:.1f}KB", bytes / 1024.0); }
        return fmt::format("{}B", bytes);
    }

}}   // namespace uc_log::FTXUIGui
