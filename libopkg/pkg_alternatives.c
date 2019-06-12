/* pkg_alternatives.c - the opkg package management system

   Copyright (C) 2017 Yousong Zhou

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
#include <sys/types.h>		/* stat */
#include <sys/stat.h>
#include <libgen.h>			/* dirname */
#include <unistd.h>

#include "file_util.h"
#include "libbb/libbb.h"
#include "opkg_message.h"
#include "pkg.h"
#include "pkg_hash.h"
#include "pkg_alternatives.h"
#include "sprintf_alloc.h"

struct alternative_provider {
	char *name;
	char *altpath;
};

static const struct alternative_provider const providers[] = {
	{
		.name = "busybox",
		.altpath = "/bin/busybox",
	},
};

static const char *pkg_alternatives_check_providers(const char *path)
{
	pkg_t *pkg;
	str_list_t *files;
	str_list_elt_t *iter;
	int i;

	for (i = 0; i < ARRAY_SIZE(providers); i++) {
		pkg = pkg_hash_fetch_installed_by_name(providers[i].name);
		if (!pkg) {
			continue;
		}
		files = pkg_get_installed_files(pkg);
		for (iter = str_list_first(files); iter; iter = str_list_next(files, iter)) {
			if (!strcmp(path, (char *)(iter->data))) {
				pkg_free_installed_files(pkg);
				return providers[i].altpath;
			}
		}
		pkg_free_installed_files(pkg);
	}
	return NULL;
}

static int pkg_alternatives_update_path(pkg_t *pkg, const pkg_vec_t *installed, const char *path)
{
	struct pkg_alternatives *pkg_alts;
	struct pkg_alternative *the_alt = NULL;
	pkg_t *the_pkg = pkg;
	int i, j;
	int r;
	char *path_in_dest;
	const char *target_path = NULL;

	for (i = 0; i < installed->len; i++) {
		pkg_t *pkg = installed->pkgs[i];
		pkg_alts = pkg_get_ptr(pkg, PKG_ALTERNATIVES);
		if (!pkg_alts)
			continue;

		for (j = 0; j < pkg_alts->nalts; j++) {
			struct pkg_alternative *alt = pkg_alts->alts[j];

			if (strcmp(path, alt->path))
				continue;
			if (!the_alt || the_alt->prio < alt->prio) {
				the_pkg = pkg;
				the_alt = alt;
			}
		}
	}

	/* path is assumed to be an absolute one */
	sprintf_alloc(&path_in_dest, "%s%s", the_pkg->dest->root_dir, &path[1]);
	if (!path_in_dest)
		return -1;

	if (the_alt) {
		target_path = the_alt->altpath;
	} else {
		target_path = pkg_alternatives_check_providers(path);
	}

	if (target_path) {
		struct stat sb;

		r = lstat(path_in_dest, &sb);
		if (!r) {
			char *realpath;

			if (!S_ISLNK(sb.st_mode)) {
				opkg_msg(ERROR, "%s exists but is not a symlink\n", path_in_dest);
				r = -1;
				goto out;
			}
			realpath = xreadlink(path_in_dest);
			if (realpath && strcmp(realpath, target_path))
				unlink(path_in_dest);
			free(realpath);
		} else if (errno != ENOENT) {
			goto out;
		}
		{
			char *path_copy = xstrdup(path_in_dest);
			char *path_parent = dirname(path_copy);

			r = file_mkdir_hier(path_parent, 0755);
			free(path_copy);
			if (r) {
				goto out;
			}
			r = symlink(target_path, path_in_dest);
			if (r && errno == EEXIST) {
				/*
				 * the strcmp & unlink check above will make sure that if EEXIST
				 * happens, the symlink target also matches
				 */
				r = 0;
			}
			if (r) {
				opkg_perror(ERROR, "failed symlinking %s -> %s", path_in_dest, target_path);
			}
		}
	} else {
		unlink(path_in_dest);
		r = 0;
	}

out:
	free(path_in_dest);
	return r;
}

int pkg_alternatives_update(pkg_t * pkg)
{
	int r = 0;
	int i;
	struct pkg_alternatives *pkg_alts;
	struct pkg_alternative *alt = NULL;
	pkg_vec_t *installed;

	pkg_alts = pkg_get_ptr(pkg, PKG_ALTERNATIVES);
	if (!pkg_alts)
		return 0;

	installed = pkg_vec_alloc();
	pkg_hash_fetch_all_installed(installed);
	for (i = 0; i < pkg_alts->nalts; i++) {
		alt = pkg_alts->alts[i];
		r |= pkg_alternatives_update_path(pkg, installed, alt->path);
	}
	pkg_vec_free(installed);

	return r;
}
