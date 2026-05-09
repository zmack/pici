cmake_minimum_required(VERSION 3.28)
project(pi-cpp VERSION 0.1.0 LANGUAGES C CXX)

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

find_package(PkgConfig REQUIRED)

# ─── cmark (Markdown parser) ─────────────────────────────────────────────────
FetchContent_Declare(
    cmark_src
    GIT_REPOSITORY https://github.com/commonmark/cmark.git
    GIT_TAG        0.31.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cmark_src)
message(STATUS "cmark: 0.31.1")

# ─── Tree-sitter (syntax highlighting) ───────────────────────────────────────
FetchContent_Declare(
    tree_sitter_runtime_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG        v0.25.10
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    tree_sitter_c_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_cpp_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_python_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_bash_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-bash.git
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_json_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-json.git
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_typescript_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-typescript.git
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_rust_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-rust.git
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_go_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-go.git
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_markdown_src
    GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-markdown.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_ruby_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-ruby.git
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_lua_src
    GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-lua.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tree_sitter_javascript_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-javascript.git
    GIT_SHALLOW    TRUE
)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

set(HTTPLIB_REQUIRE_OPENSSL  OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB     OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE    OFF CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE          OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cpp_httplib)
FetchContent_GetProperties(tree_sitter_runtime_src)
if(NOT tree_sitter_runtime_src_POPULATED)
    FetchContent_Populate(tree_sitter_runtime_src)
endif()
FetchContent_GetProperties(tree_sitter_c_src)
if(NOT tree_sitter_c_src_POPULATED)
    FetchContent_Populate(tree_sitter_c_src)
endif()
FetchContent_GetProperties(tree_sitter_cpp_src)
if(NOT tree_sitter_cpp_src_POPULATED)
    FetchContent_Populate(tree_sitter_cpp_src)
endif()
FetchContent_GetProperties(tree_sitter_python_src)
if(NOT tree_sitter_python_src_POPULATED)
    FetchContent_Populate(tree_sitter_python_src)
endif()
FetchContent_GetProperties(tree_sitter_bash_src)
if(NOT tree_sitter_bash_src_POPULATED)
    FetchContent_Populate(tree_sitter_bash_src)
endif()
FetchContent_GetProperties(tree_sitter_json_src)
if(NOT tree_sitter_json_src_POPULATED)
    FetchContent_Populate(tree_sitter_json_src)
endif()
FetchContent_GetProperties(tree_sitter_typescript_src)
if(NOT tree_sitter_typescript_src_POPULATED)
    FetchContent_Populate(tree_sitter_typescript_src)
endif()
FetchContent_GetProperties(tree_sitter_rust_src)
if(NOT tree_sitter_rust_src_POPULATED)
    FetchContent_Populate(tree_sitter_rust_src)
endif()
FetchContent_GetProperties(tree_sitter_go_src)
if(NOT tree_sitter_go_src_POPULATED)
    FetchContent_Populate(tree_sitter_go_src)
endif()
FetchContent_GetProperties(tree_sitter_markdown_src)
if(NOT tree_sitter_markdown_src_POPULATED)
    FetchContent_Populate(tree_sitter_markdown_src)
endif()
FetchContent_GetProperties(tree_sitter_ruby_src)
if(NOT tree_sitter_ruby_src_POPULATED)
    FetchContent_Populate(tree_sitter_ruby_src)
endif()
FetchContent_GetProperties(tree_sitter_lua_src)
if(NOT tree_sitter_lua_src_POPULATED)
    FetchContent_Populate(tree_sitter_lua_src)
endif()
FetchContent_GetProperties(tree_sitter_javascript_src)
if(NOT tree_sitter_javascript_src_POPULATED)
    FetchContent_Populate(tree_sitter_javascript_src)
endif()

add_library(tree_sitter_pi STATIC
    ${tree_sitter_runtime_src_SOURCE_DIR}/lib/src/lib.c
)
target_include_directories(tree_sitter_pi PUBLIC
    ${tree_sitter_runtime_src_SOURCE_DIR}/lib/include
    ${tree_sitter_runtime_src_SOURCE_DIR}/lib/src
)
message(STATUS "tree-sitter runtime: v0.25.10")

add_library(ts-lang-c STATIC
    ${tree_sitter_c_src_SOURCE_DIR}/src/parser.c
)
target_include_directories(ts-lang-c PUBLIC
    ${tree_sitter_c_src_SOURCE_DIR}/src
)

set(TS_CPP_SOURCES
    ${tree_sitter_cpp_src_SOURCE_DIR}/src/parser.c
)
if(EXISTS ${tree_sitter_cpp_src_SOURCE_DIR}/src/scanner.c)
    list(APPEND TS_CPP_SOURCES ${tree_sitter_cpp_src_SOURCE_DIR}/src/scanner.c)
endif()
if(EXISTS ${tree_sitter_cpp_src_SOURCE_DIR}/src/scanner.cc)
    list(APPEND TS_CPP_SOURCES ${tree_sitter_cpp_src_SOURCE_DIR}/src/scanner.cc)
endif()
add_library(ts-lang-cpp STATIC ${TS_CPP_SOURCES})
target_include_directories(ts-lang-cpp PUBLIC
    ${tree_sitter_cpp_src_SOURCE_DIR}/src
)

set(TS_PYTHON_SOURCES
    ${tree_sitter_python_src_SOURCE_DIR}/src/parser.c
)
if(EXISTS ${tree_sitter_python_src_SOURCE_DIR}/src/scanner.c)
    list(APPEND TS_PYTHON_SOURCES ${tree_sitter_python_src_SOURCE_DIR}/src/scanner.c)
endif()
if(EXISTS ${tree_sitter_python_src_SOURCE_DIR}/src/scanner.cc)
    list(APPEND TS_PYTHON_SOURCES ${tree_sitter_python_src_SOURCE_DIR}/src/scanner.cc)
endif()
add_library(ts-lang-python STATIC ${TS_PYTHON_SOURCES})
target_include_directories(ts-lang-python PUBLIC
    ${tree_sitter_python_src_SOURCE_DIR}/src
)

set(TS_BASH_SOURCES
    ${tree_sitter_bash_src_SOURCE_DIR}/src/parser.c
)
if(EXISTS ${tree_sitter_bash_src_SOURCE_DIR}/src/scanner.c)
    list(APPEND TS_BASH_SOURCES ${tree_sitter_bash_src_SOURCE_DIR}/src/scanner.c)
endif()
if(EXISTS ${tree_sitter_bash_src_SOURCE_DIR}/src/scanner.cc)
    list(APPEND TS_BASH_SOURCES ${tree_sitter_bash_src_SOURCE_DIR}/src/scanner.cc)
endif()
add_library(ts-lang-bash STATIC ${TS_BASH_SOURCES})
target_include_directories(ts-lang-bash PUBLIC
    ${tree_sitter_bash_src_SOURCE_DIR}/src
)

add_library(ts-lang-json STATIC
    ${tree_sitter_json_src_SOURCE_DIR}/src/parser.c
)
target_include_directories(ts-lang-json PUBLIC
    ${tree_sitter_json_src_SOURCE_DIR}/src
)

set(TS_TYPESCRIPT_SOURCES
    ${tree_sitter_typescript_src_SOURCE_DIR}/typescript/src/parser.c
)
if(EXISTS ${tree_sitter_typescript_src_SOURCE_DIR}/typescript/src/scanner.c)
    list(APPEND TS_TYPESCRIPT_SOURCES ${tree_sitter_typescript_src_SOURCE_DIR}/typescript/src/scanner.c)
endif()
if(EXISTS ${tree_sitter_typescript_src_SOURCE_DIR}/typescript/src/scanner.cc)
    list(APPEND TS_TYPESCRIPT_SOURCES ${tree_sitter_typescript_src_SOURCE_DIR}/typescript/src/scanner.cc)
endif()
add_library(ts-lang-typescript STATIC ${TS_TYPESCRIPT_SOURCES})
target_include_directories(ts-lang-typescript PUBLIC
    ${tree_sitter_typescript_src_SOURCE_DIR}/typescript/src
    ${tree_sitter_typescript_src_SOURCE_DIR}/common
)

set(TS_TSX_SOURCES
    ${tree_sitter_typescript_src_SOURCE_DIR}/tsx/src/parser.c
)
if(EXISTS ${tree_sitter_typescript_src_SOURCE_DIR}/tsx/src/scanner.c)
    list(APPEND TS_TSX_SOURCES ${tree_sitter_typescript_src_SOURCE_DIR}/tsx/src/scanner.c)
endif()
if(EXISTS ${tree_sitter_typescript_src_SOURCE_DIR}/tsx/src/scanner.cc)
    list(APPEND TS_TSX_SOURCES ${tree_sitter_typescript_src_SOURCE_DIR}/tsx/src/scanner.cc)
endif()
add_library(ts-lang-tsx STATIC ${TS_TSX_SOURCES})
target_include_directories(ts-lang-tsx PUBLIC
    ${tree_sitter_typescript_src_SOURCE_DIR}/tsx/src
    ${tree_sitter_typescript_src_SOURCE_DIR}/common
)

set(TS_RUST_SOURCES
    ${tree_sitter_rust_src_SOURCE_DIR}/src/parser.c
)
if(EXISTS ${tree_sitter_rust_src_SOURCE_DIR}/src/scanner.c)
    list(APPEND TS_RUST_SOURCES ${tree_sitter_rust_src_SOURCE_DIR}/src/scanner.c)
endif()
if(EXISTS ${tree_sitter_rust_src_SOURCE_DIR}/src/scanner.cc)
    list(APPEND TS_RUST_SOURCES ${tree_sitter_rust_src_SOURCE_DIR}/src/scanner.cc)
endif()
add_library(ts-lang-rust STATIC ${TS_RUST_SOURCES})
target_include_directories(ts-lang-rust PUBLIC
    ${tree_sitter_rust_src_SOURCE_DIR}/src
)

add_library(ts-lang-go STATIC
    ${tree_sitter_go_src_SOURCE_DIR}/src/parser.c
)
target_include_directories(ts-lang-go PUBLIC
    ${tree_sitter_go_src_SOURCE_DIR}/src
)

set(TS_MARKDOWN_SOURCES
    ${tree_sitter_markdown_src_SOURCE_DIR}/src/parser.c
)
if(EXISTS ${tree_sitter_markdown_src_SOURCE_DIR}/src/scanner.c)
    list(APPEND TS_MARKDOWN_SOURCES ${tree_sitter_markdown_src_SOURCE_DIR}/src/scanner.c)
endif()
if(EXISTS ${tree_sitter_markdown_src_SOURCE_DIR}/src/scanner.cc)
    list(APPEND TS_MARKDOWN_SOURCES ${tree_sitter_markdown_src_SOURCE_DIR}/src/scanner.cc)
endif()
add_library(ts-lang-markdown STATIC ${TS_MARKDOWN_SOURCES})
target_include_directories(ts-lang-markdown PUBLIC
    ${tree_sitter_markdown_src_SOURCE_DIR}/src
    ${tree_sitter_markdown_src_SOURCE_DIR}/common
)

set(TS_RUBY_SOURCES
    ${tree_sitter_ruby_src_SOURCE_DIR}/src/parser.c
)
if(EXISTS ${tree_sitter_ruby_src_SOURCE_DIR}/src/scanner.c)
    list(APPEND TS_RUBY_SOURCES ${tree_sitter_ruby_src_SOURCE_DIR}/src/scanner.c)
endif()
if(EXISTS ${tree_sitter_ruby_src_SOURCE_DIR}/src/scanner.cc)
    list(APPEND TS_RUBY_SOURCES ${tree_sitter_ruby_src_SOURCE_DIR}/src/scanner.cc)
endif()
add_library(ts-lang-ruby STATIC ${TS_RUBY_SOURCES})
target_include_directories(ts-lang-ruby PUBLIC
    ${tree_sitter_ruby_src_SOURCE_DIR}/src
)

set(TS_LUA_SOURCES
    ${tree_sitter_lua_src_SOURCE_DIR}/src/parser.c
)
if(EXISTS ${tree_sitter_lua_src_SOURCE_DIR}/src/scanner.c)
    list(APPEND TS_LUA_SOURCES ${tree_sitter_lua_src_SOURCE_DIR}/src/scanner.c)
endif()
if(EXISTS ${tree_sitter_lua_src_SOURCE_DIR}/src/scanner.cc)
    list(APPEND TS_LUA_SOURCES ${tree_sitter_lua_src_SOURCE_DIR}/src/scanner.cc)
endif()
add_library(ts-lang-lua STATIC ${TS_LUA_SOURCES})
target_include_directories(ts-lang-lua PUBLIC
    ${tree_sitter_lua_src_SOURCE_DIR}/src
)

set(TS_JAVASCRIPT_SOURCES
    ${tree_sitter_javascript_src_SOURCE_DIR}/src/parser.c
)
if(EXISTS ${tree_sitter_javascript_src_SOURCE_DIR}/src/scanner.c)
    list(APPEND TS_JAVASCRIPT_SOURCES ${tree_sitter_javascript_src_SOURCE_DIR}/src/scanner.c)
endif()
if(EXISTS ${tree_sitter_javascript_src_SOURCE_DIR}/src/scanner.cc)
    list(APPEND TS_JAVASCRIPT_SOURCES ${tree_sitter_javascript_src_SOURCE_DIR}/src/scanner.cc)
endif()
add_library(ts-lang-javascript STATIC ${TS_JAVASCRIPT_SOURCES})
target_include_directories(ts-lang-javascript PUBLIC
    ${tree_sitter_javascript_src_SOURCE_DIR}/src
)

# ─── Lua 5.4 ─────────────────────────────────────────────────────────────────
pkg_check_modules(LUA REQUIRED lua5.4)
add_library(lua_pi INTERFACE)
target_include_directories(lua_pi INTERFACE ${LUA_INCLUDE_DIRS})
target_link_libraries(lua_pi INTERFACE ${LUA_LIBRARIES})
message(STATUS "Lua: ${LUA_VERSION} (${LUA_INCLUDE_DIRS})")

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
    src/core/lua_tool.cpp
    src/core/markdown.cpp
    src/core/syntax_highlight.cpp
    src/core/terminal.cpp
    src/core/stream_renderer.cpp
    src/core/llm_client.cpp
    src/core/tool_validation.cpp
    src/core/env_api_keys.cpp
    src/core/models.cpp
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
    lua_pi
    cmark
    tree_sitter_pi
    ts-lang-c
    ts-lang-cpp
    ts-lang-python
    ts-lang-bash
    ts-lang-json
    ts-lang-typescript
    ts-lang-tsx
    ts-lang-rust
    ts-lang-go
    ts-lang-markdown
    ts-lang-ruby
    ts-lang-lua
    ts-lang-javascript
)

# ─── pi-cli (main executable) ───
add_executable(pi-cli
    src/main.cpp
    src/cli/args.cpp
    src/cli/config.cpp
    src/cli/readline.cpp
)

target_link_libraries(pi-cli PRIVATE pi-core pi-http tomlplusplus::tomlplusplus)

# ─── pi-acp (ACP server) ───
add_executable(pi-acp
    src/acp/main.cpp
    src/acp/server.cpp
    src/acp/types.cpp
    src/acp/sse.cpp
    src/acp/session_store.cpp
    src/acp/handlers.cpp
    src/cli/args.cpp
    src/cli/config.cpp
    src/cli/readline.cpp
)
target_include_directories(pi-acp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pi-acp PRIVATE pi-core pi-http httplib::httplib tomlplusplus::tomlplusplus)

# ─── Tests ───
if(PI_CPP_BUILD_TESTS)
    enable_testing()

    foreach(test_name IN ITEMS core agent_loop agent stream tool_validation builtin_tools lua_tool markdown terminal)
        add_executable(test-${test_name}
            test/test_${test_name}.cpp
        )

        target_link_libraries(test-${test_name} PRIVATE pi-core)
        add_test(NAME test-${test_name} COMMAND test-${test_name})
    endforeach()

    add_executable(test-markdown-display test/test_markdown_display.cpp)
    target_link_libraries(test-markdown-display PRIVATE pi-core)

    add_executable(test-faux_client test/test_faux_client.cpp)
    target_link_libraries(test-faux_client PRIVATE pi-core)
    add_test(NAME test-faux_client COMMAND test-faux_client)

    add_executable(test-openai_completions test/test_openai_completions.cpp)
    target_link_libraries(test-openai_completions PRIVATE pi-http)
    add_test(NAME test-openai_completions COMMAND test-openai_completions)

    add_executable(test-config test/test_config.cpp src/cli/config.cpp src/cli/args.cpp)
    target_include_directories(test-config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-config PRIVATE tomlplusplus::tomlplusplus)
    add_test(NAME test-config COMMAND test-config)

    add_executable(test-acp test/test_acp.cpp)
    target_include_directories(test-acp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-acp PRIVATE pi-core pi-http httplib::httplib)
    target_sources(test-acp PRIVATE
        src/acp/server.cpp src/acp/handlers.cpp src/acp/types.cpp
        src/acp/sse.cpp src/acp/session_store.cpp)
    add_test(NAME test-acp COMMAND test-acp)
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
