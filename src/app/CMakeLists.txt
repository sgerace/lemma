# Everything but main(), so the GUI tests can link it.
qt_add_library(lemma_app STATIC
    DocumentIO.cpp
    DocumentIO.h
    MainWindow.cpp
    MainWindow.h
    MathRender.cpp
    MathRender.h
    PdfExport.cpp
    PdfExport.h
    WorksheetCanvas.cpp
    WorksheetCanvas.h
)
# STIX Two Math (SIL Open Font License; see fonts/STIXTwoMath-OFL.txt).
qt_add_resources(lemma_app "fonts"
    PREFIX "/"
    FILES fonts/STIXTwoMath-Regular.otf
)
target_include_directories(lemma_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lemma_app PUBLIC lemma_core Qt6::Widgets)
target_compile_definitions(lemma_app PUBLIC LEMMA_VERSION="${PROJECT_VERSION}")

qt_add_executable(lemma WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(lemma PRIVATE lemma_app)
