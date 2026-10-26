#ifndef JXL_EXPORT_H
#define JXL_EXPORT_H

/* 1. Plattform-Erkennung für Symbole (Export / Import) */
#if defined(_WIN32) || defined(__CYGWIN__)
    /* --- Windows / MSVC / MinGW --- */
    #ifdef JXL_STATIC_DEFINE
        #define JXL_EXPORT
        #define JXL_NO_EXPORT
    #else
        #ifdef JXL_INTERNAL_LIBRARY_BUILD
            #define JXL_EXPORT __declspec(dllexport)
        #else
            #define JXL_EXPORT __declspec(dllimport)
        #endif
    #endif
#else
    /* --- Linux / macOS / GCC / Clang --- */
    #if __GNUC__ >= 4
        #define JXL_EXPORT __attribute__((visibility("default")))
        #define JXL_NO_EXPORT __attribute__((visibility("hidden")))
    #else
        #define JXL_EXPORT
        #define JXL_NO_EXPORT
    #endif
#endif

/* 2. Deprecated-Makros plattformübergreifend definieren */
#ifndef JXL_DEPRECATED
    #if defined(_MSC_VER)
        #define JXL_DEPRECATED __declspec(deprecated)
    #elif defined(__GNUC__) || defined(__clang__)
        #define JXL_DEPRECATED __attribute__((deprecated))
    #else
        #define JXL_DEPRECATED
    #endif
#endif

#ifndef JXL_DEPRECATED_EXPORT
#   define JXL_DEPRECATED_EXPORT JXL_EXPORT JXL_DEPRECATED
#endif

#ifndef JXL_DEPRECATED_NO_EXPORT
#   define JXL_DEPRECATED_NO_EXPORT JXL_NO_EXPORT JXL_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#   ifndef JXL_NO_DEPRECATED
#       define JXL_NO_DEPRECATED
#   endif
#endif

#endif /* JXL_EXPORT_H */
