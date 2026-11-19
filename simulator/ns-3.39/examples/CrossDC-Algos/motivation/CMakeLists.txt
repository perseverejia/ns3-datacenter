build_example(
  NAME crossdc-motivation
  SOURCE_FILES scenario.cc measurements.cc
  LIBRARIES_TO_LINK ${libpoint-to-point} ${libinternet} ${libapplications}
)
