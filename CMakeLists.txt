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
    enable_testing()

    foreach(test_name IN ITEMS core agent_loop agent stream)
        add_executable(test-${test_name}
            test/test_${test_name}.cpp
        )

        target_link_libraries(test-${test_name} PRIVATE pi-core)
        add_test(NAME test-${test_name} COMMAND test-${test_name})
    endforeach()
endif()

# ─── HTTP client (optional) ───
if(PI_CPP_ENABLE_HTTP)
    find_package(CURL REQUIRED)
    find_package(nlohmann_json REQUIRED)

    add_library(pi-http
        src/http/http_client.cpp
    )

    target_include_directories(pi-http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(pi-http PUBLIC pi-core CURL::libcurl nlohmann_json::nlohmann_json)
    target_compile_definitions(pi-http PUBLIC PI_CPP_HAVE_HTTP)
endif()
