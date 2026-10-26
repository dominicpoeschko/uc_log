#pragma once

// The ledger of a LossPolicy::Counted firmware (::lossLedger in the map): read every 2 s, it tells of a ring that
// dropped records and has written nothing since (printer/TargetLoss.hpp). A rebuilt map is a new snapshot: the ledger
// is looked up again and the watch starts over.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/MapFacts.hpp"
#include "uc_log/printer/MessageLog.hpp"
#include "uc_log/printer/TargetLoss.hpp"
#include "uc_log/printer/TargetOp.hpp"
#include "uc_log/printer/TargetPort.hpp"

#include <array>
#include <chrono>
#include <fmt/format.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace uc_log::printer {

class LossLedgerWatch {
public:
    using Clock = printer::Clock;

    static constexpr std::chrono::milliseconds period{2000};

    /// `counted`: whether the firmware counts its lost records (its map has one ledger), after every map change.
    LossLedgerWatch(printer::MapFacts& mapFacts       UC_LOG_LIFETIMEBOUND,
                    TargetPort& target                UC_LOG_LIFETIMEBOUND,
                    printer::TargetLoss& targetLoss   UC_LOG_LIFETIMEBOUND,
                    std::mutex& targetLossMutex       UC_LOG_LIFETIMEBOUND,
                    MessageLog& messages              UC_LOG_LIFETIMEBOUND,
                    std::function<void(bool)> counted UC_LOG_LIFETIMEBOUND)
      : mapFacts_{mapFacts}
      , target_{target}
      , targetLoss_{targetLoss}
      , targetLossMutex_{targetLossMutex}
      , messages_{messages}
      , counted_{std::move(counted)} {}

    LossLedgerWatch(LossLedgerWatch const&)            = delete;
    LossLedgerWatch& operator=(LossLedgerWatch const&) = delete;

private:
    void run(std::stop_token const& stop) {
        std::shared_ptr<printer::MapFacts::Facts const> seen;
        std::optional<printer::LedgerPlace>             ledger;
        while(!stop.stop_requested()) {
            // in steps: a stop is seen within 100 ms
            for(int i = 0; i != 20 && !stop.stop_requested(); ++i) {
                Clock::sleep_for(period / 20);
            }
            if(stop.stop_requested()) { return; }
            if(auto const facts = mapFacts_.get(); facts != seen) {
                seen   = facts;
                ledger = facts->ledger;
                if(facts->ledgers > 1) {
                    messages_.error(
                      fmt::format("{} ::lossLedger symbols in the map (two counting log "
                                  "backends?): reading none",
                                  facts->ledgers));
                }
                if(counted_) { counted_(ledger.has_value()); }
                std::scoped_lock<std::mutex> const lock{targetLossMutex_};
                targetLoss_.clearWatch();
            }
            if(!ledger) { continue; }
            std::array<printer::TargetOp, 1> const read{
              printer::target_op::Read{ledger->address, ledger->size}
            };
            auto const result = target_.accessTarget(read, TargetPort::defaultTimeout);
            if(!result || result->empty()) { continue; }
            auto const               rings = printer::decodeLossLedger(result->front());
            std::vector<std::string> said;
            {
                std::scoped_lock<std::mutex> const lock{targetLossMutex_};
                said = targetLoss_.ledger(rings, Clock::now());
            }
            for(auto const& m : said) { messages_.error(m); }
        }
    }

    printer::MapFacts&        mapFacts_;
    TargetPort&               target_;
    printer::TargetLoss&      targetLoss_;
    std::mutex&               targetLossMutex_;
    MessageLog&               messages_;
    std::function<void(bool)> counted_;
    // last: stopped and joined before the members it uses go
    Clock::Thread thread_{[this](std::stop_token const& stop) { run(stop); }};
};

}   // namespace uc_log::printer
