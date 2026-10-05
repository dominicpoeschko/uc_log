#pragma once

#include "uc_log/detail/Lifetimebound.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <expected>
#include <fstream>
#include <istream>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// The firmware's symbols from its linker map: name, address and size, demangled as lld writes
/// them. The printer has the map (--map_file) and no ELF reader, and for the TUI's inspectors
/// (fault and panic records, sanitizer counter, stacks, trace rings, watches, the profiler) the
/// map is all it takes - kvasir_bench.py gets the same from nm.
///
/// lld: "<vma> <lma> <size> <align> <indent><text>", all hex but the alignment; the indent is
/// 1 for an output section, 9 for an input section, 17 for a symbol or an assignment inside a
/// section. A symbol line's size is the symbol's (st_size). An assignment's VMA is the location
/// counter, which is the symbol's value only for `name = .`; `a = b` takes b's value if b is
/// known. GNU ld ("<indent>0x<addr> <name>", sizes on the input-section line before) gives
/// addresses and, where the section line shows it, sizes; names there are what ld printed.
namespace uc_log::detail {

struct MapSymbol {
    std::string   name;
    std::uint32_t address{};   // as linked: a Thumb function has bit 0 set
    std::uint32_t size{};
    bool          code{};   // a function (Thumb bit, or in a text section)

    std::uint32_t start() const { return code ? (address & ~1U) : address; }
};

class MapSymbols {
public:
    MapSymbols() = default;

    static MapSymbols parse(std::istream& map) {
        MapSymbols  out;
        auto&       values = out.values_;
        std::string line;
        std::string section;   // current output section
        bool        inputText = false;
        bool        thumb     = false;
        // GNU ld: the input-section line before a symbol line carries address and size
        std::optional<std::pair<std::uint32_t, std::uint32_t>> gnuSection;
        bool                                                   gnuDiscarded = false;
        while(std::getline(map, line)) {
            std::string_view text{line};
            if(text.starts_with("Discarded input sections")) {
                gnuDiscarded = true;
                continue;
            }
            if(text.starts_with("Memory Configuration")
               || text.starts_with("Linker script and memory map"))
            {
                gnuDiscarded = false;
                continue;
            }
            if(gnuDiscarded) { continue; }
            if(auto lld = lldLine(text)) {
                out.lld_ = true;
                // `a = .`, `a = b`, PROVIDE(...): at any indent (inside a section at 9)
                if(!lld->text.contains(":(") && out.assignment(lld->text, lld->vma, values)) {
                    continue;
                }
                if(lld->indent <= 1) {
                    if(!lld->text.empty() && lld->text.front() == '.') {
                        section = std::string{lld->text};
                    }
                    continue;
                }
                if(lld->indent < 17) {   // an input section: "file.o:(.text.foo)"
                    inputText = lld->text.contains(":(.text");
                    thumb     = false;
                    continue;
                }
                auto const name = lld->text;
                // mapping symbols: $t starts Thumb code (a RAM function in "(.data)" too), $d data
                if(name == "$t" || name.starts_with("$t.")) {
                    thumb = true;
                    continue;
                }
                if(name == "$d" || name.starts_with("$d.")) {
                    thumb = false;
                    continue;
                }
                if(name.starts_with('$')) { continue; }
                // a Thumb function's address has bit 0 set; data may be odd too (a byte)
                bool const code = (lld->vma & 1U) != 0 && (inputText || thumb);
                out.add(MapSymbol{std::string{name}, lld->vma, lld->size, code}, values);
                continue;
            }
            if(out.lld_) { continue; }
            gnuLine(text, out, values, gnuSection, section);
        }
        std::ranges::sort(out.byAddress_, {}, [&](std::size_t i) {
            return std::pair{out.symbols_[i].start(), i};
        });
        out.sorted_.clear();
        for(auto i : out.byAddress_) {
            if(out.symbols_[i].code && out.symbols_[i].size != 0) { out.sorted_.push_back(i); }
        }
        return out;
    }

    static std::expected<MapSymbols,
                         std::string>
    fromFile(std::string const& path) {
        std::ifstream map{path};
        if(!map) { return std::unexpected{"cannot open the map file " + path}; }
        return parse(map);
    }

    bool empty() const { return symbols_.empty(); }

    std::size_t size() const { return symbols_.size(); }

    std::vector<MapSymbol> const& all() const UC_LOG_LIFETIMEBOUND { return symbols_; }

    /// The exact name; the first one when a name repeats (static locals of inlined functions).
    MapSymbol const* find(std::string_view name) const UC_LOG_LIFETIMEBOUND {
        auto const it = byName_.find(std::string{name});
        return it == byName_.end() ? nullptr : &symbols_[it->second];
    }

    /// A name's value - also for linker-script names that are no symbol line (`_LINKER_*`).
    std::optional<std::uint32_t> value(std::string_view name) const {
        auto const it = values_.find(std::string{name});
        if(it == values_.end()) { return std::nullopt; }
        return it->second;
    }

    template<typename Pred>
    std::vector<MapSymbol const*> findIf(Pred&& pred) const {
        std::vector<MapSymbol const*> out;
        for(auto const& s : symbols_) {
            if(pred(s)) { out.push_back(&s); }
        }
        return out;
    }

    std::vector<MapSymbol const*> findAll(std::regex const& re,
                                          bool              dataOnly = false) const {
        return findIf([&](MapSymbol const& s) {
            return (!dataOnly || !s.code) && std::regex_search(s.name, re);
        });
    }

    /// The function an address is in (Thumb bit ignored), or nullptr.
    MapSymbol const* containing(std::uint32_t address) const UC_LOG_LIFETIMEBOUND {
        address &= ~1U;
        auto const it = std::ranges::upper_bound(sorted_, address, {}, [&](std::size_t i) {
            return symbols_[i].start();
        });
        if(it == sorted_.begin()) { return nullptr; }
        auto const& s = symbols_[*std::prev(it)];
        return address < s.start() + s.size ? &s : nullptr;
    }

    /// "name+0x12" or "" when the address is in no function.
    std::string describe(std::uint32_t address) const {
        auto const* s = containing(address);
        if(s == nullptr) { return {}; }
        auto const offset = (address & ~1U) - s->start();
        return offset == 0 ? s->name : s->name + "+0x" + hex(offset);
    }

    bool fromLld() const { return lld_; }

private:
    struct LldLine {
        std::uint32_t    vma{};
        std::uint32_t    size{};
        std::size_t      indent{};
        std::string_view text;
    };

    static std::string hex(std::uint32_t v) {
        std::array<char, 8> buf{};
        auto const [end, ec] = std::to_chars(buf.data(), std::to_address(buf.end()), v, 16);
        return std::string(buf.data(), end);
    }

    // a whitespace-separated number token at `pos`; advances `pos` past it
    static std::optional<std::uint32_t> number(std::string_view text,
                                               std::size_t&     pos,
                                               int              base) {
        pos = text.find_first_not_of(' ', pos);
        if(pos == std::string_view::npos) { return std::nullopt; }
        auto const end   = std::min(text.find(' ', pos), text.size());
        auto       token = text.substr(pos, end - pos);
        if(base == 16 && (token.starts_with("0x") || token.starts_with("0X"))) {
            token.remove_prefix(2);
        }
        std::uint32_t v{};
        auto const [ptr, ec] = std::from_chars(token.data(), std::to_address(token.end()), v, base);
        if(ec != std::errc{} || ptr != std::to_address(token.end()) || token.empty()) {
            return std::nullopt;
        }
        pos = end;
        return v;
    }

    static std::optional<LldLine> lldLine(std::string_view text) {
        std::size_t pos  = 0;
        auto const  vma  = number(text, pos, 16);
        auto const  lma  = vma ? number(text, pos, 16) : std::nullopt;
        auto const  size = lma ? number(text, pos, 16) : std::nullopt;
        auto const  algn = size ? number(text, pos, 10) : std::nullopt;
        if(!algn || pos >= text.size()) { return std::nullopt; }
        auto const rest   = text.substr(pos);
        auto const indent = rest.find_first_not_of(' ');
        if(indent == std::string_view::npos) { return std::nullopt; }
        return LldLine{*vma, *size, indent, rest.substr(indent)};
    }

    static bool isIdentifier(std::string_view s) {
        return !s.empty() && std::ranges::all_of(s, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '_' || c == '.' || c == '$';
        });
    }

    // `name = .` / `name = other` / `PROVIDE(name = ...)`; true when the line is one
    bool assignment(std::string_view                   text,
                    std::uint32_t                      vma,
                    std::unordered_map<std::string,
                                       std::uint32_t>& values) {
        auto const eq = text.find(" = ");
        if(eq == std::string_view::npos) { return false; }
        auto lhs = text.substr(0, eq);
        auto rhs = text.substr(eq + 3);
        if(lhs.starts_with("PROVIDE(")) {
            lhs.remove_prefix(8);
            if(rhs.ends_with(')')) { rhs.remove_suffix(1); }
        } else if(lhs.starts_with("PROVIDE_HIDDEN(")) {
            lhs.remove_prefix(15);
            if(rhs.ends_with(')')) { rhs.remove_suffix(1); }
        }
        if(!isIdentifier(lhs) || lhs == ".") { return true; }
        if(rhs == ".") {
            values.insert_or_assign(std::string{lhs}, vma);
        } else if(isIdentifier(rhs)) {
            if(auto const it = values.find(std::string{rhs}); it != values.end()) {
                values.insert_or_assign(std::string{lhs}, it->second);
            }
        }
        return true;
    }

    void add(MapSymbol                          s,
             std::unordered_map<std::string,
                                std::uint32_t>& values) {
        values.try_emplace(s.name, s.address);
        byName_.try_emplace(s.name, symbols_.size());
        byAddress_.push_back(symbols_.size());
        symbols_.push_back(std::move(s));
    }

    static void gnuLine(std::string_view                         text,
                        MapSymbols&                              out,
                        std::unordered_map<std::string,
                                           std::uint32_t>&       values,
                        std::optional<std::pair<std::uint32_t,
                                                std::uint32_t>>& gnuSection,
                        std::string&                             section) {
        // an output section: ".text           0x10000000     0x1234"
        if(text.starts_with('.')) {
            section = std::string{text.substr(0, text.find(' '))};
            return;
        }
        if(!text.starts_with(' ')) { return; }
        auto const first = text.find_first_not_of(' ');
        if(first == std::string_view::npos) { return; }
        auto const body = text.substr(first);
        // an input section: " .text.foo  0x... 0x... file.o" (numbers maybe on the next line)
        if(body.starts_with('.') || body.starts_with("COMMON")) {
            auto const nameEnd = body.find(' ');
            if(nameEnd == std::string_view::npos) {
                gnuSection = std::pair{0U, 0U};   // numbers follow on the next line
                return;
            }
            std::size_t pos  = nameEnd;
            auto const  addr = number(body, pos, 16);
            auto const  size = addr ? number(body, pos, 16) : std::nullopt;
            gnuSection       = size ? std::optional{std::pair{*addr, *size}} : std::nullopt;
            return;
        }
        std::size_t pos  = 0;
        auto const  addr = number(body, pos, 16);
        if(!addr) {
            gnuSection.reset();
            return;
        }
        auto const rest = body.substr(std::min(pos, body.size()));
        auto const name = rest.substr(std::min(rest.find_first_not_of(' '), rest.size()));
        if(gnuSection && gnuSection->first == 0 && gnuSection->second == 0) {
            // the continuation line of an input section: "0x<addr> 0x<size> file.o"
            std::size_t p2   = pos;
            auto const  size = number(body, p2, 16);
            gnuSection       = size ? std::optional{std::pair{*addr, *size}} : std::nullopt;
            return;
        }
        if(name.empty() || name.contains(' ') || name.contains('=')) {
            if(name.contains(" = ")) { out.assignment(name, *addr, values); }
            return;
        }
        std::uint32_t size = 0;
        if(gnuSection && gnuSection->first == (*addr & ~1U)) {
            size = gnuSection->second;
            gnuSection.reset();
        }
        bool const code = (*addr & 1U) != 0 && section.contains("text");
        if(*addr == 0) { return; }
        out.add(MapSymbol{std::string{name}, *addr, size, code}, values);
    }

    std::vector<MapSymbol>                         symbols_;
    std::unordered_map<std::string, std::size_t>   byName_;
    std::unordered_map<std::string, std::uint32_t> values_;
    std::vector<std::size_t>                       byAddress_;
    std::vector<std::size_t>                       sorted_;   // code with a size, by start
    bool                                           lld_{};
};

}   // namespace uc_log::detail
