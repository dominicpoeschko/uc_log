#pragma once

#include "../DuplexChannel.hpp"
#include "rtt/rtt.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <utility>

namespace uc_log { namespace detail {

    //"uc_log" is reserved as name prefix for the log up buffers
    static constexpr std::string_view LogBufferNamePrefix{"uc_log"};

    template<std::size_t I>
    consteval auto makeLogChannelNameStorage() {
        constexpr std::size_t digits = [] {
            std::size_t v{I};
            std::size_t n{1};
            while(v >= 10) {
                v /= 10;
                ++n;
            }
            return n;
        }();

        std::array<char, LogBufferNamePrefix.size() + digits + 1> storage{};
        for(std::size_t i{}; i < LogBufferNamePrefix.size(); ++i) {
            storage[i] = LogBufferNamePrefix[i];
        }
        std::size_t v{I};
        for(std::size_t i{}; i < digits; ++i) {
            storage[LogBufferNamePrefix.size() + digits - 1 - i] = static_cast<char>('0' + v % 10);
            v /= 10;
        }
        return storage;
    }

    template<std::size_t I>
    struct LogChannelName {
        static constexpr auto storage = makeLogChannelNameStorage<I>();

        constexpr operator std::string_view() const {
            return std::string_view{storage.data(), storage.size() - 1};
        }
    };

    template<ChannelName Name>
    struct DuplexBufferName {
        static constexpr auto storage = Name.value;

        constexpr operator std::string_view() const {
            return std::string_view{storage.data(), storage.size() - 1};
        }
    };

    template<rtt::BufferMode LogMode, typename DuplexChannelList, std::size_t... LogBufferSizes>
    struct RttConfigBuilder;

    template<rtt::BufferMode LogMode, typename... DuplexConfigs, std::size_t... LogBufferSizes>
    struct RttConfigBuilder<LogMode, DuplexChannels<DuplexConfigs...>, LogBufferSizes...> {
    private:
        template<std::size_t... Is>
        static auto makeLogUpConfigs(std::index_sequence<Is...>)
          -> rtt::make_ChannelConfigs_t<rtt::ChannelConfig<LogBufferSizes,
                                                           LogMode,
                                                           LogChannelName<Is>>...>;

        using LogUpConfigs
          = decltype(makeLogUpConfigs(std::make_index_sequence<sizeof...(LogBufferSizes)>{}));

        using DuplexUpConfigs = rtt::make_ChannelConfigs_t<
          rtt::ChannelConfig<DuplexConfigs::UpSize,
                             DuplexConfigs::UpMode,
                             DuplexBufferName<DuplexConfigs::Name>>...>;

        static constexpr bool duplexNamesValid() {
            std::array<std::string_view, sizeof...(DuplexConfigs)> const names{
              std::string_view{DuplexConfigs::Name}...};
            for(std::size_t i{}; i < names.size(); ++i) {
                if(names[i].empty()) { return false; }
                if(names[i].starts_with(LogBufferNamePrefix)) { return false; }
                for(std::size_t j{i + 1}; j < names.size(); ++j) {
                    if(names[i] == names[j]) { return false; }
                }
            }
            return true;
        }

        static_assert(duplexNamesValid(),
                      "duplex channel names must be non-empty, unique and must not start with the "
                      "reserved log buffer prefix \"uc_log\"");

    public:
        static constexpr std::size_t NumLogUpBuffers   = sizeof...(LogBufferSizes);
        static constexpr std::size_t NumDuplexChannels = sizeof...(DuplexConfigs);

        template<std::size_t I>
        using DuplexConfigAt = std::tuple_element_t<I, std::tuple<DuplexConfigs...>>;

        struct Config {
            using UpChannelConfigs = decltype(std::tuple_cat(std::declval<LogUpConfigs>(),
                                                             std::declval<DuplexUpConfigs>()));

            using DownChannelConfigs = rtt::make_ChannelConfigs_t<
              rtt::ChannelConfig<DuplexConfigs::DownSize,
                                 DuplexConfigs::DownMode,
                                 DuplexBufferName<DuplexConfigs::Name>>...>;

            static constexpr auto ControlBlockId{rtt::DefaultControlBlockId};
        };
    };

}}   // namespace uc_log::detail
