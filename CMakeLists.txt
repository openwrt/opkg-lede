cmake_minimum_required(VERSION 2.6)

PROJECT(opkg C)

OPTION(HOST_CPU "Override Host CPU")
OPTION(BUILD_CPU "Override Host CPU")
OPTION(LOCK_FILE "Override lock file path" /var/lock/opkg.lock)
OPTION(STATIC_UBOX "Statically link libubox")
OPTION(BUILD_TESTS "Build test programs" ON)

IF(NOT HOST_CPU)
	SET(HOST_CPU "${CMAKE_HOST_SYSTEM_PROCESSOR}")
ENDIF()

IF(NOT BUILD_CPU)
	SET(BUILD_CPU "${CMAKE_SYSTEM_PROCESSOR}")
ENDIF()

ADD_DEFINITIONS(-Os -Wall --std=gnu99 -g3 -Wmissing-declarations
	-DDATADIR="/usr/share"
	-DOPKGETCDIR="/etc"
	-DOPKGLOCKFILE="${LOCK_FILE}"
	-DOPKGLIBDIR="/usr/lib"
	-DHOST_CPU_STR="${HOST_CPU}"
	-DBUILD_CPU=${BUILD_CPU}
)

ADD_SUBDIRECTORY(libbb)
ADD_SUBDIRECTORY(libopkg)
ADD_SUBDIRECTORY(src)

IF(BUILD_TESTS)
	ADD_SUBDIRECTORY(tests)
ENDIF()
