#pragma once

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/metric_utils.hpp"
#include "uc_log/printer/tui/theme.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#ifdef __GNUC__
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wextra-semi"
    #pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#ifdef __clang__
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
    #pragma clang diagnostic ignored "-Wnewline-eof"
    #pragma clang diagnostic ignored "-Wsign-conversion"
#endif
#include <enchantum/enchantum.hpp>
#ifdef __GNUC__
    #pragma GCC diagnostic pop
#endif
#ifdef __clang__
    #pragma clang diagnostic pop
#endif
#include "remote_fmt/fmt_wrapper.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace uc_log::printer { namespace tui {

    namespace GUI_Constants {
        static constexpr std::size_t maxScrollLines = 8192;
        static constexpr std::size_t maxChannels    = 6;
        static constexpr auto        updateInterval = std::chrono::milliseconds{10};
        static constexpr std::size_t maxLogEntries  = 10'000'000;
        // Trims are batched: once maxLogEntries is exceeded, drop down to maxLogEntries -
        // trimSlack so the O(N) front-erase amortizes over many appends.
        static constexpr std::size_t trimSlack            = maxLogEntries / 16;
        static constexpr std::size_t refilterChunk        = 16 * 1024;
        static constexpr auto        filterDebounce       = std::chrono::milliseconds{300};
        static constexpr auto        liveRefilterThrottle = std::chrono::milliseconds{250};
        // metric series are bounded per key so a chatty metric cannot grow without limit
        static constexpr std::size_t maxMetricEntriesPerKey = 100'000;
        // location dropdown labels contain live counts; count-only updates are rebuilt at
        // most this often, new locations immediately
        static constexpr auto locationLabelRefresh = std::chrono::milliseconds{500};
        // duplex byte counters on the settings tab refresh without posted events
        static constexpr auto settingsRefreshInterval = std::chrono::milliseconds{250};
    }   // namespace GUI_Constants

    struct CheckboxOption {
        static CheckboxOption simple() {
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

        std::function<bool()> isChecked = [] { return false; };

        std::function<ftxui::Element(ftxui::EntryState const&)> transform;

        std::function<void()> onChange = [] {};
    };

    class CheckboxBase
      : public ftxui::ComponentBase
      , public CheckboxOption {
    public:
        explicit CheckboxBase(CheckboxOption option) : CheckboxOption(std::move(option)) {}

        ftxui::Element OnRender() override {
            bool const isFocused  = Focused();
            bool const isActive   = Active();
            auto       entryState = ftxui::EntryState{
              .label   = *label,
              .state   = isChecked(),
              .active  = isActive,
              .focused = isFocused || isHovered,
              .index   = -1,
            };
            auto element = (transform ? transform : CheckboxOption::simple().transform)(entryState);
            element |= ftxui::focus;
            element |= reflect(renderBox);
            return element;
        }

        bool OnEvent(ftxui::Event event) override {
            if(!CaptureMouse(event)) { return false; }

            if(event.is_mouse()) { return onMouseEvent(event); }

            isHovered = false;
            if(event == ftxui::Event::Character(' ') || event == ftxui::Event::Return) {
                onChange();
                TakeFocus();
                return true;
            }
            return false;
        }

    private:
        bool onMouseEvent(ftxui::Event event) {
            isHovered = renderBox.Contain(event.mouse().x, event.mouse().y);

            if(!CaptureMouse(event)) { return false; }

            if(!isHovered) { return false; }

            if(event.mouse().button == ftxui::Mouse::Left
               && event.mouse().motion == ftxui::Mouse::Pressed)
            {
                onChange();
                return true;
            }

            return false;
        }

    public:
        [[nodiscard]] bool Focusable() const final { return true; }

    private:
        bool       isHovered = false;
        ftxui::Box renderBox;
    };

    template<typename GetState,
             typename SetState>
    ftxui::Component functionCheckbox(ftxui::ConstStringRef label,
                                      GetState&&            get,
                                      SetState&&            set) {
        auto options      = CheckboxOption::simple();
        options.label     = std::move(label);
        options.isChecked = std::forward<GetState>(get);
        options.onChange  = std::forward<SetState>(set);

        return ftxui::Make<CheckboxBase>(options);
    }

    class ScrollableWithMetadata : public ftxui::Node {
    public:
        ScrollableWithMetadata(ftxui::Element scrollable,
                               ftxui::Element metadata)
          : scrollableContent_(std::move(scrollable))
          , metadataContent_(std::move(metadata)) {}

        [[nodiscard]] ftxui::Element getScrollableContent() const { return scrollableContent_; }

        [[nodiscard]] ftxui::Element getMetadataContent() const { return metadataContent_; }

    private:
        ftxui::Element scrollableContent_;
        ftxui::Element metadataContent_;
    };

    /// The keys every scrolling list of the TUI shares (the log's Scroller, scrollView, pickList): the arrows and j/k
    /// one row, Page Up/Down a page, Home/End the ends. Each list decides what a row and a page are.
    enum class ScrollKey : std::uint8_t { none, up, down, pageUp, pageDown, home, end };

    inline ScrollKey scrollKeyOf(ftxui::Event const& event) {
        if(event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k')) {
            return ScrollKey::up;
        }
        if(event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j')) {
            return ScrollKey::down;
        }
        if(event == ftxui::Event::PageUp) { return ScrollKey::pageUp; }
        if(event == ftxui::Event::PageDown) { return ScrollKey::pageDown; }
        if(event == ftxui::Event::Home) { return ScrollKey::home; }
        if(event == ftxui::Event::End) { return ScrollKey::end; }
        return ScrollKey::none;
    }

    template<typename ContainerGetter, typename Transform>
    class ScrollerBase : public ftxui::ComponentBase {
    public:
        ContainerGetter containerGetter_;
        Transform       transform_;

        ScrollerBase(ContainerGetter&& containerGetter,
                     Transform&&       transformFunc)
          : containerGetter_{containerGetter}
          , transform_{transformFunc} {}

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
                                                      GUI_Constants::maxScrollLines))));

            ftxui::Elements metadataElements;
            metadataElements.reserve(static_cast<std::size_t>(ySpace) + 2U);

            metadataElements.push_back(
              ftxui::text("")
              | ftxui::size(ftxui::HEIGHT,
                            ftxui::EQUAL,
                            static_cast<int>(std::min(static_cast<std::size_t>(hiddenBefore),
                                                      GUI_Constants::maxScrollLines))));

            // Track content widths for actual log lines (not spacers)
            std::vector<int> lineContentWidths;
            lineContentWidths.reserve(static_cast<std::size_t>(ySpace));

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

                // Compute and store content width for this line
                scrollableContent->ComputeRequirement();
                lineContentWidths.push_back(scrollableContent->requirement().min_x);

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
                                               GUI_Constants::maxScrollLines))));

            elements.push_back(
              ftxui::text("")
              | size(ftxui::HEIGHT,
                     ftxui::EQUAL,
                     static_cast<int>(std::min(static_cast<std::size_t>(hiddenBehind),
                                               GUI_Constants::maxScrollLines))));

            ftxui::Element background = ftxui::vbox(elements);
            background->ComputeRequirement();

            ftxui::Element metadataOverlay = ftxui::vbox(metadataElements);
            metadataOverlay->ComputeRequirement();

            struct HorizontallyScrolledRenderer : public ftxui::Node {
            public:
                HorizontallyScrolledRenderer(ftxui::Element   child,
                                             int              offset,
                                             ftxui::Element   fixedOverlay,
                                             int              overlayWidth,
                                             std::vector<int> contentWidths,
                                             int              firstVisibleY)
                  : child_(std::move(child))
                  , offset_(offset)
                  , fixedOverlay_(std::move(fixedOverlay))
                  , overlayWidth_(overlayWidth)
                  , lineContentWidths_(std::move(contentWidths))
                  , firstVisibleY_(firstVisibleY) {}

                void ComputeRequirement() override {
                    child_->ComputeRequirement();
                    requirement_ = child_->requirement();

                    // Only compute overlay requirements when there's metadata
                    if(overlayWidth_ > 0) {
                        fixedOverlay_->ComputeRequirement();
                        auto fixReq = fixedOverlay_->requirement();
                        requirement_.min_x += fixReq.min_x;
                    }
                }

                void SetBox(ftxui::Box box) override {
                    box_                = box;
                    ftxui::Box childBox = box;
                    childBox.x_min -= offset_;
                    childBox.x_max += 10000;
                    child_->SetBox(childBox);

                    // Only set overlay box when there's actual metadata
                    if(overlayWidth_ > 0) {
                        ftxui::Box overlayBox = box;
                        overlayBox.x_min      = box.x_max - overlayWidth_ + 1;
                        overlayBox.x_max      = box.x_max;
                        fixedOverlay_->SetBox(overlayBox);
                    }
                }

                void Render(ftxui::Screen& screen) override {
                    int const spacerHeight
                      = static_cast<int>(std::min(static_cast<std::size_t>(firstVisibleY_),
                                                  GUI_Constants::maxScrollLines));

                    int const contentEdge = box_.x_max - overlayWidth_;
                    int const fullWidth   = contentEdge - box_.x_min;
                    int const indicatorX  = overlayWidth_ > 0 ? (contentEdge - 1) : contentEdge;
                    int const separatorX  = box_.x_max - overlayWidth_;

                    {
                        auto const oldStencil = screen.stencil;
                        screen.stencil.x_max  = std::min(screen.stencil.x_max, contentEdge);
                        child_->Render(screen);
                        screen.stencil = oldStencil;
                    }

                    auto inLimit = [](auto value, auto min, auto max) {
                        return std::clamp<int>(value, min, max) == value;
                    };

                    for(int y = box_.y_min; y <= box_.y_max; ++y) {
                        int const relativeY = y - box_.y_min;
                        if(relativeY < spacerHeight) { continue; }

                        int const lineIndex = relativeY - spacerHeight;

                        if(!inLimit(lineIndex, 0, static_cast<int>(lineContentWidths_.size()) - 1)
                           || !inLimit(y, 0, screen.dimy()))
                        {
                            continue;
                        }

                        if(inLimit(indicatorX, 0, screen.dimx())) {
                            int const contentWidth
                              = lineContentWidths_[static_cast<std::size_t>(lineIndex)];
                            bool const isTruncated
                              = (overlayWidth_ > 0 ? contentWidth - offset_
                                                   : (contentWidth - offset_) - 1)
                              > fullWidth;

                            if(isTruncated) {
                                screen.PixelAt(indicatorX, y).character        = "→";
                                screen.PixelAt(indicatorX, y).foreground_color = ftxui::Color::Red;
                            }
                        }

                        if(inLimit(separatorX, 0, screen.dimx()) && overlayWidth_ > 0) {
                            screen.PixelAt(separatorX, y).character        = "|";
                            screen.PixelAt(separatorX, y).foreground_color = ftxui::Color::GrayDark;
                        }
                    }

                    if(overlayWidth_ > 0) { fixedOverlay_->Render(screen); }
                }

            private:
                ftxui::Element   child_;
                int              offset_;
                ftxui::Element   fixedOverlay_;
                int              overlayWidth_;
                std::vector<int> lineContentWidths_;
                int              firstVisibleY_;
            };

            int const metadataWidth = metadataOverlay->requirement().min_x;

            background
              = std::make_shared<HorizontallyScrolledRenderer>(std::move(background),
                                                               horizontalOffset,
                                                               std::move(metadataOverlay),
                                                               metadataWidth,
                                                               std::move(lineContentWidths),
                                                               hiddenBefore);

            // ftxui's vscroll_indicator derives its thumb from the layout height, which
            // the spacers cap at maxScrollLines: past ~8k lines the thumb froze. This one
            // is computed from the true container metrics, drawn outside the frame.
            struct AccurateScrollIndicator : public ftxui::Node {
                AccurateScrollIndicator(ftxui::Element child,
                                        long           total,
                                        long           first,
                                        long           visible)
                  : ftxui::Node({std::move(child)})
                  , total_{total}
                  , first_{first}
                  , visible_{visible} {}

                void ComputeRequirement() override {
                    children_[0]->ComputeRequirement();
                    requirement_ = children_[0]->requirement();
                    requirement_.min_x += 1;
                }

                void SetBox(ftxui::Box box) override {
                    box_          = box;
                    auto childBox = box;
                    childBox.x_max -= 1;
                    children_[0]->SetBox(childBox);
                }

                void Render(ftxui::Screen& screen) override {
                    children_[0]->Render(screen);
                    int const height = box_.y_max - box_.y_min + 1;
                    if(height <= 0 || total_ <= 0) { return; }
                    int const  x         = box_.x_max;
                    long const cells     = 2L * height;
                    long       thumbSize = std::max(1L, cells * visible_ / total_);
                    long       thumbStart
                      = visible_ >= total_ ? 0L : cells * first_ / std::max(1L, total_);
                    if(visible_ >= total_) { thumbSize = cells; }
                    thumbStart = std::min(thumbStart, cells - thumbSize);
                    for(int y = 0; y < height; ++y) {
                        long const upper    = 2L * y;
                        long const lower    = upper + 1;
                        bool const upperSet = upper >= thumbStart && upper < thumbStart + thumbSize;
                        bool const lowerSet = lower >= thumbStart && lower < thumbStart + thumbSize;
                        auto&      pixel    = screen.PixelAt(x, box_.y_min + y);
                        pixel.character
                          = upperSet ? (lowerSet ? "┃" : "╹") : (lowerSet ? "╻" : " ");
                        pixel.foreground_color = ftxui::Color::GrayDark;
                    }
                }

                long total_;
                long first_;
                long visible_;
            };

            ftxui::Element framed = std::move(background) | ftxui::yframe;
            return std::make_shared<AccurateScrollIndicator>(
                     std::move(framed),
                     static_cast<long>(containerSize),
                     static_cast<long>(hiddenBefore),
                     static_cast<long>(std::min(containerSize, ySpace)))
                 | ftxui::yflex | ftxui::reflect(renderBox);
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
            // the wheel moves the selection like the arrows; the arrows too: unhandled, a Vertical container around
            // the list takes them and moves the focus out of the log instead of the selection
            auto key = scrollKeyOf(event);
            if(event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelUp) {
                key = ScrollKey::up;
            }
            if(event.is_mouse() && event.mouse().button == ftxui::Mouse::WheelDown) {
                key = ScrollKey::down;
            }
            int const page = renderBox.y_max - renderBox.y_min;
            switch(key) {
            case ScrollKey::home:
                stick            = false;
                selectedIndex    = 0;
                horizontalOffset = 0;
                break;
            case ScrollKey::up:
                stick = false;
                selectedIndex--;
                break;
            case ScrollKey::down:     selectedIndex++; break;
            case ScrollKey::pageDown: selectedIndex += page; break;
            case ScrollKey::pageUp:
                stick = false;
                selectedIndex -= page;
                break;
            case ScrollKey::end:
                stick            = true;
                selectedIndex    = containerSize - 1;
                horizontalOffset = 0;
                break;
            case ScrollKey::none: break;
            }
            if(event == ftxui::Event::Character('y') && onYank && containerSize > 0) {
                onYank(static_cast<std::size_t>(
                  std::max(0, std::min(containerSize - 1, selectedIndex))));
                return true;
            }

            if(selectedIndex >= containerSize - 1) { stick = true; }
            selectedIndex = std::max(0, std::min(containerSize - 1, selectedIndex));

            return previousSelected != selectedIndex
                || previousHorizontalOffset != horizontalOffset;
        }

        [[nodiscard]] bool Focusable() const final { return true; }

        // programmatic navigation (e.g. jump-to-time); clamped on the next render
        void jumpTo(int index) {
            stick            = false;
            selectedIndex    = index;
            horizontalOffset = 0;
        }

        // called with the selected index when the user presses 'y'
        std::function<void(std::size_t)> onYank;

        bool       stick            = true;
        int        selectedIndex    = 0;
        int        containerSize    = 0;
        int        horizontalOffset = 0;
        ftxui::Box renderBox;
    };

    // like Scroller() but returns the concrete type so callers can wire jumpTo/onYank
    template<typename ContainerGetter,
             typename Transform>
    auto makeScroller(ContainerGetter&& containerGetter,
                      Transform&&       transformFunc) {
        return std::make_shared<ScrollerBase<ContainerGetter, Transform>>(
          std::forward<ContainerGetter>(containerGetter),
          std::forward<Transform>(transformFunc));
    }

    template<typename ContainerGetter,
             typename Transform>
    ftxui::Component scroller(ContainerGetter&& containerGetter,
                              Transform&&       transformFunc) {
        return ftxui::Make<ScrollerBase<ContainerGetter, Transform>>(
          std::forward<ContainerGetter>(containerGetter),
          std::forward<Transform>(transformFunc));
    }

    // A scrolling view of an element that is built anew every frame (a vbox of text, a table).
    // `Renderer(...) | ftxui::frame` does not scroll such a thing: a frame has no position of
    // its own, it only follows the element marked focus/select inside it, and a Renderer takes
    // no events - the wheel then moves the focus of the container around it instead. This one
    // keeps the first visible row: the wheel moves it while the pointer is over the view, the
    // keys while it has the focus (which it takes only while there is something to scroll).
    class ScrollViewBase : public ftxui::ComponentBase {
    public:
        explicit ScrollViewBase(std::function<ftxui::Element()> render UC_LOG_LIFETIMEBOUND)
          : render_{std::move(render)} {}

        // scroll as little as it takes to bring rows [first, last] of the content into view
        void show(int first,
                  int last) {
            state->showFirst = first;
            state->showLast  = last;
        }

        // the content row under a screen cell, -1 outside the view (as of the last render)
        [[nodiscard]] int rowAt(int x,
                                int y) const {
            return state->box.Contain(x, y) ? y - state->box.y_min + state->offset : -1;
        }

        [[nodiscard]] int columnAt(int x) const { return x - state->box.x_min; }

        [[nodiscard]] int visibleRows() const { return state->box.y_max - state->box.y_min + 1; }

    private:
        struct State {
            int        offset{};      // first visible row, clamped when the box is known
            int        maxOffset{};   // of the last render
            int        showFirst{-1};
            int        showLast{-1};
            ftxui::Box box;
        };

        struct Scrolled : public ftxui::Node {
            Scrolled(ftxui::Element         child,
                     std::shared_ptr<State> shared)
              : ftxui::Node({std::move(child)})
              , state_{std::move(shared)} {}

            void ComputeRequirement() override {
                children_[0]->ComputeRequirement();
                requirement_ = children_[0]->requirement();
            }

            void SetBox(ftxui::Box box) override {
                box_              = box;
                state_->box       = box;
                int const visible = box.y_max - box.y_min + 1;
                int const total   = std::max(requirement_.min_y, visible);
                if(state_->showFirst >= 0) {
                    state_->offset    = std::max(state_->offset, state_->showLast - visible + 1);
                    state_->offset    = std::min(state_->offset, state_->showFirst);
                    state_->showFirst = -1;
                }
                state_->maxOffset = total - visible;
                state_->offset    = std::max(0, std::min(state_->maxOffset, state_->offset));
                auto childBox     = box;
                childBox.y_min    = box.y_min - state_->offset;
                childBox.y_max    = childBox.y_min + total - 1;
                children_[0]->SetBox(childBox);
            }

            void Render(ftxui::Screen& screen) override {
                auto const outer = screen.stencil;
                screen.stencil   = ftxui::Box::Intersection(box_, outer);
                children_[0]->Render(screen);
                screen.stencil = outer;
            }

            std::shared_ptr<State> state_;
        };

    public:
        ftxui::Element OnRender() final {
            return std::make_shared<Scrolled>(render_() | ftxui::vscroll_indicator, state);
        }

        bool OnEvent(ftxui::Event event) final {
            int const before = state->offset;
            int const page   = std::max(1, state->box.y_max - state->box.y_min);
            if(event.is_mouse()) {
                if(!state->box.Contain(event.mouse().x, event.mouse().y)) { return false; }
                if(event.mouse().button == ftxui::Mouse::WheelUp) {
                    state->offset -= wheelRows;
                } else if(event.mouse().button == ftxui::Mouse::WheelDown) {
                    state->offset += wheelRows;
                } else {
                    if(event.mouse().button == ftxui::Mouse::Left
                       && event.mouse().motion == ftxui::Mouse::Pressed)
                    {
                        TakeFocus();
                    }
                    return false;
                }
                state->offset = std::max(0, std::min(state->maxOffset, state->offset));
                // also at either end: unhandled, the container around the view moves its focus
                return true;
            }
            switch(scrollKeyOf(event)) {
            case ScrollKey::up:       --state->offset; break;
            case ScrollKey::down:     ++state->offset; break;
            case ScrollKey::pageUp:   state->offset -= page; break;
            case ScrollKey::pageDown: state->offset += page; break;
            case ScrollKey::home:     state->offset = 0; break;
            case ScrollKey::end:      state->offset = state->maxOffset; break;
            case ScrollKey::none:     break;
            }
            state->offset = std::max(0, std::min(state->maxOffset, state->offset));
            // false at either end: ArrowUp on the first row moves the focus out, as in a menu
            return state->offset != before;
        }

        [[nodiscard]] bool Focusable() const final { return state->maxOffset > 0; }

    private:
        static constexpr int wheelRows = 3;

        std::function<ftxui::Element()> render_;
        std::shared_ptr<State>          state{std::make_shared<State>()};
    };

    inline std::shared_ptr<ScrollViewBase> scrollView(std::function<ftxui::Element()> render) {
        return std::make_shared<ScrollViewBase>(std::move(render));
    }

    // A list one item of which is picked, by key or by mouse: `count()` items of `rowsPerItem`
    // rows each, drawn anew every frame by `row()`, in a scrollView that follows the picked
    // item. A ftxui::Menu cannot be it: its entries are one line of plain text, and a Menu
    // that is not drawn itself (the watch list had one for its keys) never gets a click.
    struct PickListOption {
        struct RowState {
            bool selected{};
            bool hovered{};   // the pointer is on it
            bool focused{};   // the list has the focus
        };

        std::function<int()>                                     count;
        std::function<ftxui::Element(int index, RowState state)> row;
        std::function<ftxui::Element()> empty{};   // drawn while there is no item
        int                             rowsPerItem{1};
        int*                            selected{};   // moved from outside too: the view follows
        // false: the list never takes the focus, its keys come from elsewhere (an input above)
        bool focusable{true};
        // the pointer picks by moving over an item, not only by a click
        bool hoverSelects{false};
        // a left click on an item, after *selected moved there: on which of the item's rows,
        // and the column counted from the view's left edge
        std::function<void(int index, int line, int column)> onClick{};
        std::function<void(int index)>                       onEnter{};   // Return, focused
        std::function<void(int index)> onDelete{};   // Delete / Backspace, focused
    };

    class PickListBase : public ftxui::ComponentBase {
    public:
        explicit PickListBase(PickListOption option)
          : option_{std::move(option)}
          , view_{std::make_shared<ScrollViewBase>([this]() { return content(); })} {}

        [[nodiscard]] int hovered() const { return hovered_; }

        // bring the picked item into view at the next render - for a list whose items changed
        // under a selection that did not
        void showSelected() { shown_ = -1; }

    private:
        [[nodiscard]] int count() const { return std::max(0, option_.count()); }

        ftxui::Element content() {
            int const                   n = count();
            std::vector<ftxui::Element> rows;
            rows.reserve(static_cast<std::size_t>(n));
            PickListOption::RowState state{};
            state.focused = Focused();
            for(int i = 0; i != n; ++i) {
                state.selected = i == *option_.selected;
                state.hovered  = i == hovered_;
                rows.push_back(option_.row(i, state)
                               | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, option_.rowsPerItem));
            }
            return ftxui::vbox(std::move(rows));
        }

    public:
        ftxui::Element OnRender() final {
            int const n = count();
            int&      s = *option_.selected;
            s           = std::max(0, std::min(n - 1, s));
            if(hovered_ >= n) { hovered_ = -1; }
            if(n == 0) {
                shown_ = -1;
                return option_.empty ? option_.empty() : ftxui::emptyElement();
            }
            if(s != shown_) {
                shown_ = s;
                view_->show(option_.rowsPerItem * s, option_.rowsPerItem * (s + 1) - 1);
            }
            return view_->Render();
        }

    private:
        [[nodiscard]] int itemAt(ftxui::Mouse const& mouse) const {
            int const n = count();
            if(n == 0 || shown_ < 0) { return -1; }   // not drawn: the view's box is stale
            int const row = view_->rowAt(mouse.x, mouse.y);
            if(row < 0) { return -1; }
            int const item = row / option_.rowsPerItem;
            return item < n ? item : -1;
        }

    public:
        bool OnEvent(ftxui::Event event) final {
            int& s = *option_.selected;
            if(event.is_mouse()) {
                auto const& mouse = event.mouse();
                hovered_          = itemAt(mouse);
                if(mouse.button == ftxui::Mouse::WheelUp || mouse.button == ftxui::Mouse::WheelDown)
                {
                    if(count() == 0 || shown_ < 0) { return false; }
                    bool const taken = ftxui::Component{view_}->OnEvent(event);
                    hovered_         = itemAt(mouse);
                    return taken;
                }
                if(hovered_ < 0) { return false; }
                if(mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Pressed) {
                    s = hovered_;
                    if(option_.focusable) { TakeFocus(); }
                    if(option_.onClick) {
                        option_.onClick(hovered_,
                                        view_->rowAt(mouse.x, mouse.y) % option_.rowsPerItem,
                                        view_->columnAt(mouse.x));
                    }
                    return true;
                }
                if(option_.hoverSelects && mouse.button == ftxui::Mouse::None) {
                    // the view stays: an item under the pointer is in it already
                    s      = hovered_;
                    shown_ = s;
                }
                return false;
            }
            int const n = count();
            if(n == 0) { return false; }
            int const before = s;
            int const page   = std::max(1, view_->visibleRows() / option_.rowsPerItem - 1);
            switch(scrollKeyOf(event)) {
            case ScrollKey::up:       --s; break;
            case ScrollKey::down:     ++s; break;
            case ScrollKey::pageUp:   s -= page; break;
            case ScrollKey::pageDown: s += page; break;
            case ScrollKey::home:     s = 0; break;
            case ScrollKey::end:      s = n - 1; break;
            case ScrollKey::none:     break;
            }
            if(event == ftxui::Event::Return && option_.onEnter) {
                option_.onEnter(s);
                return true;
            }
            if((event == ftxui::Event::Delete || event == ftxui::Event::Backspace)
               && option_.onDelete)
            {
                option_.onDelete(s);
                return true;
            }
            s = std::max(0, std::min(n - 1, s));
            // false at either end: ArrowUp on the first item moves the focus out, as in a menu
            return s != before;
        }

        [[nodiscard]] bool Focusable() const final { return option_.focusable && count() > 0; }

    private:
        PickListOption                  option_;
        std::shared_ptr<ScrollViewBase> view_;
        int                             hovered_{-1};
        int                             shown_{-1};   // the selection the view was moved to
    };

    inline std::shared_ptr<PickListBase> pickList(PickListOption option) {
        return std::make_shared<PickListBase>(std::move(option));
    }

    inline ftxui::Element toElement(uc_log::printer::LogEntry::Channel const& channel) {
        static std::array<ftxui::Color, 6> const colors{
          {ftxui::Color::Black,
           ftxui::Color::Red,
           ftxui::Color::Blue,
           ftxui::Color::Magenta,
           ftxui::Color::White,
           ftxui::Color::Yellow}
        };

        return ftxui::text(fmt::format("{}", channel.channel))
             | ftxui::color(colors[channel.channel % colors.size()])
             | ((channel.channel == 0) ? ftxui::bgcolor(ftxui::Color::Green) : ftxui::nothing);
    }

    inline ftxui::Element toElement(uc_log::LogLevel const& level) {
        static std::array<std::pair<ftxui::Color, std::string_view>, 6> const lcs{
          {{ftxui::Color::Yellow, "trace"},
           {ftxui::Color::Green, "debug"},
           {ftxui::Color::BlueLight, "info"},
           {ftxui::Color::Magenta, "warn"},
           {ftxui::Color::Red, "error"},
           {ftxui::Color::White, "crit"}}
        };
        static constexpr std::size_t maxLength = 5;   // max of "trace", "debug", "error", ...

        auto index = static_cast<std::size_t>(level);

        if(index >= lcs.size()) { index = 0; }

        auto const& [color, text] = lcs[index];
        return ftxui::text(text) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, maxLength)
             | ftxui::color(color)
             | ((index == 5) ? ftxui::bgcolor(ftxui::Color::RedLight) : ftxui::nothing);
    }

    // O(1) adapter over a prebuilt label vector
    struct StringVectorAdapter : ftxui::ConstStringListRef::Adapter {
        explicit StringVectorAdapter(std::vector<std::string> const& labels_ UC_LOG_LIFETIMEBOUND)
          : labels{labels_} {}

        [[nodiscard]] std::size_t size() const override { return labels.size(); }

        std::string_view operator[](std::size_t index) const override {
            return index < labels.size() ? std::string_view{labels[index]} : std::string_view{};
        }

        std::vector<std::string> const& labels;
    };

    inline ftxui::Element ansiColoredTextToFtxui(std::string_view text) {
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
                    // from_chars instead of stoi: malformed target output must not throw
                    // in the render path, non numeric parameters are skipped
                    auto pushCode = [&codes](std::string_view param) {
                        int code{};
                        auto const [ptr, ec] = std::from_chars(std::to_address(param.begin()),
                                                               std::to_address(param.end()),
                                                               code);
                        if(ec == std::errc{} && ptr == std::to_address(param.end())) {
                            codes.push_back(code);
                        }
                    };
                    std::string current;
                    for(char const character : params) {
                        if(character == ';') {
                            if(!current.empty()) {
                                pushCode(current);
                                current.clear();
                            }
                        } else {
                            current.push_back(character);
                        }
                    }
                    if(!current.empty()) { pushCode(current); }

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
                            } else if(i + 4 < codes.size() && codes[i + 1] == 2) {
                                currentFgColor
                                  = ftxui::Color::RGB(static_cast<uint8_t>(codes[i + 2]),
                                                      static_cast<uint8_t>(codes[i + 3]),
                                                      static_cast<uint8_t>(codes[i + 4]));
                                i += 4;
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
                            } else if(i + 4 < codes.size() && codes[i + 1] == 2) {
                                currentBgColor
                                  = ftxui::Color::RGB(static_cast<uint8_t>(codes[i + 2]),
                                                      static_cast<uint8_t>(codes[i + 3]),
                                                      static_cast<uint8_t>(codes[i + 4]));
                                i += 4;
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

    inline ftxui::ButtonOption createButtonStyle(ftxui::Color bgColor,
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

    // a count for a badge: 999, 12K, 3M - 64 bits, so a count past 4 G does not wrap
    static inline std::string formatCount(std::uint64_t num) {
        if(num >= 1000000) { return fmt::format("{}M", num / 1000000); }
        if(num >= 1000) { return fmt::format("{}K", num / 1000); }
        return std::to_string(num);
    }

    static inline std::string formatBytes(std::uint64_t bytes) {
        auto const b = static_cast<double>(bytes);
        if(bytes >= 1073741824) { return fmt::format("{:.1f}GB", b / 1073741824.0); }
        if(bytes >= 1048576) { return fmt::format("{:.1f}MB", b / 1048576.0); }
        if(bytes >= 1024) { return fmt::format("{:.1f}KB", b / 1024.0); }
        return fmt::format("{}B", bytes);
    }

    static inline std::string
    toTimeStringWithMilliseconds(std::chrono::system_clock::time_point const& value) {
        // 00:00:00.000 in local time. localtime_r instead of std::chrono::zoned_time:
        // libc++ does not implement the chrono tz database
        auto const seconds = std::chrono::duration_cast<std::chrono::seconds>(
          value - std::chrono::time_point_cast<std::chrono::minutes>(value));
        auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
          value - std::chrono::time_point_cast<std::chrono::seconds>(value));
        auto const time = std::chrono::system_clock::to_time_t(value);
        std::tm    local{};
        localtime_r(&time, &local);
        return fmt::format("{:%H:%M}:{:02}.{:03}", local, seconds.count(), milliseconds.count());
    }

}}   // namespace uc_log::printer::tui
