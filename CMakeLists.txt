cmake_minimum_required(VERSION 3.13)

project(zup)

file(GLOB_RECURSE SOURCE_FILES src/*.c src/*.h)

set(CMAKE_C_STANDARD 23) # Enable the C23 standard
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(zup ${SOURCE_FILES})

target_compile_options(zup PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Werror>
    $<$<C_COMPILER_ID:MSVC>:/W4 /WX>
)

target_include_directories(zup PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(zup PRIVATE m)
