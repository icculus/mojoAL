# YOU PROBABLY DON'T HAVE TO USE THIS CMAKE PROJECT, IT'S JUST A SINGLE
#  C FILE. COPY IT INTO YOUR PROJECT INSTEAD.

cmake_minimum_required(VERSION 3.12.0)
project(mojoAL)

find_package(SDL2 REQUIRED)

add_library(mojoal SHARED mojoal.c)
target_include_directories(mojoal PRIVATE ${SDL2_INCLUDE_DIRS} AL)
target_link_libraries(mojoal ${SDL2_LIBRARIES})

macro(add_test_executable _NAME)
    add_executable(${_NAME} tests/${_NAME}.c)
    target_link_libraries(${_NAME} mojoal)
    target_include_directories(${_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(${_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(${_NAME} ${SDL2_LIBRARIES})
endmacro()

add_test_executable(testopenalinfo)
add_test_executable(testqueueing)
add_test_executable(testcapture)
add_test_executable(testposition)


