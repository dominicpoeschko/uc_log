#pragma once

#include "uc_log/FTXUIGui.hpp"
#include "uc_log/SimpleGui.hpp"
#include "uc_log/detail/LogEntry.hpp"

namespace uc_log {

struct Gui {
    static std::vector<std::string> getTypes() {
        std::vector<std::string> types;
        types.push_back("ftxui");
        types.push_back("simple");
        return types;
    }

    std::variant<SimpleGui, FTXUIGui> impl{};

    explicit Gui(std::string_view guiType) {
        if(guiType == "simple") {
            impl.emplace<SimpleGui>();
        } else if(guiType == "ftxui") {
            impl.emplace<FTXUIGui>();
        } else {
            fmt::print(stderr, "bad gui type\n");
            std::exit(1);
        }
    }

    void add(std::chrono::system_clock::time_point recv_time, uc_log::detail::LogEntry const& e) {
        std::visit([&](auto& i) { i.add(recv_time, e); }, impl);
    }

    void fatalError(std::string_view msg) {
        std::visit([&](auto& i) { i.fatalError(msg); }, impl);
    }

    void statusMessage(std::string_view msg) {
        std::visit([&](auto& i) { i.statusMessage(msg); }, impl);
    }

    void errorMessage(std::string_view msg) {
        std::visit([&](auto& i) { i.errorMessage(msg); }, impl);
    }

    template<typename Reader>
    int run(Reader& rttReader, std::string const& buildCommand) {
        return std::visit([&](auto& i) { return i.run(rttReader, buildCommand); }, impl);
    }
};
}   // namespace uc_log
