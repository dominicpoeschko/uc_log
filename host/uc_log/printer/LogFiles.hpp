#pragma once

// The printer's files in --log_dir: `<time>.rttlog` (every decoded line, RFC 4180 CSV, printer/LogFormat.hpp) and its
// `<time>.status.log` (the MessageLog's rows). A new directory or a re-enable starts a new pair. Rows go out in
// batches: scripts read the .rttlog while the printer runs, and a line is on disk flushPeriod later at most.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/LogEntry.hpp"
#include "uc_log/printer/LogFormat.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/RowFile.hpp"
#include "uc_log/printer/SocketPortStatus.hpp"

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace uc_log::printer {

class LogFiles {
public:
    using Clock   = printer::Clock;
    using StatusF = std::function<void(LogFileStatus, std::string_view)>;

    static constexpr auto flushPeriod = std::chrono::milliseconds{100};

    /// `status`: the TUI's log-file badge (Active / Error and the path).
    LogFiles(MessageLog& messages UC_LOG_LIFETIMEBOUND,
             StatusF status       UC_LOG_LIFETIMEBOUND,
             std::string const&   dir)
      : messages_{messages}
      , status_{std::move(status)} {
        std::scoped_lock<std::mutex> const lock{mutex_};
        openUnlocked(dir);
    }

    LogFiles(LogFiles const&)            = delete;
    LogFiles& operator=(LogFiles const&) = delete;

    void changeDir(std::string const& dir) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        openUnlocked(dir);
    }

    void setEnabled(bool enabled) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        enabled_ = enabled;
    }

    std::filesystem::path path() const {
        std::scoped_lock<std::mutex> const lock{mutex_};
        return path_;
    }

    void add(std::chrono::system_clock::time_point recvTime,
             printer::LogEntry const&              entry) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        if(!enabled_ || !file_.isOpen()) { return; }   // a file that did not open was said then
        std::ostringstream row;
        printer::logformat::writeEntry(row, recvTime, entry);
        file_.row(row.str());   // written by the flusher, not per line: a syscall per line
    }

    /// What the flusher does every flushPeriod; a failed write is said (printer/RowFile.hpp).
    void flush() {
        std::scoped_lock<std::mutex> const lock{mutex_};
        if(auto const said = file_.flush()) { messages_.error(*said); }
    }

private:
    // under mutex_; MessageLog takes only its own lock, so saying something here is fine
    void openUnlocked(std::string const& dir) {
        if(auto const said = file_.flush()) { messages_.error(*said); }
        path_ = std::filesystem::path{dir}
              / fmt::format("{}.rttlog",
                            printer::logformat::toIso8601Utc(std::chrono::system_clock::now()));
        auto const opened = file_.open(path_);
        auto const why    = opened ? std::string{} : std::generic_category().message(errno);
        auto       status = path_;
        status.replace_extension(".status.log");
        messages_.openStatusFile(status);
        if(!opened) {
            messages_.error(fmt::format("failed to open logfile {:?}: {}", path_.string(), why));
            if(status_) { status_(LogFileStatus::error, path_.string()); }
        } else {
            std::ostringstream header;
            printer::logformat::writeHeader(header);
            file_.row(header.str());
            if(status_) { status_(LogFileStatus::active, path_.string()); }
        }
    }

    MessageLog&           messages_;
    StatusF               status_;
    mutable std::mutex    mutex_;
    std::filesystem::path path_;
    printer::RowFile      file_;
    bool                  enabled_{true};
    // last: it stops (and joins) before the members it flushes go
    Clock::Thread flusher_{[this](std::stop_token const& stop) {
        while(!stop.stop_requested()) {
            Clock::sleep_for(flushPeriod);
            flush();
        }
    }};
};

}   // namespace uc_log::printer
