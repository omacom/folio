add_library(folio_stl STATIC render.cpp render.h)
target_include_directories(folio_stl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(folio_stl PUBLIC Qt6::Core Qt6::Gui)
target_compile_options(folio_stl PRIVATE -Wall -Wextra)

add_executable(folio-stl-thumbnailer main.cpp)
target_link_libraries(folio-stl-thumbnailer PRIVATE folio_stl)
set_target_properties(folio-stl-thumbnailer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
