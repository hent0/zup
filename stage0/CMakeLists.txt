cmake_minimum_required(VERSION 3.13)

project(stage0)

file(GLOB_RECURSE SOURCE_FILES *.c include/*.h)

set(CMAKE_C_STANDARD 23) # Enable the C23 standard
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(stage0 ${SOURCE_FILES})

target_compile_options(stage0 PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Werror>
    $<$<C_COMPILER_ID:MSVC>:/W4 /WX>
)

target_include_directories(stage0 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(stage0 PRIVATE m)
