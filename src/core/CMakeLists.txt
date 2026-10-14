# The calculation engine. Deliberately has no Qt dependency.
add_library(lemma_core STATIC
    src/dimension.cpp
    src/evaluator.cpp
    src/format.cpp
    src/parser.cpp
    src/units.cpp
    src/worksheet.cpp
)

target_include_directories(lemma_core PUBLIC include)
target_compile_features(lemma_core PUBLIC cxx_std_20)
