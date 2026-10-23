# Imports rexi-capi (the Rust EXI codec) as the static library rexi::rexi.
# rexi's source comes from GitHub, at a tag, and cargo builds it inside this
# build directory; librexi.d, which cargo writes, makes the build rerun when a
# rexi source file changes. For local work on rexi, point CMake at a checkout:
#   -DFETCHCONTENT_SOURCE_DIR_REXI=/path/to/rexi
include(FetchContent)
FetchContent_Declare(rexi
    GIT_REPOSITORY https://github.com/nhenehan/rexi.git
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE)
# rexi has no CMakeLists.txt: this only fetches it.
FetchContent_MakeAvailable(rexi)

set(REXI_TARGET_DIR ${CMAKE_BINARY_DIR}/rexi-target)
set(REXI_LIB ${REXI_TARGET_DIR}/release/librexi.a)
set(REXI_INCLUDE ${rexi_SOURCE_DIR}/crates/rexi-capi/include)
# Valgrind builds want line tables in rexi.
set(REXI_CARGO_ENV "")
if(JL_VALGRIND)
    set(REXI_CARGO_ENV CARGO_PROFILE_RELEASE_DEBUG=line-tables-only)
endif()

if(NOT EXISTS ${REXI_INCLUDE}/rexi.h)
    message(FATAL_ERROR "rexi.h not found in ${rexi_SOURCE_DIR}")
endif()

find_program(CARGO cargo HINTS $ENV{HOME}/.cargo/bin REQUIRED)
add_custom_command(
    OUTPUT ${REXI_LIB}
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${REXI_TARGET_DIR} ${REXI_CARGO_ENV}
            ${CARGO} build --release --locked -p rexi-capi
    WORKING_DIRECTORY ${rexi_SOURCE_DIR}
    DEPFILE ${REXI_TARGET_DIR}/release/librexi.d
    COMMENT "cargo build -p rexi-capi"
    USES_TERMINAL)
add_custom_target(rexi_build DEPENDS ${REXI_LIB})

# The rexi command line tool, for tools/make-vectors.sh (not built by default).
add_custom_target(rexi-cli
    COMMAND ${CMAKE_COMMAND} -E env CARGO_TARGET_DIR=${REXI_TARGET_DIR}
            ${CARGO} build --release --locked -p rexi-xml --bin rexi
    WORKING_DIRECTORY ${rexi_SOURCE_DIR}
    COMMENT "cargo build -p rexi-xml --bin rexi"
    USES_TERMINAL)

find_package(Threads REQUIRED)
add_library(rexi::rexi STATIC IMPORTED GLOBAL)
set_target_properties(rexi::rexi PROPERTIES
    IMPORTED_LOCATION ${REXI_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${REXI_INCLUDE})
# From: cargo rustc -p rexi-capi --release -- --print native-static-libs
target_link_libraries(rexi::rexi INTERFACE Threads::Threads ${CMAKE_DL_LIBS} m rt util gcc_s)
add_dependencies(rexi::rexi rexi_build)
