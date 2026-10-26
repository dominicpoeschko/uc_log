#pragma once

// The printer's status messages: one `say(level, text)` for the reader, the services and the front ends. Each message
// is echoed to stderr (headless runs), written to the `.status.log`, kept for the control socket's `messages` and
// streamed to its subscribers, counted, kept for the Status tab and handed to the observers (the TUI's redraw, the
// inspector's halt capture, which takes the struct the message comes with: MessageLog::halt).
//
// Lock order: `say` takes the log's own mutex for the file, the history and the stream (stream order = file order)
// and drops it before it calls the observers, so an observer may take any lock of its own (the Gui's mutex). Nobody
// calls `say` while holding a lock an observer takes.

#include "uc_log/printer/ControlEvents.hpp"
#include "uc_log/printer/ControlProtocol.hpp"
#include "uc_log/printer/HaltCapture.hpp"
#include "uc_log/printer/RowFile.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fmt/format.h>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

enum class MessageLevel : std::uint8_t { fatal, error, status, toolError, tool };

/// What a message is, inside the printer: the front ends act on the kind, never on the text (which stays what
/// scripts match).
enum class MessageKind : std::uint8_t {
    plain,
    halt,   // the core halted on its own (or by the TUI's captured halt): HaltCapture::text(), level error
    formatError,   // a log line lost to its format string: the decoder could not render it (remote_fmt
                   // ParseError::format), level error; the Status line's 📝 counts these
};

/// The level as the `.status.log`, the control protocol and the stderr echo spell it - scripts match these.
constexpr std::string_view wireName(MessageLevel level) {
    switch(level) {
    case MessageLevel::fatal:     return "fatal";
    case MessageLevel::error:     return "error";
    case MessageLevel::status:    return "status";
    case MessageLevel::toolError: return "tool error";
    case MessageLevel::tool:      return "tool";
    }
    return "error";
}

/// The kind as the control socket spells it: absent for a plain message.
constexpr std::optional<control::MessageKind> wireKind(MessageKind kind) {
    switch(kind) {
    case MessageKind::plain:       return std::nullopt;
    case MessageKind::halt:        return control::MessageKind::halt;
    case MessageKind::formatError: return control::MessageKind::formatError;
    }
    return std::nullopt;
}

constexpr bool isError(MessageLevel level) {
    return level == MessageLevel::fatal || level == MessageLevel::error
        || level == MessageLevel::toolError;
}

class MessageLog {
public:
    struct Entry {
        MessageLevel                          level;
        std::chrono::system_clock::time_point time;
        std::string                           text;
        MessageKind                           kind{MessageKind::plain};
    };

    /// What an observer is handed; valid during the call only.
    struct Said {
        MessageLevel       level;
        std::string_view   text;
        MessageKind        kind;
        HaltCapture const* halt;   // set for MessageKind::halt
    };

    /// What the Status tab shows (cleared by its Clear button; the format errors also by every flash).
    struct Counts {
        std::size_t errors{};
        std::size_t formatErrors{};
    };

    using Observer = std::function<void(Said const&)>;
    using Stream   = std::function<void(control::StatusMessage const&)>;

    static constexpr std::size_t historyKept = 1000;   // the control socket's `messages`

    MessageLog() = default;

    /// Headless runs (--disable_ui) have no message panel: "[level] text" on stderr instead.
    void setEcho(bool echo) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        echo_ = echo;
    }

    /// Before any thread says something; called outside the log's lock, in the order added.
    void addObserver(Observer observer) { observers_.push_back(std::move(observer)); }

    /// When no other thread says something any more (the front end that observed goes; the printer's threads have
    /// stopped before it).
    void clearObservers() { observers_.clear(); }

    /// The control socket's message stream; called under the log's lock, so its order is the file's.
    void setStream(Stream stream) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        stream_ = std::move(stream);
    }

    /// The `.status.log` next to the `.rttlog` (LogFiles opens both); a file that does not open writes nothing.
    void openStatusFile(std::filesystem::path const& path) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        static_cast<void>(statusFile_.open(path));
    }

    void say(MessageLevel     level,
             std::string_view text) {
        sayAs(Said{level, text, MessageKind::plain, nullptr});
    }

    /// A captured halt: its text as an error, the struct to the observers.
    void halt(HaltCapture const& capture) {
        auto const text = capture.text();
        sayAs(Said{MessageLevel::error, text, MessageKind::halt, &capture});
    }

    /// A log line the decoder could not render (remote_fmt::ParseError::format): an error of its own kind.
    void formatError(std::string_view text) {
        sayAs(Said{MessageLevel::error, text, MessageKind::formatError, nullptr});
    }

    void fatal(std::string_view text) { say(MessageLevel::fatal, text); }

    void error(std::string_view text) { say(MessageLevel::error, text); }

    void status(std::string_view text) { say(MessageLevel::status, text); }

    void tool(std::string_view text) { say(MessageLevel::tool, text); }

    void toolError(std::string_view text) { say(MessageLevel::toolError, text); }

    Counts counts() const {
        std::scoped_lock<std::mutex> const lock{mutex_};
        return counts_;
    }

    /// Every error since the start (the control socket's `status`); the Status tab's Clear does not reset it.
    std::size_t totalErrors() const {
        std::scoped_lock<std::mutex> const lock{mutex_};
        return totalErrors_;
    }

    /// The Status tab's Clear: its entries and counts.
    void clear() {
        std::scoped_lock<std::mutex> const lock{mutex_};
        entries_.clear();
        counts_ = {};
        ++clears_;
    }

    /// A flash ended: its format errors belong to the image before it.
    void resetFormatErrors() {
        std::scoped_lock<std::mutex> const lock{mutex_};
        counts_.formatErrors = 0;
    }

    /// The Status tab's mirror: appends what came since the last call to `out` (and starts over after a clear).
    void copyNew(std::vector<Entry>& out,
                 std::size_t&        seenClears) const {
        std::scoped_lock<std::mutex> const lock{mutex_};
        if(seenClears != clears_) {
            out.clear();
            seenClears = clears_;
        }
        out.insert(out.end(),
                   entries_.begin()
                     + static_cast<std::ptrdiff_t>(std::min(out.size(), entries_.size())),
                   entries_.end());
    }

    /// The newest `count` messages, as the control socket's `messages` answers.
    std::vector<control::StatusMessage> newest(std::size_t count) const {
        std::scoped_lock<std::mutex> const lock{mutex_};
        auto const                         n = std::min(count, history_.size());
        return {history_.end() - static_cast<std::ptrdiff_t>(n), history_.end()};
    }

private:
    void sayAs(Said const& m) {
        auto const now = std::chrono::system_clock::now();
        {
            std::scoped_lock<std::mutex> const lock{mutex_};
            if(echo_) { fmt::print(stderr, "[{}] {}\n", wireName(m.level), m.text); }
            auto message
              = printer::toStatusMessage(now, wireName(m.level), m.text, wireKind(m.kind));
            if(isError(m.level)) { ++totalErrors_; }
            statusFile_.row(printer::statusFileLine(message));
            auto const said = statusFile_.flush();
            keep(std::move(message));
            // the status file's own failure: kept and shown, not written to the file that fails (and not through
            // say, which would come back here)
            if(said) {
                ++totalErrors_;
                std::fprintf(stderr, "uc_log_printer: %s\n", said->c_str());
                keep(printer::toStatusMessage(now, "error", *said));
            }
            if(isError(m.level)) { ++counts_.errors; }
            if(m.kind == MessageKind::formatError) { ++counts_.formatErrors; }
            entries_.push_back(Entry{m.level, now, std::string{m.text}, m.kind});
        }
        for(auto const& o : observers_) { o(m); }
    }

    // under mutex_
    void keep(control::StatusMessage message) {
        if(stream_) { stream_(message); }
        history_.push_back(std::move(message));
        if(history_.size() > historyKept) { history_.pop_front(); }
    }

    std::vector<Observer> observers_;

    mutable std::mutex                 mutex_;
    bool                               echo_{};
    Stream                             stream_;
    printer::RowFile                   statusFile_;
    std::deque<control::StatusMessage> history_;
    std::size_t                        totalErrors_{};
    Counts                             counts_{};
    std::vector<Entry>                 entries_;
    std::size_t                        clears_{};
};

}   // namespace uc_log::printer
