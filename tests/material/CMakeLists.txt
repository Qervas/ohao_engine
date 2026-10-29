# OpenPBR's base (docs/plans/2026-10-29-openpbr-step1.md, T1): shaders/material/openpbr.slang on the
# CPU through tools/material's C++ build, and its energy tables current.
add_executable(material_tests material_tests.cpp)
target_compile_features(material_tests PRIVATE cxx_std_20)
target_include_directories(material_tests PRIVATE ${CMAKE_SOURCE_DIR}/ohao)
target_link_libraries(material_tests PRIVATE ohao_openpbr_cpu ohao_test_harness)
ohao_add_test(material_tests LABEL quick COMMAND material_tests)
# The checked-in tables are what this build's module integrates (regenerate them after a change to
# the module: build/Release/openpbr_tables ohao/render/material/openpbr_tables.hpp).
ohao_add_test(openpbr_tables_current LABEL quick COMMAND openpbr_tables --check ohao/render/material/openpbr_tables.hpp)
