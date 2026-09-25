#pragma once

#include "uc_log/detail/ControlProtocol.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>

/// Numbered log lines, oldest dropped past a byte limit. Not thread safe.
namespace uc_log::detail {

class ControlHistory {
public:
    static constexpr std::size_t DefaultLimitBytes = 128 * 1024 * 1024;

    struct Chunk {
        std::string   text;   // protocol lines, each with its '\n'
        std::uint64_t next{};
        std::uint64_t lost{};   // lines between the cursor and the oldest one held
        bool          atEnd{};
    };

    explicit ControlHistory(std::size_t limitBytes = DefaultLimitBytes) : limit_{limitBytes} {}

    /// Numbers `line` and keeps it; the protocol line to send live.
    std::string const& append(control::LogLine& line) {
        line.seq = firstSeq_ + entries_.size();
        entries_.push_back(Entry{.level  = line.level,
                                 .module = line.module,
                                 .line   = control::toLine(control::Event{line})});
        bytes_ += cost(entries_.back());
        while(bytes_ > limit_ && entries_.size() > 1) { dropOldest(); }
        return entries_.back().line;
    }

    void setLimit(std::size_t bytes) {
        limit_ = bytes;
        while(bytes_ > limit_ && entries_.size() > 1) { dropOldest(); }
    }

    std::uint64_t nextSeq() const { return firstSeq_ + entries_.size(); }

    std::uint64_t firstSeq() const { return firstSeq_; }

    std::size_t bytes() const { return bytes_; }

    std::uint64_t start(control::Subscribe const& s) const {
        if(s.since_seq) { return std::min(*s.since_seq, nextSeq()); }
        if(!s.last) { return nextSeq(); }
        std::uint32_t found = 0;
        std::size_t   i     = entries_.size();
        while(i != 0 && found != *s.last) {
            --i;
            if(passes(s, entries_[i])) { ++found; }
        }
        return firstSeq_ + (found == *s.last ? i : 0);
    }

    /// The lines from `cursor` on that pass `s`, up to `maxBytes` but at least one.
    Chunk read(control::Subscribe const& s,
               std::uint64_t             cursor,
               std::size_t               maxBytes) const {
        Chunk out;
        if(cursor < firstSeq_) {
            out.lost = firstSeq_ - cursor;
            cursor   = firstSeq_;
        }
        for(auto i = cursor - firstSeq_; i < entries_.size(); ++i, ++cursor) {
            auto const& e = entries_[i];
            if(!passes(s, e)) { continue; }
            if(!out.text.empty() && out.text.size() + e.line.size() > maxBytes) { break; }
            out.text += e.line;
        }
        out.next  = cursor;
        out.atEnd = cursor == nextSeq();
        return out;
    }

private:
    struct Entry {
        control::Level level{};
        std::string    module;
        std::string    line;
    };

    std::deque<Entry> entries_;
    std::uint64_t     firstSeq_{0};
    std::size_t       bytes_{0};
    std::size_t       limit_;

    static std::size_t cost(Entry const& e) {
        return sizeof(Entry) + e.module.capacity() + e.line.capacity();
    }

    static bool passes(control::Subscribe const& s,
                       Entry const&              e) {
        return control::passes(s, e.level, e.module);
    }

    void dropOldest() {
        bytes_ -= cost(entries_.front());
        entries_.pop_front();
        ++firstSeq_;
    }
};

}   // namespace uc_log::detail
