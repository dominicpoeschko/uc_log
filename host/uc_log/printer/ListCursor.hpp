#pragma once

// A list's marked row and the first row shown, for a list with more rows than room (a dialog's), for any front end:
// the mark moves by steps, the shown rows follow it. A list under a field (a question's choices) can be left upwards:
// then no row is marked - the field is. Plain arithmetic: rows by their place, the caller keeps them in place.

#include <algorithm>
#include <cstddef>
#include <optional>

namespace uc_log::printer {

struct ListCursor {
    std::optional<std::size_t> marked;   // none: no row (the field above the list)
    std::size_t                top{0};   // the first row shown

    /// From the start: the first row marked - or, `field`, none.
    void reset(bool field) {
        marked = field ? std::nullopt : std::optional<std::size_t>{0};
        top    = 0;
    }

    /// `step` rows on (back when negative) within `size` rows, the ends held; `field`: up from the first row is the
    /// field.
    void move(long        step,
              std::size_t size,
              bool        field) {
        if(size == 0) {
            marked.reset();
            return;
        }
        long const at   = marked ? static_cast<long>(*marked) : -1;
        long const next = std::clamp(at + step, field ? -1L : 0L, static_cast<long>(size) - 1);
        marked
          = next < 0 ? std::nullopt : std::optional<std::size_t>{static_cast<std::size_t>(next)};
    }

    /// The mark within `size` rows (the last when it was past them), the `shown` rows from `top` holding it and never
    /// running past the last row: a window that grows, or a list that shrinks, shows more rows above instead - rows
    /// [top, top + shown) all exist when `shown` <= `size`.
    void fit(std::size_t size,
             std::size_t shown) {
        if(marked && *marked >= size) {
            marked = size == 0 ? std::nullopt : std::optional<std::size_t>{size - 1};
        }
        top = std::min(top, size > shown ? size - shown : 0);
        if(marked) {
            top = std::min(top, *marked);
            if(*marked >= top + shown) { top = *marked + 1 - shown; }
        }
    }
};

}   // namespace uc_log::printer
