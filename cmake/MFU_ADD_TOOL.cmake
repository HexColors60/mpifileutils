FUNCTION(MFU_ADD_TOOL name)

  BLT_ADD_EXECUTABLE(NAME ${name}
    SOURCES ${name}.c
    DEPENDS_ON mfu dtcmp mpi bzip2 openssl)
  SET_TARGET_PROPERTIES(${name} PROPERTIES C_STANDARD 99)
  INSTALL(TARGETS ${name} RUNTIME DESTINATION ${X_BINDIR})

ENDFUNCTION(MFU_ADD_TOOL name)
