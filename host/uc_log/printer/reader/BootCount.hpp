#pragma once

// The reader's count of the boots nobody read (printer/BootLedger.hpp has the bookkeeping and when a boot is settled):
// it reads the image's count of boots that logged (ImageSource::loggedBoots; detail/RttEpoch.hpp: bits 31:16 of its
// word, the next word its check), settles the ledger at a session start, a session's first bytes and before the
// printer's own flash or reset, and says what it finds - a status error and ReaderSink::unreadBoots for boots that
// passed unread, ReaderSink::bootCountRestart when the count starts again. Included only by JLinkRttReaderImpl.hpp.

#include "uc_log/detail/Lifetimebound.hpp"
#include "uc_log/detail/RttEpoch.hpp"
#include "uc_log/printer/BootLedger.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/MessageLog.hpp"

#include <cstdint>
#include <fmt/format.h>
#include <functional>
#include <optional>
#include <utility>
#include <variant>

namespace uc_log::printer::reader {

template<typename TransportT>
class BootCount {
public:
    using Ledger = uc_log::printer::BootLedger;
    using Count  = Ledger::Count;

    /// `address`: ImageSource::loggedBoots; `unreadBoots`, `restarted`: ReaderSink::unreadBoots, bootCountRestart
    BootCount(MessageLog& messages                                 UC_LOG_LIFETIMEBOUND,
              std::function<std::optional<std::uint32_t>()> const& address,
              std::function<void(std::uint32_t,
                                 std::uint32_t,
                                 std::uint32_t)> const&            unreadBoots,
              std::function<void()> const&                         restarted)
      : messages_{messages}
      , address_{address}
      , unreadBoots_{unreadBoots}
      , restarted_{restarted} {}

    /// A session begins: the first look sets where counting starts.
    void sessionStart(TransportT& jlink) {
        if(auto const logged = read(jlink)) {
            sayUnread(ledger_.start(logged->first, logged->second));
        }
    }

    /// The session's first bytes: its boot logged and is read - the boots before it that logged and nobody read are
    /// said now, ahead of this boot's lines. The boot's count, for notRead.
    std::optional<Count> bootRead(TransportT& jlink) {
        auto const logged = read(jlink);
        if(!logged) { return std::nullopt; }
        sayUnread(ledger_.bytesRead(logged->second));
        return logged->second;
    }

    /// The boot bootRead() gave `count` for left no line after all (Ledger::notRead).
    void notRead(Count count) { ledger_.notRead(count); }

    /// Before the printer's own flash or reset: the boot it replaces is settled (counted if it logged and nobody read
    /// it).
    void beforeReplacing(TransportT& jlink) {
        if(auto const logged = read(jlink)) {
            sayUnread(ledger_.replacing(logged->first, logged->second));
        }
    }

    /// After the printer's own download the count starts again (Ledger::restart): said, and handed to
    /// ReaderSink::bootCountRestart (the printer writes it into the log as a row, so a reader of the log restarts its
    /// count too).
    void restartAfterDownload() {
        ledger_.restart();
        messages_.status(
          "the count of boots that logged starts again: the printer's download wrote over the "
          "target's RAM");
        if(restarted_) { restarted_(); }
    }

private:
    // the count's address and the count now; nullopt: none, or unreadable
    std::optional<std::pair<std::uint32_t,
                            Count>>
    read(TransportT& jlink) {
        auto const address = address_ ? address_() : std::nullopt;
        if(!address) { return std::nullopt; }
        auto const value = uc_log::printer::tryReadWord(jlink, *address);
        auto const check = uc_log::printer::tryReadWord(jlink, *address + 4);
        if(!value || !check) { return std::nullopt; }
        bool const restarted = *check == (~*value ^ uc_log::detail::restartMark);
        if(*check != ~*value && !restarted) {
            return std::nullopt;
        }   // being written, or junk the next boot restarts
        if(restarted && lostCountSaid_ != *value) {
            // the boot found the count not kept (the target's RAM was lost: on the RP2350 a watchdog reboot, a
            // download) and started it again: no number across it, said once per value; this count is the new baseline
            lostCountSaid_ = value;
            ledger_.restart();
            messages_.status(
              "the count of boots that logged was not kept (the target's RAM was lost: a watchdog "
              "reboot, "
              "a download): it starts again");
            if(restarted_) { restarted_(); }
        }
        return std::pair{*address, static_cast<Count>(*value >> 16U)};
    }

    // Boots nobody read: said as a status error and handed to ReaderSink::unreadBoots (the printer writes it into the
    // log as a row too); a delta that cannot be counted from is said with no number.
    void sayUnread(Ledger::Verdict const& verdict) {
        if(auto const* u = std::get_if<Ledger::Uncountable>(&verdict)) {
            messages_.error(
              fmt::format("the count of boots that logged moved from {} to {}: boots may have "
                          "passed unread, how many is unknown (a power-on, a download over RAM, or "
                          "an image of another layout)",
                          u->from,
                          u->to));
            return;
        }
        auto const* unread = std::get_if<Ledger::Unread>(&verdict);
        if(unread == nullptr) { return; }
        messages_.error(
          fmt::format("{} boot{} passed unread (boots that logged {} -> {}): {} log nobody read{}",
                      unread->boots,
                      unread->boots == 1 ? "" : "s",
                      unread->from,
                      unread->to,
                      unread->boots == 1 ? "its" : "their",
                      unread->replacing ? ", the last one replaced now by the printer" : ""));
        if(unreadBoots_) { unreadBoots_(unread->boots, unread->from, unread->to); }
    }

    MessageLog&                                                      messages_;
    std::function<std::optional<std::uint32_t>()>                    address_;
    std::function<void(std::uint32_t, std::uint32_t, std::uint32_t)> unreadBoots_;
    std::function<void()>                                            restarted_;
    Ledger                                                           ledger_;
    std::optional<std::uint32_t> lostCountSaid_;   // the junk count word said
};

}   // namespace uc_log::printer::reader
