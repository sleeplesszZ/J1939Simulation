cmake_minimum_required(VERSION 3.10)
project(j1939_simulation VERSION 1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Create shared library
add_library(j1939sim SHARED
    src/j1939_simulation.cpp
    src/j1939_sim.cpp
)

target_include_directories(j1939sim
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Build examples if requested
option(BUILD_EXAMPLES "Build examples" OFF)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Installation
install(TARGETS j1939sim
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# 更新安装文件列表，移除不存在的头文件
install(FILES 
    include/j1939sim/j1939_sim.h
    include/j1939sim/j1939_simulation.hpp
    include/j1939sim/j1939_types.hpp
    DESTINATION include/j1939sim
)
