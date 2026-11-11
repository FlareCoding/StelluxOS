# Programs the recipe measures the kit's flags on: a plain one, one on QtWidgets and QtSvg, one also
# on QtGui's private headers, and one also on Core5Compat. The tree links its own platform plugin.
cmake_minimum_required(VERSION 3.22)
project(qtprobe LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg GuiPrivate Core5Compat)

add_executable(probe_plain plain.cpp)

qt_add_executable(probe_public widgets.cpp)
target_link_libraries(probe_public PRIVATE Qt6::Widgets Qt6::Svg)
qt_import_plugins(probe_public EXCLUDE_BY_TYPE platforms)

qt_add_executable(probe_private widgets.cpp)
target_link_libraries(probe_private PRIVATE Qt6::Widgets Qt6::Svg Qt6::GuiPrivate)
qt_import_plugins(probe_private EXCLUDE_BY_TYPE platforms)

qt_add_executable(probe_core5compat widgets.cpp)
target_link_libraries(probe_core5compat PRIVATE Qt6::Widgets Qt6::Svg Qt6::Core5Compat)
qt_import_plugins(probe_core5compat EXCLUDE_BY_TYPE platforms)
