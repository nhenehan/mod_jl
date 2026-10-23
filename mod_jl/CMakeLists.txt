# The httpd module. Linked with gcc rather than apxs/libtool, which do not get
# along with static archives and version scripts. Only jl_module is exported:
# libjlproto, and rexi with Rust's std inside it, stay private to the module.
find_package(APXS REQUIRED)

add_library(mod_jl MODULE mod_jl.c jl_conf.c jl_map.c jl_io.c jl_worker.c jl_lb.c jl_shm.c jl_request.c jl_status.c
    jl_watchdog.c)
set_target_properties(mod_jl PROPERTIES PREFIX "" C_VISIBILITY_PRESET hidden)
target_include_directories(mod_jl PRIVATE ${APXS_INCLUDE_DIRS})
target_compile_options(mod_jl PRIVATE ${APXS_DEFINITIONS} -ffunction-sections -fdata-sections)
# ap_* and apr_* come from the httpd process that loads the module: no libapr,
# no -z defs.
target_link_options(mod_jl PRIVATE
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/mod_jl.map
    -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,--as-needed -Wl,-z,relro,-z,now)
set_property(TARGET mod_jl APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mod_jl.map)
target_link_libraries(mod_jl PRIVATE jlproto)
