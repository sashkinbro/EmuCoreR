set(SRCS
  openbios.bin.h
)

add_library(openbios INTERFACE ${SRCS})
target_include_directories(openbios INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
