cmake_minimum_required(VERSION 3.10)
INCLUDE(CheckIncludeFiles)

PROJECT(quectel-timesync C)

SET(SOURCES quectel-timesync.c)

ADD_DEFINITIONS(-Os -Wall -Werror --std=gnu99 -g3)

ADD_EXECUTABLE(quectel-timesync ${SOURCES})

TARGET_LINK_LIBRARIES(quectel-timesync)

SET(CMAKE_INSTALL_PREFIX /usr)

INSTALL(TARGETS quectel-timesync RUNTIME DESTINATION sbin)
