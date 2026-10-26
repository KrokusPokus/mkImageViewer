#ifndef JXL_THREADS_EXPORT_H
#define JXL_THREADS_EXPORT_H

/* 1. Plattform-Erkennung für Symbole (Export / Import) */
#if defined(_WIN32) || defined(__CYGWIN__)
    /* --- Windows / MSVC / MinGW --- */
    #ifdef JXL_THREADS_STATIC_DEFINE
        #define JXL_THREADS_EXPORT
        #define JXL_THREADS_NO_EXPORT
    #else
        #ifdef JXL_THREADS_INTERNAL_LIBRARY_BUILD
            #define JXL_THREADS_EXPORT __declspec(dllexport)
        #else
            #define JXL_THREADS_EXPORT __declspec(dllimport)
        #endif
    #endif
#else
    /* --- Linux / macOS / GCC / Clang --- */
    #if __GNUC__ >= 4
        #define JXL_THREADS_EXPORT __attribute__((visibility("default")))
        #define JXL_THREADS_NO_EXPORT __attribute__((visibility("hidden")))
    #else
        #define JXL_THREADS_EXPORT
        #define JXL_THREADS_NO_EXPORT
    #endif
#endif

/* 2. Deprecated-Makros plattformübergreifend definieren */
#ifndef JXL_THREADS_DEPRECATED
    #if defined(_MSC_VER)
        #define JXL_THREADS_DEPRECATED __declspec(deprecated)
    #elif defined(__GNUC__) || defined(__clang__)
        #define JXL_THREADS_DEPRECATED __attribute__((deprecated))
    #else
        #define JXL_THREADS_DEPRECATED
    #endif
#endif

#ifndef JXL_THREADS_DEPRECATED_EXPORT
#   define JXL_THREADS_DEPRECATED_EXPORT JXL_THREADS_EXPORT JXL_THREADS_DEPRECATED
#endif

#ifndef JXL_THREADS_DEPRECATED_NO_EXPORT
#   define JXL_THREADS_DEPRECATED_NO_EXPORT JXL_THREADS_NO_EXPORT JXL_THREADS_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#   ifndef JXL_THREADS_NO_DEPRECATED
#       define JXL_THREADS_NO_DEPRECATED
#   endif
#endif

#endif /* JXL_THREADS_EXPORT_H */
