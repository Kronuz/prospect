# Fails when the test-only faultlab reaches prospect's interface target.
if(LIBS MATCHES "faultlab")
  message(FATAL_ERROR "prospect INTERFACE_LINK_LIBRARIES names faultlab: ${LIBS}")
endif()
