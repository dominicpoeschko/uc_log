#pragma once

// The `records` answer (ControlProtocol.hpp) from what RecordReader read: every record decoded here, so a client
// holds no layout, shift, field order or name table of its own.

#include "uc_log/printer/ControlProtocol.hpp"
#include "uc_log/printer/MapSymbols.hpp"
#include "uc_log/printer/RecordReader.hpp"
#include "uc_log/printer/TargetRecords.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uc_log::printer {

/// The map a printer decodes with, as the control server sees it: the current parse (null: no map file), its path
/// and how many parses came before it.
struct ControlMap {
    std::shared_ptr<MapSnapshot const> snapshot;
    std::string                        path;
    std::uint64_t                      generation{};
};

namespace control_records {

    inline records::Kinds kindsOf(std::vector<control::RecordKind> const& kinds) {
        records::Kinds out;
        for(auto const k : kinds) {
            switch(k) {
            case control::RecordKind::crash:  out.crash = true; break;
            case control::RecordKind::health: out.health = true; break;
            case control::RecordKind::ubsan:  out.ubsan = true; break;
            case control::RecordKind::stacks: out.stacks = true; break;
            case control::RecordKind::trace:  out.trace = true; break;
            }
        }
        return out;
    }

    template<std::size_t N>
    std::vector<std::string> bitNames(std::uint32_t        bits,
                                      std::array<std::string_view,
                                                 N> const& names) {
        std::vector<std::string> out;
        for(std::size_t b = 0; b != N; ++b) {
            if(((bits >> b) & 1U) != 0) { out.emplace_back(names[b]); }
        }
        return out;
    }

    // address, valid, error, record of any record answer
    template<typename Answer,
             typename T>
    Answer base(records::InImage<T> const& in) {
        Answer out{};
        out.address = in.address;
        out.valid   = in.record.has_value();
        if(!in.error.empty()) { out.error = in.error; }
        if(in.record) { out.record = *in.record; }
        return out;
    }

    inline control::FaultAnswer fault(records::InImage<records::Fault> const& in) {
        auto out = base<control::FaultAnswer>(in);
        if(!in.record) { return out; }
        auto const& f     = *in.record;
        out.core          = f.core();
        out.arch          = f.arch();
        out.flagNames     = bitNames(f.flags, records::faultFlagNames);
        out.exception     = f.exception();
        out.exceptionName = records::exceptionName(f.exception());
        std::vector<std::string> cfsr;
        if(auto const text = records::cfsrText(f.cfsr); text != "-") {
            for(std::size_t at = 0; at <= text.size();) {
                auto const end = std::min(text.find(' ', at), text.size());
                cfsr.push_back(text.substr(at, end - at));
                at = end + 1;
            }
        }
        out.cfsrNames = std::move(cfsr);
        std::vector<control::NamedWord> words;
        auto const                      values = f.words();
        for(std::size_t i = 0; i != values.size(); ++i) {
            words.push_back({std::string{records::faultFieldNames[i]}, values[i]});
        }
        out.words = std::move(words);
        return out;
    }

    inline control::StackSnapshotAnswer
    stackSnapshot(records::InImage<records::StackSnapshot> const& in) {
        control::StackSnapshotAnswer out{.address = in.address,
                                         .valid   = in.record.has_value(),
                                         .error   = {},
                                         .from    = {},
                                         .bytes   = {},
                                         .body    = {}};
        if(!in.error.empty()) { out.error = in.error; }
        if(!in.record) { return out; }
        out.from        = in.record->from;
        out.bytes       = in.record->bytes;
        auto const body = in.record->body();
        out.body        = std::vector<std::uint32_t>{body.begin(), body.end()};
        return out;
    }

    inline control::PanicAnswer panic(records::InImage<records::Panic> const& in) {
        auto out = base<control::PanicAnswer>(in);
        if(!in.record) { return out; }
        out.causeName = in.record->causeName();
        out.timeUs    = in.record->timeUs();
        out.site      = in.record->site();
        return out;
    }

    inline control::RecoveriesAnswer recoveries(records::InImage<records::Recoveries> const& in) {
        auto out = base<control::RecoveriesAnswer>(in);
        if(in.record && in.record->inProgress != 0) {
            out.inProgressCheck   = in.record->inProgressCheck();
            out.inProgressAttempt = in.record->inProgressAttempt();
        }
        return out;
    }

    inline control::GuardAnswer guard(records::InImage<records::GuardState> const& in) {
        auto out = base<control::GuardAnswer>(in);
        if(!in.record) { return out; }
        out.failureName    = in.record->failureName();
        out.guardFlagNames = bitNames(in.record->flags, records::guardFlagNames);
        return out;
    }

    inline control::StackAnswer stack(records::StackRead const& in) {
        control::StackAnswer out{.name  = in.name,
                                 .low   = in.low,
                                 .high  = in.high,
                                 .valid = in.use.has_value(),
                                 .error = {},
                                 .size  = {},
                                 .used  = {},
                                 .never = {}};
        // read but not painted: no error, only not valid
        if(!in.read && !in.error.empty()) { out.error = in.error; }
        if(in.use) {
            out.size  = in.use->size;
            out.used  = in.use->used;
            out.never = in.use->never;
        }
        return out;
    }

    inline control::RingAnswer ring(records::RingRead const& in) {
        control::RingAnswer out{.symbol              = in.symbol,
                                .address             = in.address,
                                .valid               = in.error.empty(),
                                .error               = {},
                                .name                = {},
                                .fieldNames          = {},
                                .capacity            = {},
                                .count               = {},
                                .writtenWhileReading = {},
                                .rows                = {}};
        if(!in.error.empty()) {
            out.error = in.error;
            return out;
        }
        out.name                = in.name;
        out.fieldNames          = in.fields;
        out.capacity            = in.capacity;
        out.count               = in.count;
        out.writtenWhileReading = in.writtenWhileReading;
        std::vector<control::RingRowAnswer> rows;
        rows.reserve(in.rows.size());
        for(auto const& r : in.rows) { rows.push_back({r.index, r.values}); }
        out.rows = std::move(rows);
        return out;
    }

    inline control::RecordsAnswer answer(records::Read const&  read,
                                         records::Kinds const& kinds,
                                         ControlMap const&     map) {
        control::RecordsAnswer out;
        out.map = {.path = map.path, .generation = map.generation};
        if(read.fault) { out.fault = fault(*read.fault); }
        if(read.stack) { out.stack = stackSnapshot(*read.stack); }
        if(read.panic) { out.panic = panic(*read.panic); }
        if(read.starvation) { out.starvation = base<control::StarvationAnswer>(*read.starvation); }
        if(read.recoveries) { out.recoveries = recoveries(*read.recoveries); }
        if(read.guard) { out.guard = guard(*read.guard); }
        if(read.ubsan) { out.ubsan = base<control::UbsanAnswer>(*read.ubsan); }
        if(kinds.stacks) {
            std::vector<control::StackAnswer> stacks;
            stacks.reserve(read.stacks.size());
            for(auto const& s : read.stacks) { stacks.push_back(stack(s)); }
            out.stacks = std::move(stacks);
        }
        if(kinds.trace) {
            std::vector<control::RingAnswer> rings;
            rings.reserve(read.rings.size());
            for(auto const& r : read.rings) { rings.push_back(ring(r)); }
            out.rings = std::move(rings);
        }
        for(auto const c : records::panicCauses) { out.panicCauses.emplace_back(c); }
        for(std::uint32_t e = 0; e != 16; ++e) {
            out.exceptionNames.push_back(records::exceptionName(e));
        }
        return out;
    }

}   // namespace control_records
}   // namespace uc_log::printer
