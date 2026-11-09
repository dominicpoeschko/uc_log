// The front-end-neutral questions (Questions.hpp): what each one says, and the choices under its field - the answers
// before, a few numbers, the addresses a socket binds, the J-Links on USB, a directory's entries (a tree of its own
// under the temporary directory), the data symbols that match, the log's times. No window, no clock.
#include "support/Check.hpp"
#include "support/CheckFmt.hpp"
#include "uc_log/printer/Questions.hpp"

#include <cstddef>
#include <filesystem>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace uc_log::printer;

namespace {

std::vector<std::string> texts(std::vector<Choice> const& choices) {
    std::vector<std::string> out;
    out.reserve(choices.size());
    for(auto const& c : choices) { out.push_back(c.text); }
    return out;
}

void everyQuestionSaysWhat() {
    for(auto f = static_cast<unsigned>(Field::search);
        f <= static_cast<unsigned>(Field::metricFilter);
        ++f)
    {
        auto const q = questionOf(static_cast<Field>(f), 3);
        CHECK(!q.title.empty() && !q.about.empty() && !q.placeholder.empty(),
              fmt::format("question {}: a title, what it is about, a placeholder", f).c_str());
    }
    CHECK(questionOf(Field::duplexPort, 3).title.contains("channel 3"), "a channel's port: which");
    CHECK(questionOf(Field::watch).firstMarked && !questionOf(Field::probe).firstMarked,
          "the watch's first match is marked (part of a name is no answer); a probe is typed");
}

void recentAndNumbers() {
    std::vector<std::string> const recent{"usb error", "i2c timeout", "usb reset"};
    CHECK_EQ(texts(recentChoices(recent, "USB")),
             (std::vector<std::string>{"usb error", "usb reset"}),
             "the answers before that hold every word, case aside, newest first");
    CHECK_EQ(recentChoices(recent, "").size(), std::size_t{3}, "an empty field: all of them");
    auto const n = numberChoices({1, 5, 10}, 5, "s");
    CHECK(n.size() == 3 && n[1].on && n[1].value == "5" && n[1].text == "5 s" && !n[0].on,
          "a few numbers, the one now ticked");
}

void addresses() {
    auto const c = addressChoices(
      {
        {"192.0.2.5", "eth0"}
    },
      "0.0.0.0");
    CHECK(c.size() == 5 && c[0].value == "127.0.0.1" && c[1].value == "0.0.0.0" && c[1].on
            && !c[0].on,
          "loopback first, every interface next, the one now ticked");
    CHECK(c[4].value == "192.0.2.5" && c[4].detail.contains("eth0"),
          "then this machine's own, with its interface");
    CHECK(isIpAddress("127.0.0.1") && isIpAddress("::1") && isIpAddress("fd00::7"),
          "IPv4 and IPv6 addresses");
    CHECK(!isIpAddress("localhost") && !isIpAddress("1.2.3") && !isIpAddress(""),
          "no host names, nothing half");
    for(auto const& own : localAddresses()) {
        CHECK(isIpAddress(own.address) && own.address != "127.0.0.1" && own.address != "::1",
              fmt::format("this machine's {}: an address, not loopback", own.address).c_str());
    }
}

void probes() {
    TargetPort::Probe usb;
    usb.serialNumber = 123;
    usb.product      = "J-Link";
    usb.nickName     = "left";
    usb.onUsb        = true;
    TargetPort::Probe ip;
    ip.serialNumber = 456;
    ip.product      = "J-Link";
    auto const c    = probeChoices({usb, ip}, "left");
    CHECK_EQ(texts(c),
             (std::vector<std::string>{"any", "J-Link 123 \"left\""}),
             "the only one on USB, then each on USB (none over IP)");
    CHECK(c[1].value == "123" && c[1].on && !c[0].on,
          "by its serial number, ticked by its nickname");
    CHECK(probeChoices({usb}, "")[0].on, "none named: any");
}

// a tree of its own: dir/{a/, b.json, c.txt, .hidden/}
struct Tree {
    std::filesystem::path root;

    Tree()
      : root{std::filesystem::temp_directory_path()
             / fmt::format("uc_log_questions_{}",
                           ::getpid())} {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "dir" / "a");
        std::filesystem::create_directories(root / "dir" / ".hidden");
        std::ofstream{root / "dir" / "b.json"} << "{}";
        std::ofstream{root / "dir" / "c.txt"} << "x";
    }

    Tree(Tree const&)            = delete;
    Tree& operator=(Tree const&) = delete;

    ~Tree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

void paths() {
    Tree const tree;
    auto const dir = (tree.root / "dir").string();
    auto const all = pathChoices(dir + "/", ".json");
    CHECK_EQ(texts(all),
             (std::vector<std::string>{"📁 ..", "📁 a/", "📄 b.json"}),
             "up, the directories, the files that end in .json; none hidden, no other file");
    CHECK(all[0].value == tree.root.string() + "/", "up: the directory above by its own path");
    CHECK(all[0].into && all[1].into && all[1].value == dir + "/a/" && !all[2].into
            && all[2].value == dir + "/b.json",
          "a directory goes into it, a file answers with its path");
    CHECK_EQ(texts(pathChoices(dir, "")),
             (std::vector<std::string>{"📁 ..", "📁 a/"}),
             "a directory without its '/': what is in it; no files asked for");
    CHECK_EQ(texts(pathChoices(dir + "/b", ".json")),
             (std::vector<std::string>{"📄 b.json"}),
             "a part of a name: the entries starting with it");
    CHECK_EQ(texts(pathChoices(dir + "/.h", "")),
             (std::vector<std::string>{"📁 .hidden/"}),
             "the hidden ones for a part starting with a dot");
    CHECK(pathChoices((tree.root / "gone" / "x").string(), "").empty(),
          "a directory that is not there: nothing");
}

void symbolsAndTimes() {
    TargetInspector::Completions c;
    c.best = {
      {  "counter", 0x2000'0010, 4},
      {"count_max", 0x2000'0020, 2}
    };
    c.total      = 2;
    auto const s = symbolChoices(c, {"counter"});
    CHECK(s.size() == 2 && s[0].on && !s[1].on && s[0].value == "counter"
            && s[0].detail == "4 B @0x20000010 · watched",
          "the matches, a watched one ticked, its size and address");
    auto const t = timeChoices({
      {"a", 12.5},
      {"b",  3.0},
      {"c", 1e-6}
    });
    CHECK_EQ(texts(t),
             (std::vector<std::string>{"12.5 s", "3 s", "0.000001 s"}),
             "to the microsecond, without trailing zeros");
    CHECK(t[0].value == "12.5" && t[0].detail == "a", "the value parses as seconds");
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {"everyQuestionSaysWhat", everyQuestionSaysWhat},
                               {     "recentAndNumbers",      recentAndNumbers},
                               {            "addresses",             addresses},
                               {               "probes",                probes},
                               {                "paths",                 paths},
                               {      "symbolsAndTimes",       symbolsAndTimes},
    });
}
