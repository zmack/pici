cmake_minimum_required(VERSION 3.28)
project(pi-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PI_CPP_BUILD_TESTS "Build tests" ON)

# ─── FetchContent dependencies ───
include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

set(JSON_VALIDATOR_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    json_schema_validator
    GIT_REPOSITORY https://github.com/pboettch/json-schema-validator.git
    GIT_TAG        2.3.0
)
FetchContent_MakeAvailable(json_schema_validator)

find_package(CURL QUIET)
if(NOT CURL_FOUND)
    set(CURL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TESTS ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)
    set(CURL_USE_WOLFSSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_BEARSSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_RUSTLS OFF CACHE BOOL "" FORCE)
    set(CMAKE_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(CMAKE_USE_SECTRANSP OFF CACHE BOOL "" FORCE)
    set(CMAKE_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
    set(CMAKE_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_ALTSVC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG        curl-8_7_1
    )
    FetchContent_MakeAvailable(curl)
endif()

# ─── libpi-core ───
add_library(pi-core
    src/core/message_types.cpp
    src/core/event_types.cpp
    src/core/agent_state.cpp
    src/core/stream.cpp
    src/core/agent_loop.cpp
    src/core/agent.cpp
    src/core/builtin_tools.cpp
    src/core/llm_client.cpp
    src/core/tool_validation.cpp
    src/core/env_api_keys.cpp
    src/core/providers/faux.cpp
    src/core/providers/transform_messages.cpp
)

target_include_directories(pi-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(pi-core PUBLIC
    PI_CPP_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(pi-core PUBLIC
    nlohmann_json::nlohmann_json
    nlohmann_json_schema_validator
)

# ─── pi-cli (main executable) ───
add_executable(pi-cli
    src/main.cpp
)

target_link_libraries(pi-cli PRIVATE pi-core pi-http)

# ─── Tests ───
if(PI_CPP_BUILD_TESTS)
    enable_testing()

    foreach(test_name IN ITEMS core agent_loop agent stream tool_validation builtin_tools)
        add_executable(test-${test_name}
            test/test_${test_name}.cpp
        )

        target_link_libraries(test-${test_name} PRIVATE pi-core)
        add_test(NAME test-${test_name} COMMAND test-${test_name})
    endforeach()

    add_executable(test-faux_client test/test_faux_client.cpp)
    target_link_libraries(test-faux_client PRIVATE pi-core)
    add_test(NAME test-faux_client COMMAND test-faux_client)

    add_executable(test-openai_completions test/test_openai_completions.cpp)
    target_link_libraries(test-openai_completions PRIVATE pi-http)
    add_test(NAME test-openai_completions COMMAND test-openai_completions)
endif()

add_library(pi-http
    src/http/http_client.cpp
    src/core/providers/openai_completions.cpp
)
target_include_directories(pi-http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pi-http PUBLIC pi-core CURL::libcurl)

# ─── clang-tidy ───────────────────────────────────────────────────────────────

find_program(CLANG_TIDY_BIN NAMES clang-tidy-23 clang-tidy-22 clang-tidy-20 clang-tidy)
find_program(RUN_CLANG_TIDY_BIN NAMES run-clang-tidy-23 run-clang-tidy-22 run-clang-tidy-20 run-clang-tidy)

if(CLANG_TIDY_BIN)
    # Regex passed to run-clang-tidy matching only our src/ files,
    # excluding FetchContent deps (which live under build/_deps) and tests.
    set(TIDY_REGEX "${CMAKE_CURRENT_SOURCE_DIR}/src/")

    if(RUN_CLANG_TIDY_BIN)
        add_custom_target(tidy
            COMMAND ${RUN_CLANG_TIDY_BIN}
                -clang-tidy-binary ${CLANG_TIDY_BIN}
                -p ${CMAKE_BINARY_DIR}
                ${TIDY_REGEX}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Running clang-tidy (parallel, ${CLANG_TIDY_BIN})"
            VERBATIM
        )
        add_custom_target(tidy-fix
            COMMAND ${RUN_CLANG_TIDY_BIN}
                -clang-tidy-binary ${CLANG_TIDY_BIN}
                -p ${CMAKE_BINARY_DIR}
                -fix
                ${TIDY_REGEX}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Running clang-tidy --fix (parallel)"
            VERBATIM
        )
    else()
        # Serial fallback — enumerate files manually
        file(GLOB_RECURSE TIDY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
        add_custom_target(tidy
            COMMAND ${CLANG_TIDY_BIN}
                -p ${CMAKE_BINARY_DIR}
                ${TIDY_SOURCES}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Running clang-tidy (serial, ${CLANG_TIDY_BIN})"
            VERBATIM
        )
    endif()

    message(STATUS "clang-tidy: ${CLANG_TIDY_BIN}  →  'cmake --build build --target tidy'")
else()
    message(STATUS "clang-tidy not found — skipping tidy target (install clang-tidy-23)")
endif()

# ─── clang-format ─────────────────────────────────────────────────────────────

find_program(CLANG_FORMAT_BIN NAMES clang-format-23 clang-format-22 clang-format-20 clang-format)

if(CLANG_FORMAT_BIN)
    # Keep this aligned with tidy: only format our src/ tree, not tests or deps.
    file(GLOB_RECURSE FORMAT_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
    )

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_BIN}
            -i
            ${FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-format (${CLANG_FORMAT_BIN})"
        VERBATIM
    )

    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_BIN}
            --dry-run
            --Werror
            ${FORMAT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Checking clang-format (${CLANG_FORMAT_BIN})"
        VERBATIM
    )

    message(STATUS "clang-format: ${CLANG_FORMAT_BIN}  →  'cmake --build build --target format'")
else()
    message(STATUS "clang-format not found — skipping format target (install clang-format-23)")
endif()
