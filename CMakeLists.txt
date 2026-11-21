cmake_minimum_required(VERSION 3.20)
project(kaleidoscope LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Locate LLVM through the LLVMConfig.cmake of a build or install tree, e.g.
#   -DLLVM_DIR=$HOME/work/llvm-project/build/lib/cmake/llvm
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

# LLVM is built without RTTI by default; code that uses its headers has to
# match, otherwise the link fails with "undefined reference to typeinfo".
if(NOT LLVM_ENABLE_RTTI)
  add_compile_options(-fno-rtti)
endif()
add_compile_options(-Wall -Wextra)

add_executable(toy src/toy.cpp)

# The LLVM components toy links against. This list grows with the chapters.
llvm_map_components_to_libnames(LLVM_LIBS analysis core instcombine passes scalaropts support)
target_link_libraries(toy PRIVATE ${LLVM_LIBS})

# `ninja -C build check` rebuilds toy and runs the lit tests under test/.
find_program(LLVM_LIT NAMES llvm-lit lit HINTS ${LLVM_TOOLS_BINARY_DIR})
if(LLVM_LIT)
  add_custom_target(check
    COMMAND ${LLVM_LIT} -v ${CMAKE_SOURCE_DIR}/test
            -Dbuild_dir=${CMAKE_BINARY_DIR}
            -Dllvm_tools_dir=${LLVM_TOOLS_BINARY_DIR}
    DEPENDS toy
    USES_TERMINAL
    COMMENT "Running the Kaleidoscope regression tests")
else()
  message(WARNING "llvm-lit not found: the 'check' target is unavailable")
endif()