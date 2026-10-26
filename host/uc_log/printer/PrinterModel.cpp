#include "uc_log/printer/PrinterModel.hpp"

#include <mutex>
#include <string>
#include <utility>

namespace uc_log::printer {

PrinterModel::PrinterModel(std::string   bindAddress,
                           std::uint16_t controlPort,
                           std::uint16_t duplexBasePort)
  : configuredControlPort_{controlPort} {
    state_.bindAddress    = std::move(bindAddress);
    state_.duplexBasePort = duplexBasePort;
    // the build thread: no lock of a front end here
    builds_.onBuildFinished = [this](bool success) {
        std::scoped_lock<std::mutex> const lock{mutex_};
        ++(success ? statistics_.successfulBuilds : statistics_.failedBuilds);
    };
}

PrinterModel::~PrinterModel() = default;

bool PrinterModel::startBuild(bool andFlash) {
    if(builds_.busy()) { return false; }
    {
        std::scoped_lock<std::mutex> const lock{mutex_};
        ++statistics_.totalBuildsStarted;
    }
    if(andFlash) {
        builds_.executeAndFlash();
    } else {
        builds_.execute();
    }
    return true;
}

}   // namespace uc_log::printer
