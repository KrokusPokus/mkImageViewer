#ifndef JXL_CMS_EXPORT_H
#define JXL_CMS_EXPORT_H

/* 1. Plattform-Erkennung für Symbole (Export / Import) */
#if defined(_WIN32) || defined(__CYGWIN__)
    /* --- Windows / MSVC / MinGW --- */
    #ifdef JXL_CMS_STATIC_DEFINE
        #define JXL_CMS_EXPORT
        #define JXL_CMS_NO_EXPORT
    #else
        #ifdef jxl_cms_EXPORTS
            /* We are building this library */
            #define JXL_CMS_EXPORT __declspec(dllexport)
        #else
            /* We are using this library */
            #define JXL_CMS_EXPORT __declspec(dllimport)
        #endif
    #endif
#else
    /* --- Linux / macOS / GCC / Clang --- */
    #if __GNUC__ >= 4
        #define JXL_CMS_EXPORT __attribute__((visibility("default")))
        #define JXL_CMS_NO_EXPORT __attribute__((visibility("hidden")))
    #else
        #define JXL_CMS_EXPORT
        #define JXL_CMS_NO_EXPORT
    #endif
#endif

/* 2. Deprecated-Makros plattformübergreifend definieren */
#ifndef JXL_CMS_DEPRECATED
    #if defined(_MSC_VER)
        #define JXL_CMS_DEPRECATED __declspec(deprecated)
    #elif defined(__GNUC__) || defined(__clang__)
        #define JXL_CMS_DEPRECATED __attribute__((deprecated))
    #else
        #define JXL_CMS_DEPRECATED
    #endif
#endif

#ifndef JXL_CMS_DEPRECATED_EXPORT
#   define JXL_CMS_DEPRECATED_EXPORT JXL_CMS_EXPORT JXL_CMS_DEPRECATED
#endif

#ifndef JXL_CMS_DEPRECATED_NO_EXPORT
#   define JXL_CMS_DEPRECATED_NO_EXPORT JXL_CMS_NO_EXPORT JXL_CMS_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#   ifndef JXL_CMS_NO_DEPRECATED
#       define JXL_CMS_NO_DEPRECATED
#   endif
#endif

#endif /* JXL_CMS_EXPORT_H */
