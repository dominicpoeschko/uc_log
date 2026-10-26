// The printer's layering: every header under host/uc_log/printer/ outside tui/ compiles without FTXUI - a second
// front end needs only rendering (PrinterModel.hpp). The test's include path puts a poisoned copy of every FTXUI
// header first (each one an #error), so a model header that reaches FTXUI, directly or through another header, does
// not compile. Case 0 includes every model header (the list tests/CMakeLists.txt generates); case 1 includes a tui/
// header and must hit the poison, which shows the poison works. Keep in step with tests/CMakeLists.txt.
#if UC_LOG_FAIL_CASE == 0
    #include UC_LOG_PRINTER_HEADERS
#else
    #include "uc_log/printer/tui/Widgets.hpp"
#endif

int main() { return 0; }
