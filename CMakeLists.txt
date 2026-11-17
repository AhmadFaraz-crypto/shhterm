cmake_minimum_required(VERSION 3.20)
project(shhterm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(SHHTERM_BUILD_TESTS "Build the test suite" ON)

# Default server address compiled into the client so ordinary users only need
# `shhterm create` / `shhterm join <key>`. This points at the public community
# server by default; it has a trusted (Let's Encrypt) certificate, so clients
# need no extra flags. SELF-HOSTERS: override this with your own address at
# configure time (-DSHHTERM_DEFAULT_SERVER=host:port) or at runtime
# (--server / SHHTERM_SERVER).
set(SHHTERM_DEFAULT_SERVER "shhterm.duckdns.org:8443"
    CACHE STRING "Default server host:port baked into the client")

# ---- System dependencies ---------------------------------------------------
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED)

# ---- Fetched dependencies --------------------------------------------------
include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
  GIT_TAG v5.0.0
  GIT_SHALLOW TRUE)

set(JSON_BuildTests OFF CACHE INTERNAL "")
set(FTXUI_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(FTXUI_BUILD_DOCS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json ftxui)

if(SHHTERM_BUILD_TESTS)
  FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE)
  # doctest's CMakeLists declares an ancient minimum that CMake 4 rejects.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
  FetchContent_MakeAvailable(doctest)
  unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
endif()

# ---- Shared protocol library ----------------------------------------------
add_library(shhterm_proto STATIC proto/protocol.cpp)
target_include_directories(shhterm_proto PUBLIC proto)
target_link_libraries(shhterm_proto PUBLIC nlohmann_json::nlohmann_json
                                            OpenSSL::Crypto)

# ---- Server core library (no main) ----------------------------------------
add_library(shhterm_server_lib STATIC
  server/room.cpp
  server/session.cpp
  server/server.cpp)
target_include_directories(shhterm_server_lib PUBLIC server)
target_link_libraries(shhterm_server_lib PUBLIC
  shhterm_proto
  Boost::boost
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads)

# ---- Server executable -----------------------------------------------------
add_executable(shhterm-server server/main.cpp)
target_link_libraries(shhterm-server PRIVATE shhterm_server_lib)

# ---- Client networking library (no UI, so tests can link it) ---------------
configure_file(client/config.hpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/shhterm_config.hpp" @ONLY)

add_library(shhterm_client_lib STATIC client/net_client.cpp client/crypto.cpp)
target_include_directories(shhterm_client_lib PUBLIC client
                           "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(shhterm_client_lib PUBLIC
  shhterm_proto
  Boost::boost
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads)

# ---- Client executable -----------------------------------------------------
add_executable(shhterm client/main.cpp client/ui.cpp)
target_link_libraries(shhterm PRIVATE
  shhterm_client_lib
  ftxui::component)  # transitively pulls ftxui::dom and ftxui::screen

# ---- Crypto interop tool (verification aid; built with tests) --------------
if(SHHTERM_BUILD_TESTS)
  add_executable(crypto_tool tools/crypto_tool.cpp)
  target_link_libraries(crypto_tool PRIVATE shhterm_client_lib)
endif()

# ---- Tests -----------------------------------------------------------------
if(SHHTERM_BUILD_TESTS)
  enable_testing()
  add_executable(shhterm_tests
    tests/test_main.cpp
    tests/test_protocol.cpp
    tests/test_room.cpp
    tests/test_crypto.cpp
    tests/test_integration.cpp)
  target_link_libraries(shhterm_tests PRIVATE
    shhterm_server_lib
    shhterm_client_lib
    doctest::doctest)
  add_test(NAME shhterm_tests COMMAND shhterm_tests)
endif()
