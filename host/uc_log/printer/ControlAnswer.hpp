#pragma once

// The control protocol's answers (ControlProtocol.hpp) for one request, computed against a ControlTarget: what the
// control server's workers run (ControlServer.hpp), and what the golden files and the fuzzers check without a socket.
// No asio here: a test of the answers needs no server.

#include "uc_log/printer/Clock.hpp"
#include "uc_log/printer/ControlProtocol.hpp"
#include "uc_log/printer/ControlRecords.hpp"
#include "uc_log/printer/CortexM.hpp"
#include "uc_log/printer/Le32.hpp"
#include "uc_log/printer/PanicRaise.hpp"
#include "uc_log/printer/RecordReader.hpp"
#include "uc_log/printer/TargetOp.hpp"
#include "uc_log/printer/TargetPort.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fmt/format.h>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uc_log::printer {

/// What the server acts on. A function left empty answers an error for its request.
struct ControlTarget {
    /// Gets `cancelled`: true once the printer stops or the client is gone.
    using Action = std::function<std::expected<void, std::string>(std::function<bool()> const&)>;

    /// read, write, halt, go, registers, write_registers: the reader's TargetOp queue; null: "no target here"
    printer::TargetPort*                   target{};
    std::function<control::StatusAnswer()> status{};
    Action                                 reset{};
    /// a reset through the probe's reset pin (`reset` with "line": true)
    Action                                                          resetLine{};
    Action                                                          flash{};
    std::function<std::vector<control::StatusMessage>(std::size_t)> messages{};
    /// records, raise_panic: the map the printer decodes with
    std::function<ControlMap()> map{};
    /// µs clocks; empty = the real ones.
    std::function<std::int64_t()> unixMicros{};
    std::function<std::int64_t()> steadyMicros{};
    std::atomic<bool> const*      stop{};   // when set, running waits, resets and flashes return
};

namespace detail {
    inline printer::TargetPort::Result run(ControlTarget const&         t,
                                           std::vector<TargetOp> const& ops) {
        return t.target->accessTarget(ops, printer::TargetPort::defaultTimeout);
    }

    inline printer::TargetPort::Result readPieces(ControlTarget const&            t,
                                                  std::span<control::Piece const> pieces) {
        std::vector<TargetOp> ops;
        ops.reserve(pieces.size());
        for(auto const& p : pieces) { ops.emplace_back(target_op::Read{p.address, p.size}); }
        return run(t, ops);
    }

    // halt (true) or resume (false) the core; the halted state after
    inline std::expected<bool,
                         std::string>
    core(ControlTarget const& t,
         bool                 halt) {
        auto const answer = halt ? run(t, {target_op::Halt{}}) : run(t, {target_op::Go{}});
        if(!answer) { return std::unexpected{answer.error()}; }
        return !answer->empty() && !answer->front().empty()
            && answer->front().front() != std::byte{0};
    }

    inline std::int64_t realUnixMicros() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    }

    inline std::int64_t realSteadyMicros() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                 Clock::now().time_since_epoch())
          .count();
    }

    inline std::int64_t unixMicros(ControlTarget const& t) {
        return t.unixMicros ? t.unixMicros() : realUnixMicros();
    }

    inline std::int64_t steadyMicros(ControlTarget const& t) {
        return t.steadyMicros ? t.steadyMicros() : realSteadyMicros();
    }

    inline std::string hex(std::span<std::byte const> bytes) {
        static constexpr std::string_view digits = "0123456789abcdef";
        std::string                       out;
        out.reserve(2 * bytes.size());
        for(auto const b : bytes) {
            out += digits[std::to_integer<unsigned>(b) >> 4U];
            out += digits[std::to_integer<unsigned>(b) & 0xFU];
        }
        return out;
    }

    inline control::Error error(std::string text) {
        return control::Error{.error = std::move(text)};
    }

    inline control::Answer answer(control::Ping const&,
                                  ControlTarget const&) {
        return control::PingAnswer{};
    }

    inline control::Answer answer(control::Status const&,
                                  ControlTarget const& t) {
        if(!t.status) { return error("no status here"); }
        return t.status();
    }

    inline control::Answer answer(control::Messages const& m,
                                  ControlTarget const&     t) {
        if(!t.messages) { return error("no messages here"); }
        return control::MessagesAnswer{.messages = t.messages(m.count)};
    }

    inline control::Answer answer(control::Read const& r,
                                  ControlTarget const& t) {
        if(r.pieces.empty()) { return error("read: nothing to read"); }
        std::size_t total = 0;
        for(auto const& p : r.pieces) {
            if(p.size == 0) { return error("read: a piece of 0 bytes"); }
            total += p.size;
        }
        if(r.pieces.size() > control::maxPieces || total > control::maxReadBytes) {
            return error(fmt::format("read: too much for one request ({} pieces, {} bytes)",
                                     control::maxPieces,
                                     control::maxReadBytes));
        }
        if(t.target == nullptr) { return error("no target here"); }
        auto const result = readPieces(t, r.pieces);
        auto const now    = unixMicros(t);
        if(!result) { return error(result.error()); }
        control::ReadAnswer out{.unixUs = now, .data = {}};
        out.data.reserve(result->size());
        for(auto const& bytes : *result) { out.data.push_back(hex(bytes)); }
        return out;
    }

    inline control::Answer answer(control::Write const& w,
                                  ControlTarget const&  t) {
        if(w.words.empty()) { return error("write: nothing to write"); }
        if(w.words.size() > control::maxWriteWords) {
            return error(fmt::format("write: at most {} words a request", control::maxWriteWords));
        }
        for(auto const& word : w.words) {
            if(word.address % 4 != 0) {
                return error(fmt::format("write: {:#010x} is not word aligned", word.address));
            }
        }
        if(t.target == nullptr) { return error("no target here"); }
        std::vector<TargetOp> ops;
        ops.reserve(w.words.size());
        for(auto const& word : w.words) {
            ops.emplace_back(target_op::WriteWord{word.address, word.value});
        }
        auto const result = run(t, ops);
        auto const now    = unixMicros(t);
        if(!result) { return error(result.error()); }
        control::WriteAnswer out{.unixUs = now, .data = {}};
        out.data.reserve(result->size());
        for(auto const& bytes : *result) { out.data.push_back(hex(bytes)); }
        return out;
    }

    inline control::Answer answer(control::Wait const&         w,
                                  ControlTarget const&         t,
                                  std::function<bool()> const& cancelled = {}) {
        using control::Condition;
        if(w.piece.size == 0 || w.piece.size > control::maxWaitBytes) {
            return error("wait: a value is 1 to 8 bytes");
        }
        if(w.timeoutMs > control::maxWaitTimeoutMs) {
            return error("wait: timeout_ms is at most 600000");
        }
        if(w.condition != Condition::changed && !w.value) {
            return error("wait: this condition needs a value");
        }
        if(t.target == nullptr) { return error("no target here"); }
        std::uint64_t const mask  = w.mask.value_or(~std::uint64_t{});
        std::uint64_t const value = w.value.value_or(0) & mask;

        // paced on the printer's clock only: a target with a clock of its own (steadyMicros) has its own time
        auto       nextSample = Clock::now();
        auto const sample     = [&]() -> std::expected<std::uint64_t, std::string> {
            if(!t.steadyMicros) {
                Clock::sleep_until(nextSample);
                nextSample = Clock::now() + std::chrono::milliseconds{1};
            }
            auto const result = readPieces(t, std::span{&w.piece, 1});
            if(!result) { return std::unexpected{result.error()}; }
            if(result->empty()) {
                return std::unexpected{std::string{"wait: the read returned nothing"}};
            }
            return leValue(result->front()) & mask;
        };
        auto const start = steadyMicros(t);
        auto const first = sample();
        if(!first) { return error(first.error()); }
        auto last = *first;
        while(true) {
            bool const hit = [&] {
                switch(w.condition) {
                case Condition::changed: return last != *first;
                case Condition::eq:      return last == value;
                case Condition::ne:      return last != value;
                case Condition::lt:      return last < value;
                case Condition::gt:      return last > value;
                }
                return false;
            }();
            auto const waited = steadyMicros(t) - start;
            bool const late   = waited >= std::int64_t{w.timeoutMs} * 1000
                             || (t.stop != nullptr && *t.stop) || (cancelled && cancelled());
            if(hit || late) {
                return control::WaitAnswer{.hit      = hit,
                                           .unixUs   = unixMicros(t),
                                           .waitedUs = waited,
                                           .first    = *first,
                                           .last     = last};
            }
            auto const next = sample();
            if(!next) { return error(next.error()); }
            last = *next;
        }
    }

    inline control::Answer action(ControlTarget::Action const& a,
                                  ControlTarget const&         t,
                                  std::function<bool()> const& cancelled,
                                  control::Answer              done) {
        if(!a) { return error("not available here"); }
        auto const result
          = a([&] { return (t.stop != nullptr && *t.stop) || (cancelled && cancelled()); });
        if(!result) { return error(result.error()); }
        return done;
    }

    inline control::Answer answer(control::Reset const&        r,
                                  ControlTarget const&         t,
                                  std::function<bool()> const& cancelled) {
        return action(r.line.value_or(false) ? t.resetLine : t.reset,
                      t,
                      cancelled,
                      control::ResetAnswer{});
    }

    inline control::Answer answer(control::Flash const&,
                                  ControlTarget const&         t,
                                  std::function<bool()> const& cancelled) {
        return action(t.flash, t, cancelled, control::FlashAnswer{});
    }

    inline control::Answer answer(control::Halt const&,
                                  ControlTarget const& t) {
        if(t.target == nullptr) { return error("no target here"); }
        auto const result = core(t, true);
        if(!result) { return error(result.error()); }
        return control::HaltAnswer{.halted = *result};
    }

    inline control::Answer answer(control::Go const&,
                                  ControlTarget const& t) {
        if(t.target == nullptr) { return error("no target here"); }
        auto const result = core(t, false);
        if(!result) { return error(result.error()); }
        return control::GoAnswer{.halted = *result};
    }

    inline control::Answer answer(control::Registers const&,
                                  ControlTarget const& t) {
        if(t.target == nullptr) { return error("no target here"); }
        auto const raw = run(t, {target_op::ReadRegisters{}});
        if(!raw) { return error(raw.error()); }
        std::vector<std::uint32_t> values;
        if(!raw->empty()) {
            auto const& bytes = raw->front();
            for(std::size_t i = 0; i + 4 <= bytes.size(); i += 4) {
                values.push_back(le32(bytes, i));
            }
        }
        return control::RegistersAnswer{.values = std::move(values)};
    }

    inline control::Answer answer(control::WriteRegisters const& w,
                                  ControlTarget const&           t) {
        if(t.target == nullptr) { return error("no target here"); }
        for(auto const& r : w.writes) {
            if(r.index >= cortex_m::coreRegisterCount) {
                return error(fmt::format("write_registers: no register {} (0..{})",
                                         r.index,
                                         cortex_m::coreRegisterCount - 1));
            }
        }
        std::vector<TargetOp> ops;
        ops.reserve(w.writes.size());
        for(auto const& r : w.writes) {
            ops.emplace_back(target_op::WriteRegister{r.index, r.value});
        }
        auto const result = run(t, ops);
        if(!result) { return error(result.error()); }
        return control::WriteRegistersAnswer{};
    }

    // the map's places, or why there are none
    inline std::expected<std::pair<ControlMap,
                                   records::Places>,
                         std::string>
    places(ControlTarget const& t,
           std::string_view     request) {
        auto map = t.map ? t.map() : ControlMap{};
        if(!map.snapshot) {
            return std::unexpected{
              fmt::format("{}: no map file (the printer was started without --map_file)", request)};
        }
        if(!map.snapshot->error.empty()) {
            return std::unexpected{fmt::format("{}: the map {} could not be read: {}",
                                               request,
                                               map.path,
                                               map.snapshot->error)};
        }
        auto at = records::Places::of(map.snapshot->symbols);
        return std::pair{std::move(map), std::move(at)};
    }

    inline control::Answer answer(control::Records const&      r,
                                  ControlTarget const&         t,
                                  std::function<bool()> const& cancelled) {
        if(r.kinds.empty()) {
            return error("records: name the kinds (crash, health, ubsan, stacks, trace)");
        }
        if(t.target == nullptr) { return error("no target here"); }
        auto found = places(t, "records");
        if(!found) { return error(found.error()); }
        auto const& [map, at] = *found;
        // in pieces through the reader's queue (RecordReader): the RTT reads go on between them, and a client that
        // hangs up or a printer that stops ends the read at the next one
        records::RecordReader reader{[&](std::vector<TargetOp> const& ops) -> TargetPort::Result {
            if((t.stop != nullptr && *t.stop) || (cancelled && cancelled())) {
                return std::unexpected{std::string{"cancelled"}};
            }
            return run(t, ops);
        }};
        auto const            kinds = control_records::kindsOf(r.kinds);
        auto const            read  = reader.read(at, kinds, {});
        return control_records::answer(read, kinds, map);
    }

    inline control::Answer answer(control::RaisePanic const& r,
                                  ControlTarget const&       t) {
        auto const cause = panic_raise::causeOf(r.cause);
        if(!cause) {
            return error(fmt::format("raise_panic: no cause \"{}\" (one of {})",
                                     r.cause,
                                     panic_raise::argumentNames()));
        }
        if(t.target == nullptr) { return error("no target here"); }
        auto const found = places(t, "raise_panic");
        if(!found) { return error(found.error()); }
        auto const raise = found->second.raise;
        if(!raise) {
            return error(
              fmt::format("raise_panic: {} is not in the map: a firmware without Kvasir::Panic",
                          panic_raise::raiseSymbol));
        }
        auto const raised
          = panic_raise::raise([&](std::vector<TargetOp> const& ops) { return run(t, ops); },
                               *raise,
                               *cause);
        if(!raised) { return error("raise_panic: " + raised.error()); }
        return control::RaisePanicAnswer{.cause = *cause,
                                         .name  = std::string{records::panicCauses[*cause]}};
    }

    inline control::Answer answer(control::Subscribe const&,
                                  ControlTarget const&) {
        return error("subscribe: only on a connection to the server");
    }
}   // namespace detail

/// `cancelled`: true once the client is gone, so a wait, reset or flash stops early.
inline control::Answer controlAnswer(control::Request const&      request,
                                     ControlTarget const&         target,
                                     std::function<bool()> const& cancelled = {}) {
    return std::visit(
      [&](auto const& r) -> control::Answer {
          if constexpr(requires { detail::answer(r, target, cancelled); }) {
              return detail::answer(r, target, cancelled);
          } else {
              return detail::answer(r, target);
          }
      },
      request);
}

inline std::string controlAnswerLine(std::string_view     line,
                                     ControlTarget const& target) {
    auto const parsed = control::parseRequest(line);
    if(auto const* e = std::get_if<control::Error>(&parsed)) {
        return control::toLine(control::Answer{*e});
    }
    return control::toLine(controlAnswer(std::get<control::Request>(parsed), target));
}

}   // namespace uc_log::printer
