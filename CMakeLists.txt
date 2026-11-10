cmake_minimum_required(VERSION 3.21)
project(numkit VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
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

# ── Gather Sources ───────────────────────────────────────────────────────
file(GLOB_RECURSE NUMKIT_SOURCES
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
)

target_compile_definitions(numkit PUBLIC NUMKIT_CORE_BUILD=1)

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
        tests/gtest/lang_test.cpp
        tests/gtest/matrix_test.cpp
        tests/gtest/ops_test.cpp
        tests/gtest/numeric_test.cpp
        tests/gtest/package_manager_test.cpp
        tests/gtest/reactive_session_test.cpp
        tests/gtest/known_bugs_test.cpp
    )

    add_executable(numkit_gtest ${NUMKIT_TEST_SOURCES})
    target_include_directories(numkit_gtest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/gtest/support
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(numkit_gtest PRIVATE
        numkit
        GTest::gtest
    )

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
