cmake_minimum_required(VERSION 3.28)
project(pi-cpp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PI_CPP_BUILD_TESTS "Build tests" ON)
option(PI_CPP_OTEL_API   "Compile OpenTelemetry API instrumentation" OFF)
option(PI_CPP_OTEL_SDK   "Link OpenTelemetry SDK + OTLP/HTTP exporter for real trace export" OFF)
option(PI_CPP_USE_CCACHE "Use ccache when it is available" ON)

if(PI_CPP_USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        message(STATUS "ccache: ${CCACHE_PROGRAM}")
    endif()
endif()

# ─── FetchContent dependencies ───
include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    OVERRIDE_FIND_PACKAGE   # lets otel-cpp's find_package(nlohmann_json) use this copy
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
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(SQLite3 REQUIRED)

# ─── cmark (Markdown parser) ─────────────────────────────────────────────────
FetchContent_Declare(
    cmark_src
    GIT_REPOSITORY https://github.com/commonmark/cmark.git
    GIT_TAG        0.31.1
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
)
get_property(_PI_CPP_CMAKE_WARN_DEPRECATED_WAS_SET CACHE CMAKE_WARN_DEPRECATED
    PROPERTY VALUE SET)
if(_PI_CPP_CMAKE_WARN_DEPRECATED_WAS_SET)
    get_property(_PI_CPP_CMAKE_WARN_DEPRECATED CACHE CMAKE_WARN_DEPRECATED
        PROPERTY VALUE)
endif()
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
# cmark's own CMakeLists registers its CommonMark spec-conformance test suite
# (api_test, spectest_executable, etc.) whenever BUILD_TESTING is set — which
# it is here, as a side effect of another dependency's CMakeLists turning it
# on globally via the CTest module. We vendor cmark as EXCLUDE_FROM_ALL (it's
# only used as a library), so those test binaries never get built by default
# and the registered tests fail with "no such file" instead of actually
# validating anything of ours. Force it off for this one dependency.
set(_PI_CPP_BUILD_TESTING_WAS_SET FALSE)
get_property(_PI_CPP_BUILD_TESTING_WAS_SET CACHE BUILD_TESTING PROPERTY VALUE SET)
if(_PI_CPP_BUILD_TESTING_WAS_SET)
    get_property(_PI_CPP_BUILD_TESTING CACHE BUILD_TESTING PROPERTY VALUE)
endif()
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cmark_src)
if(_PI_CPP_BUILD_TESTING_WAS_SET)
    set(BUILD_TESTING ${_PI_CPP_BUILD_TESTING} CACHE BOOL "" FORCE)
else()
    unset(BUILD_TESTING CACHE)
endif()
unset(_PI_CPP_BUILD_TESTING_WAS_SET)
unset(_PI_CPP_BUILD_TESTING)
if(_PI_CPP_CMAKE_WARN_DEPRECATED_WAS_SET)
    set(CMAKE_WARN_DEPRECATED ${_PI_CPP_CMAKE_WARN_DEPRECATED}
        CACHE BOOL "" FORCE)
else()
    unset(CMAKE_WARN_DEPRECATED CACHE)
endif()
unset(_PI_CPP_CMAKE_WARN_DEPRECATED_WAS_SET)
unset(_PI_CPP_CMAKE_WARN_DEPRECATED)
message(STATUS "cmark: 0.31.1")

# ─── Tree-sitter (syntax highlighting) ───────────────────────────────────────
FetchContent_Declare(
    tree_sitter_runtime_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter.git
    GIT_TAG        v0.25.10
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)

FetchContent_Declare(
    tree_sitter_c_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-c.git
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_cpp_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_python_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-python.git
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_bash_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-bash.git
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_json_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-json.git
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_typescript_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-typescript.git
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_rust_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-rust.git
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_go_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-go.git
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_markdown_src
    GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-markdown.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_ruby_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-ruby.git
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_lua_src
    GIT_REPOSITORY https://github.com/tree-sitter-grammars/tree-sitter-lua.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
)
FetchContent_Declare(
    tree_sitter_javascript_src
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-javascript.git
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SOURCE_SUBDIR  cmake-do-not-add
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

# ─── OpenTelemetry (optional) ───────────────────────────────────────────────
if(PI_CPP_OTEL_API)
    # WITH_STL=ON replaces nostd:: with std:: aliases. Must be pinned globally —
    # mismatched STL mode across TUs causes silent ABI corruption.
    set(WITH_STL              ON  CACHE BOOL "" FORCE)
    set(WITH_ABSEIL           OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
    set(WITH_EXAMPLES         OFF CACHE BOOL "" FORCE)
    set(WITH_BENCHMARK        OFF CACHE BOOL "" FORCE)
    set(WITH_OTLP_HTTP        OFF CACHE BOOL "" FORCE)
    set(WITH_OTLP_GRPC        OFF CACHE BOOL "" FORCE)
    set(WITH_JAEGER           OFF CACHE BOOL "" FORCE)
    set(OPENTELEMETRY_INSTALL OFF CACHE BOOL "" FORCE)
    if(PI_CPP_OTEL_SDK)
        set(WITH_OTLP_HTTP    ON  CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        opentelemetry-cpp
        GIT_REPOSITORY https://github.com/open-telemetry/opentelemetry-cpp
        GIT_TAG        v1.17.0
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(opentelemetry-cpp)
endif()

FetchContent_MakeAvailable(
    tree_sitter_runtime_src
    tree_sitter_c_src
    tree_sitter_cpp_src
    tree_sitter_python_src
    tree_sitter_bash_src
    tree_sitter_json_src
    tree_sitter_typescript_src
    tree_sitter_rust_src
    tree_sitter_go_src
    tree_sitter_markdown_src
    tree_sitter_ruby_src
    tree_sitter_lua_src
    tree_sitter_javascript_src
)

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
pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua5.4)
add_library(lua_pi INTERFACE)
target_link_libraries(lua_pi INTERFACE PkgConfig::LUA)
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
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(curl)
endif()

# ─── warnings ───
# -Wmissing-field-initializers fires on every partial designated initializer
# (Foo{.field = x}), which is this codebase's normal style, not a defect.
# -Wunused-parameter fires on the many intentionally-unused parameters in
# virtual overrides (already suppressed as an editor diagnostic in .clangd).
set(PI_CPP_WARNING_FLAGS
    -Wall -Wextra
    -Wno-missing-field-initializers
    -Wno-unused-parameter
)

# ─── libpi-core ───
add_library(pi-core
    src/core/mailbox/mailbox_bindings.cpp
    src/core/mailbox/mailbox_types.cpp
    src/core/mailbox/mailbox_store.cpp
    src/core/mailbox/mailbox_coordinator.cpp
    src/core/message_types.cpp
    src/core/event_types.cpp
    src/core/event_json.cpp
    src/core/agent_state.cpp
    src/core/stream.cpp
    src/core/agent_loop.cpp
    src/core/agent.cpp
    src/core/agent_task.cpp
    src/core/subagent_activity.cpp
    src/core/subagent_panel.cpp
    src/core/compaction.cpp
    src/core/builtin_tools.cpp
    src/core/apply_patch.cpp
    src/core/skills.cpp
    src/core/sandbox.cpp
    src/core/lua_tool.cpp
    src/core/markdown.cpp
    src/core/syntax_highlight.cpp
    src/core/terminal.cpp
    src/core/stream_renderer.cpp
    src/core/region_renderer.cpp
    src/core/stream_diagnostics.cpp
    src/core/llm_client.cpp
    src/core/memory_stats.cpp
    src/core/tool_validation.cpp
    src/core/auth/credential_store.cpp
    src/core/env_api_keys.cpp
    src/core/models.cpp
    src/core/process/pici_process.cpp
    src/core/providers/faux.cpp
    src/core/providers/faux_control.cpp
    src/core/providers/transform_messages.cpp
    src/core/otel_init.cpp
    src/core/session/session_id.cpp
    src/core/session/session_runtime.cpp
    src/core/session/session_store.cpp
    src/core/session/session_tree.cpp
    src/core/session/mailbox_runtime.cpp
)

target_include_directories(pi-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(pi-core PRIVATE ${PI_CPP_WARNING_FLAGS})

target_compile_definitions(pi-core PUBLIC
    PI_CPP_VERSION="${PROJECT_VERSION}"
)

if(PI_CPP_MEMSTATS)
    # jemalloc's malloc override and TSan's interceptors are mutually
    # exclusive (plan session-memory-stats.md §Risks 6): fail at configure
    # time, never at runtime.
    if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=" OR CMAKE_EXE_LINKER_FLAGS MATCHES "-fsanitize=")
        message(FATAL_ERROR
            "PI_CPP_MEMSTATS is incompatible with sanitizer builds: "
            "jemalloc's malloc override and TSan's malloc interceptors conflict.")
    endif()
    target_compile_definitions(pi-core PUBLIC PI_CPP_MEMSTATS_ENABLED)
    # Primary path is dlsym + LD_PRELOAD (no link step). The optional link-
    # time fallback exists for environments where LD_PRELOAD isn't practical.
    option(PI_CPP_MEMSTATS_LINK_JEMALLOC
        "Link system jemalloc via pkg-config instead of relying on LD_PRELOAD" OFF)
    if(PI_CPP_MEMSTATS_LINK_JEMALLOC)
        pkg_check_modules(JEMALLOC REQUIRED IMPORTED_TARGET jemalloc)
        target_link_libraries(pi-core PUBLIC PkgConfig::JEMALLOC)
    endif()
endif()


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
    SQLite::SQLite3
)

if(PI_CPP_OTEL_API)
    # FetchContent exposes the raw CMake target names (underscores), not the
    # installed alias names (opentelemetry-cpp::api). Use the raw names here.
    target_link_libraries(pi-core PUBLIC opentelemetry_api)
    target_compile_definitions(pi-core PUBLIC PI_CPP_OTEL_ENABLED)
    if(PI_CPP_OTEL_SDK)
        target_link_libraries(pi-core PUBLIC
            opentelemetry_trace
            opentelemetry_sdk
            opentelemetry_exporter_otlp_http
        )
        target_compile_definitions(pi-core PUBLIC PI_CPP_OTEL_SDK_ENABLED)
    endif()
endif()

# ─── pi-cli (main executable) ───
add_executable(pi-cli
    src/main.cpp
    src/cli/args.cpp
    src/cli/config.cpp
    src/cli/mailbox_runtime.cpp
    src/cli/session_runtime.cpp
    src/cli/session_runtime_bundle.cpp
    src/cli/readline.cpp
    src/cli/wrap.cpp
    src/cli/vim_mode.cpp
    src/cli/model_selector.cpp
    src/cli/rpc_mode.cpp
    src/cli/faux_control_mode.cpp
    src/cli/system_prompt.cpp
    src/cli/tree_selector.cpp
)

target_link_libraries(pi-cli PRIVATE pi-core pi-http tomlplusplus::tomlplusplus)
target_compile_definitions(pi-cli PRIVATE
    PI_CPP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_options(pi-cli PRIVATE ${PI_CPP_WARNING_FLAGS})

# ─── pi-acp (ACP server) ───
add_executable(pi-acp
    src/acp/main.cpp
    src/acp/server.cpp
    src/acp/types.cpp
    src/acp/sse.cpp
    src/acp/task_events.cpp
    src/acp/handlers.cpp
    src/cli/args.cpp
    src/cli/config.cpp
    src/cli/session_runtime.cpp
    src/cli/readline.cpp
    src/cli/wrap.cpp
    src/cli/vim_mode.cpp
    src/cli/system_prompt.cpp
)
target_include_directories(pi-acp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pi-acp PRIVATE pi-core pi-http httplib::httplib tomlplusplus::tomlplusplus)
target_compile_options(pi-acp PRIVATE ${PI_CPP_WARNING_FLAGS})
# cli/session_runtime.cpp's Lua-hook-composition helper needs this even
# though ACP always calls it with SessionRuntimeCapabilities::enable_hooks
# off (see cli/session_runtime.h) -- the code path is compiled in either
# way, just never reached at runtime for ACP.
target_compile_definitions(pi-acp PRIVATE
    PI_CPP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

# ─── Tests ───
if(PI_CPP_BUILD_TESTS)
    enable_testing()

    # Keep the test framework isolated from normal builds and pin it to a
    # known release. The individual test binaries below intentionally remain
    # separate CTest entries; this preserves their existing process isolation.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.18.0
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(googletest)

    foreach(test_name IN ITEMS core agent_loop agent agent_tasks compaction stream tool_validation builtin_tools apply_patch lua_tool markdown terminal skills memory_stats)
        add_executable(test-${test_name}
            test/test_${test_name}.cpp
        )
        target_link_libraries(test-${test_name} PRIVATE pi-core)
        add_test(NAME test-${test_name} COMMAND test-${test_name})
    endforeach()

    add_executable(test-subagent-activity test/test_subagent_activity.cpp)
    target_link_libraries(test-subagent-activity PRIVATE pi-core)
    add_test(NAME test-subagent-activity COMMAND test-subagent-activity)

    add_executable(test-subagent-panel test/test_subagent_panel.cpp)
    target_link_libraries(test-subagent-panel PRIVATE pi-core)
    add_test(NAME test-subagent-panel COMMAND test-subagent-panel)

    add_executable(test-region-renderer test/test_region_renderer.cpp)
    target_link_libraries(test-region-renderer PRIVATE pi-core)
    add_test(NAME test-region-renderer COMMAND test-region-renderer)

    target_compile_definitions(test-lua_tool PRIVATE
        PI_CPP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_definitions(test-agent_tasks PRIVATE
        PI_CPP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_definitions(test-skills PRIVATE
        PI_CPP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )

    add_executable(test-markdown-display test/test_markdown_display.cpp)
    target_link_libraries(test-markdown-display PRIVATE pi-core)

    add_executable(test-faux_client test/test_faux_client.cpp)
    target_link_libraries(test-faux_client PRIVATE pi-core)
    add_test(NAME test-faux_client COMMAND test-faux_client)
    add_executable(test-faux_control test/test_faux_control.cpp)
    target_link_libraries(test-faux_control PRIVATE pi-core)
    add_test(NAME test-faux_control COMMAND test-faux_control)

    add_executable(test-auth_resolver test/test_auth_resolver.cpp)
    add_executable(test-authentication test/test_authentication.cpp)
    target_link_libraries(test-authentication PRIVATE pi-http)
    add_test(NAME test-authentication COMMAND test-authentication)
    target_link_libraries(test-auth_resolver PRIVATE pi-http)
    add_test(NAME test-auth_resolver COMMAND test-auth_resolver)

    add_executable(test-model_catalog test/test_model_catalog.cpp)
    target_link_libraries(test-model_catalog PRIVATE pi-core)
    add_test(NAME test-model_catalog COMMAND test-model_catalog)

    add_executable(test-pici_process test/test_pici_process.cpp)
    target_link_libraries(test-pici_process PRIVATE pi-http)
    add_test(NAME test-pici_process COMMAND test-pici_process)

    add_executable(test-openai_completions test/test_openai_completions.cpp)
    target_link_libraries(test-openai_completions PRIVATE pi-http)
    add_test(NAME test-openai_completions COMMAND test-openai_completions)

    add_executable(test-muse_messages test/test_muse_messages.cpp)
    target_link_libraries(test-muse_messages PRIVATE pi-http)
    add_test(NAME test-muse_messages COMMAND test-muse_messages)

    add_executable(test-config test/test_config.cpp src/cli/config.cpp src/cli/args.cpp)
    target_include_directories(test-config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-config PRIVATE pi-core tomlplusplus::tomlplusplus nlohmann_json::nlohmann_json)
    add_test(NAME test-config COMMAND test-config)

    add_executable(test-mailbox-runtime
        test/test_mailbox_runtime.cpp
        src/cli/mailbox_runtime.cpp
        src/cli/config.cpp
        src/cli/args.cpp)
    target_link_libraries(test-mailbox-runtime PRIVATE pi-core tomlplusplus::tomlplusplus)
    add_test(NAME test-mailbox-runtime COMMAND test-mailbox-runtime)

    add_executable(test-mailbox-store test/test_mailbox_store.cpp)
    target_include_directories(test-mailbox-store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-mailbox-store PRIVATE pi-core)
    add_test(NAME test-mailbox-store COMMAND test-mailbox-store)

    add_executable(test-mailbox-coordinator test/test_mailbox_coordinator.cpp)
    target_include_directories(test-mailbox-coordinator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-mailbox-coordinator PRIVATE pi-core)
    add_test(NAME test-mailbox-coordinator COMMAND test-mailbox-coordinator)

    add_executable(test-mailbox-bindings test/test_mailbox_bindings.cpp)
    target_include_directories(test-mailbox-bindings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-mailbox-bindings PRIVATE pi-core)
    add_test(NAME test-mailbox-bindings COMMAND test-mailbox-bindings)

    add_executable(test-credential_store test/test_credential_store.cpp)
    target_link_libraries(test-credential_store PRIVATE pi-core)
    add_test(NAME test-credential_store COMMAND test-credential_store)

    add_executable(test-openai_codex_oauth test/test_openai_codex_oauth.cpp)
    target_link_libraries(test-openai_codex_oauth PRIVATE pi-http)
    add_test(NAME test-openai_codex_oauth COMMAND test-openai_codex_oauth)

    add_executable(test-openai_codex_responses
        test/test_openai_codex_responses.cpp)
    target_link_libraries(test-openai_codex_responses PRIVATE pi-http)
    add_test(NAME test-openai_codex_responses COMMAND test-openai_codex_responses)

    add_executable(test-sandbox test/test_sandbox.cpp)
    target_link_libraries(test-sandbox PRIVATE pi-core)
    add_test(NAME test-sandbox COMMAND test-sandbox)

    add_executable(test-system_prompt test/test_system_prompt.cpp src/cli/system_prompt.cpp)
    target_include_directories(test-system_prompt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME test-system_prompt COMMAND test-system_prompt)

    add_executable(test-rpc_mode test/test_rpc_mode.cpp src/cli/rpc_mode.cpp)
    target_include_directories(test-rpc_mode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-rpc_mode PRIVATE pi-http)
    add_test(NAME test-rpc_mode COMMAND test-rpc_mode)
    add_executable(test-faux_control_mode test/test_faux_control_mode.cpp src/cli/faux_control_mode.cpp)
    target_include_directories(test-faux_control_mode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-faux_control_mode PRIVATE pi-core)
    add_test(NAME test-faux_control_mode COMMAND test-faux_control_mode)

    add_executable(test-readline test/test_readline.cpp src/cli/readline.cpp src/cli/wrap.cpp src/cli/vim_mode.cpp)
    target_include_directories(test-readline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-readline PRIVATE pi-core)
    add_test(NAME test-readline COMMAND test-readline)

    add_executable(test-wrap test/test_wrap.cpp src/cli/wrap.cpp)
    target_include_directories(test-wrap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME test-wrap COMMAND test-wrap)

    add_executable(test-vim-mode test/test_vim_mode.cpp src/cli/vim_mode.cpp src/cli/readline.cpp src/cli/wrap.cpp)
    target_include_directories(test-vim-mode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-vim-mode PRIVATE pi-core)
    add_test(NAME test-vim-mode COMMAND test-vim-mode)

    add_test(
        NAME test-addons
        COMMAND $<TARGET_FILE:pi-cli>
            --config ${CMAKE_CURRENT_SOURCE_DIR}/config.toml.example
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_costline.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_context.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_turn_guard.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_permissions.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_context_trim.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_tool_format.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_nfo_format.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_nfo_classic.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_scene_release.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_matrix_drop.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_root_shell.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_circuit_board.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_glitch_tag.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_ascii_skull.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_trade_stats.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_hud_brackets.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_block_meter.lua
            --test ${CMAKE_CURRENT_SOURCE_DIR}/addons/test_minimal_dot.lua
    )
    set_tests_properties(test-addons
                         PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(test-acp test/test_acp.cpp)
    target_include_directories(test-acp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-acp PRIVATE pi-core pi-http httplib::httplib)
    target_sources(test-acp PRIVATE
        src/acp/server.cpp src/acp/handlers.cpp src/acp/types.cpp
        src/acp/sse.cpp src/acp/task_events.cpp src/cli/session_runtime.cpp)
    # See pi-acp's own PI_CPP_SOURCE_DIR comment: session_runtime.cpp's
    # Lua-hook helper needs this even though it's compiled-but-unreached
    # here (ACP always calls it with hooks disabled).
    target_compile_definitions(test-acp PRIVATE
        PI_CPP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME test-acp COMMAND test-acp)

    add_executable(test-frontend-parity test/test_frontend_parity.cpp)
    target_include_directories(test-frontend-parity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-frontend-parity PRIVATE pi-core pi-http httplib::httplib)
    target_sources(test-frontend-parity PRIVATE
        src/acp/server.cpp src/acp/handlers.cpp src/acp/types.cpp
        src/acp/sse.cpp src/acp/task_events.cpp src/cli/rpc_mode.cpp
        src/cli/session_runtime.cpp)
    target_compile_definitions(test-frontend-parity PRIVATE
        PI_CPP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME test-frontend-parity COMMAND test-frontend-parity)

    # End-to-end characterization tests for cmd_run() (src/cli/cmd_run_session.h):
    # drives the real compiled pi-cli binary as a subprocess, exercising
    # argument parsing, config-file resolution, and process exit codes -- the
    # things only a real process boundary observes. See test_cmd_run.cpp's
    # file comment. test-cmd_run_repl below covers REPL slash-command logic
    # in-process instead, against a mocked Renderer.
    add_executable(test-cmd_run test/test_cmd_run.cpp)
    target_compile_definitions(test-cmd_run PRIVATE
        PI_CLI_BINARY="$<TARGET_FILE:pi-cli>"
    )
    add_dependencies(test-cmd_run pi-cli)
    add_test(NAME test-cmd_run COMMAND test-cmd_run)

    # Unit tests for CmdRunSession's REPL slash-command dispatch, in-process
    # against a gmock MockRenderer -- see test_cmd_run_repl.cpp's file
    # comment. Needs the same cli/*.cpp sources as pi-cli itself, since
    # cli/cmd_run_session.h (header-only) pulls in the same dependency graph.
    add_executable(test-cmd_run_repl
        test/test_cmd_run_repl.cpp
        src/cli/args.cpp
        src/cli/config.cpp
        src/cli/mailbox_runtime.cpp
        src/cli/session_runtime.cpp
        src/cli/session_runtime_bundle.cpp
        src/cli/readline.cpp
        src/cli/wrap.cpp
        src/cli/vim_mode.cpp
        src/cli/model_selector.cpp
        src/cli/rpc_mode.cpp
        src/cli/faux_control_mode.cpp
        src/cli/system_prompt.cpp
        src/cli/tree_selector.cpp
    )
    target_include_directories(test-cmd_run_repl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test-cmd_run_repl PRIVATE pi-core pi-http tomlplusplus::tomlplusplus)
    target_compile_definitions(test-cmd_run_repl PRIVATE
        PI_CPP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME test-cmd_run_repl COMMAND test-cmd_run_repl)

    # Link every automated C++ test with gtest_main. test-markdown-display is
    # a manual demo and is deliberately excluded.
    set(PI_CPP_GTEST_TEST_TARGETS
        test-core test-agent_loop test-agent test-agent_tasks test-compaction
        test-stream test-tool_validation test-builtin_tools test-apply_patch
        test-lua_tool test-markdown test-terminal test-skills test-memory_stats
        test-subagent-activity test-subagent-panel test-region-renderer
        test-faux_client test-faux_control test-auth_resolver test-authentication
        test-model_catalog test-pici_process test-openai_completions test-muse_messages test-config
        test-mailbox-runtime test-mailbox-store test-mailbox-coordinator
        test-mailbox-bindings test-credential_store test-openai_codex_oauth
        test-openai_codex_responses test-sandbox test-system_prompt test-rpc_mode
        test-faux_control_mode test-readline test-wrap test-vim-mode test-acp
        test-frontend-parity test-cmd_run test-cmd_run_repl
    )
    foreach(test_target IN LISTS PI_CPP_GTEST_TEST_TARGETS)
        target_link_libraries(${test_target} PRIVATE GTest::gtest_main
                                                       GTest::gmock)
    endforeach()
endif()

add_library(pi-http
    src/http/http_client.cpp
    src/core/auth/authentication.cpp
    src/core/auth/openai_codex_oauth.cpp
    src/core/providers/openai_completions.cpp
    src/core/providers/openai_codex_responses.cpp
    src/core/providers/muse_messages.cpp
)
target_include_directories(pi-http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pi-http PUBLIC pi-core CURL::libcurl OpenSSL::Crypto
                                     httplib::httplib)
target_compile_options(pi-http PRIVATE ${PI_CPP_WARNING_FLAGS})

# ─── clang-tidy ───────────────────────────────────────────────────────────────

set(PI_CPP_LLVM_BIN_DIR "" CACHE PATH
    "Directory containing clang-tidy, run-clang-tidy, and clang-format")
set(PI_CPP_LLVM_TOOL_HINTS)
if(PI_CPP_LLVM_BIN_DIR)
    list(APPEND PI_CPP_LLVM_TOOL_HINTS "${PI_CPP_LLVM_BIN_DIR}")
endif()
if(APPLE)
    list(APPEND PI_CPP_LLVM_TOOL_HINTS
        /opt/homebrew/opt/llvm/bin
        /usr/local/opt/llvm/bin
    )
endif()

find_program(CLANG_TIDY_BIN
    NAMES clang-tidy-23 clang-tidy-22 clang-tidy-20 clang-tidy
    HINTS ${PI_CPP_LLVM_TOOL_HINTS}
)
find_program(RUN_CLANG_TIDY_BIN
    NAMES run-clang-tidy-23 run-clang-tidy-22 run-clang-tidy-20 run-clang-tidy
    HINTS ${PI_CPP_LLVM_TOOL_HINTS}
)

if(CLANG_TIDY_BIN)
    # Enumerate sources instead of relying on run-clang-tidy's path-regex
    # matching. The latter accepts a regex as a positional argument but a
    # non-match silently results in no translation units being analyzed.
    # Passing the enumerated absolute paths as run-clang-tidy's positional
    # "files" arguments keeps that safety property (each file matches at
    # least itself) while still selecting the file list explicitly.
    file(GLOB_RECURSE TIDY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
    list(FILTER TIDY_SOURCES EXCLUDE REGEX "/src/acp/handlers[.]cpp$")
    set(TIDY_HANDLERS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/acp/handlers.cpp")

    include(ProcessorCount)
    ProcessorCount(PI_CPP_NPROC)
    if(PI_CPP_NPROC EQUAL 0)
        set(PI_CPP_NPROC 1)
    endif()

    if(RUN_CLANG_TIDY_BIN)
        # NOTE: -source-filter was replaced by a positional path-regex
        # argument in newer run-clang-tidy releases (LLVM 18+); older
        # releases used -source-filter instead. If `make lint` reports an
        # "unrecognized arguments" error for whichever form is in use here,
        # the installed run-clang-tidy predates or postdates this switch —
        # check `run-clang-tidy -h` and swap the flag accordingly.
        add_custom_target(tidy
            COMMAND ${RUN_CLANG_TIDY_BIN}
                -clang-tidy-binary ${CLANG_TIDY_BIN}
                -p ${CMAKE_BINARY_DIR}
                # Debian's clang 18 frontend supports concepts but does not
                # define the feature-test macro that libstdc++'s C++23
                # <expected> header uses to expose std::expected.
                -extra-arg=-D__cpp_concepts=202002L
                -extra-arg=-Wno-builtin-macro-redefined
                -quiet
                -j ${PI_CPP_NPROC}
                ${TIDY_SOURCES}
            COMMAND ${CLANG_TIDY_BIN}
                -p ${CMAKE_BINARY_DIR}
                -extra-arg=-D__cpp_concepts=202002L
                -extra-arg=-Wno-builtin-macro-redefined
                -quiet
                -checks=-clang-analyzer-*
                ${TIDY_HANDLERS_SOURCE}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Running clang-tidy (parallel x${PI_CPP_NPROC}, ${CLANG_TIDY_BIN})"
            VERBATIM
        )
        add_custom_target(tidy-fix
            COMMAND ${RUN_CLANG_TIDY_BIN}
                -clang-tidy-binary ${CLANG_TIDY_BIN}
                -p ${CMAKE_BINARY_DIR}
                -extra-arg=-D__cpp_concepts=202002L
                -extra-arg=-Wno-builtin-macro-redefined
                -quiet
                -fix
                -j ${PI_CPP_NPROC}
                ${TIDY_SOURCES}
            COMMAND ${CLANG_TIDY_BIN}
                -p ${CMAKE_BINARY_DIR}
                -extra-arg=-D__cpp_concepts=202002L
                -extra-arg=-Wno-builtin-macro-redefined
                -quiet
                -fix
                -checks=-clang-analyzer-*
                ${TIDY_HANDLERS_SOURCE}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Running clang-tidy --fix (parallel x${PI_CPP_NPROC})"
            VERBATIM
        )
    else()
        add_custom_target(tidy
            COMMAND ${CLANG_TIDY_BIN}
                -p ${CMAKE_BINARY_DIR}
                -extra-arg=-D__cpp_concepts=202002L
                -extra-arg=-Wno-builtin-macro-redefined
                -quiet
                ${TIDY_SOURCES}
            COMMAND ${CLANG_TIDY_BIN}
                -p ${CMAKE_BINARY_DIR}
                -extra-arg=-D__cpp_concepts=202002L
                -extra-arg=-Wno-builtin-macro-redefined
                -quiet
                -checks=-clang-analyzer-*
                ${TIDY_HANDLERS_SOURCE}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Running clang-tidy (${CLANG_TIDY_BIN})"
            VERBATIM
        )
    endif()

    message(STATUS "clang-tidy: ${CLANG_TIDY_BIN}  →  'cmake --build build --target tidy'")
else()
    message(STATUS "clang-tidy not found — skipping tidy target (install LLVM or set PI_CPP_LLVM_BIN_DIR)")
endif()

# ─── clang-format ─────────────────────────────────────────────────────────────

find_program(CLANG_FORMAT_BIN
    NAMES clang-format-23 clang-format-22 clang-format-20 clang-format
    HINTS ${PI_CPP_LLVM_TOOL_HINTS}
)

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
    message(STATUS "clang-format not found — skipping format target (install LLVM or set PI_CPP_LLVM_BIN_DIR)")
endif()
