// Any bytes from RTT, through the printer's event conversions: every event line must be valid.

#include "FuzzChecks.hpp"
#include "uc_log/detail/ControlEvents.hpp"
#include "uc_log/detail/LogEntry.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size) {
    namespace ctl = uc_log::control;
    std::string_view const input{reinterpret_cast<char const*>(data), size};
    auto const             now = std::chrono::system_clock::time_point{};

    uc_log::detail::LogEntry const entry{size % 3, input};
    uc_log::fuzz::checkLine<ctl::Event>(
      ctl::toLine(ctl::Event{uc_log::detail::toLogLine(now, entry)}));
    for(auto const& sample : uc_log::detail::toMetricSamples(now, entry)) {
        uc_log::fuzz::checkLine<ctl::Event>(ctl::toLine(ctl::Event{sample}));
    }
    uc_log::detail::LogEntry plain{0, {}};
    plain.logMsg = std::string{input};
    for(auto const& sample : uc_log::detail::toMetricSamples(now, plain)) {
        uc_log::fuzz::checkLine<ctl::Event>(ctl::toLine(ctl::Event{sample}));
    }
    uc_log::fuzz::checkLine<ctl::Event>(
      ctl::toLine(ctl::Event{uc_log::detail::toStatusMessage(now, "error", input)}));
    return 0;
}
