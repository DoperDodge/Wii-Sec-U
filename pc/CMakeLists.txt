# Wii-Sec-U PC app (host / client / console simulator).
# Core builds with zero external dependencies on Linux/macOS/Windows.
# Optional integrations:
#   -DWSU_WITH_SDL=ON     controller input + display window (needs SDL2)
cmake_minimum_required(VERSION 3.16)
project(wsu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(WSU_WITH_SDL "Build the SDL2 controller/input backend" OFF)
option(WSU_BUILD_TESTS "Build unit and integration tests" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(wsu_core STATIC
    src/net/udp_socket.cpp
    src/core/input_hub.cpp
    src/core/frame_assembler.cpp
    src/host/wiiu_link.cpp
    src/host/session_server.cpp
    src/host/host_app.cpp
    src/client/client_app.cpp
    src/input/synthetic_backend.cpp
    src/sim/console_sim.cpp
)
target_include_directories(wsu_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../protocol
)

if(WIN32)
    target_link_libraries(wsu_core PUBLIC ws2_32)
endif()

find_package(Threads REQUIRED)
target_link_libraries(wsu_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(wsu_core PRIVATE /W4)
else()
    target_compile_options(wsu_core PRIVATE -Wall -Wextra)
endif()

if(WSU_WITH_SDL)
    find_package(SDL2 REQUIRED)
    target_sources(wsu_core PRIVATE src/input/sdl_backend.cpp)
    target_compile_definitions(wsu_core PUBLIC WSU_WITH_SDL)
    target_link_libraries(wsu_core PUBLIC SDL2::SDL2)
endif()

add_executable(wsu src/main.cpp)
target_link_libraries(wsu PRIVATE wsu_core)

if(WSU_BUILD_TESTS)
    enable_testing()
    foreach(test_name
            test_protocol
            test_frame_assembler
            test_input_hub
            test_end_to_end)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name} PRIVATE wsu_core)
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
    # The end-to-end test spins real sockets and threads; give it headroom.
    set_tests_properties(test_end_to_end PROPERTIES TIMEOUT 60)
endif()
