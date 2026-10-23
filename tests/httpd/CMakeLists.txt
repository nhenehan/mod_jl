# Tests of the httpd module.

# The module exports jl_module only, and links nothing httpd won't provide.
add_test(NAME module_symbols COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/check-symbols.sh $<TARGET_FILE:mod_jl>)

# One JlServ worker behind httpd, driven by curl: the basic suite on pooled
# connections, the same over a single connection (connection_pool_size=1, so
# whatever one test leaves on the connection reaches the next), and the pool's
# own tests.
if(JULIA AND APACHE2)
    set(_env "JULIA=${JULIA};APACHE2=${APACHE2}")
    set(_suffix "")
    if(JL_SANITIZE)
        # The module carries ASan: httpd runs as one process with libasan preloaded.
        set(_env "${_env};JL_ASAN=1")
        set(_suffix "_asan")
    endif()
    add_test(NAME httpd_basic${_suffix} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_basic.sh
             $<TARGET_FILE:mod_jl> ${PROJECT_SOURCE_DIR})
    set_tests_properties(httpd_basic${_suffix} PROPERTIES ENVIRONMENT "${_env}" TIMEOUT 1800 RESOURCE_LOCK httpd)
    add_test(NAME httpd_basic_pool1${_suffix} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_basic.sh
             $<TARGET_FILE:mod_jl> ${PROJECT_SOURCE_DIR})
    set_tests_properties(httpd_basic_pool1${_suffix} PROPERTIES
        ENVIRONMENT "${_env};JL_T_J1_OPTS=connection_pool_size=1;JL_T_BIG_MIB=64" TIMEOUT 1800 RESOURCE_LOCK httpd)
    add_test(NAME httpd_pool${_suffix} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_pool.sh
             $<TARGET_FILE:mod_jl> ${PROJECT_SOURCE_DIR})
    set_tests_properties(httpd_pool${_suffix} PROPERTIES ENVIRONMENT "${_env}" TIMEOUT 900 RESOURCE_LOCK httpd)
    # Balancing, sticky sessions and failover; under prefork as well, where
    # the state must be shared between processes (not under ASan, which runs
    # httpd as one process).
    add_test(NAME httpd_lb${_suffix} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_lb.sh
             $<TARGET_FILE:mod_jl> ${PROJECT_SOURCE_DIR})
    set_tests_properties(httpd_lb${_suffix} PROPERTIES ENVIRONMENT "${_env}" TIMEOUT 900 RESOURCE_LOCK httpd)
    # The jl-status page and the watchdog.
    add_test(NAME httpd_status${_suffix} COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_status.sh
             $<TARGET_FILE:mod_jl> ${PROJECT_SOURCE_DIR})
    set_tests_properties(httpd_status${_suffix} PROPERTIES ENVIRONMENT "${_env}" TIMEOUT 600 RESOURCE_LOCK httpd)
    if(NOT JL_SANITIZE)
        add_test(NAME httpd_status_prefork COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_status.sh
                 $<TARGET_FILE:mod_jl> ${PROJECT_SOURCE_DIR})
        set_tests_properties(httpd_status_prefork PROPERTIES
            ENVIRONMENT "${_env};JL_T_MPM=prefork" TIMEOUT 600 RESOURCE_LOCK httpd)
        # examples/httpd/ still loads.
        add_test(NAME examples_httpd COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_examples.sh
                 $<TARGET_FILE:mod_jl> ${PROJECT_SOURCE_DIR})
        set_tests_properties(examples_httpd PROPERTIES ENVIRONMENT "${_env}" TIMEOUT 120 RESOURCE_LOCK httpd)
        add_test(NAME httpd_lb_prefork COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_lb.sh
                 $<TARGET_FILE:mod_jl> ${PROJECT_SOURCE_DIR})
        set_tests_properties(httpd_lb_prefork PROPERTIES
            ENVIRONMENT "${_env};JL_T_MPM=prefork" TIMEOUT 900 RESOURCE_LOCK httpd)
    endif()
endif()
