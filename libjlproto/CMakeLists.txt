set(SCHEMA_C ${CMAKE_CURRENT_BINARY_DIR}/jlp_schema.c)
add_custom_command(
    OUTPUT ${SCHEMA_C}
    COMMAND ${CMAKE_COMMAND} -DIN=${JL_SCHEMA} -DOUT=${SCHEMA_C} -DSYM=jlp_schema_xsd
            -P ${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${JL_SCHEMA} ${PROJECT_SOURCE_DIR}/cmake/EmbedFile.cmake
    COMMENT "Embedding jlp2.xsd")

add_library(jlproto STATIC
    src/jlp_frame.c
    src/jlp_ctx.c
    src/jlp_encode.c
    src/jlp_decode.c
    src/gen/jlp_vocab.c
    ${SCHEMA_C})
set_target_properties(jlproto PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(jlproto
    PUBLIC include
    PRIVATE src src/gen)
target_link_libraries(jlproto PUBLIC rexi::rexi)
