cmake_minimum_required(VERSION 3.25)

project(lemma
    VERSION 0.1.0
    DESCRIPTION "A Mathcad-style worksheet for engineering calculations"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LEMMA_BUILD_APP "Build the Qt desktop application" ON)
option(LEMMA_BUILD_TESTS "Build the unit tests" ON)

if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_subdirectory(src/core)

if(LEMMA_BUILD_APP)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Test)
    qt_standard_project_setup()
    add_subdirectory(src/app)
endif()

if(LEMMA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
