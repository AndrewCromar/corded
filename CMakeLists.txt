cmake_minimum_required(VERSION 3.25)
project(corded VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(CORDED_BUILD_TESTS "Build the test suite" ON)
option(CORDED_BUILD_TUI "Build the terminal client" ON)

# ---- dependencies ---------------------------------------------------------
find_package(Threads REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# libsodium: the vcpkg copy if the `vendored-sodium` manifest feature is on,
# otherwise the system copy.
add_library(corded_sodium INTERFACE)
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
  target_link_libraries(corded_sodium INTERFACE unofficial-sodium::sodium)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.19)
  target_link_libraries(corded_sodium INTERFACE PkgConfig::SODIUM)
endif()

# Encrypted SQLite for the client vault. vcpkg has no SQLCipher port for
# Linux, so the prototype uses SQLite3 Multiple Ciphers (ChaCha20-Poly1305).
include(FetchContent)
FetchContent_Declare(sqlite3mc
  URL https://github.com/utelle/SQLite3MultipleCiphers/releases/download/v2.5.1/sqlite3mc-2.5.1-sqlite-3.53.4-amalgamation.zip
  URL_HASH SHA256=4125f8ff275ea953dabb3289331b20a0e76d4fc060f57148f4a5df3bf3b0d5e0
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite3mc)
add_library(corded_sqlite3mc STATIC ${sqlite3mc_SOURCE_DIR}/sqlite3mc_amalgamation.c)
target_include_directories(corded_sqlite3mc SYSTEM PUBLIC ${sqlite3mc_SOURCE_DIR})
target_compile_definitions(corded_sqlite3mc PRIVATE
  SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_DQS=0)
target_compile_options(corded_sqlite3mc PRIVATE -O2 -w)
target_link_libraries(corded_sqlite3mc PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)

# ---- wire schema ----------------------------------------------------------
find_program(FLATC flatc
  HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/flatbuffers REQUIRED)
set(CORDED_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
add_custom_command(
  OUTPUT ${CORDED_GENERATED_DIR}/corded_generated.h
  COMMAND ${FLATC} --cpp --gen-object-api --cpp-std c++11 -o ${CORDED_GENERATED_DIR}
          ${CMAKE_SOURCE_DIR}/proto/corded.fbs
  DEPENDS ${CMAKE_SOURCE_DIR}/proto/corded.fbs
  COMMENT "Generating wire protocol code")
add_custom_target(corded_proto_gen DEPENDS ${CORDED_GENERATED_DIR}/corded_generated.h)

# ---- shared code ----------------------------------------------------------
add_library(corded_warnings INTERFACE)
target_compile_options(corded_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)

add_library(corded_common INTERFACE)
add_dependencies(corded_common corded_proto_gen)
target_include_directories(corded_common INTERFACE common/include ${CORDED_GENERATED_DIR})
target_link_libraries(corded_common INTERFACE flatbuffers::flatbuffers corded_sodium)

# ---- core library ---------------------------------------------------------
add_library(corded_core STATIC
  core/src/crypto/crypto.cpp
  core/src/vault/vault.cpp
  core/src/engine/engine.cpp)
target_include_directories(corded_core PUBLIC core/src include)
target_compile_definitions(corded_core PUBLIC CORDED_USE_SQLITE3MC ASIO_STANDALONE)
target_link_libraries(corded_core
  PUBLIC corded_common corded_sqlite3mc asio::asio nlohmann_json::nlohmann_json Threads::Threads
  PRIVATE corded_warnings)
set_target_properties(corded_core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

# libcorded: only the C ABI is exported.
add_library(corded SHARED core/src/abi/abi.cpp)
target_compile_definitions(corded PRIVATE CORDED_BUILDING)
target_include_directories(corded PUBLIC include)
target_link_libraries(corded PRIVATE corded_core corded_warnings)
target_link_options(corded PRIVATE -Wl,--exclude-libs,ALL)
set_target_properties(corded PROPERTIES
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON VERSION ${PROJECT_VERSION} SOVERSION 0)

# ---- server ---------------------------------------------------------------
add_executable(cordedd server/src/main.cpp)
target_compile_definitions(cordedd PRIVATE ASIO_STANDALONE)
target_link_libraries(cordedd PRIVATE
  corded_common unofficial::sqlite3::sqlite3 asio::asio spdlog::spdlog Threads::Threads
  corded_warnings)

# ---- frontends: they use the public header and nothing else ---------------
add_executable(corded-cli tools/corded-cli.cpp)
target_link_libraries(corded-cli PRIVATE corded Threads::Threads corded_warnings)

if(CORDED_BUILD_TUI)
  find_package(ftxui CONFIG REQUIRED)
  add_executable(corded-tui frontends/tui/main.cpp)
  target_link_libraries(corded-tui PRIVATE
    corded ftxui::screen ftxui::dom ftxui::component nlohmann_json::nlohmann_json
    Threads::Threads corded_warnings)
endif()

if(CORDED_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
