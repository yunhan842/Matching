cmake_minimum_required(VERSION 3.16)
project(Matching LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Put executables in build/bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(orderbook
    src/main.cpp
)

# Boost headers from Homebrew
target_include_directories(orderbook PRIVATE
    ${CMAKE_SOURCE_DIR}
    /opt/homebrew/opt/boost/include
)

target_compile_options(orderbook PRIVATE
    $<$<CONFIG:Release>:-O3 -march=native -funroll-loops>
)

# Link-Time Optimization (Release)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    set_property(TARGET orderbook PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()