cmake_minimum_required(VERSION 3.16)
project(JPEGView LANGUAGES C CXX RC)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "This project only supports 64-bit systems.")
endif()

# Optional: Explizit Makros für den gesamten Code definieren
add_compile_definitions(PROJECT_IS_64BIT=1)
# C++20 Standard erzwingen
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC Statische C/C++-Laufzeitbibliothek (/MT fuer Release, /MTd fuer Debug) erzwingen
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Unicode-Unterstuetzung
add_compile_definitions(UNICODE _UNICODE)

# Plattform dependent folder mapping for libs (x64 -> lib64, x86 -> lib)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH_DIR "lib64")
else()
    set(ARCH_DIR "lib")
endif()

# --- 1. GLOBALE PERFORMANCE FLAGS & BUILD TYPE ---
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT result)
if(result)
    if(MINGW)
        # MinGW hat einen bekannten Bug mit LTO bei statischen Phasen ("plugin needed to handle lto object").
        # Da wir ohnehin -O3 nutzen, schalten wir IPO hier fuer Windows/MinGW ab.
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
    else()
        # Unter Linux (GCC/Clang) oder mit MSVC laeuft es sauber durch
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

if(MSVC)
    add_compile_options(/O2 /Ot /GL /arch:AVX2)
else()
    # -O3: Aggressive Optimierung
    # -march=native: Nutzt SSE/AVX der aktuellen CPU
    # -flto: Link Time Optimization (removed here, since enabled by "set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)" above
    # -fomit-frame-pointer: Befreit ein Register fuer Daten (statt Stack-Tracking)
    add_compile_options(-O3 -march=native -fomit-frame-pointer)
endif()

# --- 2. QT & STANDARD SETUP ---
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Concurrent LinguistTools Network DBus)
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

# Quelldateien definieren
set(SOURCES
    src/ApplyFilterAVX.cpp
    src/BasicProcessing.cpp
    src/Clipboard.cpp
    src/DirectoryWatcher.cpp
    src/EXIFReader.cpp
    src/FileList.cpp
    src/Helpers.cpp

    src/HistogramCorr.cpp
    src/ICCProfileTransform.cpp
    src/ImageLoadThread.cpp
    src/JPEGImage.cpp
    src/JPEGProvider.cpp
    src/main.cpp
    src/KeyMap.cpp
    src/LocalDensityCorr.cpp
    src/MainDlg.cpp
    src/MultiMonitorSupport.cpp
    src/NLS.cpp

    src/ProcessingThreadPool.cpp
    src/ReaderBMP.cpp
    src/ReaderTGA.cpp
    src/ResizeFilter.cpp
    src/SettingsProvider.cpp
    src/stdafx.cpp
    src/WorkThread.cpp
    
    src/wrapperAVIF.cpp
    src/wrapperHEIF.cpp
    src/wrapperJXL.cpp
    src/wrapperPNG.cpp
    src/wrapperPSD.cpp
    src/wrapperQOI.cpp
    src/wrapperTJPEG.cpp
    src/wrapperWEBP.cpp
    
    src/XMMImage.cpp
    src/zip/zip.c
)

# Windows-spezifische Ressourcen (.rc)
if(WIN32)
    enable_language(RC)
    list(APPEND SOURCES src/JPEGView.rc)
endif()

add_executable(JPEGView WIN32 ${SOURCES})

target_compile_definitions(JPEGView PRIVATE
    WIN32
    _WINDOWS
    STRICT
    BIT7Z_AUTO_FORMAT
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# Header-Suchpfade (mit src/ erweitert)
target_include_directories(JPEGView PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WTL-sf/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libpng/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libjxl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libheif/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libwebp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libavif/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lcms2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bit7z/include
)

# Bibliothekspfade (mit src/ erweitert)
target_link_directories(JPEGView PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libjpeg-turbo/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libpng/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libjxl/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libheif/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libwebp/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libavif/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lcms2/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bit7z/${ARCH_DIR}
)

# Bibliotheken verknuepfen
target_link_libraries(JPEGView PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::DBus
    Qt6::Network
    dbghelp
    gdiplus
    winmm
    turbojpeg-static
    libpng18_static
    zlib
    jxl_dec
    jxl_threads
    heif
    libwebp
    libwebpdemux
    avif
    lcms2
    bit7z
)

# Precompiled Header (PCH) - Pfad auf src/stdafx.h angepasst
target_precompile_headers(JPEGView PRIVATE src/stdafx.h)

# C-Datei src/zip/zip.c von PCH ausschliessen
set_source_files_properties(src/zip/zip.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Spezifische Einstellungen fuer den MSVC-Compiler
if(MSVC)
    # AVX2 fuer ApplyFilterAVX.cpp aktivieren
    set_source_files_properties(src/ApplyFilterAVX.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX2")

    # Linker-Optionen: PDB-Warnung unterdruecken & konfligierende Release-Bibliothek LIBCMT im Debug-Build ausschliessen
    target_link_options(JPEGView PRIVATE
        "/IGNORE:4099"
        "$<$<CONFIG:Debug>:/NODEFAULTLIB:LIBCMT>"
    )
endif()
