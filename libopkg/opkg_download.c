/* vi: set noexpandtab sw=4 sts=4: */
/* opkg_download.c - the opkg package management system

   Carl D. Worth

   Copyright (C) 2001 University of Southern California
   Copyright (C) 2008 OpenMoko Inc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#include <stdio.h>
#include <unistd.h>
#include <libgen.h>

#include "opkg_download.h"
#include "opkg_message.h"

#include "sprintf_alloc.h"
#include "xsystem.h"
#include "file_util.h"
#include "opkg_defines.h"
#include "libbb/libbb.h"

static int str_starts_with(const char *str, const char *prefix)
{
	return (strncmp(str, prefix, strlen(prefix)) == 0);
}

int
opkg_download(const char *src, const char *dest_file_name,
              const short hide_error)
{
	int err = 0;

	char *src_basec = xstrdup(src);
	char *src_base = basename(src_basec);
	char *tmp_file_location;

	opkg_msg(NOTICE, "Downloading %s\n", src);

	if (str_starts_with(src, "file:")) {
		char *file_src = urldecode_path(src + 5);
		opkg_msg(INFO, "Copying %s to %s...", file_src, dest_file_name);
		err = file_copy(file_src, dest_file_name);
		opkg_msg(INFO, "Done.\n");
		free(src_basec);
		free(file_src);
		return err;
	}

	sprintf_alloc(&tmp_file_location, "%s/%s", conf->tmp_dir, src_base);
	free(src_basec);
	err = unlink(tmp_file_location);
	if (err && errno != ENOENT) {
		opkg_perror(ERROR, "Failed to unlink %s", tmp_file_location);
		free(tmp_file_location);
		return -1;
	}

	if (conf->http_proxy) {
		opkg_msg(DEBUG,
			 "Setting environment variable: http_proxy = %s.\n",
			 conf->http_proxy);
		setenv("http_proxy", conf->http_proxy, 1);
	}
	if (conf->ftp_proxy) {
		opkg_msg(DEBUG,
			 "Setting environment variable: ftp_proxy = %s.\n",
			 conf->ftp_proxy);
		setenv("ftp_proxy", conf->ftp_proxy, 1);
	}
	if (conf->no_proxy) {
		opkg_msg(DEBUG,
			 "Setting environment variable: no_proxy = %s.\n",
			 conf->no_proxy);
		setenv("no_proxy", conf->no_proxy, 1);
	}

	{
		int res;
		const char *argv[11];
		int i = 0;

		argv[i++] = "wget";
		argv[i++] = "-q";
		if (conf->no_check_certificate) {
			argv[i++] = "--no-check-certificate";
		}
		if (conf->http_timeout) {
			argv[i++] = "--timeout";
			argv[i++] = conf->http_timeout;
		}
		if (conf->http_proxy || conf->ftp_proxy) {
			argv[i++] = "-Y";
			argv[i++] = "on";
		}
		argv[i++] = "-O";
		argv[i++] = tmp_file_location;
		argv[i++] = src;
		argv[i++] = NULL;
		res = xsystem(argv);

		if (res) {
			opkg_msg(ERROR,
				 "Failed to download %s, wget returned %d.\n",
				 src, res);
			if (res == 4)
				opkg_msg(ERROR,
					 "Check your network settings and connectivity.\n\n");
			free(tmp_file_location);
			return -1;
		}
	}

	err = file_move(tmp_file_location, dest_file_name);

	free(tmp_file_location);

	return err;
}

static int
opkg_download_cache(const char *src, const char *dest_file_name)
{
	char *cache_name = xstrdup(src);
	char *cache_location, *p;
	int err = 0;

	if (!conf->cache || str_starts_with(src, "file:")) {
		err = opkg_download(src, dest_file_name, 0);
		goto out1;
	}

	if (!file_is_dir(conf->cache)) {
		opkg_msg(ERROR, "%s is not a directory.\n", conf->cache);
		err = 1;
		goto out1;
	}

	for (p = cache_name; *p; p++)
		if (*p == '/')
			*p = ',';	/* looks nicer than | or # */

	sprintf_alloc(&cache_location, "%s/%s", conf->cache, cache_name);
	if (file_exists(cache_location))
		opkg_msg(NOTICE, "Copying %s.\n", cache_location);
	else {
		/* cache file with funky name not found, try simple name */
		free(cache_name);
		char *filename = strrchr(dest_file_name, '/');
		if (filename)
			cache_name = xstrdup(filename + 1);	// strip leading '/'
		else
			cache_name = xstrdup(dest_file_name);
		free(cache_location);
		sprintf_alloc(&cache_location, "%s/%s", conf->cache,
			      cache_name);
		if (file_exists(cache_location))
			opkg_msg(NOTICE, "Copying %s.\n", cache_location);
		else {
			err = opkg_download(src, cache_location, 0);
			if (err) {
				(void)unlink(cache_location);
				goto out2;
			}
		}
	}

	err = file_copy(cache_location, dest_file_name);

out2:
	free(cache_location);
out1:
	free(cache_name);
	return err;
}

int opkg_download_pkg(pkg_t * pkg, const char *dir)
{
	int err;
	char *url;
	char *local_filename;
	char *stripped_filename;
	char *urlencoded_path;
	char *filename;

	if (pkg->src == NULL) {
		opkg_msg(ERROR,
			 "Package %s is not available from any configured src.\n",
			 pkg->name);
		return -1;
	}

	filename = pkg_get_string(pkg, PKG_FILENAME);

	if (filename == NULL) {
		opkg_msg(ERROR,
			 "Package %s does not have a valid filename field.\n",
			 pkg->name);
		return -1;
	}

	urlencoded_path = urlencode_path(filename);
	sprintf_alloc(&url, "%s/%s", pkg->src->value, urlencoded_path);
	free(urlencoded_path);

	/* The filename might be something like
	   "../../foo.opk". While this is correct, and exactly what we
	   want to use to construct url above, here we actually need to
	   use just the filename part, without any directory. */

	stripped_filename = strrchr(filename, '/');
	if (!stripped_filename)
		stripped_filename = filename;

	sprintf_alloc(&local_filename, "%s/%s", dir, stripped_filename);
	pkg_set_string(pkg, PKG_LOCAL_FILENAME, local_filename);

	err = opkg_download_cache(url, local_filename);
	free(url);

	return err;
}

/*
 * Downloads file from url, installs in package database, return package name.
 */
int opkg_prepare_url_for_install(const char *url, char **namep)
{
	int err = 0;
	pkg_t *pkg;
	abstract_pkg_t *ab_pkg;

	pkg = pkg_new();

	if (str_starts_with(url, "http://")
	    || str_starts_with(url, "ftp://")) {
		char *tmp_file;
		char *file_basec = xstrdup(url);
		char *file_base = basename(file_basec);

		sprintf_alloc(&tmp_file, "%s/%s", conf->tmp_dir, file_base);
		err = opkg_download(url, tmp_file, 0);
		if (err)
			return err;

		err = pkg_init_from_file(pkg, tmp_file);
		if (err)
			return err;

		free(tmp_file);
		free(file_basec);

	} else if (strcmp(&url[strlen(url) - 4], OPKG_PKG_EXTENSION) == 0
		   || strcmp(&url[strlen(url) - 4], IPKG_PKG_EXTENSION) == 0
		   || strcmp(&url[strlen(url) - 4], DPKG_PKG_EXTENSION) == 0) {

		err = pkg_init_from_file(pkg, url);
		if (err)
			return err;
		opkg_msg(DEBUG2, "Package %s provided by hand (%s).\n",
			 pkg->name, pkg_get_string(pkg, PKG_LOCAL_FILENAME));
		pkg->provided_by_hand = 1;

	} else {
		ab_pkg = ensure_abstract_pkg_by_name(url);

		if (!(ab_pkg->state_flag & SF_NEED_DETAIL)) {
			opkg_msg(DEBUG, "applying abpkg flag to %s\n", ab_pkg->name);
			ab_pkg->state_flag |= SF_NEED_DETAIL;
		}

		pkg_deinit(pkg);
		free(pkg);
		return 0;
	}

	pkg->dest = conf->default_dest;
	pkg->state_want = SW_INSTALL;
	pkg->state_flag |= SF_PREFER;
	hash_insert_pkg(pkg, 1);

	if (namep) {
		*namep = xstrdup(pkg->name);
	}
	return 0;
}

int opkg_verify_file(char *text_file, char *sig_file)
{
#if defined HAVE_USIGN
	const char *argv[] = { conf->verify_program, "verify", sig_file,
	                       text_file, NULL };

	return xsystem(argv) ? -1 : 0;
#else
	/* mute `unused variable' warnings. */
	(void)sig_file;
	(void)text_file;
	(void)conf;
	return 0;
#endif
}
