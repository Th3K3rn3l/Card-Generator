cmake_minimum_required(VERSION 3.10)
project(card_generator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
    # Suppress noisy "deprecated" warnings for the C runtime functions we use
    # deliberately (localtime, getenv, fopen). Each call site is reviewed.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
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

# -----------------------------------------------------------------------------
# Optional: BIN database
#
# The generator's --info / --lookup / --check features can enrich output with
# bank, country and card-type data. The dataset is sourced from
# https://github.com/venelinkochev/bin-list-data (CC-BY-4.0). It's ~27 MB and
# changes slowly, so we keep it out of the repository and provide an opt-in
# CMake target to fetch it.
#
# Usage:
#   cmake --build build --target download-bin-db
#
# Or set CARD_GENERATOR_BIN_DB to point at any compatible CSV.
# -----------------------------------------------------------------------------
set(BIN_DB_URL  "https://raw.githubusercontent.com/venelinkochev/bin-list-data/master/bin-list-data.csv"
    CACHE STRING "URL of the BIN-list CSV used by --info / --lookup")
set(BIN_DB_PATH "${CMAKE_SOURCE_DIR}/data/bin-list-data.csv"
    CACHE PATH   "Local path where the BIN database is stored")

add_custom_target(download-bin-db
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/data"
    COMMAND ${CMAKE_COMMAND}
            -DBIN_DB_URL=${BIN_DB_URL}
            -DBIN_DB_PATH=${BIN_DB_PATH}
            -P "${CMAKE_SOURCE_DIR}/cmake/DownloadBinDb.cmake"
    COMMENT "Downloading BIN database (CC-BY-4.0) to ${BIN_DB_PATH}"
    VERBATIM)
