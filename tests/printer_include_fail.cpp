// A firmware target links only uc_log::uc_log (include root src/), and the printer's headers live under host/
// (uc_log::host): a firmware file that includes one does not compile. Case 0 is the control (a firmware header
// builds), case 1 includes a printer header and must fail to find it. Keep in step with tests/CMakeLists.txt.
#include "uc_log/uc_log.hpp"

#if UC_LOG_FAIL_CASE == 1
    #include "uc_log/printer/ControlServer.hpp"
#endif

int main() { return 0; }
