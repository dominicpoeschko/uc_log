// The RTXUI window's theme (rtx/App.hpp Detail::theme): a template names a colour by what it is for, var(--name), and
// gets the colour itself at compile time. Case 0 (must build) themes a text and checks it at compile time; case 1 names
// a colour the theme does not have and must not compile. Keep in step with tests/CMakeLists.txt.
#include "uc_log/printer/rtx/App.hpp"

namespace theme = uc_log::printer::rtx::Detail;

#if UC_LOG_FAIL_CASE == 0
constexpr std::string_view      source{"a { color: var(--accent); background-color: var(--bg); }"};
[[maybe_unused]] constexpr auto themed = theme::themed<theme::themedSize(source)>(source);
static_assert(themed.view()
                == "a { color: rgb(110, 190, 230); background-color: rgb(18, 20, 25); }",
              "each var(--name) its colour, the rest as it was");
#else
constexpr std::string_view      source{"a { color: var(--no-such-colour); }"};
[[maybe_unused]] constexpr auto themed = theme::themed<theme::themedSize(source)>(source);
#endif

int main() { return 0; }
