cmake_minimum_required(VERSION 3.24)
project(github_pages_webrtc_server VERSION 1.0.0 LANGUAGES CXX)

include(FetchContent)
find_package(Threads REQUIRED)

# Only the DataChannel and integrated WebSocket signaling features are needed.
set(NO_MEDIA ON CACHE BOOL "Build libdatachannel without media support" FORCE)
set(NO_WEBSOCKET OFF CACHE BOOL "Keep WebSocket support enabled" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "Do not build libdatachannel examples" FORCE)
set(NO_TESTS ON CACHE BOOL "Do not build libdatachannel tests" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Prefer a static libdatachannel build" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "Do not treat upstream warnings as errors" FORCE)

FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.6
    GIT_SHALLOW TRUE
    GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(libdatachannel)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(rtc_server
    server/main.cpp
)

target_compile_features(rtc_server PRIVATE cxx_std_20)
target_include_directories(rtc_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/server)
target_link_libraries(rtc_server PRIVATE
    LibDataChannel::LibDataChannel
    nlohmann_json::nlohmann_json
    Threads::Threads
)

if(MSVC)
    target_compile_options(rtc_server PRIVATE /W4 /permissive-)
else()
    target_compile_options(rtc_server PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS rtc_server RUNTIME DESTINATION bin)
