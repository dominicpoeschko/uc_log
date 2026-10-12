// uc_log_log_stress_check: checks a stress run (uc_log/stress/Records.hpp) from what the printer wrote.
//
//   uc_log_log_stress_check <file>...
//       each argument a .rttlog or a .status.log; the .rttlogs in the order given (oldest first: the printer names
//       them by their start time). Exit 0 = no anomaly, 1 = anomalies (the report says which), 2 = usage.
//
//   uc_log_log_stress_check [--floor <level>] ...
//       a levels variant (Records.hpp levelOf): its compile-time floor; shapes below it are not expected
//
//   uc_log_log_stress_check --follow <log dir> [--since <file name>]
//       reads the printer's files as they grow - every .rttlog / .status.log whose name sorts at or after --since,
//       oldest first, a newer file once the current one has ended - and prints one line per event:
//       `ANOMALY <what>` at once, `QUIESCED <summary>` after each quiesced boot's accounting. Runs until killed
//       (SIGINT/SIGTERM: prints the report and exits with the verdict).

#include "LogStressCheck.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__clang__)
    #pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

namespace {

std::atomic<bool> stopRequested{false};

void onSignal(int) { stopRequested.store(true); }

void rttlogLine(uc_log::stress::Checker& checker,
                std::string const&       line) {
    if(line.starts_with("recv_time_utc,")) { return; }   // a header
    auto const f = uc_log::stress::csvFields(line);
    if(f.size() != 9) {
        checker.status(fmt::format("malformed .rttlog row ({} columns): {}", f.size(), line));
        return;
    }
    unsigned channel = 0;
    std::from_chars(f[1].data(), f[1].data() + f[1].size(), channel);
    checker.row({channel, f[7], f[8], f[4], f[5]});
}

void statusLine(uc_log::stress::Checker& checker,
                std::string const&       line) {
    if(line.find("[tool]") != std::string::npos) {
        return;
    }   // "[tool error]" goes on: counted, not an anomaly
    checker.status(line);
}

std::optional<unsigned> floorArg;   // --floor <level name>: the levels variant

int files(int    argc,
          char** argv) {
    uc_log::stress::Checker checker;
    checker.floor = floorArg;
    for(int i = 1; i < argc; ++i) {
        std::string_view const path{argv[i]};
        std::ifstream          in{std::string{path}};
        if(!in) {
            std::fprintf(stderr, "cannot read %s\n", argv[i]);
            return 2;
        }
        bool const  status = path.ends_with(".status.log");
        std::string line;
        while(std::getline(in, line)) {
            if(status) {
                statusLine(checker, line);
            } else {
                rttlogLine(checker, line);
            }
        }
    }
    std::fputs(checker.report().c_str(), stdout);
    return checker.anomalies() == 0 ? 0 : 1;
}

// One growing file: complete lines only (a line being written stays until its newline arrives).
struct Tail {
    std::filesystem::path path;
    std::ifstream         in;
    std::string           partial;

    explicit Tail(std::filesystem::path p)
      : path{std::move(p)}
      , in{path} {}

    template<typename F>
    bool pump(F&& f) {
        bool        any = false;
        std::string chunk(1 << 16, '\0');
        while(true) {
            in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            auto const n = in.gcount();
            if(n <= 0) { break; }
            any = true;
            partial.append(chunk.data(), static_cast<std::size_t>(n));
            std::size_t start = 0;
            while(true) {
                auto const nl = partial.find('\n', start);
                if(nl == std::string::npos) { break; }
                f(partial.substr(start, nl - start));
                start = nl + 1;
            }
            partial.erase(0, start);
        }
        in.clear();   // EOF for now: the printer may write more
        return any;
    }
};

int follow(std::filesystem::path const& dir,
           std::string const&           since) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    uc_log::stress::Checker checker;
    checker.floor     = floorArg;
    checker.onAnomaly = [](std::string_view what) {
        std::printf("ANOMALY %.*s\n", static_cast<int>(what.size()), what.data());
        std::fflush(stdout);
    };
    checker.onQuiesced = [&] {
        std::printf("QUIESCED %s\n", checker.summary().c_str());
        std::fflush(stdout);
    };

    auto const list = [&](std::string_view suffix) {
        std::vector<std::filesystem::path> out;
        for(auto const& e : std::filesystem::directory_iterator{dir}) {
            auto const name = e.path().filename().string();
            if(name.ends_with(suffix) && name >= since) { out.push_back(e.path()); }
        }
        std::ranges::sort(out);
        return out;
    };

    std::optional<Tail> rtt;
    std::optional<Tail> status;
    while(!stopRequested.load()) {
        bool progress = false;
        for(auto const& [suffix, tail, isStatus] :
            {
              std::tuple{    std::string_view{".rttlog"},    &rtt, false},
              std::tuple{std::string_view{".status.log"}, &status,  true}
        })
        {
            auto const files = list(suffix);
            if(!*tail && !files.empty()) { tail->emplace(files.front()); }
            while(*tail) {
                bool const got = (*tail)->pump([&](std::string const& line) {
                    if(isStatus) {
                        statusLine(checker, line);
                    } else {
                        rttlogLine(checker, line);
                    }
                });
                progress       = progress || got;
                // on to the next file once this one has nothing more and a newer one exists
                auto const next = std::ranges::upper_bound(files, (*tail)->path);
                if(got || next == files.end()) { break; }
                tail->emplace(*next);
                progress = true;
            }
        }
        if(!progress) { std::this_thread::sleep_for(std::chrono::milliseconds{200}); }
    }
    std::fputs(checker.report().c_str(), stdout);
    std::fflush(stdout);
    return checker.anomalies() == 0 ? 0 : 1;
}

}   // namespace

int main(int    argc,
         char** argv) {
    if(argc >= 3 && std::string_view{argv[1]} == "--floor") {
        for(unsigned l = 0; l != uc_log::stress::LevelNames.size(); ++l) {
            if(uc_log::stress::LevelNames[l] == argv[2]) { floorArg = l; }
        }
        if(!floorArg) {
            std::fprintf(stderr, "unknown level %s\n", argv[2]);
            return 2;
        }
        argv += 2;
        argc -= 2;
    }
    if(argc >= 3 && std::string_view{argv[1]} == "--follow") {
        std::string since;
        if(argc >= 5 && std::string_view{argv[3]} == "--since") { since = argv[4]; }
        return follow(argv[2], since);
    }
    if(argc < 2) {
        std::fputs(
          "usage: uc_log_log_stress_check <.rttlog|.status.log>... | --follow <dir> [--since "
          "<name>]\n",
          stderr);
        return 2;
    }
    return files(argc, argv);
}
