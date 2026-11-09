// The RTXUI window under a monkey, drawn headless (support/RtxWindow.hpp): thousands of left and right clicks, drags,
// wheel turns, keys and resizes - from 20 x 8 to 284 x 87 - picked by a seeded generator, on every tab, while lines,
// modules and a hundred metrics keep coming. After each the window draws; it must never read past the end of what it
// shows (a page's rows, a list scrolled away while its window grew). Here a slip is at least a crash; in the ASan
// and checked-library builds (CI) any read past a vector fails the test. The seeds are fixed: a failure repeats.
// It runs in a directory of its own: what the monkey types into a path question lands there.
#include "support/Check.hpp"
#include "support/RtxWindow.hpp"
#include "uc_log/detail/Lifetimebound.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

using namespace uc_log::test::rtx_window;

namespace {

// the window's sizes the monkey switches between: tiny, a laptop's, a big terminal's
constexpr std::array<std::pair<int, int>, 6> sizes{
  {{20, 8}, {40, 12}, {80, 24}, {100, 40}, {160, 45}, {284, 87}}
};

// what the monkey presses: the tabs, moving, Enter, Escape, Tab, the commands, a few letters - never q
constexpr std::array<std::string_view, 37> keys{
  "1",      "2",      "3",      "4",      "5",       "6",       "7",      "8",      "9",  "0",
  "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D", "\x1b[5~", "\x1b[6~", "\x1b[H", "\x1b[F", "\r", "\x1b",
  "\t",     " ",      ":",      "/",      "|",       "z",       "d",      "y",      "s",  "o",
  "?",      "-",      "+",      "M",      "G",       "m5",      "a"};

// a directory of its own for the run, gone after it
struct OwnDirectory {
    std::filesystem::path before;
    std::filesystem::path dir;

    OwnDirectory()
      : before{std::filesystem::current_path()}
      , dir{std::filesystem::temp_directory_path()
            / fmt::format("uc_log_monkey_{}",
                          ::getpid())} {
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);
    }

    OwnDirectory(OwnDirectory const&)            = delete;
    OwnDirectory& operator=(OwnDirectory const&) = delete;

    ~OwnDirectory() {
        std::error_code ec;
        std::filesystem::current_path(before, ec);
        std::filesystem::remove_all(dir, ec);
    }
};

class Monkey {
public:
    Monkey(Window& w     UC_LOG_LIFETIMEBOUND,
           std::uint32_t seed)
      : w_{w}
      , gen_{seed} {}

    // `steps` things done, the target going on between them; how many times the window drew
    std::size_t run(std::size_t steps) {
        std::size_t drawn = 0;
        for(std::size_t i = 0; i < steps; ++i) {
            feed(i);
            act();
            // a question that would write a file (the filter saved under a typed name): closed, not answered
            if(w_.app->asking() == Field::saveFilter) { w_.screen->input("\x1b"); }
            w_.frame(i % 7 == 0 ? 3 : 0);
            w_.screen->input("");   // drawn
            ++drawn;
        }
        return drawn;
    }

private:
    Window&      w_;
    std::mt19937 gen_;
    int          width_{80};
    int          height_{12};

    int pick(int n) { return std::uniform_int_distribution<int>{0, n - 1}(gen_); }

    // the target goes on: a line of a module, a value of one of a hundred metrics in ten scopes
    void feed(std::size_t i) {
        auto const at
          = std::chrono::system_clock::time_point{} + std::chrono::milliseconds{100 * i};
        auto e   = entry(fmt::format("tick {}", i), std::chrono::milliseconds{100 * i});
        e.module = fmt::format("area{}.part{}", i % 4, i % 10);
        w_.store.addEntry(at, e);
        auto const m = i % 100;
        w_.add(fmt::format("value @METRIC(group{}::m{}[V]={})",
                           m / 10,
                           m % 10,
                           static_cast<double>((i * 37) % 200) - 100.0),
               at);
    }

    void act() {
        int const x = pick(width_);
        int const y = pick(height_);
        switch(pick(12)) {
        case 0:
        case 1:
        case 2:
        case 3: w_.screen->click(x, y); break;
        case 4: w_.screen->input(sgrMouse(2, x, y) + sgrMouse(2, x, y, true)); break;   // right
        case 5: w_.screen->input(sgrMouse(pick(2) == 0 ? 64 : 65, x, y)); break;        // the wheel
        case 6:
            {
                // a drag: pressed here, moved, let go there (a panel's title, a scrollbar, anything)
                int const tx = pick(width_);
                int const ty = pick(height_);
                w_.screen->input(sgrMouse(0, x, y) + sgrMouse(32, tx, ty)
                                 + sgrMouse(0, tx, ty, true));
                break;
            }
        case 7:
        case 8:
        case 9: w_.screen->input(keys[static_cast<std::size_t>(pick(keys.size()))]); break;
        case 10:
            {
                // a new size now and then: the page's rows grow and shrink under what it showed
                if(pick(4) != 0) { break; }
                auto const [wd, ht] = sizes[static_cast<std::size_t>(pick(sizes.size()))];
                width_              = wd;
                height_             = ht;
                w_.resize(wd, ht);
                break;
            }
        default: w_.screen->input(sgrMouse(35, x, y)); break;   // a move: hover
        }
    }
};

void monkeyRun(std::uint32_t seed,
               std::size_t   steps) {
    OwnDirectory const here;
    Window             w{50};
    w.resize(100, 40);
    Monkey     monkey{w, seed};
    auto const drawn = monkey.run(steps);
    CHECK(drawn == steps,
          fmt::format("seed {}: drawn after every one of {} steps", seed, steps).c_str());
    w.resize(100, 40);
    CHECK(contains(w.text(), "LOGS"), fmt::format("seed {}: the window still draws", seed).c_str());
}

// three seeds, each a different mix of sizes, tabs and clicks
void monkeyA() { monkeyRun(1, 1500); }

void monkeyB() { monkeyRun(20261105, 1500); }

void monkeyC() { monkeyRun(424242, 1500); }

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {"monkeyA", monkeyA},
                               {"monkeyB", monkeyB},
                               {"monkeyC", monkeyC},
    });
}
