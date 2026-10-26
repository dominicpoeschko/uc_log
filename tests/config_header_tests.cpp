// The config-header hook of uc_log.hpp: this TU includes nothing but uc_log.hpp, which reads the header named by
// UC_LOG_CONFIG_HEADER (config_header/test_config.hpp) before anything else - the log sink and the log clock come from
// there, and a log line works without the TU specialising either.
#include "uc_log/uc_log.hpp"
// a second include (another header of the TU) neither re-reads the config header nor trips the re-entry check
#include "support/Check.hpp"

#ifndef UC_LOG_CONFIG_HEADER
    #error "the target must set UC_LOG_CONFIG_HEADER"
#endif

static_assert(uc_log::ComBackend<uc_log::Tag::User>::fromConfigHeader,
              "the sink is the config header's");

namespace {
void logsThroughTheConfigHeader() {
    config_header_test::written().clear();
    int const before = config_header_test::clockReads();
    int const value  = 42;
    UC_LOG_I("config header {}", value);
    CHECK(!config_header_test::written().empty(), "the line reached the config header's sink");
    CHECK(config_header_test::clockReads() > before, "the line read the config header's clock");
}
}   // namespace

int main(int    argc,
         char** argv) {
    return uc_log::test::run(argc,
                             argv,
                             {
                               {"logsThroughTheConfigHeader", logsThroughTheConfigHeader}
    });
}
