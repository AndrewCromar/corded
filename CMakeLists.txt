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

if(WIN32)
  # Keep windows.h from defining min/max macros, and target Windows 10 and later.
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
  add_compile_options(/utf-8 /bigobj)
endif()

option(CORDED_BUILD_TESTS "Build the test suite" ON)
option(CORDED_PORTABLE "Link the C++ runtime statically so the programs run on other machines" ON)

# Programs find libcorded next to themselves, so a folder of them can be copied
# to another machine and run from anywhere.
set(CMAKE_BUILD_RPATH_USE_ORIGIN ON)
set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
option(CORDED_BUILD_TUI "Build the terminal client" ON)
# A phone or a GUI embeds libcorded and has no use for the server or the
# terminal programs.
option(CORDED_CORE_ONLY "Build only libcorded" OFF)
if(ANDROID)
  set(CORDED_CORE_ONLY ON)
endif()
if(CORDED_CORE_ONLY)
  set(CORDED_BUILD_TESTS OFF)
  set(CORDED_BUILD_TUI OFF)
endif()

# ---- dependencies ---------------------------------------------------------
find_package(Threads REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(flatbuffers CONFIG REQUIRED)
if(NOT CORDED_CORE_ONLY)  # only the server uses these
  find_package(unofficial-sqlite3 CONFIG REQUIRED)
  find_package(spdlog CONFIG REQUIRED)
endif()
find_package(nlohmann_json CONFIG REQUIRED)

# libsodium. On Windows vcpkg provides it. Elsewhere the system's copy is used
# if it is new enough; otherwise the official release is downloaded and built
# as a static library, which needs only a C compiler and make.
option(CORDED_BUNDLED_SODIUM "Build libsodium from its official release instead of using the system's" OFF)
add_library(corded_sodium INTERFACE)
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
  target_link_libraries(corded_sodium INTERFACE unofficial-sodium::sodium)
else()
  if(NOT CORDED_BUNDLED_SODIUM)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(SODIUM QUIET IMPORTED_TARGET libsodium>=1.0.19)
    endif()
  endif()
  if(SODIUM_FOUND AND NOT CORDED_BUNDLED_SODIUM)
    message(STATUS "libsodium: using the system's ${SODIUM_VERSION}")
    target_link_libraries(corded_sodium INTERFACE PkgConfig::SODIUM)
  else()
    message(STATUS "libsodium: building the bundled 1.0.20")
    include(ExternalProject)
    set(sodium_prefix ${CMAKE_BINARY_DIR}/sodium)
    ExternalProject_Add(corded_sodium_build
      URL https://download.libsodium.org/libsodium/releases/libsodium-1.0.20.tar.gz
          https://github.com/jedisct1/libsodium/releases/download/1.0.20-RELEASE/libsodium-1.0.20.tar.gz
      URL_HASH SHA256=ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${sodium_prefix} --libdir=${sodium_prefix}/lib
                        --disable-shared --enable-static --with-pic --disable-dependency-tracking
                        CC=${CMAKE_C_COMPILER}
      BUILD_COMMAND make -j4
      INSTALL_COMMAND make install
      BUILD_BYPRODUCTS ${sodium_prefix}/lib/libsodium.a
      LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON)
    file(MAKE_DIRECTORY ${sodium_prefix}/include)
    add_library(corded_sodium_static STATIC IMPORTED GLOBAL)
    set_target_properties(corded_sodium_static PROPERTIES
      IMPORTED_LOCATION ${sodium_prefix}/lib/libsodium.a
      INTERFACE_INCLUDE_DIRECTORIES ${sodium_prefix}/include)
    add_dependencies(corded_sodium_static corded_sodium_build)
    target_link_libraries(corded_sodium INTERFACE corded_sodium_static Threads::Threads)
  endif()
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
if(MSVC)
  target_compile_options(corded_sqlite3mc PRIVATE /O2 /w)
  target_link_libraries(corded_sqlite3mc PRIVATE Threads::Threads)
else()
  target_compile_options(corded_sqlite3mc PRIVATE -O2 -w)
  target_link_libraries(corded_sqlite3mc PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# ---- wire schema ----------------------------------------------------------
# flatc is a build tool: when building for another kind of machine than this
# one, vcpkg keeps it with the host's packages.
file(GLOB corded_flatc_dirs ${VCPKG_INSTALLED_DIR}/*/tools/flatbuffers)
find_program(FLATC flatc
  HINTS ${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/flatbuffers
        ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/flatbuffers
        ${corded_flatc_dirs}
  REQUIRED)
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
if(MSVC)
  target_compile_options(corded_warnings INTERFACE /W3)
else()
  target_compile_options(corded_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
endif()

# Keep C++ template statics out of libcorded's dynamic symbol table.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-gnu-unique>)
endif()

add_library(corded_common INTERFACE)
add_dependencies(corded_common corded_proto_gen)
target_include_directories(corded_common INTERFACE common/include ${CORDED_GENERATED_DIR})
target_link_libraries(corded_common INTERFACE
  flatbuffers::flatbuffers corded_sodium asio::asio OpenSSL::SSL OpenSSL::Crypto)
target_compile_definitions(corded_common INTERFACE ASIO_STANDALONE)

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
# On Linux a linker script keeps everything but corded_* private. On Windows
# only functions marked for export in corded.h leave the DLL anyway.
if(UNIX AND NOT APPLE)
  target_link_options(corded PRIVATE -Wl,--exclude-libs,ALL
    -Wl,--version-script=${CMAKE_SOURCE_DIR}/core/corded.map)
  set_target_properties(corded PROPERTIES LINK_DEPENDS ${CMAKE_SOURCE_DIR}/core/corded.map)
endif()
set_target_properties(corded PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(ANDROID)
  # An app loads plain "libcorded.so". Newer phones use 16 KB memory pages and
  # refuse a library laid out for 4 KB ones.
  target_link_options(corded PRIVATE -Wl,-z,max-page-size=16384)
  target_link_libraries(corded PRIVATE log)
else()
  set_target_properties(corded PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
endif()

# Which release this is, and the public key that updates are checked against
# (see server/src/update.hpp and the verify_release command in the core).
execute_process(COMMAND git describe --tags --match v* --always
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE CORDED_BUILD_VERSION
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT CORDED_BUILD_VERSION)
  set(CORDED_BUILD_VERSION "dev")
endif()
file(STRINGS "${CMAKE_SOURCE_DIR}/packaging/release-signing.pub" CORDED_RELEASE_KEY_LINES REGEX "^[A-Za-z0-9+/=]+$")
list(JOIN CORDED_RELEASE_KEY_LINES "" CORDED_RELEASE_KEY)
target_compile_definitions(corded_core PRIVATE CORDED_RELEASE_KEY="${CORDED_RELEASE_KEY}")
target_compile_definitions(corded PRIVATE CORDED_BUILD_VERSION="${CORDED_BUILD_VERSION}")

if(CORDED_CORE_ONLY)
  install(TARGETS corded RUNTIME DESTINATION . LIBRARY DESTINATION .)
  install(FILES include/corded/corded.h LICENSE NOTICE DESTINATION .)
  return()
endif()

# ---- server ---------------------------------------------------------------
add_executable(cordedd server/src/main.cpp)
target_compile_definitions(cordedd PRIVATE ASIO_STANDALONE
  CORDED_BUILD_VERSION="${CORDED_BUILD_VERSION}" CORDED_RELEASE_KEY="${CORDED_RELEASE_KEY}")
target_link_libraries(cordedd PRIVATE
  corded_common unofficial::sqlite3::sqlite3 asio::asio spdlog::spdlog Threads::Threads
  nlohmann_json::nlohmann_json corded_warnings)

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

# With the C++ runtime linked in, the programs need only the system C library.
if(CORDED_PORTABLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  foreach(target cordedd corded corded-cli)
    target_link_options(${target} PRIVATE -static-libstdc++ -static-libgcc)
  endforeach()
  if(TARGET corded-tui)
    target_link_options(corded-tui PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()

include(GNUInstallDirs)
install(TARGETS cordedd corded-cli corded RUNTIME DESTINATION . LIBRARY DESTINATION .)
if(TARGET corded-tui)
  install(TARGETS corded-tui RUNTIME DESTINATION .)
endif()
install(FILES LICENSE NOTICE DESTINATION .)

if(CORDED_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
