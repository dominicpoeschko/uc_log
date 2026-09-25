// Any request line: the answer must be one valid Answer line.

#include "../ControlProtocolFixture.hpp"
#include "FuzzChecks.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <variant>

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data,
                                      std::size_t         size) {
    namespace ctl = uc_log::control;
    std::string_view const input{reinterpret_cast<char const*>(data), size};

    // cap a wait's timeout so that one input cannot cost ten minutes
    auto const parsed = ctl::parseRequest(input);
    if(auto const* request = std::get_if<ctl::Request>(&parsed)) {
        if(auto const* wait = std::get_if<ctl::Wait>(request);
           wait != nullptr && wait->timeout_ms > 50 && wait->timeout_ms <= ctl::MaxWaitTimeoutMs)
        {
            auto capped       = *wait;
            capped.timeout_ms = 50;
            uc_log::control_fixture::FakeTarget fake;
            uc_log::fuzz::checkLine<ctl::Answer>(
              ctl::toLine(uc_log::detail::controlAnswer(ctl::Request{capped}, fake.target())));
            return 0;
        }
    }
    uc_log::control_fixture::FakeTarget fake;
    uc_log::fuzz::checkLine<ctl::Answer>(uc_log::detail::controlAnswerLine(input, fake.target()));
    return 0;
}
