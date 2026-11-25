# modx-core with CMake. The Makefile stays the primary build (tests, sanitizers,
# fuzzing, benchmarks); this file builds the library for CMake consumers:
#
#   find_package(modx-core 0.3 REQUIRED)        after `make install` or `cmake --install`
#   add_subdirectory(modx-core)                 vendored / submodule
#   FetchContent_Declare(modx-core GIT_REPOSITORY https://github.com/saidmashhud/modx-core GIT_TAG <tag>)
#   FetchContent_MakeAvailable(modx-core)
#
#   target_link_libraries(app PRIVATE modx::core)
#
# Options: MODX_ZLIB (gzip through the system zlib, ON), BUILD_SHARED_LIBS (OFF),
# MODX_INSTALL (install rules, ON when top level), MODX_BUILD_TESTS (the unit
# tests as `ctest`, ON when top level).
cmake_minimum_required(VERSION 3.14)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/modx/version.h" _modx_ver REGEX "^#define MODX_VERSION_(MAJOR|MINOR|PATCH) ")
foreach(_part MAJOR MINOR PATCH)
  string(REGEX REPLACE ".*MODX_VERSION_${_part} ([0-9]+).*" "\\1" _modx_${_part} "${_modx_ver}")
endforeach()
project(modx-core VERSION ${_modx_MAJOR}.${_modx_MINOR}.${_modx_PATCH} LANGUAGES C)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(_modx_top ON)
else()
  set(_modx_top OFF)
endif()
option(MODX_ZLIB "gzip responses through the system zlib" ON)
option(MODX_INSTALL "Generate install rules" ${_modx_top})
option(MODX_BUILD_TESTS "Build the unit tests (ctest)" ${_modx_top})

include(GNUInstallDirs)

set(MODX_SOURCES
  src/util.c src/json.c src/crypto.c src/http.c src/sse.c src/static.c src/ot.c
  src/session.c src/store.c src/timer.c src/cache.c src/gzip.c src/ws.c src/idem.c
  src/version.c src/pool.c)

add_library(modx-core ${MODX_SOURCES})
add_library(modx::core ALIAS modx-core)

# 0.x: every minor release may change the ABI
if(PROJECT_VERSION_MAJOR EQUAL 0)
  set(_modx_soversion ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
else()
  set(_modx_soversion ${PROJECT_VERSION_MAJOR})
endif()
set_target_properties(modx-core PROPERTIES
  OUTPUT_NAME modx
  EXPORT_NAME core
  VERSION ${PROJECT_VERSION}
  SOVERSION ${_modx_soversion}
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(modx-core PRIVATE _DEFAULT_SOURCE _DARWIN_C_SOURCE)
target_include_directories(modx-core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(modx-core PUBLIC Threads::Threads)
if(MODX_ZLIB)
  find_package(ZLIB REQUIRED)
  target_link_libraries(modx-core PUBLIC ZLIB::ZLIB)
  set(_modx_libs_private "-lz -lm")
else()
  target_compile_definitions(modx-core PRIVATE MODX_NO_ZLIB)
  set(_modx_libs_private "-lm")
endif()
find_library(MODX_LIBM m)
if(MODX_LIBM)
  target_link_libraries(modx-core PUBLIC ${MODX_LIBM})
endif()

if(MODX_INSTALL)
  include(CMakePackageConfigHelpers)
  set(_modx_cmakedir ${CMAKE_INSTALL_LIBDIR}/cmake/modx-core)
  install(TARGETS modx-core EXPORT modx-core-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(DIRECTORY include/modx DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h")
  install(EXPORT modx-core-targets NAMESPACE modx:: FILE modx-core-targets.cmake DESTINATION ${_modx_cmakedir})
  configure_package_config_file(cmake/modx-core-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/modx-core-config.cmake INSTALL_DESTINATION ${_modx_cmakedir})
  write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/modx-core-config-version.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/modx-core-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/modx-core-config-version.cmake DESTINATION ${_modx_cmakedir})
  # the same pkg-config file as `make install`
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(_modx_pc_libdir "${CMAKE_INSTALL_LIBDIR}")
  else()
    set(_modx_pc_libdir "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(_modx_pc_includedir "${CMAKE_INSTALL_INCLUDEDIR}")
  else()
    set(_modx_pc_includedir "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
  endif()
  file(READ ${CMAKE_CURRENT_SOURCE_DIR}/modx-core.pc.in _modx_pc)
  string(REPLACE "@PREFIX@" "${CMAKE_INSTALL_PREFIX}" _modx_pc "${_modx_pc}")
  string(REPLACE "@LIBDIR@" "${_modx_pc_libdir}" _modx_pc "${_modx_pc}")
  string(REPLACE "@INCLUDEDIR@" "${_modx_pc_includedir}" _modx_pc "${_modx_pc}")
  string(REPLACE "@VERSION@" "${PROJECT_VERSION}" _modx_pc "${_modx_pc}")
  string(REPLACE "@LIBS_PRIVATE@" "${_modx_libs_private}" _modx_pc "${_modx_pc}")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/modx-core.pc "${_modx_pc}")
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/modx-core.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

if(MODX_BUILD_TESTS)
  enable_testing()
  file(GLOB _modx_test_sources ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c)
  add_executable(modx-core-tests ${_modx_test_sources})
  set_target_properties(modx-core-tests PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
  target_compile_definitions(modx-core-tests PRIVATE _DEFAULT_SOURCE _DARWIN_C_SOURCE)
  target_link_libraries(modx-core-tests PRIVATE modx::core)
  add_test(NAME unit COMMAND modx-core-tests WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
