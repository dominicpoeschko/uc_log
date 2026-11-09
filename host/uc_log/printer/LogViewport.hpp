#pragma once

// Where a log view stands, for any front end: following the newest line, or anchored at a line. A position is a line
// (its multiline group and its place in the group), never an index: the store trims its front and a refilter
// rebuilds the filtered list, and both move every index while the line the view shows stays where it was (the
// nearest one after it, if it went). The view's list is ordered by line id (EntryStore keeps the filtered lines in
// group order), so finding a line is a binary search.
//
// Lines: anything with size() and idAt(index) -> LineId, ordered by id.

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace uc_log::printer {

struct LineId {
    std::size_t   group{};
    std::uint32_t offset{};   // the line's first byte in the group's message

    auto operator<=>(LineId const&) const = default;
};

class LogViewport {
public:
    /// What a view of `rows` rows shows: lines [first, first + count) and the marked one.
    struct Window {
        std::size_t first{};
        std::size_t count{};
        std::optional<std::size_t>
             selected;   // an index in [first, first + count), none while following
        bool follow{true};

        bool operator==(Window const&) const = default;
    };

    [[nodiscard]] bool following() const { return follow_; }

    template<typename Lines>
    [[nodiscard]] Window place(Lines const& lines,
                               std::size_t  rows) const {
        auto const size = lines.size();
        rows            = std::max<std::size_t>(rows, 1);
        if(size == 0) { return {0, 0, std::nullopt, follow_}; }
        if(follow_) {
            auto const count = std::min(size, rows);
            return {size - count, count, std::nullopt, true};
        }
        auto const maxTop = size > rows ? size - rows : 0;
        auto       top    = std::min(indexOf(lines, top_), maxTop);
        auto const sel    = std::min(indexOf(lines, selected_), size - 1);
        // the marked line stays on screen: a view that got smaller (a panel opened) moves to it
        if(sel < top) { top = sel; }
        if(sel >= top + rows) { top = sel + 1 - rows; }
        auto const count = std::min(rows, size - top);
        return {top, count, sel, false};
    }

    /// End: the newest line, and every new one after it.
    void follow() { follow_ = true; }

    /// Up/Down (+-1), PageUp/PageDown (+-rows): the mark moves, the view keeps it on screen. Leaves follow; moving
    /// past the newest line does not follow again (End does).
    template<typename Lines>
    void moveSelection(Lines const& lines,
                       std::size_t  rows,
                       long         delta) {
        auto const size = lines.size();
        if(size == 0) { return; }
        auto const w      = anchor(lines, rows);
        auto const marked = w.selected.value_or(w.first);
        auto const target
          = std::clamp<long>(static_cast<long>(marked) + delta, 0, static_cast<long>(size) - 1);
        auto const sel = static_cast<std::size_t>(target);
        auto       top = w.first;
        if(sel < top) { top = sel; }
        if(sel >= top + std::max<std::size_t>(rows, 1)) {
            top = sel + 1 - std::max<std::size_t>(rows, 1);
        }
        set(lines, top, sel);
    }

    /// The wheel: the view moves by `delta` lines, the mark stays on its line while that is on screen. Scrolling down
    /// to the newest line follows again.
    template<typename Lines>
    void scroll(Lines const& lines,
                std::size_t  rows,
                long         delta) {
        auto const size = lines.size();
        if(size == 0 || (follow_ && delta >= 0)) { return; }
        rows              = std::max<std::size_t>(rows, 1);
        auto const w      = anchor(lines, rows);
        auto const maxTop = size > rows ? size - rows : 0;
        auto const top    = static_cast<std::size_t>(
          std::clamp<long>(static_cast<long>(w.first) + delta, 0, static_cast<long>(maxTop)));
        if(delta > 0 && top == maxTop) {
            follow_ = true;
            return;
        }
        auto const sel
          = std::clamp(w.selected.value_or(w.first), top, std::min(top + rows, size) - 1);
        set(lines, top, sel);
    }

    /// Home: the oldest line.
    template<typename Lines>
    void home(Lines const& lines) {
        if(lines.size() == 0) { return; }
        set(lines, 0, 0);
    }

    /// A click on the row `row` of the view.
    template<typename Lines>
    void selectRow(Lines const& lines,
                   std::size_t  rows,
                   std::size_t  row) {
        auto const w = anchor(lines, rows);
        if(w.count == 0) { return; }
        set(lines, w.first, w.first + std::min(row, w.count - 1));
    }

    /// A line by index (a jump to a target time, a search hit): marked, and on screen with as many lines before it as
    /// fit in half the view.
    template<typename Lines>
    void show(Lines const& lines,
              std::size_t  rows,
              std::size_t  index) {
        auto const size = lines.size();
        if(size == 0) { return; }
        index           = std::min(index, size - 1);
        auto const half = std::max<std::size_t>(rows, 1) / 2;
        set(lines, index > half ? index - half : 0, index);
    }

private:
    bool   follow_{true};
    LineId top_{};
    LineId selected_{};

    // the first line at or after `id`: a line that went leaves the view at the one after it
    template<typename Lines>
    static std::size_t indexOf(Lines const& lines,
                               LineId       id) {
        std::size_t lo = 0;
        std::size_t hi = lines.size();
        while(lo < hi) {
            auto const mid = lo + (hi - lo) / 2;
            if(lines.idAt(mid) < id) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return std::min(lo, lines.size() == 0 ? 0 : lines.size() - 1);
    }

    // the window as it stands, anchored: following becomes "at the end, the newest line marked"
    template<typename Lines>
    Window anchor(Lines const& lines,
                  std::size_t  rows) {
        if(follow_) {
            auto w = place(lines, rows);
            if(w.count == 0) { return w; }
            set(lines, w.first, w.first + w.count - 1);
        }
        return place(lines, rows);
    }

    template<typename Lines>
    void set(Lines const& lines,
             std::size_t  top,
             std::size_t  selected) {
        follow_   = false;
        top_      = lines.idAt(top);
        selected_ = lines.idAt(selected);
    }
};

}   // namespace uc_log::printer
