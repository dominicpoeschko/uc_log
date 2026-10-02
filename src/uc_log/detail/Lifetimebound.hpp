#pragma once

// Tells clang that a returned reference or a constructed object refers to this argument (or to *this), so its lifetime
// analysis can see a dangling use. Nothing on other compilers.
#if defined(__has_cpp_attribute)
    #if __has_cpp_attribute(clang::lifetimebound)
        #define UC_LOG_LIFETIMEBOUND [[clang::lifetimebound]]
    #endif
#endif
#ifndef UC_LOG_LIFETIMEBOUND
    #define UC_LOG_LIFETIMEBOUND
#endif
