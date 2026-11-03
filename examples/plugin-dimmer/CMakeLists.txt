# An example of a master module as a plugin: built with the host tools, installed where hubusd looks for plugins by default.
add_library(hubus_plugin_dimmer MODULE dimmer_plugin.c)
target_include_directories(hubus_plugin_dimmer PRIVATE ${PROJECT_SOURCE_DIR}/master ${PROJECT_SOURCE_DIR}/core)
set_target_properties(hubus_plugin_dimmer PROPERTIES PREFIX "" OUTPUT_NAME dimmer)
target_compile_options(hubus_plugin_dimmer PRIVATE ${HUBUS_WARNINGS})
install(TARGETS hubus_plugin_dimmer LIBRARY DESTINATION lib/hubus/modules)
