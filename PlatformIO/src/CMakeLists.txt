# Registers the application sources as the IDF "main" component. PlatformIO
# compiles them itself with flags mirrored from the IDF build; this file exists
# so the component graph (Arduino core, USB host, lwIP, ...) links to them.
FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "../include")
