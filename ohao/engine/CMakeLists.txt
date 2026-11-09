# The one install of a World's systems (system design 4.2, step 2b): every host calls installEngine, and
# installRendering once it has a renderer.
add_library(ohao_engine STATIC install.cpp install.hpp)
target_include_directories(ohao_engine PUBLIC ${CMAKE_SOURCE_DIR}/ohao)
target_compile_features(ohao_engine PUBLIC cxx_std_20)
target_link_libraries(ohao_engine PUBLIC ohao_ecs ohao_level_world)
