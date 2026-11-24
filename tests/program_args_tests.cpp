// printer/ProgramArgs.hpp: the command line of uc_log_gui_test and uc_log_rtxui_bench - numbers, flags and positional
// words are told apart, a number given twice counts the last time, and an unknown option or a bad number is refused.
#include "support/Check.hpp"
#include "uc_log/printer/ProgramArgs.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::array<std::string_view, 2> numberNames{"rate", "seed"};
constexpr std::array<std::string_view, 1> flagNames{"format_errors"};

auto parse(std::vector<std::string> words) {
    words.insert(words.begin(), "program");
    std::vector<char*> argv;
    argv.reserve(words.size());
    for(auto& w : words) { argv.push_back(w.data()); }
    return uc_log::printer::parseProgramArgs(static_cast<int>(argv.size()),
                                             argv.data(),
                                             numberNames,
                                             flagNames);
}

void numbersFlagsPositional() {
    auto const none = parse({});
    CHECK(none && !none->number("rate") && !none->flag("format_errors") && none->positional.empty(),
          "nothing given, nothing set");
    auto const all
      = parse({"200", "--rate", "50000", "--format_errors", "60", "--seed", "0", "--rate", "7"});
    CHECK(all.has_value(), "parses");
    if(!all) { return; }
    CHECK(all->number("rate") == 7U, "a number given twice: the last");
    CHECK(all->number("seed") == 0U, "a zero is a number given");
    CHECK(all->flag("format_errors"), "the flag");
    CHECK((all->positional == std::vector<std::string>{"200", "60"}),
          "the positional words, in order");
}

void refusals() {
    CHECK(!parse({"--rates", "1"}), "an option neither list names");
    CHECK(!parse({"--rate"}), "a number missing at the end");
    CHECK(!parse({"--rate", "12x"}), "a number with a tail");
    CHECK(!parse({"--rate", "-1"}), "a negative number");
    CHECK(!parse({"--rate", ""}), "an empty number");
    CHECK(!parse({"--format_errors=1"}), "a flag with a value is no flag");
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {"numbersFlagsPositional", numbersFlagsPositional},
                               {              "refusals",               refusals},
    });
}
