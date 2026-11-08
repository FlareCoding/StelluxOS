# Programs the recipe measures the kit's flags on: a plain one, one on QtWidgets and QtSvg, one on QtGui's
# private headers too, and one on Qt Quick. The tree links its own platform plugin and no QML debug server.
cmake_minimum_required(VERSION 3.22)
project(qtprobe LANGUAGES CXX)

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg GuiPrivate Quick QuickControls2)

add_executable(probe_plain plain.cpp)

qt_add_executable(probe_public widgets.cpp)
target_link_libraries(probe_public PRIVATE Qt6::Widgets Qt6::Svg)
qt_import_plugins(probe_public EXCLUDE_BY_TYPE platforms)

qt_add_executable(probe_private widgets.cpp)
target_link_libraries(probe_private PRIVATE Qt6::Widgets Qt6::Svg Qt6::GuiPrivate)
qt_import_plugins(probe_private EXCLUDE_BY_TYPE platforms)

qt_policy(SET QTP0001 NEW)
qt_add_executable(probe_quick quick.cpp)
set_target_properties(probe_quick PROPERTIES AUTOMOC ON)
qt_add_qml_module(probe_quick URI Probe QML_FILES Main.qml)
target_link_libraries(probe_quick PRIVATE Qt6::Widgets Qt6::Svg Qt6::Quick Qt6::QuickControls2)
qt_import_plugins(probe_quick EXCLUDE_BY_TYPE platforms qmltooling)
qt_import_qml_plugins(probe_quick)
