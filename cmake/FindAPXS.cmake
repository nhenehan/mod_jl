# Finds what an httpd module is compiled against: apxs (INCLUDEDIR,
# EXTRA_CPPFLAGS), APR and APR-util includes, and the httpd binary for tests.
# Not apxs's CFLAGS: they carry the distribution's -O2 and _FORTIFY_SOURCE,
# which the build type should decide.
#
# Sets APXS_FOUND, APXS_INCLUDE_DIRS, APXS_DEFINITIONS, APACHE2 (the binary).

find_program(APXS NAMES apxs apxs2)
find_program(APR_CONFIG NAMES apr-1-config)
find_program(APU_CONFIG NAMES apu-1-config)
find_program(APACHE2 NAMES apache2 httpd PATHS /usr/sbin /usr/local/apache2/bin)

if(APXS AND APR_CONFIG)
    execute_process(COMMAND ${APXS} -q INCLUDEDIR OUTPUT_VARIABLE _httpd_inc OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${APXS} -q EXTRA_CPPFLAGS OUTPUT_VARIABLE _httpd_cpp OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${APR_CONFIG} --includedir OUTPUT_VARIABLE _apr_inc OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${APR_CONFIG} --cppflags OUTPUT_VARIABLE _apr_cpp OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(_apu_inc "")
    if(APU_CONFIG)
        execute_process(COMMAND ${APU_CONFIG} --includedir OUTPUT_VARIABLE _apu_inc OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    set(APXS_INCLUDE_DIRS ${_httpd_inc} ${_apr_inc} ${_apu_inc})
    separate_arguments(APXS_DEFINITIONS UNIX_COMMAND "${_httpd_cpp} ${_apr_cpp}")
    list(REMOVE_DUPLICATES APXS_DEFINITIONS)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(APXS REQUIRED_VARS APXS APR_CONFIG APXS_INCLUDE_DIRS)
