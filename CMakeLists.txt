cmake_minimum_required(VERSION 3.18)
project(rpc_messenger LANGUAGES CXX)

find_package(CapnProto CONFIG REQUIRED)
capnp_generate_cpp(chat_sources chat_headers chat.capnp)

add_executable(messenger main.cpp chat.cpp transport.cpp terminal.cpp ${chat_sources} ${chat_headers})
target_compile_features(messenger PRIVATE cxx_std_20)
target_compile_options(messenger PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(messenger PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(messenger PRIVATE CapnProto::capnp-rpc)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME messenger-integration
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/chat_test.py"
                     "$<TARGET_FILE:messenger>")
    set_tests_properties(messenger-integration PROPERTIES TIMEOUT 30)
endif()
