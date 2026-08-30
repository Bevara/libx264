cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBX264_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/enc_x264.c
)

SET(LIBX264_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libx264.a
)

SET(LIBX264_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libx264
        "${LIBX264_SRC}"
        "${LIBX264_LIB}"
        []
        ""
        "${LIBX264_INC}"
        ""
        "1")
