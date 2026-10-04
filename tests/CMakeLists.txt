cmake_minimum_required(VERSION 3.10)
project(NprValidation LANGUAGES C CXX)

add_executable(npr_tests NprAdvancedTests.cpp
    ../src/TargaImage.cpp ../src/ScriptHandler.cpp ../src/libtarga.c)
target_include_directories(npr_tests PRIVATE ../src ../include)
target_compile_features(npr_tests PRIVATE cxx_std_14)
target_compile_definitions(npr_tests PRIVATE _CRT_SECURE_NO_WARNINGS _XKEYCHECK_H)
if(MSVC)
    target_compile_options(npr_tests PRIVATE /utf-8)
endif()
enable_testing()
add_test(NAME advanced_npr COMMAND npr_tests)
