add_executable(rpc_tests rpc_tests.cpp)
target_compile_features(rpc_tests PRIVATE cxx_std_20)
target_link_libraries(rpc_tests PRIVATE ohao_rpc ohao_test_harness)
if(MSVC)
    target_link_options(rpc_tests PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
ohao_add_test(rpc_tests LABEL quick COMMAND rpc_tests)

# Test 10's server: ohao_rpc with a method that prints to stdout through every C and C++
# path; tests/serve/check_serve.py checks the protocol stream stays clean.
add_executable(rpc_print_server rpc_print_server.cpp)
target_compile_features(rpc_print_server PRIVATE cxx_std_20)
target_link_libraries(rpc_print_server PRIVATE ohao_rpc)
if(MSVC)
    target_link_options(rpc_print_server PRIVATE /FORCE:MULTIPLE /IGNORE:4088)
endif()
