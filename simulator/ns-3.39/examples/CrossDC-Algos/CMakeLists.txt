build_example(
  NAME crossDC-evaluation-workload
  SOURCE_FILES
    crossDC-evaluation-workload.cc
    workload-generator.cc
    cdf.c
  HEADER_FILES
    cdf.h
    workload-generator.h
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libtraffic-control}
    ${libinternet}
    ${libapplications}
)
set_source_files_properties(cdf.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
