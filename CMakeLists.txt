cmake_minimum_required(VERSION 3.16)
project(mkImageViewer LANGUAGES C CXX)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "This project only supports 64-bit systems.")
endif()

# Kein RPATH: alle dynamischen Bibliotheken kommen aus dem System. CMake erzeugte sonst einen RUNPATH aus leeren
# Eintraegen, die der Loader als aktuelles Arbeitsverzeichnis deutet (SKIP_BUILD_RPATH am Target wirkte nicht).
set(CMAKE_SKIP_RPATH ON)

# C++20 Standard erzwingen
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Plattform dependent folder mapping for libs
set(ARCH_DIR "lib64")

# --- 1. GLOBALE PERFORMANCE FLAGS & BUILD TYPE ---
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT result)
if(result)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
endif()

# -O3: Aggressive Optimierung
# -march=native: Nutzt SSE/AVX der aktuellen CPU
# -flto: Link Time Optimization (removed here, since enabled by "set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)" above
# -fomit-frame-pointer: Befreit ein Register fuer Daten (statt Stack-Tracking)
add_compile_options(
    $<$<NOT:$<CONFIG:Debug>>:-O3>
    $<$<NOT:$<CONFIG:Debug>>:-march=native>
    $<$<NOT:$<CONFIG:Debug>>:-fomit-frame-pointer>
)


# --- 2. QT & STANDARD SETUP ---
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Concurrent LinguistTools Network DBus)

# --- 3. DRITTANBIETER (ZLIB) GLOBAL BEREITSTELLEN ---
find_package(ZLIB REQUIRED)

# AVIF, HEIF, JPEG XL und Little CMS kommen aus den Systempaketen (Arch: libavif, libheif, libjxl, lcms2)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVIF REQUIRED IMPORTED_TARGET libavif)
pkg_check_modules(HEIF REQUIRED IMPORTED_TARGET libheif)
pkg_check_modules(JXL REQUIRED IMPORTED_TARGET libjxl libjxl_threads)
pkg_check_modules(LCMS2 REQUIRED IMPORTED_TARGET lcms2)

qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

qt_add_executable(mkImageViewer
    src/main.cpp

    src/BasicProcessing.h src/BasicProcessing.cpp
    src/Clipboard.h src/Clipboard.cpp
    src/Commands.h src/Commands.cpp
    src/DirectoryWatcher.h src/DirectoryWatcher.cpp
    src/EXIFReader.h src/EXIFReader.cpp
    src/FileList.h src/FileList.cpp
    src/Helpers.h src/Helpers.cpp
    src/HistogramCorr.h src/HistogramCorr.cpp
    src/ICCProfileTransform.h src/ICCProfileTransform.cpp
    src/ImageLoadThread.h src/ImageLoadThread.cpp
    src/ImageLoadResult.h
    src/BaseImage.h src/BaseImage.cpp
    src/ImageProvider.h src/ImageProvider.cpp
    src/KeyMap.h src/KeyMap.cpp
    src/MainDlg.h src/MainDlg.cpp
    src/MultiMonitorSupport.h src/MultiMonitorSupport.cpp
    src/ProcessingThreadPool.h src/ProcessingThreadPool.cpp
    src/ReaderBMP.h src/ReaderBMP.cpp
    src/ReaderTGA.h src/ReaderTGA.cpp
    src/RenameDialog.h src/RenameDialog.cpp
    src/ResizeFilter.h src/ResizeFilter.cpp
    src/SettingsProvider.h src/SettingsProvider.cpp
    src/WorkThread.h src/WorkThread.cpp
    src/wrapperAVIF.h src/wrapperAVIF.cpp
    src/wrapperHEIF.h src/wrapperHEIF.cpp
    src/wrapperJXL.h src/wrapperJXL.cpp
    src/wrapperPNG.h src/wrapperPNG.cpp
    src/wrapperQOI.h src/wrapperQOI.cpp
    src/wrapperTJPEG.h src/wrapperTJPEG.cpp
    src/wrapperWEBP.h src/wrapperWEBP.cpp
    src/XMMImage.h src/XMMImage.cpp
    src/zip/zip.h src/zip/zip.c

    src/ImageProcessingTypes.h
    src/LookupTables.h
    src/MaxImageDef.h
    src/ProcessParams.h

    src/WTLCompat.h

    res/resources.qrc
)

qt_add_translations(mkImageViewer
    TS_FILES
        i18n/mkImageViewer_de.ts
        i18n/mkImageViewer_en.ts
    RESOURCE_PREFIX
        "/i18n"
)

target_compile_definitions(mkImageViewer PRIVATE
    BIT7Z_AUTO_FORMAT
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# Header-Suchpfade (mit src/ erweitert)
target_include_directories(mkImageViewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libpng/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libwebp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bit7z/include
)

# Bibliotheken verknuepfen
target_link_libraries(mkImageViewer PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::DBus
    Qt6::Network
#static
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libbit7z/lib64/libbit7z64.a
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libjpeg-turbo/lib64/libturbojpeg.a
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libpng/lib64/libpng.a
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libpng/lib64/libzs.a
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libwebp/lib64/libwebp.a
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libwebp/lib64/libwebpdemux.a
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libwebp/lib64/libsharpyuv.a
# dynamic (Systempakete, siehe pkg_check_modules oben)
    PkgConfig::LCMS2
    PkgConfig::AVIF
    PkgConfig::HEIF
    PkgConfig::JXL
)

# Beschleunigt den Start unter Linux, indem ungenutzte Shared Libraries ignoriert werden
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(mkImageViewer PRIVATE -Wl,--as-needed)
    # Symbole der statisch gelinkten Bibliotheken (libpng 1.8, zlib, libjpeg-turbo, libwebp, ...) nicht exportieren.
    # Sonst landen Aufrufe von Qt/FreeType, die gegen die System-Versionen gebaut sind (z.B. libpng 1.6), in diesen
    # Kopien: libpng meldet dann "Application built with libpng-1.6.x but running with 1.8.0" und verweigert den Dienst.
    target_link_options(mkImageViewer PRIVATE -Wl,--exclude-libs,ALL)
endif()

install(TARGETS mkImageViewer
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

#qt_generate_deploy_app_script(
#    TARGET mkImageViewer
#    OUTPUT_SCRIPT deploy_script
#    NO_UNSUPPORTED_PLATFORM_ERROR
#)
#install(SCRIPT ${deploy_script})

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

if(LINUX)
    add_custom_command(TARGET mkImageViewer POST_BUILD
        # Erster Kopiervorgang
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:mkImageViewer>
            $ENV{HOME}/.local/bin/mkImageViewer

        COMMENT "Copying mkFolderWidget to ~/.local/bin/"
    )
endif()
