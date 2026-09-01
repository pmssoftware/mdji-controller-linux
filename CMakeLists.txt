cmake_minimum_required(VERSION 3.16)
project(mdji-controller VERSION 1.0.0 LANGUAGES CXX)

add_executable(mdji-controller src/main.cpp)
target_compile_features(mdji-controller PRIVATE cxx_std_17)
target_compile_options(mdji-controller PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS mdji-controller RUNTIME DESTINATION bin)
install(FILES 60-mdji-controller.rules DESTINATION lib/udev/rules.d)
