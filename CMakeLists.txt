cmake_minimum_required(VERSION 3.20)
project(MaDView LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT for single-exe distribution
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- md4c (third-party) ----
add_library(md4c STATIC third_party/md4c/md4c.c)
target_include_directories(md4c PUBLIC third_party/md4c)
if(MSVC)
    target_compile_options(md4c PRIVATE /W0)
    set_target_properties(md4c PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---- Core library (testable code, no WinMain / window) ----
add_library(MaDView_core STATIC
    src/parser.cpp
    src/layout.cpp
    src/file_loader.cpp
    src/theme.cpp
    src/syntax.cpp
    src/file_explorer.cpp
    src/toc.cpp
)
target_include_directories(MaDView_core PUBLIC src third_party/md4c)
target_compile_definitions(MaDView_core PUBLIC
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)
target_link_libraries(MaDView_core PUBLIC
    md4c d2d1 dwrite windowscodecs shlwapi comctl32
)
if(MSVC)
    target_compile_options(MaDView_core PRIVATE /W4 /utf-8)
endif()

# ---- Main executable ----
add_executable(MaDView WIN32
    src/main.cpp
    src/window.cpp
    src/renderer.cpp
    res/MaDView.rc
)
target_link_libraries(MaDView PRIVATE MaDView_core)
if(MSVC)
    target_compile_options(MaDView PRIVATE /W4 /utf-8)
endif()
# Disable auto-generated manifest (we provide our own via RC)
set_target_properties(MaDView PROPERTIES LINK_FLAGS "/MANIFEST:NO")

# ---- Tests ----
option(MADVIEW_BUILD_TESTS "Build tests" ON)
if(MADVIEW_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    )
    # Prevent GoogleTest from overriding our CRT settings
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(tests)
endif()
