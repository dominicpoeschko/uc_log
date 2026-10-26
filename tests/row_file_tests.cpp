// printer/RowFile.hpp: a log file's rows written with write(2) - short writes continued, a failed write said once with
// errno and the rows lost counted, no row glued to one a failure cut, and the recovery said.
#include "support/Check.hpp"
#include "uc_log/printer/RowFile.hpp"

#include <cerrno>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

// the disk: what write() put there; `room` bytes fit (then `error`), each write takes at most `chunk` bytes
struct Disk {
    std::string data;
    std::size_t room  = static_cast<std::size_t>(-1);
    std::size_t chunk = static_cast<std::size_t>(-1);
    int         error = EDQUOT;

    uc_log::printer::RowFile::WriteF writer() UC_LOG_LIFETIMEBOUND {
        return [this](int, void const* p, std::size_t n) -> ssize_t {
            if(room == 0) {
                errno = error;
                return -1;
            }
            auto const take = std::min({n, chunk, room});
            data.append(static_cast<char const*>(p), take);
            room -= take;
            return static_cast<ssize_t>(take);
        };
    }
};

std::vector<std::string> lines(std::string const& text) {
    std::vector<std::string> out;
    std::size_t              at = 0;
    while(at < text.size()) {
        auto const nl = text.find('\n', at);
        out.push_back(text.substr(at, nl - at));
        at = nl == std::string::npos ? text.size() : nl + 1;
    }
    return out;
}

void cases() {
    {   // short writes: every row whole
        Disk d;
        d.chunk = 7;
        uc_log::printer::RowFile f{d.writer()};
        f.attach(99, "t.rttlog");
        f.row("row one");
        f.row("row two, a little longer\n");
        CHECK(!f.flush(), "short writes: nothing to say");
        CHECK(d.data == "row one\nrow two, a little longer\n",
              "short writes: both rows whole, in order");
    }
    {   // the quota is hit in the middle of a row, then space comes back
        Disk d;
        d.room = 12;   // "row one\n" (8) and 4 bytes of the next
        uc_log::printer::RowFile f{d.writer()};
        f.attach(99, "t.rttlog");
        f.row("row one");
        f.row("row two");
        f.row("row three");
        auto const said = f.flush();
        CHECK(said && said->contains("errno 122") && said->contains("2 rows lost"),
              "quota: said with errno, the broken row and the one after it counted");
        if(said) { std::printf("  %s\n", said->c_str()); }
        f.row("row four");
        CHECK(!f.flush() && f.lostRows() == 3, "still failing: said only once, counted on");
        d.room = static_cast<std::size_t>(-1);
        f.row("row five");
        auto const again = f.flush();
        CHECK(again && again->contains("again: 3 rows lost"),
              "space again: said with the rows lost");
        auto const l = lines(d.data);
        CHECK(l.size() == 3 && l[0] == "row one" && l[1] == "row " && l[2] == "row five",
              "the next row starts on a new line, never glued to the half row");
    }
    {   // ENOSPC as well, and a failure before anything of a batch went out
        Disk d;
        d.room  = 0;
        d.error = ENOSPC;
        uc_log::printer::RowFile f{d.writer()};
        f.attach(99, "t.status.log");
        f.row("a");
        auto const said = f.flush();
        CHECK(said && said->contains("errno 28") && said->contains("1 row lost"),
              "disk full: said");
        d.room = static_cast<std::size_t>(-1);
        f.row("b");
        CHECK(f.flush().has_value() && d.data == "b\n", "nothing half written: no extra newline");
    }
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {"cases", cases},
    });
}
