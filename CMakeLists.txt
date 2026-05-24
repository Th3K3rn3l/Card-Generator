cmake_minimum_required(VERSION 3.10)
project(card_generator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/O2 /W4 /permissive-)
else()
    add_compile_options(-O3 -Wall -Wextra -Wpedantic)
    # Optional: enable native CPU tuning. Disabled by default to keep binaries
    # portable; turn on with -DGENERATOR_NATIVE=ON.
    option(GENERATOR_NATIVE "Tune for the building host's CPU" OFF)
    if(GENERATOR_NATIVE)
        add_compile_options(-march=native)
    endif()
endif()

add_executable(generator src/main.cpp)
target_include_directories(generator PRIVATE src)

enable_testing()
add_executable(test_cards src/test.cpp)
target_include_directories(test_cards PRIVATE src)
add_test(NAME cards COMMAND test_cards)
