include(FetchContent)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(lemma_tests
    test_dimension.cpp
    test_format.cpp
    test_math.cpp
    test_math_layout.cpp
    test_parser.cpp
    test_worksheet.cpp
)
target_link_libraries(lemma_tests PRIVATE lemma_core Catch2::Catch2WithMain)

if(MINGW)
    # Run without needing the MinGW runtime DLLs on PATH.
    target_link_options(lemma_tests PRIVATE -static)
endif()

include(Catch)
catch_discover_tests(lemma_tests)

# GUI tests: drive the canvas with Qt Test on the offscreen platform, so they never
# touch the real desktop.
if(LEMMA_BUILD_APP)
    qt_add_executable(lemma_gui_tests gui/test_canvas.cpp)
    target_link_libraries(lemma_gui_tests PRIVATE lemma_app Qt6::Test)
    target_compile_definitions(lemma_gui_tests PRIVATE
        LEMMA_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")

    add_test(NAME lemma_gui_tests COMMAND lemma_gui_tests)
    set_tests_properties(lemma_gui_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;$<$<PLATFORM_ID:Windows>:QT_QPA_FONTDIR=$ENV{WINDIR}/Fonts>"
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
