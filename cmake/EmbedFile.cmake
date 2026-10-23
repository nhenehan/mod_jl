# Run as a script: cmake -DIN=<file> -DOUT=<file.c> -DSYM=<name> -P EmbedFile.cmake
# Writes <name> (the file's bytes plus a NUL, so it can be used as a C string),
# <name>_len (without the NUL) and <name>_sha256.

file(READ ${IN} hex HEX)
string(LENGTH "${hex}" hex_len)
math(EXPR len "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " bytes "${bytes}")

file(SHA256 ${IN} sha)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," sha_bytes "${sha}")

file(WRITE ${OUT}.tmp
"/* Generated from ${IN} by cmake/EmbedFile.cmake. */
#include <stddef.h>
#include <stdint.h>

const char ${SYM}[] = {
    ${bytes}0x00
};
const size_t ${SYM}_len = ${len};
const uint8_t ${SYM}_sha256[32] = { ${sha_bytes} };
")
file(COPY_FILE ${OUT}.tmp ${OUT} ONLY_IF_DIFFERENT)
file(REMOVE ${OUT}.tmp)
