// Unit test for uc_log::printer::buildRttChannelMap: which RTT up buffers are log channels (each logged as its up
// index), and the pairing of duplex channels by name.
#include "support/Check.hpp"
#include "uc_log/printer/RttChannelMap.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

    [[nodiscard]] Status status() const {
        return {static_cast<int>(up.size()), static_cast<int>(down.size())};
    }

    [[nodiscard]] std::optional<BufferDesc> rttBufferDesc(bool          isDown,
                                                          std::uint32_t index) const {
        auto const& name = isDown ? down.at(index) : up.at(index);
        return name ? std::optional{BufferDesc{*name}} : std::nullopt;
    }
};

struct Result {
    uc_log::printer::RttChannelMap map;
    std::vector<std::string>       errors;
};

namespace {

Result build(FakeJLink const& jlink) {
    Result r;
    r.map = uc_log::printer::buildRttChannelMap(
      jlink,
      jlink.status(),
      [](std::string_view) {},
      [&r](std::string_view e) { r.errors.emplace_back(e); });
    return r;
}

// the up buffer of every log channel, in channel order; the channel is its up index
bool logChannelsAre(Result const&              r,
                    std::vector<std::uint32_t> want) {
    if(r.map.logChannels.size() != want.size()) { return false; }
    for(std::size_t i = 0; i != want.size(); ++i) {
        if(r.map.logChannels[i].upIndex != want[i]) { return false; }
    }
    return true;
}

void inOrder() {
    auto const r = build({
      {"uc_log0", "uc_log1", "uc_log2", "uc_log3"},
      {}
    });
    CHECK(logChannelsAre(r, {0, 1, 2, 3}), "channel = buffer");
    CHECK(r.errors.empty(), "nothing to say");
}

void namesDoNotNumber() {
    auto const r = build({
      {"uc_log1", "Terminal", "uc_log1"},
      {}
    });
    CHECK(logChannelsAre(r, {0, 1, 2}), "the buffer names do not number the channels");
    CHECK(r.errors.empty(), "names: nothing to say");
}

void namesUnavailable() {
    auto const r = build({
      {std::nullopt, std::nullopt},
      {}
    });
    CHECK(logChannelsAre(r, {0, 1}), "no names: positional");
    CHECK(r.errors.empty(), "no names: nothing to say");
}

void withDuplex() {
    auto const r = build({
      {"uc_log0", "uc_log1", "tui"},
      {"tui"}
    });
    CHECK(logChannelsAre(r, {0, 1}), "duplex: the log channels are the unpaired buffers");
    CHECK(r.map.duplexChannels.size() == 1 && r.map.duplexChannels[0].upIndex == 2U
            && r.map.duplexChannels[0].downIndex == 0U,
          "duplex: paired by name");
    CHECK(r.errors.empty(), "duplex in order: nothing to say");
}

}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {         "inOrder",          inOrder},
                               {"namesDoNotNumber", namesDoNotNumber},
                               {"namesUnavailable", namesUnavailable},
                               {      "withDuplex",       withDuplex},
    });
}
