cmake_minimum_required(VERSION 3.16)

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets Quick QuickWidgets Qml)

add_library(qml-inspector STATIC
    inspectorserver.h
    inspectorserver.cpp
)

target_include_directories(qml-inspector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(qml-inspector PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Widgets
    Qt6::Quick
    Qt6::QuickWidgets
    Qt6::Qml
)
