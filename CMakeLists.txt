cmake_minimum_required(VERSION 3.10)
project(devrun CXX)
set(CMAKE_CXX_STANDARD 17)
add_executable(devrun src/main.cpp src/process.cpp src/config.cpp)
