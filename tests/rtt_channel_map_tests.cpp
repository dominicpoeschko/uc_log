// Unit test for uc_log::detail::buildRttChannelMap: which RTT up buffer each log channel is read
// from and which channel it is logged as, and the pairing of duplex channels by name.
#include "uc_log/detail/RttChannelMap.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

static int failures = 0;

#define CHECK(cond, msg)                                        \
    do {                                                        \
        if(!(cond)) {                                           \
            std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
            ++failures;                                         \
        }                                                       \
    } while(0)

// the J-Link as far as the map asks it: buffer counts and names (nullopt: a DLL without them)
struct FakeJLink {
    struct Status {
        int numUpBuffers{};
        int numDownBuffers{};
    };

    struct BufferDesc {
        std::string name;
    };

    std::vector<std::optional<std::string>> up;
    std::vector<std::optional<std::string>> down;

    Status status() const { return {static_cast<int>(up.size()), static_cast<int>(down.size())}; }

    std::optional<BufferDesc> rttBufferDesc(bool          isDown,
                                            std::uint32_t index) const {
        auto const& name = isDown ? down.at(index) : up.at(index);
        return name ? std::optional{BufferDesc{*name}} : std::nullopt;
    }
};

struct Result {
    uc_log::detail::RttChannelMap map;
    std::vector<std::string>      errors;
};

static Result build(FakeJLink const& jlink) {
    Result r;
    r.map = uc_log::detail::buildRttChannelMap(
      jlink,
      jlink.status(),
      [](std::string_view) {},
      [&r](std::string_view e) { r.errors.emplace_back(e); });
    return r;
}

// (upIndex, channel) of every log channel, in channel order
static bool logChannelsAre(Result const&                         r,
                           std::vector<std::pair<std::uint32_t,
                                                 std::uint32_t>> want) {
    if(r.map.logChannels.size() != want.size()) { return false; }
    for(std::size_t i = 0; i != want.size(); ++i) {
        if(r.map.logChannels[i].upIndex != want[i].first
           || r.map.logChannels[i].channel != want[i].second)
        {
            return false;
        }
    }
    return true;
}

static void inOrder() {
    auto const r = build({
      {"uc_log0", "uc_log1", "uc_log2", "uc_log3"},
      {}
    });
    CHECK(logChannelsAre(r,
                         {
                           {0, 0},
                           {1, 1},
                           {2, 2},
                           {3, 3}
    }),
          "names in order: channel = buffer");
    CHECK(r.errors.empty(), "names in order: nothing to say");
}

static void reversedByOldLibstdcxxLayout() {
    // buffers in reverse, as a libstdc++ build of the old rtt.hpp had them
    auto const r = build({
      {"uc_log3", "uc_log2", "uc_log1", "uc_log0"},
      {}
    });
    CHECK(logChannelsAre(r,
                         {
                           {3, 0},
                           {2, 1},
                           {1, 2},
                           {0, 3}
    }),
          "reversed buffers: each channel read from the buffer that carries its name");
    CHECK(r.errors.size() == 1 && r.errors[0].find("uc_log0 in buffer 3") != std::string::npos,
          "reversed buffers: told once, with the order found");
}

static void namesUnavailable() {
    auto const r = build({
      {std::nullopt, std::nullopt},
      {}
    });
    CHECK(logChannelsAre(r,
                         {
                           {0, 0},
                           {1, 1}
    }),
          "no names: positional");
    CHECK(r.errors.empty(), "no names: nothing to say");
}

static void namesNotUsable() {
    auto const duplicate = build({
      {"uc_log0", "uc_log0"},
      {}
    });
    CHECK(logChannelsAre(duplicate,
                         {
                           {0, 0},
                           {1, 1}
    }),
          "a name twice: positional");
    auto const foreign = build({
      {"uc_log1", "Terminal"},
      {}
    });
    CHECK(logChannelsAre(foreign,
                         {
                           {0, 0},
                           {1, 1}
    }),
          "a name that is no uc_logN: positional");
    auto const bare = build({{"uc_log"}, {}});
    CHECK(logChannelsAre(bare,
                         {
                           {0, 0}
    }),
          "uc_log without a number: positional");
}

static void withDuplex() {
    auto const r = build({
      {"uc_log0", "uc_log1", "tui"},
      {"tui"}
    });
    CHECK(logChannelsAre(r,
                         {
                           {0, 0},
                           {1, 1}
    }),
          "duplex: the log channels by name");
    CHECK(r.map.duplexChannels.size() == 1 && r.map.duplexChannels[0].upIndex == 2u
            && r.map.duplexChannels[0].downIndex == 0u,
          "duplex: paired by name");
    CHECK(r.errors.empty(), "duplex in order: nothing to say");
}

int main() {
    inOrder();
    reversedByOldLibstdcxxLayout();
    namesUnavailable();
    namesNotUsable();
    withDuplex();
    if(failures == 0) { std::puts("all checks passed"); }
    return failures == 0 ? 0 : 1;
}
