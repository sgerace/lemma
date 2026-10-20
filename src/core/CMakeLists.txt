# The calculation engine. Deliberately has no Qt dependency.
include(FetchContent)

FetchContent_Declare(fast_float
    GIT_REPOSITORY https://github.com/fastfloat/fast_float.git
    GIT_TAG v8.3.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fast_float)

add_library(lemma_core STATIC
    src/dimension.cpp
    src/evaluator.cpp
    src/format.cpp
    src/math.cpp
    src/math_edit.cpp
    src/parser.cpp
    src/plot.cpp
    src/units.cpp
    src/worksheet.cpp
)

target_include_directories(lemma_core PUBLIC include)
target_compile_features(lemma_core PUBLIC cxx_std_20)
target_link_libraries(lemma_core PRIVATE FastFloat::fast_float)
