cmake_minimum_required(VERSION 3.16)
project(JPEGView LANGUAGES C CXX RC)

# C++17 Standard erzwingen
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
    ApplyFilterAVX.cpp
    BasicProcessing.cpp
    Clipboard.cpp
    DirectoryWatcher.cpp
    EXIFReader.cpp
    FileList.cpp
    Helpers.cpp
    HelpersGUI.cpp
    HistogramCorr.cpp
    ICCProfileTransform.cpp
    ImageLoadThread.cpp
    JPEGImage.cpp
    JPEGProvider.cpp
    main.cpp
    KeyMap.cpp
    LocalDensityCorr.cpp
    MainDlg.cpp
    MultiMonitorSupport.cpp
    NLS.cpp
    ParameterDB.cpp
    ProcessingThreadPool.cpp
    ReaderBMP.cpp
    ReaderTGA.cpp
    ResizeFilter.cpp
    SettingsProvider.cpp
    stdafx.cpp
    WorkThread.cpp
	
    wrapperAVIF.cpp
	wrapperHEIF.cpp
    wrapperJXL.cpp
    wrapperPNG.cpp
    wrapperPSD.cpp
    wrapperQOI.cpp
    wrapperTJPEG.cpp
    wrapperWEBP.cpp
	
    XMMImage.cpp
    zip/zip.c
    JPEGView.rc
)

add_executable(JPEGView WIN32 ${SOURCES})

target_compile_definitions(JPEGView PRIVATE
    WIN32
    _WINDOWS
    STRICT
    BIT7Z_AUTO_FORMAT
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>
)

# Header-Suchpfade
target_include_directories(JPEGView PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/WTL-sf/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/libpng/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libjxl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libheif/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libwebp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/libavif/include
    ${CMAKE_CURRENT_SOURCE_DIR}/lcms2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/bit7z/include
)

# Bibliothekspfade
target_link_directories(JPEGView PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/libjpeg-turbo/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libpng/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libjxl/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libheif/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libwebp/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/libavif/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lcms2/${ARCH_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/bit7z/${ARCH_DIR}
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

# Precompiled Header (PCH)
target_precompile_headers(JPEGView PRIVATE stdafx.h)

# C-Datei zip/zip.c von PCH ausschliessen
set_source_files_properties(zip/zip.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Spezifische Einstellungen fuer den MSVC-Compiler
if(MSVC)
    # AVX2 fuer ApplyFilterAVX.cpp aktivieren
    set_source_files_properties(ApplyFilterAVX.cpp PROPERTIES COMPILE_FLAGS "/arch:AVX2")

    # Linker-Optionen: PDB-Warnung unterdruecken & konfligierende Release-Bibliothek LIBCMT im Debug-Build ausschliessen
    target_link_options(JPEGView PRIVATE
        "/IGNORE:4099"
        "$<$<CONFIG:Debug>:/NODEFAULTLIB:LIBCMT>"
    )
endif()
