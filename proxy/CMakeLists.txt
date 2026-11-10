add_library(version SHARED
    version_proxy.cpp
    version_proxy.def
)

target_link_libraries(version PRIVATE shlwapi)
set_target_properties(version PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
    SUFFIX ".dll"
)
