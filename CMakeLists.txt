cmake_minimum_required(VERSION 3.9)
project(balancer VERSION 0.0.1 DESCRIPTION "balancer module")



set(DEFAULT_BUILD_TYPE "Release")
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-ggdb")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-o2")
set (CMAKE_CXX_COMPILER_VERSION 14)
set (CMAKE_CXX_STANDARD 23)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_COMPILER ${C_COMPILER})
set (CMAKE_CXX_COMPILER ${CXX_COMPILER})
set(CMAKE_C_FLAGS_DEBUG "-ggdb -DDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-ggdb -DDEBUG")
set(CMAKE_CXX_FLAGS "-std=gnu++2b -ggdb")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(balancer ${PROJECT_SOURCE_DIR}/main.cpp ${PROJECT_SOURCE_DIR}/balancer.hpp)
target_link_libraries(balancer -lgcc -lstdc++ -ldl -lpthread -lm -lvirt)

