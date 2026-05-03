cmake_minimum_required(VERSION 3.28)
project(pi-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    src/core/llm_client.cpp
    src/core/tool_validation.cpp
    src/core/providers/faux.cpp
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

    foreach(test_name IN ITEMS core agent_loop agent stream tool_validation)
        add_executable(test-${test_name}
            test/test_${test_name}.cpp
        )

        target_link_libraries(test-${test_name} PRIVATE pi-core)
        add_test(NAME test-${test_name} COMMAND test-${test_name})
    endforeach()

    add_executable(test-faux_client test/test_faux_client.cpp)
    target_link_libraries(test-faux_client PRIVATE pi-core)
    add_test(NAME test-faux_client COMMAND test-faux_client)
endif()

add_library(pi-http
    src/http/http_client.cpp
)
target_include_directories(pi-http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pi-http PUBLIC pi-core CURL::libcurl)
