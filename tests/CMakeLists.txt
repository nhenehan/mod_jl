add_executable(test_codec test_codec.c)
target_link_libraries(test_codec PRIVATE jlproto)
add_test(NAME codec COMMAND test_codec ${PROJECT_SOURCE_DIR}/testdata/vectors)

add_executable(bench_codec bench_codec.c)
target_link_libraries(bench_codec PRIVATE jlproto)

# A real JlServ worker driven by jlc; JL_E2E_ECHO_MIB=1024 for the 1 GiB echo.
if(JULIA AND JL_BUILD_CLIENT)
    add_test(NAME e2e COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/e2e.sh $<TARGET_FILE:jlc> ${PROJECT_SOURCE_DIR})
    add_test(NAME examples COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/examples.sh $<TARGET_FILE:jlc> ${PROJECT_SOURCE_DIR})
    set_tests_properties(e2e examples PROPERTIES ENVIRONMENT JULIA=${JULIA} TIMEOUT 900)
endif()

# The user guide builds, so its {{#include}}s of the examples still resolve.
if(MDBOOK)
    add_test(NAME docs COMMAND ${MDBOOK} build ${PROJECT_SOURCE_DIR}/docs -d ${CMAKE_CURRENT_BINARY_DIR}/book)
    # mdbook reports a missing include as an ERROR but still exits 0.
    set_tests_properties(docs PROPERTIES FAIL_REGULAR_EXPRESSION "ERROR|WARN")
endif()

# Optional memcheck runs of the C side (configure with -DJL_VALGRIND=ON, run
# with ctest -L valgrind). Julia workers run natively; every jlc and the codec
# test run under tests/vg.sh, and each test then checks its own logs.
if(JL_VALGRIND)
    set(VG ${CMAKE_CURRENT_SOURCE_DIR}/vg.sh)
    set(VG_LOGS ${CMAKE_CURRENT_BINARY_DIR}/valgrind)

    add_test(NAME vg_codec COMMAND sh -c "rm -rf \"$1\" && VG_LOG_DIR=\"$1\" \"$2\" \"$3\" \"$4\" && \"$5\" \"$1\""
        _ ${VG_LOGS}/codec ${VG} $<TARGET_FILE:test_codec> ${PROJECT_SOURCE_DIR}/testdata/vectors
        ${CMAKE_CURRENT_SOURCE_DIR}/vg-check.sh)
    set_tests_properties(vg_codec PROPERTIES ENVIRONMENT JLP_MUTATIONS=2000)

    if(JULIA AND JL_BUILD_CLIENT)
        add_test(NAME vg_e2e COMMAND sh -c "rm -rf \"$1\" && exec \"$2\" \"$3\" \"$4\""
            _ ${VG_LOGS}/e2e ${CMAKE_CURRENT_SOURCE_DIR}/e2e.sh $<TARGET_FILE:jlc> ${PROJECT_SOURCE_DIR})
        set_tests_properties(vg_e2e PROPERTIES ENVIRONMENT
            "JULIA=${JULIA};JLC_WRAP=${VG};VG_LOG_DIR=${VG_LOGS}/e2e;JL_E2E_ECHO_MIB=4;JL_E2E_BENCH_SECS=8;JL_E2E_SIGINT_DELAY=5")
        add_test(NAME vg_examples COMMAND sh -c "rm -rf \"$1\" && exec \"$2\" \"$3\" \"$4\""
            _ ${VG_LOGS}/examples ${CMAKE_CURRENT_SOURCE_DIR}/examples.sh $<TARGET_FILE:jlc> ${PROJECT_SOURCE_DIR})
        set_tests_properties(vg_examples PROPERTIES ENVIRONMENT
            "JULIA=${JULIA};JLC_WRAP=${VG};VG_LOG_DIR=${VG_LOGS}/examples;JL_EX_SUBSCRIBE_DELAY=3")
    endif()
    set_property(TEST vg_codec PROPERTY LABELS valgrind)
    set_tests_properties(vg_codec PROPERTIES TIMEOUT 1800)
    if(TEST vg_e2e)
        set_tests_properties(vg_e2e vg_examples PROPERTIES LABELS valgrind TIMEOUT 1800)
    endif()
endif()
