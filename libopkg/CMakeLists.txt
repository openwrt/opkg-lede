cmake_minimum_required(VERSION 2.6)

PROJECT(libopkg C)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/..)
LINK_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/../libbb)

ADD_LIBRARY(opkg STATIC
	active_list.c conffile.c conffile_list.c file_util.c hash_table.c
	nv_pair.c nv_pair_list.c opkg.c opkg_cmd.c opkg_conf.c opkg_configure.c
	opkg_download.c opkg_install.c opkg_message.c opkg_remove.c
	opkg_upgrade.c opkg_utils.c parse_util.c pkg.c pkg_depends.c pkg_dest.c
	pkg_dest_list.c pkg_extract.c pkg_hash.c pkg_parse.c pkg_src.c
	pkg_src_list.c pkg_vec.c sha256.c sprintf_alloc.c str_list.c
	void_list.c xregex.c xsystem.c
)
