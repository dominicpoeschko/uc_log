// The rtt_reader tests: the reader loop against the fake J-Link (support/FakeJLink.hpp), one file of cases per concern
// (rtt_reader/<concern>_tests.hpp), all in this one binary. Each case header is included only here, so the headers and
// the reader on the fake are compiled once and linked once: a binary per concern parsed the same ~34 G instructions of
// headers six times and linked six times, x4.7 the cost of one.
#include "support/Check.hpp"
#include "support/FakeJLink.hpp"
#include "uc_log/printer/JLinkRttReaderImpl.hpp"

// the reader's members for the fake, every one of them (the printer instantiates the J-Link's in ReaderJLink.cpp)
template struct BasicJLinkRttReader<uc_log_test_rtt_reader::FakeTransport>;

#include "rtt_reader/boots_tests.hpp"
#include "rtt_reader/decode_tests.hpp"
#include "rtt_reader/flash_gate_tests.hpp"
#include "rtt_reader/recovery_tests.hpp"
#include "rtt_reader/resets_tests.hpp"
#include "rtt_reader/target_ops_tests.hpp"

#include <span>
#include <vector>

int main(int    argc,
         char** argv) {
    // every wait of a case is on the printer's virtual clock: it moves only while the reader, its decoder and the
    // case's thread all wait, so no check depends on how fast the machine runs (first, so it outlives every thread)
    uc_log::printer::VirtualTime const virtualTime;
    namespace t = uc_log_test_rtt_reader;
    std::vector<uc_log::test::Case> cases;
    for(std::span<uc_log::test::Case const> const concern :
        {std::span<uc_log::test::Case const>{t::decode::cases},
         std::span<uc_log::test::Case const>{t::target_ops::cases},
         std::span<uc_log::test::Case const>{t::resets::cases},
         std::span<uc_log::test::Case const>{t::recovery::cases},
         std::span<uc_log::test::Case const>{t::flash_gate::cases},
         std::span<uc_log::test::Case const>{t::boots::cases}})
    {
        cases.insert(cases.end(), concern.begin(), concern.end());
    }
    return uc_log::test::run(argc, argv, cases);
}
