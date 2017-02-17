cmake_minimum_required(VERSION 2.6)

PROJECT(opkg-tests C)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/../libopkg ${CMAKE_CURRENT_SOURCE_DIR}/..)
LINK_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/../libopkg ${CMAKE_CURRENT_SOURCE_DIR}/../libbb)

IF(STATIC_UBOX)
	FIND_LIBRARY(ubox NAMES libubox.a)
ELSE()
	FIND_LIBRARY(ubox NAMES ubox)
ENDIF()

FIND_LIBRARY(pthread NAMES pthread)

ADD_EXECUTABLE(libopkg_test libopkg_test.c)
TARGET_LINK_LIBRARIES(libopkg_test bb opkg bb ${ubox} ${pthread})

ADD_EXECUTABLE(opkg_active_list_test opkg_active_list_test.c)
TARGET_LINK_LIBRARIES(opkg_active_list_test bb opkg bb ${ubox} ${pthread})

ADD_EXECUTABLE(opkg_extract_test opkg_extract_test.c)
TARGET_LINK_LIBRARIES(opkg_extract_test bb opkg bb ${ubox} ${pthread})

#ADD_EXECUTABLE(opkg_hash_test opkg_hash_test.c)
#TARGET_LINK_LIBRARIES(opkg_hash_test bb opkg bb ${ubox} ${pthread})
