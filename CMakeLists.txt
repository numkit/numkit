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

# ── Codegen AOT Config ───────────────────────────────────────────────────
set(NUMKIT_CODEGEN_AOT_AVAILABLE 0)
set(NUMKIT_CODEGEN_AOT_IS_MSVC 0)
set(NUMKIT_CODEGEN_AOT_VCVARS "")
set(NUMKIT_CODEGEN_AOT_CXX "${CMAKE_CXX_COMPILER}")
if(MSVC AND CMAKE_GENERATOR_INSTANCE)
    set(_nk_vcvars "${CMAKE_GENERATOR_INSTANCE}/VC/Auxiliary/Build/vcvars64.bat")
    if(EXISTS "${_nk_vcvars}")
        set(NUMKIT_CODEGEN_AOT_AVAILABLE 1)
        set(NUMKIT_CODEGEN_AOT_IS_MSVC 1)
        set(NUMKIT_CODEGEN_AOT_VCVARS "${_nk_vcvars}")
    endif()
elseif(NOT MSVC AND CMAKE_CXX_COMPILER)
    set(NUMKIT_CODEGEN_AOT_AVAILABLE 1)
endif()
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/codegen/aot_config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/numkit/codegen/aot_config.hpp
    @ONLY
)

# ── Gather Sources ───────────────────────────────────────────────────────
file(GLOB_RECURSE NUMKIT_SOURCES
    "src/core/*.cpp"
    "src/runtime/*.cpp"
    "src/builtin/*.cpp"
    "src/pkg/*.cpp"
    "src/ode/*.cpp"
    "src/optim/*.cpp"
    "src/io/*.cpp"
    "src/graphics/*.cpp"
    "src/scriptgraph/*.cpp"
    "src/codegen/*.cpp"
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
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/builtin
        ${CMAKE_CURRENT_SOURCE_DIR}/src/builtin/register
        ${CMAKE_CURRENT_SOURCE_DIR}/src/builtin/register/support
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ode
        ${CMAKE_CURRENT_SOURCE_DIR}/src/optim
        ${CMAKE_CURRENT_SOURCE_DIR}/src/io
        ${CMAKE_CURRENT_SOURCE_DIR}/src/graphics
        ${CMAKE_CURRENT_SOURCE_DIR}/src/scriptgraph
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codegen
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

# ── Executables: numkit CLI / REPL & numkit_codegen ──────────────────────
if(NUMKIT_BUILD_CLI)
    add_executable(numkit_cli apps/numkit/main.cpp)
    set_target_properties(numkit_cli PROPERTIES
        RUNTIME_OUTPUT_NAME numkit
        ARCHIVE_OUTPUT_NAME numkit_cli
    )
    target_link_libraries(numkit_cli PRIVATE numkit)

    if(NOT EMSCRIPTEN)
        add_executable(numkit_codegen apps/numkit_codegen/main.cpp)
        target_link_libraries(numkit_codegen PRIVATE numkit)
    endif()
endif()

# ── Testing: numkit_gtest ────────────────────────────────────────────────
if(NUMKIT_BUILD_TESTS)
    enable_testing()

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    set(NUMKIT_TEST_SOURCES
        tests/gtest/main.cpp
        tests/gtest/plugin_loader_test.cpp
        tests/gtest/builtins_test.cpp
        tests/gtest/lang_test.cpp
        tests/gtest/matrix_test.cpp
        tests/gtest/ops_test.cpp
        tests/gtest/numeric_test.cpp
        tests/gtest/package_manager_test.cpp
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
endif()
