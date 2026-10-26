#pragma once

// A log file written row by row (the .rttlog, the .status.log): rows are collected and written with write(2), and a
// write that fails or comes up short is never silent (an ofstream at a full disk or quota fails without a word and
// leaves half a row that the next one is glued to).
// - a failed write: said once per episode, with errno; the rows that could not be written are counted and dropped
//   (the file cannot hold them; keeping them would grow without bound);
// - a short write that left half a row on disk: the next write starts with a newline, so no row is glued to it;
// - when writes work again: said, with the number of rows lost meanwhile.

#include "uc_log/detail/Lifetimebound.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace uc_log::printer {

class RowFile {
public:
    using WriteF = std::function<ssize_t(int, void const*, std::size_t)>;

    explicit RowFile(WriteF write UC_LOG_LIFETIMEBOUND
                     = [](int         fd,
                          void const* data,
                          std::size_t size) { return ::write(fd, data, size); })
      : write_{std::move(write)} {}

    RowFile(RowFile const&)            = delete;
    RowFile& operator=(RowFile const&) = delete;

    ~RowFile() { close(); }

    /// A new file (truncated); false: it could not be opened (errno).
    bool open(std::filesystem::path const& path) {
        close();
        path_ = path;
        fd_   = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        return fd_ >= 0;
    }

    /// For tests: write to `fd` (not owned).
    void attach(int                   fd,
                std::filesystem::path path) {
        close();
        fd_    = fd;
        owned_ = false;
        path_  = std::move(path);
    }

    [[nodiscard]] bool isOpen() const { return fd_ >= 0; }

    [[nodiscard]] std::filesystem::path const& path() const UC_LOG_LIFETIMEBOUND { return path_; }

    /// One whole row, its newline included; written by the next flush().
    void row(std::string_view text) {
        pending_ += text;
        if(text.empty() || text.back() != '\n') { pending_ += '\n'; }
    }

    /// Writes what was collected. A sentence for the person when something changed: a write failed (with errno and the
    /// rows lost so far) or works again (with the rows lost meanwhile); nullopt otherwise.
    std::optional<std::string> flush() {
        if(pending_.empty() || fd_ < 0) { return std::nullopt; }
        if(midRow_ && !writeAll("\n")) {
            written_ = 0;   // nothing of the rows went out
            return failed();
        }
        midRow_ = false;
        if(!writeAll(pending_)) { return failed(); }
        pending_.clear();
        if(!failing_) { return std::nullopt; }
        failing_        = false;
        auto const lost = std::exchange(lostRows_, 0);
        return fmt::format("writing {} again: {} row{} lost while it failed",
                           path_.string(),
                           lost,
                           lost == 1 ? "" : "s");
    }

    [[nodiscard]] std::size_t lostRows() const { return lostRows_; }

private:
    // the whole of `text` (short writes continued); false at the first failed write - `written_` says how far it got
    bool writeAll(std::string_view text) {
        written_ = 0;
        while(written_ < text.size()) {
            auto const rest = text.substr(written_);
            auto const n    = write_(fd_, rest.data(), rest.size());
            if(n < 0) {
                if(errno == EINTR) { continue; }
                errno_ = errno;
                return false;
            }
            if(n == 0) {
                errno_ = ENOSPC;
                return false;
            }
            written_ += static_cast<std::size_t>(n);
        }
        return true;
    }

    std::optional<std::string> failed() {
        // rows written whole stay; the row the write broke off in is lost too, and the next write starts on a new line
        auto const rest = std::string_view{pending_}.substr(written_);
        auto const rows = static_cast<std::size_t>(std::count(rest.begin(), rest.end(), '\n'));
        midRow_         = midRow_ || (written_ != 0 && pending_[written_ - 1] != '\n');
        lostRows_ += rows;
        pending_.clear();
        if(std::exchange(failing_, true)) { return std::nullopt; }
        return fmt::format(
          "writing {} failed: {} (errno {}) - {} row{} lost so far, the rest of the log goes on in "
          "the "
          "printer",
          path_.string(),
          std::generic_category().message(errno_),
          errno_,
          lostRows_,
          lostRows_ == 1 ? "" : "s");
    }

    void close() {
        if(fd_ >= 0 && owned_) { ::close(fd_); }
        fd_    = -1;
        owned_ = true;
    }

    WriteF                write_;
    int                   fd_{-1};
    bool                  owned_{true};
    std::filesystem::path path_;
    std::string           pending_;
    std::size_t           written_{};
    bool                  midRow_{};
    bool                  failing_{};
    std::size_t           lostRows_{};
    int                   errno_{};
};

}   // namespace uc_log::printer
