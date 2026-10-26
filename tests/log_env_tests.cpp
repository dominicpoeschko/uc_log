// Global floor at info. Each backend records "<name><level>" per entry.
#include "support/Check.hpp"
#include "uc_log/detail/LostRecord.hpp"
#include "uc_log/uc_log.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

static_assert(uc_log::minLevel == uc_log::LogLevel::info,
              "target must set UC_LOG_MIN_LEVEL=info");

namespace {

// Leaked on purpose: avoids global-constructor / exit-time-destructor warnings.
std::vector<std::string>& events() {
    static auto& v = *new std::vector<std::string>{};
    return v;
}

std::vector<std::string> take() {
    auto e = events();
    events().clear();
    return e;
}

using Events = std::vector<std::string>;

}   // namespace

namespace test {
struct Other {};

struct Counted {};
}   // namespace test

namespace {
// what the Counted backend saw: boots noted, the bytes of its records, losses settled
struct CountedSeen {
    int                    notes   = 0;
    int                    settles = 0;
    std::uint32_t          pending = 0;
    std::vector<std::byte> bytes;
};

CountedSeen& counted() {
    static auto& seen = *new CountedSeen{};
    return seen;
}
}   // namespace

namespace uc_log {

template<>
struct ComBackend<Tag::User> {
    template<LogLevel Level>
    static void initTransfer() {
        events().push_back("user" + std::to_string(static_cast<int>(Level)));
    }

    static void write(std::span<std::byte const>) {}
};

template<>
struct ComBackend<::test::Other> {
    template<LogLevel Level>
    static void initTransfer() {
        events().push_back("other" + std::to_string(static_cast<int>(Level)));
    }

    static void write(std::span<std::byte const>) {}
};

// LossPolicy::Counted's hooks as RecordFrame calls them (Sink.hpp): a boot noted per record, a pending loss told
template<>
struct ComBackend<::test::Counted> {
    struct Mark {
        std::uint32_t pending;
    };

    static void noteRecord() { ++counted().notes; }

    static Mark lossMark() { return Mark{counted().pending}; }

    static void lossSettle(Mark) {
        ++counted().settles;
        counted().pending = 0;
    }

    static void write(std::span<std::byte const> data) {
        counted().bytes.insert(counted().bytes.end(), data.begin(), data.end());
    }
};

template<>
struct LogClock<::test::Counted> {
    static constexpr std::chrono::milliseconds now() { return std::chrono::milliseconds{3}; }
};

template<>
struct LogClock<Tag::User> {
    static constexpr std::chrono::milliseconds now() { return std::chrono::milliseconds{1}; }
};

template<>
struct LogClock<::test::Other> {
    static constexpr std::chrono::milliseconds now() { return std::chrono::milliseconds{2}; }
};

}   // namespace uc_log

namespace Kvasir::I2C::detail {
template<typename T>
struct Probe {
    static constexpr std::string_view member() { return __PRETTY_FUNCTION__; }

    static constexpr std::string_view inLambda() {
        return [] { return std::string_view{__PRETTY_FUNCTION__}; }();
    }
};
}   // namespace Kvasir::I2C::detail

namespace MyApp::Reg {
constexpr char const* name() { return __PRETTY_FUNCTION__; }

constexpr int const& counterRef(char const*& signature) {
    signature                  = __PRETTY_FUNCTION__;
    static constexpr int value = 0;
    return value;
}

constexpr std::string_view counter() {
    char const* signature = nullptr;
    (void)counterRef(signature);
    return signature;
}
}   // namespace MyApp::Reg

namespace {
struct AnonProbe {
    static constexpr std::string_view member() { return __PRETTY_FUNCTION__; }
};
}   // namespace

using uc_log::LogLevel;
using uc_log::detail::EnvModule_t;
using uc_log::detail::EnvTag_t;
using uc_log::detail::lineEnabled;
using uc_log::detail::NoModule;

template<char... Chars>
using Name = sc::StringConstant<Chars...>;

// The lowest level a line in `Env` still gets through at.
template<LogLevel Level,
         typename Env>
consteval bool floorIs() {
    constexpr auto below = static_cast<LogLevel>(static_cast<std::uint8_t>(Level) - 1);
    return lineEnabled<Level, Env>({})
        && (Level == LogLevel::trace || !lineEnabled<below, Env>({}));
}

static_assert(floorIs<LogLevel::info,
                      uc_log_env_t>());
static_assert(std::is_same_v<EnvTag_t<uc_log_env_t>,
                             uc_log::Tag::User>);

namespace {

namespace global_scope {
    Events run() {
        UC_LOG_D("debug");
        UC_LOG_I("info");
        return take();
    }
}   // namespace global_scope

namespace quiet_driver {
    UC_LOG_SCOPE_MIN_LEVEL(warn);

    static_assert(floorIs<LogLevel::warn,
                          uc_log_env_t>());

    Events run() {
        UC_LOG_I("info");
        UC_LOG_W("warn");
        return take();
    }

    // An inner scope lowers the floor again - down to the global one, not below it.
    Events reopened() {
        UC_LOG_SCOPE_MIN_LEVEL(trace);
        static_assert(floorIs<LogLevel::info, uc_log_env_t>());
        UC_LOG_D("debug");
        UC_LOG_I("info");
        return take();
    }

    namespace nested {
        Events run() {
            UC_LOG_I("info");
            UC_LOG_E("error");
            return take();
        }
    }   // namespace nested
}   // namespace quiet_driver

struct Motor {
    UC_LOG_SCOPE_MIN_LEVEL(error);

    [[nodiscard]] Events run() const {
        UC_LOG_W("warn");
        UC_LOG_E("error");
        return take();
    }
};

template<typename T>
struct Channel {
    UC_LOG_ENV(::uc_log::setting::Backend<test::Other>,
               ::uc_log::setting::MinLevel<LogLevel::warn>);

    static_assert(floorIs<LogLevel::warn,
                          uc_log_env_t>());
    static_assert(std::is_same_v<EnvTag_t<uc_log_env_t>,
                                 test::Other>);

    [[nodiscard]] Events run() const {
        UC_LOG_I("info {}", T{});
        UC_LOG_W("warn {}", T{});
        return take();
    }
};

namespace routed {
    UC_LOG_SCOPE_BACKEND(test::Other);

    Events run() {
        UC_LOG_I("to other");
        {
            UC_LOG_SCOPE_BACKEND(uc_log::Tag::User);
            UC_LOG_I("back to user");
        }
        UC_LOG_I("to other again");
        return take();
    }

    // a nested namespace adds to the outer settings: the backend stays, the floor is its own
    namespace loud {
        UC_LOG_SCOPE_MIN_LEVEL(error);
        static_assert(std::is_same_v<EnvTag_t<uc_log_env_t>,
                                     test::Other>);

        Events run() {
            UC_LOG_W("warn");
            UC_LOG_E("error");
            return take();
        }
    }   // namespace loud
}   // namespace routed

// Without the catalog a record goes through the same frame as with it: its boot noted, a loss told in front of it.
namespace counted_scope {
    UC_LOG_SCOPE_BACKEND(test::Counted);

    bool contains(std::vector<std::byte> const& bytes,
                  std::string_view              text) {
        std::string const all(reinterpret_cast<char const*>(bytes.data()), bytes.size());
        return all.contains(text);
    }

    void run() {
        UC_LOG_I("first {}", 1);
        CHECK(counted().notes == 1, "inline record: its boot noted");
        CHECK(counted().settles == 0, "nothing lost, nothing told");
        CHECK(!contains(counted().bytes, "log records lost"), "no lost-record line");
        counted().pending = 5;
        counted().bytes.clear();
        UC_LOG_I("second {}", 2);
        CHECK(counted().notes == 2, "the second record noted");
        CHECK(counted().settles == 1, "the loss settled once");
        CHECK(contains(counted().bytes, "log records lost on this channel"),
              "the lost-record line is written");
        auto const all  = std::string(reinterpret_cast<char const*>(counted().bytes.data()),
                                      counted().bytes.size());
        auto const lost = all.find("log records lost");
        auto const own  = all.find("second {}");
        CHECK(lost != std::string::npos && own != std::string::npos && lost < own,
              "the loss before the line");
        CHECK(contains(counted().bytes, "uc_log"), "the lost-record line is module uc_log");
        // an inline string is ASCII: the '!' sign, the text the printer and the stress checker read (both signs:
        // printer/LostRecordLine.hpp, its tests in target_loss_tests and log_stress_check_tests)
        CHECK(contains(counted().bytes,
                       "! {} log records lost on this channel (ring full, or no debugger)"),
              "the lost-record line's inline text, sign '!'");
        namespace text = uc_log::detail::lost_record;
        CHECK(contains(counted().bytes,
                       std::string{text::inlineSign} + " {} " + std::string{text::phrase} + " "
                         + std::string{text::cause}),
              "the text is the shared one (uc_log/detail/LostRecord.hpp)");
    }
}   // namespace counted_scope

Events withEnvBlock() {
    UC_LOG_WITH_ENV(::uc_log::setting::Backend<test::Other>) {
        UC_LOG_I("in the block");
        UC_LOG_WITH_ENV(::uc_log::setting::MinLevel<LogLevel::crit>) { UC_LOG_E("dropped"); }
    }
    UC_LOG_I("after the block");
    return take();
}

// An unbraced if around the macro keeps its own else (clang-format would add the braces).
Events withEnvDanglingElse(bool condition) {
    // clang-format off
    // NOLINTBEGIN(readability-braces-around-statements) // the case is the unbraced if/else around the macro
    if(condition)
        UC_LOG_WITH_ENV(::uc_log::setting::Backend<test::Other>) UC_LOG_I("then");
    else
        UC_LOG_I("else");
    // NOLINTEND(readability-braces-around-statements)
    // clang-format on
    return take();
}

namespace modules {
    static_assert(std::is_same_v<EnvModule_t<uc_log_env_t>,
                                 NoModule>);

    namespace usb {
        UC_LOG_SCOPE_MODULE("usb");
        static_assert(std::is_same_v<EnvModule_t<uc_log_env_t>,
                                     Name<'u',
                                          's',
                                          'b'>>);

        namespace ep {   // a nested namespace inherits the module
            static_assert(std::is_same_v<EnvModule_t<uc_log_env_t>,
                                         Name<'u',
                                              's',
                                              'b'>>);
        }   // namespace ep

        struct Hub {   // a class replaces it, and it combines with the other settings
            UC_LOG_ENV(UC_LOG_MODULE("hub"),
                       ::uc_log::setting::MinLevel<LogLevel::warn>);
            static_assert(std::is_same_v<EnvModule_t<uc_log_env_t>,
                                         Name<'h',
                                              'u',
                                              'b'>>);
            static_assert(floorIs<LogLevel::warn,
                                  uc_log_env_t>());
        };

        [[maybe_unused]] void block() {
            UC_LOG_WITH_ENV(UC_LOG_MODULE("x")) {
                static_assert(std::is_same_v<EnvModule_t<uc_log_env_t>, Name<'x'>>);
            }
            static_assert(std::is_same_v<EnvModule_t<uc_log_env_t>, Name<'u', 's', 'b'>>);
        }
    }   // namespace usb

    template<typename E>
    consteval bool tailIs(std::string_view expected) {
        return uc_log::detail::headerTail<E>().stringView == expected;
    }

    static_assert(tailIs<uc_log_env_t>(R"(""")"));
    static_assert(tailIs<uc_log::Env<UC_LOG_MODULE("usb")>>(R"("usb", """)"));

    consteval bool namesAre(std::string_view signature,
                            std::string_view module,
                            std::string_view function) {
        auto const names = uc_log::detail::namesOf(signature, "f");
        return names.module.view() == module && names.function.view() == function;
    }

    static_assert(namesAre("",
                           "",
                           "f"));
    static_assert(namesAre("void Kvasir::I2C::f()",
                           "i2c",
                           "I2C::f"));
    static_assert(namesAre("const char *MyApp::Reg::f()",
                           "myapp.reg",
                           "Reg::f"));

    static_assert(uc_log::detail::validModuleName("kvasir::i2c/bus-0.x_y"));
    static_assert(!uc_log::detail::validModuleName(""));
    static_assert(!uc_log::detail::validModuleName("a\"b"));
    static_assert(!uc_log::detail::validModuleName("a,b"));
    static_assert(!uc_log::detail::validModuleName("a{b"));
    static_assert(!uc_log::detail::validModuleName("a b"));
    static_assert(!uc_log::detail::validModuleName("I2C"));
    static_assert(uc_log::detail::validModuleName(
      "012345678901234567890123456789012345678901234567890123456789012"));   // 63
    static_assert(!uc_log::detail::validModuleName(
      "0123456789012345678901234567890123456789012345678901234567890123"));
}   // namespace modules

// Signatures as gcc and clang print __PRETTY_FUNCTION__.
namespace derived {
    consteval bool derivesTo(std::string_view signature,
                             std::string_view module) {
        return uc_log::detail::moduleOf(uc_log::detail::scanSignature(signature), signature).view()
            == module;
    }

    // gcc
    static_assert(
      derivesTo("static std::array<int, N> Kvasir::I2C::detail::Q<T>::f(T) [with int N = 2; T = "
                "std::array<char, 3>]",
                "i2c.q"));
    static_assert(derivesTo(
      "Kvasir::I2C::detail::Q<std::array<char, 3> >::f<2>(std::array<char, 3>)::<lambda()>",
      "i2c.q"));
    static_assert(derivesTo("static void Kvasir::Nvic::onIsr()",
                            "nvic"));
    static_assert(derivesTo("void {anonymous}::A::g()",
                            "a"));
    // clang
    static_assert(
      derivesTo("static std::array<int, N> Kvasir::I2C::detail::Q<std::array<char, 3>>::f(T) [T = "
                "std::array<char, 3>, N = 2]",
                "i2c.q"));
    static_assert(derivesTo(
      "auto Kvasir::I2C::detail::Q<std::array<char, 3>>::f(std::array<char, 3>)::(lambda)::"
      "operator()() const [T = std::array<char, 3>]",
      "i2c.q"));
    static_assert(derivesTo("void (anonymous namespace)::A::g()",
                            "a"));
    static_assert(
      derivesTo("void Kvasir::USB::CDC::Mixin<Clock, Cfg, 3>::handleSetup(const Request&)",
                "usb.cdc.mixin"));
    static_assert(derivesTo("void MyApp::Regulator::step(Kvasir::Units::Celsius)",
                            "myapp.regulator"));
    static_assert(
      derivesTo("std::optional<Kvasir::StaticString<30>> Kvasir::Bootrom::detail::read(int)",
                "bootrom"));
    static_assert(derivesTo("void Kvasir::logBoot(ResetCause) [with ResetCause = Reset]",
                            ""));
    static_assert(derivesTo("int main()",
                            ""));
    static_assert(derivesTo("void __ubsan_handle_add_overflow_minimal()",
                            ""));
    static_assert(derivesTo("Kvasir::I2C::Bus<C, D>::Bus()",
                            "i2c.bus"));
    static_assert(derivesTo("bool Kvasir::I2C::Link::operator<(const Link&) const",
                            "i2c.link"));
    static_assert(derivesTo("void Kvasir::I2C::Engine::operator()(int)",
                            "i2c.engine"));
    static_assert(derivesTo("decltype(auto) Kvasir::Fault::Core::get()",
                            "fault.core"));
    static_assert(derivesTo("static void Kvasir::Detail::I2CBase<Cfg>::reset()",
                            "i2cbase"));
    // the path ends at the first class template: what is nested in it adds nothing
    static_assert(derivesTo("void Kvasir::A<int>::Inner::f()",
                            "a"));
    static_assert(derivesTo("void Kvasir::A<T>::Inner::f() [with T = int]",
                            "a"));   // gcc's form
    // the function's own template arguments, and a return type's
    static_assert(derivesTo("void Kvasir::I2C::f<3>(int)",
                            "i2c"));
    static_assert(derivesTo("std::array<int, 3> Kvasir::I2C::Bus<X>::read()",
                            "i2c.bus"));
    static_assert(derivesTo("void Kvasir::Ubsan::ubsanReport(std::string_view)",
                            "ubsan"));
    static_assert(derivesTo("void kvasir::detail::ubsanReport(std::string_view)",
                            "kvasir"));   // lowercase: not stripped
    static_assert(derivesTo("not a signature",
                            ""));
    static_assert(derivesTo("",
                            ""));
    // pointer and reference return types: clang glues `*`/`&` to the name, gcc to the type
    static_assert(derivesTo("const char *MyApp::Reg::f()",
                            "myapp.reg"));
    static_assert(derivesTo("const char* MyApp::Reg::f()",
                            "myapp.reg"));
    static_assert(derivesTo("int &MyApp::Reg::f()",
                            "myapp.reg"));
    static_assert(derivesTo("int& MyApp::Reg::f()",
                            "myapp.reg"));
    static_assert(derivesTo("int &&MyApp::Reg::f()",
                            "myapp.reg"));
    static_assert(derivesTo("int&& MyApp::Reg::f()",
                            "myapp.reg"));
    static_assert(derivesTo("const char *const MyApp::Reg::f()",
                            "myapp.reg"));
    static_assert(derivesTo("const char *MyApp::Reg::Table::name() const",
                            "myapp.reg.table"));
    static_assert(derivesTo("std::vector<int> &MyApp::Reg::f()",
                            "myapp.reg"));
    static_assert(derivesTo("std::vector<int>& MyApp::Reg::f()",
                            "myapp.reg"));
    static_assert(derivesTo("std::vector<int>* MyApp::Reg::f()",
                            "myapp.reg"));
    static_assert(derivesTo("int &MyApp::Reg::Bus<X>::get()",
                            "myapp.reg.bus"));
    static_assert(derivesTo("int& MyApp::Reg::Bus<T>::get() [with T = int]",
                            "myapp.reg.bus"));
    static_assert(derivesTo("Value &MyApp::Reg::Ptr::operator*() const",
                            "myapp.reg.ptr"));
    static_assert(derivesTo("Value& MyApp::Reg::Ptr::operator*() const",
                            "myapp.reg.ptr"));
    static_assert(derivesTo("Ptr *MyApp::Reg::Ptr::operator&()",
                            "myapp.reg.ptr"));
    static_assert(derivesTo("bool MyApp::Reg::Ptr::operator&&(const Ptr &) const",
                            "myapp.reg.ptr"));
    // Kvasir is dropped only at the front, as in the function name
    static_assert(derivesTo("void Foo::Kvasir::Bar::f()",
                            "foo.kvasir.bar"));
    static_assert(derivesTo("void Foo::Kvasir::f()",
                            "foo.kvasir"));
    // over the limit: whole trailing components go, the name is never cut in the middle
    static_assert(derivesTo(
      "void Aaaaaaaaaaaaaaaaaaaa::Bbbbbbbbbbbbbbbbbbbb::Cccccccccccccccccccc::Dddddddddd::f()",
      "aaaaaaaaaaaaaaaaaaaa.bbbbbbbbbbbbbbbbbbbb.cccccccccccccccccccc"));

    // the function name: what tells instantiations apart
    consteval bool namesFunction(std::string_view signature,
                                 std::string_view function,
                                 std::string_view expected) {
        auto const scan = uc_log::detail::scanSignature(signature);
        return uc_log::detail::qualifiedFunction(scan, signature, function).view() == expected;
    }

    static_assert(namesFunction(
      "static void Kvasir::I2C::Device<Kvasir::Test::FakeBusFor<>, Kvasir::Test::FakeClockT<>, "
      "Kvasir::I2C::Chips::Tca9548a, Kvasir::I2C::DefaultConfig>::finishVerify_(TimePoint)",
      "finishVerify_",
      "Device<FakeBusFor<>, FakeClockT<>, Tca9548a, DefaultConfig>::finishVerify_"));
    static_assert(namesFunction(
      "static void Kvasir::I2C::detail::Q<T>::f(T) [with int N = 2; T = int]",   // gcc
      "f",
      "Q<T>::f"));
    static_assert(
      namesFunction("void Kvasir::I2C::detail::logUp(std::string_view, std::uint8_t, bool)",
                    "logUp",
                    "I2C::logUp"));
    static_assert(namesFunction("static void Kvasir::Nvic::DefaultIsrs::onIsr()",
                                "onIsr",
                                "DefaultIsrs::onIsr"));
    static_assert(namesFunction("int main()",
                                "main",
                                "main"));
    static_assert(namesFunction("void (anonymous namespace)::helper(int)",
                                "helper",
                                "helper"));
    static_assert(
      namesFunction("auto Kvasir::USB::Device<C, 3>::handle(int)::(lambda)::operator()() const",
                    "operator()",
                    "Device<C, 3>::operator()"));   // behind a class template: __FUNCTION__
    static_assert(namesFunction("auto Kvasir::USB::Stack::run()::(lambda)::operator()() const",
                                "operator()",
                                "Stack::run::lambda"));
    static_assert(namesFunction("void Kvasir::X<Kvasir::Cfg{1, 2}>::f()",
                                "f",
                                "X<Cfg{1, 2}>::f"));
    static_assert(namesFunction("const char *MyApp::Reg::f()",
                                "f",
                                "Reg::f"));
    static_assert(namesFunction("int &MyApp::Reg::Bus<X>::get()",
                                "get",
                                "Bus<X>::get"));
    static_assert(namesFunction("Value &MyApp::Reg::Ptr::operator*() const",
                                "operator*",
                                "Ptr::operator*"));
    static_assert(namesFunction("void Foo::Kvasir::Bar::f()",
                                "f",
                                "Bar::f"));
    static_assert(namesFunction("void Foo::Kvasir::f()",
                                "f",
                                "Kvasir::f"));
    static_assert(namesFunction("void Kvasir::f()",
                                "f",
                                "f"));
    static_assert(namesFunction("not a signature",
                                "f",
                                "f"));

    // a template signature of kilobytes, like the drivers'
    consteval bool longSignature() {
        constexpr std::size_t     xs = 5000;
        std::array<char, xs + 64> buffer{};
        std::string_view const    head = "static void Kvasir::I2C::Bus<";
        std::string_view const    tail = ">::run() [with T = int]";
        std::size_t               n    = 0;
        for(char const c : head) { buffer[n++] = c; }
        for(std::size_t i = 0; i < xs; ++i) { buffer[n++] = (i % 7 == 0) ? ',' : 'x'; }
        for(char const c : tail) { buffer[n++] = c; }
        auto const sig = std::string_view{buffer.data(), n};
        auto const fn
          = uc_log::detail::qualifiedFunction(uc_log::detail::scanSignature(sig), sig, "run");
        return derivesTo(sig, "i2c.bus") && fn.view().starts_with("Bus<,xxxxxx,")
            && fn.view().ends_with("...>::run") && fn.size < 120;
    }

    static_assert(longSignature());

    // real signatures, as this compiler prints them
    static_assert(derivesTo(Kvasir::I2C::detail::Probe<std::array<char,
                                                                  3>>::member(),
                            "i2c.probe"));
    static_assert(derivesTo(Kvasir::I2C::detail::Probe<int>::inLambda(),
                            "i2c.probe"));
    static_assert(derivesTo(AnonProbe::member(),
                            "anonprobe"));
    static_assert(derivesTo(MyApp::Reg::name(),
                            "myapp.reg"));
    static_assert(derivesTo(MyApp::Reg::counter(),
                            "myapp.reg"));
}   // namespace derived

}   // namespace

int main() {
    CHECK((global_scope::run() == Events{"user2"}), "global: debug dropped, info to user");
    CHECK((quiet_driver::run() == Events{"user3"}), "namespace at warn: info dropped");
    CHECK((quiet_driver::reopened() == Events{"user2"}),
          "function lowers to trace: info back, debug still under the global floor");
    CHECK((quiet_driver::nested::run() == Events{"user4"}), "nested namespace inherits warn");
    CHECK((Motor{}.run() == Events{"user4"}), "class at error: warn dropped");
    CHECK((Channel<int>{}.run() == Events{"other3"}), "class template: warn, to other");
    CHECK((routed::run() == Events{"other2", "user2", "other2"}),
          "namespace backend, a block scope switches back");
    CHECK((routed::loud::run() == Events{"other4"}), "nested namespace keeps the backend");
    CHECK((withEnvBlock() == Events{"other2", "user2"}),
          "UC_LOG_WITH_ENV covers its block only, and nests");
    CHECK((withEnvDanglingElse(true) == Events{"other2"}), "UC_LOG_WITH_ENV under if: then");
    CHECK((withEnvDanglingElse(false) == Events{"user2"}),
          "UC_LOG_WITH_ENV under if: the caller's else binds to the caller's if");
    counted_scope::run();

    // operator names: their '<' '>' '(' are no brackets
    static_assert(uc_log::detail::functionOfDemangled("ns::operator<<(std::ostream&, X const&)")
                  == "operator<<");
    static_assert(uc_log::detail::functionOfDemangled("ns::X::operator->() const") == "operator->");
    static_assert(uc_log::detail::functionOfDemangled("bool ns::operator< <int>(A<int>, A<int>)")
                  == "operator<");
    static_assert(uc_log::detail::functionOfDemangled("ns::X::operator()(int)") == "operator()");
    static_assert(uc_log::detail::functionOfDemangled("ns::X::operator[](int)") == "operator[]");
    static_assert(uc_log::detail::functionOfDemangled("ns::X::operator bool() const")
                  == "operator bool");
    static_assert(uc_log::detail::functionOfDemangled("ns::f<int>(int)") == "f");
    static_assert(uc_log::detail::functionOfDemangled("no_parameter_list") == "no_parameter_list");

    // an argument longer than 1024 characters still closes
    {
        std::string signature = "(anonymous namespace)::BenchApp<Kvasir::I2C::Bus<";
        for(int i = 0; i < 300; ++i) { signature += "Arg, "; }
        signature += "Last>, HW::Config>::run()";
        auto const names = uc_log::detail::namesOfDemangled(signature);
        CHECK(names.function.view() == "BenchApp<Bus<>, Config>::run",
              "a long template argument is abbreviated, not cut");
        CHECK(names.module.view() == "benchapp", "its module");
    }

    return uc_log::test::report();
}
