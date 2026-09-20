cmake_minimum_required(VERSION 3.20)

project(linux_shell_runtime VERSION 1.0.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "linux_shell_runtime currently targets Linux (procfs, namespaces, cgroups, ptrace).")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_WERROR "Treat compiler warnings as errors" OFF)
option(ENABLE_FUZZING "Build the libFuzzer parser target (Clang only)" OFF)
option(BUILD_SHELL "Build the interactive shell executable" ON)

add_library(project_warnings INTERFACE)
target_compile_options(project_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
)
if(ENABLE_WERROR)
    target_compile_options(project_warnings INTERFACE -Werror)
endif()

add_library(project_sanitizers INTERFACE)
if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(project_sanitizers INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(project_sanitizers INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
    else()
        message(WARNING "ENABLE_SANITIZERS requested, but compiler is not GCC/Clang")
    endif()
endif()

set(SHELL_SOURCES
    src/builtins.cpp
    src/executor.cpp
    src/history.cpp
    src/job_control.cpp
    src/lexer.cpp
    src/main.cpp
    src/parser.cpp
    src/process_info.cpp
    src/resource_control.cpp
    src/sandbox.cpp
    src/shell_utils.cpp
    src/trace.cpp
)

if(BUILD_SHELL)
    find_path(READLINE_INCLUDE_DIR readline/readline.h REQUIRED)
    find_library(READLINE_LIBRARY NAMES readline REQUIRED)

    add_executable(shell ${SHELL_SOURCES})
    target_include_directories(shell PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${READLINE_INCLUDE_DIR}
    )
    target_link_libraries(shell PRIVATE
        ${READLINE_LIBRARY}
        project_warnings
        project_sanitizers
    )
endif()

function(configure_test_target target)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${target} PRIVATE project_warnings project_sanitizers)
endfunction()

include(CTest)
if(BUILD_TESTING)
    add_executable(parser_smoke_tests
        tests/parser_smoke.cpp
        src/parser.cpp
        src/lexer.cpp
    )
    configure_test_target(parser_smoke_tests)
    add_test(NAME parser_smoke COMMAND parser_smoke_tests)
    set_tests_properties(parser_smoke PROPERTIES LABELS "smoke;parser")

    add_executable(parser_fuzz_smoke_tests
        tests/parser_fuzz_smoke.cpp
        src/parser.cpp
        src/lexer.cpp
    )
    configure_test_target(parser_fuzz_smoke_tests)
    add_test(NAME parser_fuzz_smoke COMMAND parser_fuzz_smoke_tests)
    set_tests_properties(parser_fuzz_smoke PROPERTIES LABELS "hardening;fuzz;parser")

    add_executable(resource_control_smoke_tests
        tests/resource_control_smoke.cpp
        src/resource_control.cpp
        src/shell_utils.cpp
    )
    configure_test_target(resource_control_smoke_tests)
    add_test(NAME resource_control_smoke COMMAND resource_control_smoke_tests)
    set_tests_properties(resource_control_smoke PROPERTIES LABELS "smoke;resource")

    add_executable(sandbox_smoke_tests
        tests/sandbox_smoke.cpp
        src/sandbox.cpp
        src/shell_utils.cpp
    )
    configure_test_target(sandbox_smoke_tests)
    add_test(NAME sandbox_smoke COMMAND sandbox_smoke_tests)
    set_tests_properties(sandbox_smoke PROPERTIES LABELS "smoke;sandbox")

    add_executable(trace_smoke_tests
        tests/trace_smoke.cpp
        src/trace.cpp
        src/shell_utils.cpp
    )
    configure_test_target(trace_smoke_tests)
    add_test(NAME trace_smoke COMMAND trace_smoke_tests)
    set_tests_properties(trace_smoke PROPERTIES LABELS "smoke;trace")

    add_executable(process_info_smoke_tests
        tests/process_info_smoke.cpp
        src/process_info.cpp
    )
    configure_test_target(process_info_smoke_tests)
    add_test(NAME process_info_smoke COMMAND process_info_smoke_tests)
    set_tests_properties(process_info_smoke PROPERTIES LABELS "smoke;procfs")

    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE AND TARGET shell)
        set(SHELL_REGRESSION_TESTS
            shell_basics
            parser
            job_control
            process_observability
            resource_control
            isolation
            tracing
        )
        foreach(test_name IN LISTS SHELL_REGRESSION_TESTS)
            add_test(
                NAME ${test_name}_regression
                COMMAND ${BASH_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/${test_name}_regression.sh
                        $<TARGET_FILE:shell>
            )
            set_tests_properties(${test_name}_regression PROPERTIES LABELS "regression;${test_name}")
        endforeach()
    endif()

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND TARGET shell)
        add_test(
                NAME job_control_pty
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/job_control_pty.py
                    $<TARGET_FILE:shell>
        )
            set_tests_properties(job_control_pty PROPERTIES LABELS "pty;job-control")

        add_test(
                NAME process_observability_pty
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/process_observability_pty.py
                    $<TARGET_FILE:shell>
        )
            set_tests_properties(process_observability_pty PROPERTIES LABELS "pty;procfs")

        add_test(
                NAME tracing_job_control_pty
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/tracing_job_control_pty.py
                    $<TARGET_FILE:shell>
        )
            set_tests_properties(tracing_job_control_pty PROPERTIES LABELS "pty;trace")

        add_test(
                NAME lifecycle_stress
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle_stress.py
                    $<TARGET_FILE:shell>
        )
            set_tests_properties(lifecycle_stress PROPERTIES
            LABELS "hardening;stress;lifecycle"
            TIMEOUT 45
        )
    endif()
endif()

if(ENABLE_FUZZING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_FUZZING requires Clang/libFuzzer")
    endif()

    add_executable(parser_fuzzer
        fuzz/parser_fuzzer.cpp
        src/parser.cpp
        src/lexer.cpp
    )
    target_include_directories(parser_fuzzer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_options(parser_fuzzer PRIVATE
        -Wall -Wextra -Wpedantic
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(parser_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined
        -fno-omit-frame-pointer
    )
endif()
