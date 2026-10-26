cmake_minimum_required(VERSION 3.16)
project(mkImageViewer LANGUAGES C CXX)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "This project only supports 64-bit systems.")
endif()

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

qt_standard_project_setup()
set(CMAKE_AUTORCC ON)

qt_add_executable(mkImageViewer
    src/main.cpp
    src/ApplyFilterAVX.h src/ApplyFilterAVX.cpp
    src/BasicProcessing.h src/BasicProcessing.cpp
    src/Clipboard.h src/Clipboard.cpp
    src/DirectoryWatcher.h src/DirectoryWatcher.cpp
    src/EXIFReader.h src/EXIFReader.cpp
    src/FileList.h src/FileList.cpp
    src/Helpers.h src/Helpers.cpp
    src/HistogramCorr.h src/HistogramCorr.cpp
    src/ICCProfileTransform.h src/ICCProfileTransform.cpp
    src/ImageLoadThread.h src/ImageLoadThread.cpp
    src/JPEGImage.h src/JPEGImage.cpp
    src/JPEGProvider.h src/JPEGProvider.cpp
    src/KeyMap.h src/KeyMap.cpp
    src/LocalDensityCorr.h src/LocalDensityCorr.cpp
    src/MainDlg.h src/MainDlg.cpp
    src/MultiMonitorSupport.h src/MultiMonitorSupport.cpp
    src/ProcessingThreadPool.h src/ProcessingThreadPool.cpp
    src/ReaderBMP.h src/ReaderBMP.cpp
    src/ReaderTGA.h src/ReaderTGA.cpp
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
    src/resource.h
    src/WTLCompat.h

    res/resources.qrc
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
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libjxl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libheif/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libwebp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libavif/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lcms2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/bit7z/include
)

# Bibliothekspfade (mit src/ erweitert)
target_link_directories(mkImageViewer PRIVATE
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
# dynamic (Linux .so)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lcms2/lib64/liblcms2.so
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libavif/lib64/libavif.so
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libheif/lib64/libheif.so
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libjxl/lib64/libjxl_dec.so
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libjxl/lib64/libjxl_threads.so
)
