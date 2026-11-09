// The printer's layering: every header under host/uc_log/printer/ outside rtx/ compiles without RTXUI - the window
// needs only rendering (PrinterModel.hpp). The test's include path puts a poisoned copy of every RTXUI header first
// (each one an #error), so a model header that reaches RTXUI, directly or through another header, does not compile.
// Case 0 includes every model header (the list tests/CMakeLists.txt generates); case 1 includes an rtx/ header, which
// must hit the poison - that shows the poison works. Keep in step with tests/CMakeLists.txt.
#if UC_LOG_FAIL_CASE == 0
    #include UC_LOG_PRINTER_HEADERS
#else
    #include "uc_log/printer/rtx/Rtxui.hpp"
#endif

int main() { return 0; }
