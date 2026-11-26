cmake_minimum_required(VERSION 3.21)
project(numkit VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    add_compile_options(/MP /bigobj)
else()
    add_compile_options(-Wall -Wextra -fPIC)
endif()

option(NUMKIT_WITH_SIMD   "Enable Highway SIMD acceleration" ON)
option(NUMKIT_BUILD_CLI   "Build numkit command-line executable" ON)
option(NUMKIT_BUILD_TESTS "Build GoogleTest suite" ON)

include(FetchContent)

# ── Dependency: numkit/value ─────────────────────────────────────────────
if(NOT TARGET numkit_value)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../value/CMakeLists.txt")
        message(STATUS "Using local numkit/value from ${CMAKE_CURRENT_SOURCE_DIR}/../value")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../value" _deps/numkit_value)
    else()
        FetchContent_Declare(
            numkit_value
            GIT_REPOSITORY https://github.com/numkit/value.git
            GIT_TAG main
        )
        FetchContent_MakeAvailable(numkit_value)
    endif()
endif()

# ── Dependency: numkit/sys ───────────────────────────────────────────────
if(NOT TARGET numkit_sys)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../sys/CMakeLists.txt")
        message(STATUS "Using local numkit/sys from ${CMAKE_CURRENT_SOURCE_DIR}/../sys")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../sys" _deps/numkit_sys)
    else()
        FetchContent_Declare(
            numkit_sys
            GIT_REPOSITORY https://github.com/numkit/sys.git
            GIT_TAG main
        )
        FetchContent_MakeAvailable(numkit_sys)
    endif()
endif()

# ── Dependency: numkit/ops ───────────────────────────────────────────────
if(NOT TARGET numkit_ops)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../ops/CMakeLists.txt")
        message(STATUS "Using local numkit/ops from ${CMAKE_CURRENT_SOURCE_DIR}/../ops")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../ops" _deps/numkit_ops)
    else()
        FetchContent_Declare(
            numkit_ops
            GIT_REPOSITORY https://github.com/numkit/ops.git
            GIT_TAG main
        )
        FetchContent_MakeAvailable(numkit_ops)
    endif()
endif()

# ── Dependency: keel::matfile ────────────────────────────────────────────
if(NOT TARGET keel::matfile)
    find_package(keel_matfile QUIET CONFIG)
    if(TARGET keel::keel_matfile AND NOT TARGET keel::matfile)
        add_library(keel::matfile ALIAS keel::keel_matfile)
    endif()
endif()
if(NOT TARGET keel::matfile)
    if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_KEEL_MATFILE)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../keelcpp/matfile/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_KEEL_MATFILE
                "${CMAKE_CURRENT_SOURCE_DIR}/../../../keelcpp/matfile"
                CACHE PATH "")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../keelcpp/matfile/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_KEEL_MATFILE
                "${CMAKE_CURRENT_SOURCE_DIR}/../../keelcpp/matfile"
                CACHE PATH "")
        endif()
    endif()
    set(KEEL_MATFILE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        keel_matfile
        GIT_REPOSITORY https://github.com/keelcpp/matfile.git
        GIT_TAG        b4c9d4c8fd68e506dd1a374c6da431b7ec5fda08
    )
    FetchContent_MakeAvailable(keel_matfile)
endif()

# ── Dependency: keel::crypto ─────────────────────────────────────────────
if(NOT TARGET keel::crypto)
    find_package(keel_crypto QUIET CONFIG)
    if(TARGET keel::keel_crypto AND NOT TARGET keel::crypto)
        add_library(keel::crypto ALIAS keel::keel_crypto)
    endif()
endif()
if(NOT TARGET keel::crypto)
    if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_KEEL_CRYPTO)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../keelcpp/crypto/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_KEEL_CRYPTO
                "${CMAKE_CURRENT_SOURCE_DIR}/../../../keelcpp/crypto"
                CACHE PATH "")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../keelcpp/crypto/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_KEEL_CRYPTO
                "${CMAKE_CURRENT_SOURCE_DIR}/../../keelcpp/crypto"
                CACHE PATH "")
        endif()
    endif()
    set(KEEL_CRYPTO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        keel_crypto
        GIT_REPOSITORY https://github.com/keelcpp/crypto.git
        GIT_TAG        3f825bff7da1bf774f55b3598111c9b1a58d54c3
    )
    FetchContent_MakeAvailable(keel_crypto)
endif()

# ── Dependency: keel::compress ───────────────────────────────────────────
if(NOT TARGET keel::compress)
    find_package(keel_compress QUIET CONFIG)
    if(TARGET keel::keel_compress AND NOT TARGET keel::compress)
        add_library(keel::compress ALIAS keel::keel_compress)
    endif()
endif()
if(NOT TARGET keel::compress)
    if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_KEEL_COMPRESS)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../keelcpp/compress/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_KEEL_COMPRESS
                "${CMAKE_CURRENT_SOURCE_DIR}/../../../keelcpp/compress"
                CACHE PATH "")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../keelcpp/compress/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_KEEL_COMPRESS
                "${CMAKE_CURRENT_SOURCE_DIR}/../../keelcpp/compress"
                CACHE PATH "")
        endif()
    endif()
    set(KEEL_COMPRESS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        keel_compress
        GIT_REPOSITORY https://github.com/keelcpp/compress.git
        GIT_TAG        404bc3321bc0f973479b7752c9614c666d59843a
    )
    FetchContent_MakeAvailable(keel_compress)
endif()

# ── Dependency: keel::http ───────────────────────────────────────────────
if(NOT TARGET keel::http)
    find_package(keel_http QUIET CONFIG)
    if(TARGET keel::keel_http AND NOT TARGET keel::http)
        add_library(keel::http ALIAS keel::keel_http)
    endif()
endif()
if(NOT TARGET keel::http)
    if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_KEEL_HTTP)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../../keelcpp/http/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_KEEL_HTTP
                "${CMAKE_CURRENT_SOURCE_DIR}/../../../keelcpp/http"
                CACHE PATH "")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../keelcpp/http/CMakeLists.txt")
            set(FETCHCONTENT_SOURCE_DIR_KEEL_HTTP
                "${CMAKE_CURRENT_SOURCE_DIR}/../../keelcpp/http"
                CACHE PATH "")
        endif()
    endif()
    set(KEEL_HTTP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        keel_http
        GIT_REPOSITORY https://github.com/keelcpp/http.git
        GIT_TAG        821d3ea93ed2c728da38c09651e9f1d88f975a73
    )
    FetchContent_MakeAvailable(keel_http)
endif()

# ── Gather Sources ───────────────────────────────────────────────────────
file(GLOB_RECURSE NUMKIT_SOURCES CONFIGURE_DEPENDS
    "src/core/*.cpp"
    "src/runtime/*.cpp"
    "src/builtin/*.cpp"
    "src/pkg/*.cpp"
    "src/io/*.cpp"
    "src/graphics/*.cpp"
)

if(NOT NUMKIT_WITH_SIMD)
    list(FILTER NUMKIT_SOURCES EXCLUDE REGEX ".*_highway\\.cpp$")
else()
    list(FILTER NUMKIT_SOURCES EXCLUDE REGEX ".*_portable\\.cpp$")
endif()

# ── Library: numkit ──────────────────────────────────────────────────────
add_library(numkit STATIC ${NUMKIT_SOURCES})
add_library(numkit::numkit ALIAS numkit)
add_library(numkit::core ALIAS numkit)

target_include_directories(numkit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/builtin
        ${CMAKE_CURRENT_SOURCE_DIR}/src/builtin/register
        ${CMAKE_CURRENT_SOURCE_DIR}/src/builtin/register/support
        ${CMAKE_CURRENT_SOURCE_DIR}/src/io
        ${CMAKE_CURRENT_SOURCE_DIR}/src/graphics
)

target_link_libraries(numkit
    PUBLIC
        numkit_value
        numkit_sys
        numkit_ops
    PRIVATE
        keel::matfile
        keel::compress
        keel::crypto
        keel::http
)


if(NUMKIT_WITH_SIMD)
    target_compile_definitions(numkit PUBLIC NUMKIT_WITH_SIMD=1)
    target_link_libraries(numkit PRIVATE hwy hwy_contrib)
endif()

# ── Shared C-ABI Library: numkit_c ───────────────────────────────────────
add_library(numkit_c SHARED
    src/c_api/c_api.cpp
)
add_library(numkit::c ALIAS numkit_c)
target_compile_definitions(numkit_c PRIVATE NUMKIT_C_BUILD=1)
target_include_directories(numkit_c
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(numkit_c
    PUBLIC  numkit_value_c
    PRIVATE numkit ${CMAKE_DL_LIBS}
)
if(APPLE)
    set_target_properties(numkit_c PROPERTIES
        BUILD_RPATH "@loader_path;@loader_path/.."
        INSTALL_RPATH "@loader_path"
    )
elseif(UNIX)
    set_target_properties(numkit_c PROPERTIES
        BUILD_RPATH "$ORIGIN;$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# ── Executable: numkit CLI / REPL ────────────────────────────────────────
if(NUMKIT_BUILD_CLI)
    add_executable(numkit_cli apps/numkit/main.cpp)
    set_target_properties(numkit_cli PROPERTIES
        RUNTIME_OUTPUT_NAME numkit
        ARCHIVE_OUTPUT_NAME numkit_cli
    )
    target_link_libraries(numkit_cli PRIVATE numkit)
    if(MSVC)
        target_link_options(numkit_cli PRIVATE /STACK:8388608)
    endif()
endif()

# ── Testing: numkit_gtest ────────────────────────────────────────────────
if(NUMKIT_BUILD_TESTS)
    enable_testing()

    if(NOT TARGET GTest::gtest)
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    set(NUMKIT_TEST_SOURCES
        tests/gtest/main.cpp
        tests/gtest/plugin_loader_test.cpp
        tests/gtest/builtins_test.cpp
        tests/gtest/decomposition_test.cpp
        tests/gtest/graphics_scene_test.cpp
        tests/gtest/lang_test.cpp
        tests/gtest/matrix_test.cpp
        tests/gtest/ops_test.cpp
        tests/gtest/numeric_test.cpp
        tests/gtest/package_manager_test.cpp
        tests/gtest/reactive_session_test.cpp
        tests/gtest/known_bugs_test.cpp
        tests/gtest/wave1_safety_test.cpp
        tests/gtest/wave2_vm_tw_independence_test.cpp
        tests/gtest/wave3_semantics_isolation_test.cpp
        tests/gtest/wave4_perf_and_debloat_test.cpp
    )

    add_executable(numkit_gtest ${NUMKIT_TEST_SOURCES})
    target_include_directories(numkit_gtest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/gtest/support
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(numkit_gtest PRIVATE
        numkit
        keel::compress
        GTest::gtest
    )
    if(MSVC)
        target_link_options(numkit_gtest PRIVATE /STACK:8388608)
    endif()

    include(GoogleTest)
    gtest_discover_tests(numkit_gtest)

    add_executable(numkit_engine_c_test
        tests/c_api/engine_c_api.cpp
    )
    target_link_libraries(numkit_engine_c_test PRIVATE
        numkit_c
        GTest::gtest_main
    )
    add_test(NAME numkit_engine_c_test COMMAND numkit_engine_c_test)
endif()
