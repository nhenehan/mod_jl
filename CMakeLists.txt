cmake_minimum_required(VERSION 3.24)
project(mod_jl VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

option(JL_BUILD_MODULE "Build the Apache httpd module (needs apxs and APR headers)" OFF)
option(JL_BUILD_CLIENT "Build jlc, the C test and benchmark client" ON)
option(JL_BUILD_TESTS "Build the C tests" ON)
option(JL_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(JL_VALGRIND "Register memcheck tests (ctest -L valgrind)" OFF)
if(JL_VALGRIND AND JL_SANITIZE)
    message(FATAL_ERROR "JL_VALGRIND and JL_SANITIZE exclude each other: valgrind cannot run ASan binaries")
endif()

if(JL_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

set(JL_SCHEMA ${CMAKE_CURRENT_SOURCE_DIR}/schema/jlp2.xsd)

include(Rexi)
add_subdirectory(libjlproto)

if(JL_BUILD_CLIENT)
    add_subdirectory(jl_client)
endif()
if(JL_BUILD_MODULE)
    add_subdirectory(mod_jl)
endif()

find_program(JULIA julia)
if(JL_VALGRIND)
    find_program(VALGRIND valgrind REQUIRED)
endif()
find_program(MDBOOK mdbook HINTS $ENV{HOME}/.cargo/bin)

if(JL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    if(JL_BUILD_MODULE)
        add_subdirectory(tests/httpd)
    endif()
endif()

# Regenerate (vocab) or verify (vocab-check) everything derived from the schema.
if(JULIA)
    foreach(t vocab vocab-check)
        set(flag "")
        if(t STREQUAL "vocab-check")
            set(flag --check)
        endif()
        add_custom_target(${t}
            COMMAND ${JULIA} --startup-file=no --project=${CMAKE_CURRENT_SOURCE_DIR}/jl_serv/JlServ
                    ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen.jl ${flag}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            USES_TERMINAL)
    endforeach()
endif()
