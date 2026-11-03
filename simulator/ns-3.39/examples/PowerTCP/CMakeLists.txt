build_example(
  NAME powertcp-evaluation-burst
  SOURCE_FILES 
    powertcp-evaluation-burst.cc
    cdf.c
  HEADER_FILES
    cdf.h
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libtraffic-control}
    ${libinternet}
    ${libapplications}
)
build_example(
  NAME crossDC-evaluation
  SOURCE_FILES 
    crossDC-evaluation.cc
    cdf.c
  HEADER_FILES
    cdf.h
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libtraffic-control}
    ${libinternet}
    ${libapplications}
)
# crossDC-evaluation-workload 已移至 CrossDC-Algos 目录，避免重复定义
# build_example(
#   NAME crossDC-evaluation-workload
#   SOURCE_FILES
#     crossDC-evaluation-workload.cc
#     cdf.c
#   HEADER_FILES
#     cdf.h
#   LIBRARIES_TO_LINK
#     ${libpoint-to-point}
#     ${libtraffic-control}
#     ${libinternet}
#     ${libapplications}
# )
build_example(
  NAME powertcp-evaluation-fairness
  SOURCE_FILES 
    powertcp-evaluation-fairness.cc
    cdf.c
  HEADER_FILES
    cdf.h
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libtraffic-control}
    ${libinternet}
    ${libapplications}
)
build_example(
  NAME powertcp-evaluation-workload
  SOURCE_FILES 
    powertcp-evaluation-workload.cc
    cdf.c
  HEADER_FILES
    cdf.h
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libtraffic-control}
    ${libinternet}
    ${libapplications}
)
set_source_files_properties(cdf.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)