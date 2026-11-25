# Consumer of modx-core, two ways:
#   cmake -S examples/install -B b -DCMAKE_PREFIX_PATH=<prefix>        installed package: find_package
#   cmake -S examples/install -B b -DMODX_CORE_SOURCE_DIR=<checkout>   source tree: add_subdirectory
# (FetchContent_Declare(modx-core GIT_REPOSITORY ...) + FetchContent_MakeAvailable works like the second.)
cmake_minimum_required(VERSION 3.14)
project(modx-hello C)

if(MODX_CORE_SOURCE_DIR)
  add_subdirectory(${MODX_CORE_SOURCE_DIR} modx-core)
else()
  find_package(modx-core 0.3 REQUIRED)
endif()

add_executable(hello hello.c)
set_target_properties(hello PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(hello PRIVATE modx::core)
