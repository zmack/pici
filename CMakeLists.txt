cmake_minimum_required(VERSION 3.28)
project(pi-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Option to build with HTTP client (libcurl) support
option(PI_CPP_BUILD_TESTS "Build tests" ON)
option(PI_CPP_ENABLE_HTTP "Enable HTTP client support" OFF)

# ─── libpi-core ───
add_library(pi-core
    src/core/message_types.cpp
    src/core/event_types.cpp
    src/core/agent_state.cpp
    src/core/stream.cpp
    src/core/agent_loop.cpp
    src/core/agent.cpp
    src/core/llm_client.cpp
)

target_include_directories(pi-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(pi-core PUBLIC
    PI_CPP_VERSION="${PROJECT_VERSION}"
)

# ─── pi-cli (main executable) ───
add_executable(pi-cli
    src/main.cpp
)

target_link_libraries(pi-cli PRIVATE pi-core)

# ─── Tests ───
if(PI_CPP_BUILD_TESTS)
    add_executable(test-core
        test/test_core.cpp
        test/test_agent_loop.cpp
        test/test_agent.cpp
        test/test_stream.cpp
    )

    target_link_libraries(test-core PRIVATE pi-core)

    add_test(NAME test-core COMMAND test-core)
endif()

# ─── HTTP client (optional) ───
if(PI_CPP_ENABLE_HTTP)
    find_package(CURL REQUIRED)

    add_library(pi-http
        src/http/http_client.cpp
    )

    target_include_directories(pi-http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(pi-http PUBLIC pi-core CURL::libcurl)
    target_compile_definitions(pi-http PUBLIC PI_CPP_HAVE_HTTP)
endif()
