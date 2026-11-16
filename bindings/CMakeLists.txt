pybind11_add_module(_nexus MODULE nexus_bindings.cpp)
target_link_libraries(_nexus PRIVATE nexus_core)
target_compile_definitions(_nexus PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
install(TARGETS _nexus LIBRARY DESTINATION . RUNTIME DESTINATION .)
