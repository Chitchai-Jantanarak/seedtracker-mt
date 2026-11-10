cmake_minimum_required(VERSION 3.20)
project(seedfinder LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# GPU backend. Needs no network: the OpenCL headers are vendored under
# third_party/ and the runtime is the system OpenCL library (the ICD loader that
# ships with the GPU driver), loaded at run time. OFF builds a CPU-only binary.
option(SEEDFINDER_OPENCL "Build the OpenCL GPU backend" OFF)

find_package(Threads REQUIRED)

set(SEEDFINDER_SOURCES
  src/config/options.cpp
  src/config/cli.cpp
  src/logs/observation_log.cpp
  src/rng/mapping_probe.cpp
  src/compute/chain.cpp
  src/compute/volatility.cpp
  src/compute/stats.cpp
  src/compute/cpu_scanner.cpp
  src/compute/backend_select.cpp
  src/export/csv.cpp
  src/export/report.cpp
)

if(SEEDFINDER_OPENCL)
  list(APPEND SEEDFINDER_SOURCES src/compute/opencl_scanner.cpp)

  # Embed the kernel: sim_core.h and scan.cl become string literals in a
  # generated header, so the exe needs no files at run time. Editing either
  # source re-runs configure and regenerates the header.
  set(SF_SIM_CORE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/include/seedfinder/core/sim_core.h")
  set(SF_SCAN_CL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/compute/kernels/scan.cl")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SF_SIM_CORE_PATH}" "${SF_SCAN_CL_PATH}")
  file(READ "${SF_SIM_CORE_PATH}" SF_SIM_CORE_SOURCE)
  file(READ "${SF_SCAN_CL_PATH}" SF_SCAN_CL_SOURCE)
  configure_file(src/compute/kernels/kernel_source.hpp.in
                 "${CMAKE_CURRENT_BINARY_DIR}/generated/seedfinder/kernel_source.hpp" @ONLY)
endif()

add_library(seedfinder_core ${SEEDFINDER_SOURCES})
target_include_directories(seedfinder_core PUBLIC include)
target_link_libraries(seedfinder_core PUBLIC Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(seedfinder_core PUBLIC -Wall -Wextra -Wpedantic)
endif()

if(SEEDFINDER_OPENCL)
  target_compile_definitions(seedfinder_core PUBLIC
    SEEDFINDER_HAVE_OPENCL=1
    CL_TARGET_OPENCL_VERSION=120)   # stay on the OpenCL 1.2 API
  target_include_directories(seedfinder_core SYSTEM PUBLIC third_party/OpenCL-Headers)
  target_include_directories(seedfinder_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  # The OpenCL runtime is loaded at run time (LoadLibrary/dlopen), not linked:
  # see src/compute/opencl_scanner.cpp. No import library is needed.
  target_link_libraries(seedfinder_core PUBLIC ${CMAKE_DL_LIBS})
endif()

add_executable(seedfinder apps/seedfinder/main.cpp)
target_link_libraries(seedfinder PRIVATE seedfinder_core)
