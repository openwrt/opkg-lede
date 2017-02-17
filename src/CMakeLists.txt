cmake_minimum_required(VERSION 2.6)

PROJECT(opkg-cl C)

INCLUDE(CheckLibraryExists)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/../libopkg)
LINK_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/../libopkg ${CMAKE_CURRENT_SOURCE_DIR}/../libbb)

IF(STATIC_UBOX)
	FIND_LIBRARY(ubox NAMES libubox.a)
ELSE()
	FIND_LIBRARY(ubox NAMES ubox)
ENDIF()

ADD_EXECUTABLE(opkg-cl opkg-cl.c)
TARGET_LINK_LIBRARIES(opkg-cl opkg bb ${ubox})

CHECK_LIBRARY_EXISTS(pthread pthread_create "" NEED_PTHREAD)
IF(NEED_PTHREAD)
	TARGET_LINK_LIBRARIES(opkg-cl pthread)
ENDIF()
