cmake_minimum_required(VERSION 3.10)
project(CppCache CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compile every .cpp under src/ into one executable named "cppcache".
file(GLOB_RECURSE SOURCES src/*.cpp)
add_executable(cppcache ${SOURCES})

# Let files include each other with paths relative to src/ (e.g. "net/Server.h").
target_include_directories(cppcache PRIVATE src)
