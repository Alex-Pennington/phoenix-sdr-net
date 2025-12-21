# Phoenix SDR Net - CMake Build System
# Network tools for Phoenix SDR: sdr_server, signal_relay, etc.

cmake_minimum_required(VERSION 3.16)

project(phoenix-sdr-net
    VERSION 0.1.0
    DESCRIPTION "Phoenix SDR Network Tools"
    LANGUAGES C
)

# C standard
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

#============================================================================
# Dependencies
#============================================================================

# phoenix-sdr-core (submodule)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/phoenix-sdr-core/CMakeLists.txt")
    add_subdirectory(external/phoenix-sdr-core)
    message(STATUS "Using phoenix-sdr-core from external/")
else()
    message(FATAL_ERROR "phoenix-sdr-core not found. Run: git submodule update --init --recursive")
endif()

#============================================================================
# Version header
#============================================================================

# Get git info
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
else()
    set(GIT_COMMIT "unknown")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/version.h"
    @ONLY
)

#============================================================================
# Common include directories
#============================================================================

set(PHOENIX_NET_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include
)

#============================================================================
# Executables
#============================================================================

# SDR Server - TCP control + I/Q streaming (Windows + Linux)
add_executable(sdr_server src/sdr_server.c)
target_link_libraries(sdr_server PRIVATE phoenix_sdr_core)
target_include_directories(sdr_server PRIVATE ${PHOENIX_NET_INCLUDES})
if(WIN32)
    target_link_libraries(sdr_server PRIVATE ws2_32 shell32)
    
    # Copy SDRplay DLL to output directory
    if(SDRplayAPI_DLL)
        add_custom_command(TARGET sdr_server POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SDRplayAPI_DLL}"
                "$<TARGET_FILE_DIR:sdr_server>"
            COMMENT "Copying sdrplay_api.dll"
        )
    endif()
else()
    target_link_libraries(sdr_server PRIVATE pthread)
endif()

# Signal Splitter - Split I/Q to detector/display paths (Windows + Linux)
add_executable(signal_splitter 
    src/signal_splitter.c
    src/waterfall_dsp.c
)
target_include_directories(signal_splitter PRIVATE ${PHOENIX_NET_INCLUDES})
target_link_libraries(signal_splitter PRIVATE m)
if(WIN32)
    target_link_libraries(signal_splitter PRIVATE ws2_32)
else()
    target_link_libraries(signal_splitter PRIVATE pthread)
endif()

# Wormhole - 3D constellation mapper (DISABLED for now)
# add_executable(wormhole src/wormhole.c)
# target_include_directories(wormhole PRIVATE ${PHOENIX_NET_INCLUDES})
# if(WIN32)
#     target_link_libraries(wormhole PRIVATE ws2_32)
# else()
#     target_link_libraries(wormhole PRIVATE pthread)
# endif()

# Signal Relay - Linux only (uses epoll, etc.)
if(NOT WIN32)
    add_executable(signal_relay src/signal_relay.c)
    target_include_directories(signal_relay PRIVATE ${PHOENIX_NET_INCLUDES})
    target_link_libraries(signal_relay PRIVATE pthread)
endif()

#============================================================================
# Install
#============================================================================

set(INSTALL_TARGETS sdr_server signal_splitter)
if(NOT WIN32)
    list(APPEND INSTALL_TARGETS signal_relay)
endif()

install(TARGETS ${INSTALL_TARGETS}
    RUNTIME DESTINATION bin
)

#============================================================================
# Summary
#============================================================================

message(STATUS "")
message(STATUS "Phoenix SDR Net v${PROJECT_VERSION}")
message(STATUS "  Git commit: ${GIT_COMMIT}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Platform:   ${CMAKE_SYSTEM_NAME}")
message(STATUS "")
message(STATUS "Targets:")
message(STATUS "  - sdr_server      (TCP control + I/Q streaming)")
message(STATUS "  - signal_splitter (split to detector/display paths)")
if(NOT WIN32)
    message(STATUS "  - signal_relay    (Linux relay server)")
endif()
message(STATUS "")
