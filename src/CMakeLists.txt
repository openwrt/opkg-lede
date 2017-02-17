cmake_minimum_required(VERSION 2.6)

PROJECT(opkg-cl C)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/../libopkg)
LINK_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/../libopkg ${CMAKE_CURRENT_SOURCE_DIR}/../libbb)

IF(STATIC_UBOX)
	FIND_LIBRARY(ubox NAMES libubox.a)
ELSE()
	FIND_LIBRARY(ubox NAMES ubox)
ENDIF()

FIND_LIBRARY(pthread NAMES pthread)

ADD_EXECUTABLE(opkg-cl opkg-cl.c)
TARGET_LINK_LIBRARIES(opkg-cl opkg bb ${ubox} ${pthread})
