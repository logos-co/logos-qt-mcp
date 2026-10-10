cmake_minimum_required(VERSION 3.16)
project(qml-inspector-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Quick QuickWidgets)

add_subdirectory(../qt-plugin ${CMAKE_CURRENT_BINARY_DIR}/qml-inspector)

add_executable(inspector-test-app main.cpp)
qt_add_resources(inspector-test-app "scene" PREFIX "/" FILES popup.qml)
target_link_libraries(inspector-test-app PRIVATE qml-inspector Qt6::Widgets Qt6::QuickWidgets)

install(TARGETS inspector-test-app RUNTIME DESTINATION bin)
