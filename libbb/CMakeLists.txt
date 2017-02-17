cmake_minimum_required(VERSION 2.6)

PROJECT(libbb C)

ADD_LIBRARY(bb STATIC
	all_read.c concat_path_file.c copy_file.c copy_file_chunk.c gzip.c
	gz_open.c last_char_is.c make_directory.c mode_string.c parse_mode.c
	safe_strncpy.c time_string.c unarchive.c unzip.c wfopen.c xfuncs.c
	xreadlink.c
)
