#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Module and short function name from a signature. The firmware's filter and the printer run the
// same scan, so a line is filtered by the module the printer shows.

namespace uc_log { namespace detail {
    // Quoted in the header the printer parses: no quote, comma, brace or anything to unescape.
    inline constexpr std::size_t MaxModuleName = 63;

    constexpr bool validModuleName(std::string_view name) {
        if(name.empty() || name.size() > MaxModuleName) { return false; }
        for(char const c : name) {
            // lower case only: derived modules are, and a filter rule must be able to match
            bool const ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'
                         || c == ':' || c == '/' || c == '-';
            if(!ok) { return false; }
        }
        return true;
    }

    // Chars, not a type: the filter's result must be usable in a constant expression.
    struct DerivedModule {
        std::array<char, MaxModuleName> chars{};
        std::size_t                     size{0};

        constexpr std::string_view view() const { return {chars.data(), size}; }
    };

    constexpr bool isIdentChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '_';
    }

    inline constexpr std::string_view ClangAnonymous{"(anonymous namespace)"};

    // Offsets into the first ScanLimit characters; `atTemplate`: stopped at a class template,
    // the function's own name is not seen.
    struct SignatureScan {
        static constexpr std::size_t MaxComponents = 16;
        static constexpr std::size_t ScanLimit     = 512;

        std::array<std::size_t, MaxComponents> compBegin{};
        std::array<std::size_t, MaxComponents> compEnd{};
        std::size_t                            comps{0};
        std::size_t                            openBegin{0};
        std::size_t                            openEnd{0};
        bool                                   found{false};
        bool                                   atTemplate{false};
    };

    // One pass up to the parameter list: a space, '*' or '&' outside <> drops the return type,
    // '::' ends a component, and the path ends at the first class template so gcc (Q<T>) and clang
    // (Q<int>) agree. Runs in the constant evaluator per call site, where reading
    // __PRETTY_FUNCTION__ is what costs compile time: read at most ScanLimit characters and never
    // walk a template argument list, or compiles get many times slower.
    constexpr SignatureScan scanSignature(std::string_view sig) {
        constexpr std::string_view Operator{"operator"};
        constexpr std::size_t      MaxComponents = SignatureScan::MaxComponents;
        constexpr auto             npos          = std::string_view::npos;
        constexpr auto&            ClangAnon     = ClangAnonymous;
        SignatureScan              scan{};
        SignatureScan const        result{};   // does not parse

        auto&       compBegin = scan.compBegin;
        auto&       compEnd   = scan.compEnd;
        auto&       comps     = scan.comps;
        std::size_t compStart = 0;
        bool&       found     = scan.found;

        sig           = sig.substr(0, SignatureScan::ScanLimit);
        auto const  n = sig.size();
        std::size_t i = 0;
        while(i < n) {
            char const c = sig[i];
            if(c == '<') {
                // followed by ' ', '*' or '&': the return type's; by '(': the function's own;
                // else a class template, which ends the path
                int         depth = 1;
                std::size_t p     = i + 1;
                for(; p < n && depth > 0; ++p) {
                    if(sig[p] == '<') { ++depth; }
                    if(sig[p] == '>') { --depth; }
                }
                if(depth == 0 && p < n && (sig[p] == ' ' || sig[p] == '*' || sig[p] == '&')) {
                    i = p;
                    continue;
                }
                if(depth == 0 && p < n && sig[p] == '(') {
                    scan.openBegin = compStart;
                    scan.openEnd   = i;
                    found          = true;
                    break;
                }
                if(comps < MaxComponents) {   // the class template is the last component kept
                    compBegin[comps] = compStart;
                    compEnd[comps]   = i;
                    ++comps;
                }
                scan.atTemplate = true;
                found           = true;
                break;
            }
            if(c == 'o' && (i == 0 || !isIdentChar(sig[i - 1]))
               && sig.substr(i, Operator.size()) == Operator
               && (i + Operator.size() >= n || !isIdentChar(sig[i + Operator.size()])))
            {
                // operator<, operator() ...: the symbol is no bracket
                i += Operator.size();
                if(sig.substr(i, 2) == "()") { i += 2; }
                auto const open = sig.find('(', i);
                if(open == npos) { return result; }
                scan.openBegin = compStart;
                scan.openEnd   = open;
                found          = true;
                break;
            }
            if(c == '(') {
                if(sig.substr(i, ClangAnon.size()) == ClangAnon) {
                    i += ClangAnon.size();
                    continue;
                }
                std::size_t w = i;
                while(w > compStart && isIdentChar(sig[w - 1])) { --w; }
                auto const word = sig.substr(w, i - w);
                if(word == "decltype" || word == "noexcept" || word == "__attribute__") {
                    int         depth = 1;   // skip the balanced parentheses
                    std::size_t p     = i + 1;
                    while(depth > 0) {
                        auto const lp = sig.find('(', p);
                        auto const rp = sig.find(')', p);
                        if(rp == npos) { return result; }
                        if(lp < rp) {
                            ++depth;
                            p = lp + 1;
                        } else {
                            --depth;
                            p = rp + 1;
                        }
                    }
                    i = p;
                    continue;
                }
                scan.openBegin = compStart;
                scan.openEnd   = i;
                found          = true;
                break;
            }
            if(c == ' ' || c == '*' || c == '&') {
                comps     = 0;   // what came before was the return type or "static"
                compStart = i + 1;
            } else if(c == ':' && i + 1 < n && sig[i + 1] == ':') {
                if(comps < MaxComponents) {
                    compBegin[comps] = compStart;
                    compEnd[comps]   = i;
                    ++comps;
                }
                compStart = i + 2;
                ++i;
            }
            ++i;
        }
        return found ? scan : result;
    }

    constexpr bool isAnonymous(std::string_view part) {
        return part == "{anonymous}" || part == ClangAnonymous;
    }

    constexpr DerivedModule moduleOf(SignatureScan const& scan,
                                     std::string_view     sig) {
        constexpr auto npos = std::string_view::npos;
        DerivedModule  result{};
        if(!scan.found) { return result; }
        sig        = sig.substr(0, SignatureScan::ScanLimit);
        bool first = true;
        for(std::size_t k = 0; k < scan.comps; ++k) {
            auto part = sig.substr(scan.compBegin[k], scan.compEnd[k] - scan.compBegin[k]);
            if(isAnonymous(part)) { continue; }
            bool const isFirst = first;   // the first after anonymous namespaces
            first              = false;
            if(auto const lt = part.find('<'); lt != npos) { part = part.substr(0, lt); }
            if(part.empty() || part == "detail" || part == "Detail"
               || (isFirst && part == "Kvasir"))
            {
                continue;
            }
            bool ident = true;
            for(char const ch : part) { ident = ident && isIdentChar(ch); }
            if(!ident) { continue; }
            std::size_t const need = part.size() + (result.size == 0 ? 0 : 1);
            // too long: a shorter path, not a cut-off name
            if(result.size + need > MaxModuleName) { break; }
            if(result.size != 0) { result.chars[result.size++] = '.'; }
            for(char const ch : part) {
                result.chars[result.size++]
                  = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
            }
        }
        return result;
    }

    // Tells instantiations apart: Device<FakeBusFor<>, FakeClockT<>, Tca9548a, ...>::finishVerify_
    struct FunctionName {
        static constexpr std::size_t Capacity = 192;

        std::array<char, Capacity> chars{};
        std::size_t                size{0};

        constexpr std::string_view view() const { return {chars.data(), size}; }
    };

    // `argRead`: how much of the signature the template arguments are read from.
    constexpr FunctionName qualifiedFunction(SignatureScan const& scan,
                                             std::string_view     sig,
                                             std::string_view     function,
                                             std::size_t          argRead = 1024) {
        constexpr auto        npos     = std::string_view::npos;
        std::size_t const     ArgRead  = argRead;
        constexpr std::size_t ArgChars = 100;
        FunctionName          result{};
        auto                  put = [&](char ch) {
            if(result.size + 1 > FunctionName::Capacity) { return; }
            result.chars[result.size++] = ch;
        };
        auto putAll = [&](std::string_view text) {
            for(char const ch : text) { put(ch); }
        };

        auto putArguments = [&](std::size_t open) {
            std::size_t const limit      = sig.size() < ArgRead ? sig.size() : ArgRead;
            std::size_t const start      = result.size;
            std::size_t       tokenStart = result.size + 1;
            int               depth      = 0;
            for(std::size_t i = open; i < limit; ++i) {
                char const c = sig[i];
                if(result.size - start >= ArgChars) {
                    putAll("...>");
                    return;
                }
                if(c == '<') {
                    ++depth;
                    if(depth == 1) {
                        put('<');
                        tokenStart = result.size;
                    } else if(depth == 2) {
                        put('<');
                    }
                    continue;
                }
                if(c == '>') {
                    --depth;
                    if(depth == 1) { put('>'); }
                    if(depth == 0) {
                        put('>');
                        return;
                    }
                    continue;
                }
                if(depth != 1) { continue; }
                if(c == ':' && i + 1 < limit && sig[i + 1] == ':') {
                    result.size = tokenStart;   // drop the qualifier
                    ++i;
                    continue;
                }
                put(c);
                if(c == ',' || c == ' ' || c == '(' || c == ')') { tokenStart = result.size; }
            }
            putAll("...>");
        };

        sig = sig.substr(0, ArgRead);
        if(!scan.found) {
            putAll(function);
            return result;
        }
        std::size_t kept  = SignatureScan::MaxComponents;
        bool        first = true;
        for(std::size_t k = 0; k < scan.comps; ++k) {
            auto part = sig.substr(scan.compBegin[k], scan.compEnd[k] - scan.compBegin[k]);
            if(isAnonymous(part)) { continue; }
            bool const isFirst = first;   // Kvasir is dropped where moduleOf drops it
            first              = false;
            if(auto const lt = part.find('<'); lt != npos) { part = part.substr(0, lt); }
            if(part.empty() || part == "detail" || part == "Detail"
               || (isFirst && part == "Kvasir"))
            {
                continue;
            }
            kept = k;
        }
        if(kept != SignatureScan::MaxComponents) {
            auto const begin = scan.compBegin[kept];
            auto const end   = scan.compEnd[kept];
            auto const part  = sig.substr(begin, end - begin);
            auto const lt    = part.find('<');
            putAll(lt == npos ? part : part.substr(0, lt));
            if(lt != npos) {
                putArguments(begin + lt);
            } else if(scan.atTemplate && kept + 1 == scan.comps && end < sig.size()
                      && sig[end] == '<')
            {
                putArguments(end);
            }
            putAll("::");
        }
        if(scan.atTemplate) {
            putAll(function);
        } else {
            auto open = sig.substr(scan.openBegin, scan.openEnd - scan.openBegin);
            if(auto const lt = open.find('<'); lt != npos && !open.starts_with("operator")) {
                open = open.substr(0, lt);
            }
            putAll(open);
            if(function == "operator()" && open != "operator()") { putAll("::lambda"); }
        }
        return result;
    }

    struct SignatureNames {
        DerivedModule module;
        FunctionName  function;
    };

    // `function` is what __FUNCTION__ was in it ("operator()" in a lambda).
    constexpr SignatureNames namesOf(std::string_view signature,
                                     std::string_view function,
                                     std::size_t      argRead = 1024) {
        SignatureScan const scan = scanSignature(signature);
        return {moduleOf(scan, signature), qualifiedFunction(scan, signature, function, argRead)};
    }

    // __FUNCTION__ of a demangled signature (a log tag's symbol, remote_fmt tools/extract_sites.py):
    // the name before the parameter list, "operator()" inside a lambda.
    constexpr std::string_view functionOfDemangled(std::string_view sig) {
        constexpr auto npos = std::string_view::npos;
        std::string_view constexpr OperatorChars{"<>=-!+*/%^&|~,"};
        if(sig.find("'lambda") != npos || sig.find("{lambda(") != npos) { return "operator()"; }
        std::size_t open   = sig.size();
        std::size_t opName = npos;   // an operator's name, whose '<' '>' '(' are no brackets
        std::size_t opEnd  = npos;
        int         depth  = 0;
        for(std::size_t i = 0; i < sig.size(); ++i) {
            char const c = sig[i];
            if(depth == 0 && sig.substr(i).starts_with("operator")
               && (i == 0 || sig[i - 1] == ':' || sig[i - 1] == ' '))
            {
                auto const  rest = sig.substr(i + 8);
                std::size_t j    = i + 8;
                if(rest.starts_with("()") || rest.starts_with("[]")) {
                    j += 2;
                } else {
                    while(j < sig.size() && OperatorChars.find(sig[j]) != npos) { ++j; }
                }
                if(j != i + 8) {
                    opName = i;
                    opEnd  = j;
                    i      = j - 1;
                    continue;
                }
            }
            if(c == '<') { ++depth; }
            if(c == '>') { --depth; }
            if(c == '(' && depth == 0) {
                if(sig.substr(i).starts_with(ClangAnonymous)) {
                    i += ClangAnonymous.size() - 1;
                    continue;
                }
                open = i;
                break;
            }
        }
        if(opName != npos && opName < open) { return sig.substr(opName, opEnd - opName); }
        auto head = sig.substr(0, open);
        if(head.ends_with('>')) {   // a function template's arguments
            int d = 0;
            for(std::size_t i = head.size(); i-- > 0;) {
                if(head[i] == '>') { ++d; }
                if(head[i] == '<' && --d == 0) {
                    head = head.substr(0, i);
                    break;
                }
            }
        }
        if(auto const colons = head.rfind("::"); colons != npos) { head = head.substr(colons + 2); }
        if(auto const space = head.rfind(' '); space != npos && !head.starts_with("operator")) {
            head = head.substr(space + 1);   // a function template's return type
        }
        return head;
    }

    constexpr SignatureNames namesOfDemangled(std::string_view signature) {
        return namesOf(signature, functionOfDemangled(signature), signature.size());
    }
}}   // namespace uc_log::detail
