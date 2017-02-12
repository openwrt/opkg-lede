/* pkg.c - the opkg package management system

   Carl D. Worth

   Copyright (C) 2001 University of Southern California

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2, or (at
   your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.
*/

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>

#include "pkg.h"

#include "pkg_parse.h"
#include "pkg_extract.h"
#include "opkg_message.h"
#include "opkg_utils.h"

#include "libbb/libbb.h"
#include "sprintf_alloc.h"
#include "file_util.h"
#include "xsystem.h"
#include "opkg_conf.h"

typedef struct enum_map enum_map_t;
struct enum_map {
	unsigned int value;
	const char *str;
};

static const enum_map_t pkg_state_want_map[] = {
	{SW_UNKNOWN, "unknown"},
	{SW_INSTALL, "install"},
	{SW_DEINSTALL, "deinstall"},
	{SW_PURGE, "purge"}
};

static const enum_map_t pkg_state_flag_map[] = {
	{SF_OK, "ok"},
	{SF_REINSTREQ, "reinstreq"},
	{SF_HOLD, "hold"},
	{SF_REPLACE, "replace"},
	{SF_NOPRUNE, "noprune"},
	{SF_PREFER, "prefer"},
	{SF_OBSOLETE, "obsolete"},
	{SF_USER, "user"},
};

static const enum_map_t pkg_state_status_map[] = {
	{SS_NOT_INSTALLED, "not-installed"},
	{SS_UNPACKED, "unpacked"},
	{SS_HALF_CONFIGURED, "half-configured"},
	{SS_INSTALLED, "installed"},
	{SS_HALF_INSTALLED, "half-installed"},
	{SS_CONFIG_FILES, "config-files"},
	{SS_POST_INST_FAILED, "post-inst-failed"},
	{SS_REMOVAL_FAILED, "removal-failed"}
};

static void pkg_init(pkg_t * pkg)
{
	pkg->name = NULL;
	pkg->dest = NULL;
	pkg->src = NULL;
	pkg->state_want = SW_UNKNOWN;
	pkg->state_flag = SF_OK;
	pkg->state_status = SS_NOT_INSTALLED;

	active_list_init(&pkg->list);

	pkg->installed_files = NULL;
	pkg->installed_files_ref_cnt = 0;
	pkg->essential = 0;
	pkg->provided_by_hand = 0;

	blob_buf_init(&pkg->blob, 0);
}

pkg_t *pkg_new(void)
{
	pkg_t *pkg;

	pkg = xcalloc(1, sizeof(pkg_t));
	pkg_init(pkg);

	return pkg;
}

void *pkg_set_raw(pkg_t *pkg, int id, const void *val, size_t len)
{
	int rem;
	struct blob_attr *cur;

	blob_for_each_attr(cur, pkg->blob.head, rem) {
		if (blob_id(cur) == id) {
			if (blob_len(cur) < len) {
				fprintf(stderr, "ERROR: truncating field %d <%p> to %d byte",
				        id, val, blob_len(cur));
			}
			memcpy(blob_data(cur), val, blob_len(cur));
			return blob_data(cur);
		}
	}

	cur = blob_put(&pkg->blob, id, val, len);
	return cur ? blob_data(cur) : NULL;
}

void *pkg_get_raw(const pkg_t * pkg, int id)
{
	int rem;
	struct blob_attr *cur;

	blob_for_each_attr(cur, pkg->blob.head, rem)
		if (blob_id(cur) == id)
			return blob_data(cur);

	return NULL;
}

char *pkg_set_string(pkg_t *pkg, int id, const char *s)
{
	size_t len;
	char *p;

	if (!s)
		return NULL;

	len = strlen(s);

	while (isspace(*s)) {
		s++;
		len--;
	}

	while (len > 0 && isspace(s[len - 1]))
		len--;

	if (!len)
		return NULL;

	p = pkg_set_raw(pkg, id, s, len + 1);
	p[len] = 0;

	return p;
}


static void compound_depend_deinit(compound_depend_t * depends)
{
	int i;
	for (i = 0; i < depends->possibility_count; i++) {
		depend_t *d;
		d = depends->possibilities[i];
		free(d->version);
		free(d);
	}
	free(depends->possibilities);
}

void pkg_deinit(pkg_t * pkg)
{
	compound_depend_t *deps, *dep;

	if (pkg->name)
		free(pkg->name);
	pkg->name = NULL;

	/* owned by opkg_conf_t */
	pkg->dest = NULL;
	/* owned by opkg_conf_t */
	pkg->src = NULL;

	pkg->state_want = SW_UNKNOWN;
	pkg->state_flag = SF_OK;
	pkg->state_status = SS_NOT_INSTALLED;

	active_list_clear(&pkg->list);

	deps = pkg_get_ptr(pkg, PKG_DEPENDS);

	if (deps) {
		for (dep = deps; dep->type; dep++)
			compound_depend_deinit(dep);

		free(deps);
		pkg_set_ptr(pkg, PKG_DEPENDS, NULL);
	}

	deps = pkg_get_ptr(pkg, PKG_CONFLICTS);

	if (deps) {
		for (dep = deps; dep->type; dep++)
			compound_depend_deinit(dep);

		free(deps);
		pkg_set_ptr(pkg, PKG_CONFLICTS, NULL);
	}

	//conffile_list_deinit(&pkg->conffiles);

	/* XXX: QUESTION: Is forcing this to 1 correct? I suppose so,
	   since if they are calling deinit, they should know. Maybe do an
	   assertion here instead? */
	pkg->installed_files_ref_cnt = 1;
	pkg_free_installed_files(pkg);
	pkg->essential = 0;

	//blob_buf_free(&pkg->blob);
}

int pkg_init_from_file(pkg_t * pkg, const char *filename)
{
	int fd, err = 0;
	FILE *control_file;
	char *control_path, *tmp;

	pkg_init(pkg);

	pkg_set_string(pkg, PKG_LOCAL_FILENAME, filename);

	tmp = xstrdup(filename);
	sprintf_alloc(&control_path, "%s/%s.control.XXXXXX",
		      conf->tmp_dir, basename(tmp));
	free(tmp);
	fd = mkstemp(control_path);
	if (fd == -1) {
		opkg_perror(ERROR, "Failed to make temp file %s", control_path);
		err = -1;
		goto err0;
	}

	control_file = fdopen(fd, "r+");
	if (control_file == NULL) {
		opkg_perror(ERROR, "Failed to fdopen %s", control_path);
		close(fd);
		err = -1;
		goto err1;
	}

	err = pkg_extract_control_file_to_stream(pkg, control_file);
	if (err) {
		opkg_msg(ERROR, "Failed to extract control file from %s.\n",
			 filename);
		goto err2;
	}

	rewind(control_file);

	if ((err = pkg_parse_from_stream(pkg, control_file, 0))) {
		if (err == 1) {
			opkg_msg(ERROR, "Malformed package file %s.\n",
				 filename);
		}
		err = -1;
	}

err2:
	fclose(control_file);
err1:
	unlink(control_path);
err0:
	free(control_path);

	return err;
}

/* Merge any new information in newpkg into oldpkg */
int pkg_merge(pkg_t * oldpkg, pkg_t * newpkg)
{
	abstract_pkg_t **ab;
	conffile_list_t *cf, head;

	if (oldpkg == newpkg) {
		return 0;
	}

	if (!oldpkg->auto_installed)
		oldpkg->auto_installed = newpkg->auto_installed;

	if (!oldpkg->src)
		oldpkg->src = newpkg->src;
	if (!oldpkg->dest)
		oldpkg->dest = newpkg->dest;
	if (!pkg_get_string(oldpkg, PKG_ARCHITECTURE))
		pkg_set_string(oldpkg, PKG_ARCHITECTURE, pkg_get_string(newpkg, PKG_ARCHITECTURE));
	if (!pkg_get_int(oldpkg, PKG_ARCH_PRIORITY))
		pkg_set_int(oldpkg, PKG_ARCH_PRIORITY, pkg_get_int(newpkg, PKG_ARCH_PRIORITY));
	if (!pkg_get_string(oldpkg, PKG_SECTION))
		pkg_set_string(oldpkg, PKG_SECTION, pkg_get_string(newpkg, PKG_SECTION));
	if (!pkg_get_string(oldpkg, PKG_MAINTAINER))
		pkg_set_string(oldpkg, PKG_MAINTAINER, pkg_get_string(newpkg, PKG_MAINTAINER));
	if (!pkg_get_string(oldpkg, PKG_DESCRIPTION))
		pkg_set_string(oldpkg, PKG_DESCRIPTION, pkg_get_string(newpkg, PKG_DESCRIPTION));

	if (!pkg_get_ptr(oldpkg, PKG_DEPENDS)) {
		pkg_set_ptr(oldpkg, PKG_DEPENDS, pkg_get_ptr(newpkg, PKG_DEPENDS));
		pkg_set_ptr(newpkg, PKG_DEPENDS, NULL);
	}

	ab = pkg_get_ptr(oldpkg, PKG_PROVIDES);

	if (!ab || !ab[0] || !ab[1]) {
		pkg_set_ptr(oldpkg, PKG_PROVIDES, pkg_get_ptr(newpkg, PKG_PROVIDES));
		pkg_set_ptr(newpkg, PKG_PROVIDES, NULL);
	}

	if (!pkg_get_ptr(oldpkg, PKG_CONFLICTS)) {
		pkg_set_ptr(oldpkg, PKG_CONFLICTS, pkg_get_ptr(newpkg, PKG_CONFLICTS));
		pkg_set_ptr(newpkg, PKG_CONFLICTS, NULL);
	}

	if (!pkg_get_ptr(oldpkg, PKG_REPLACES)) {
		pkg_set_ptr(oldpkg, PKG_REPLACES, pkg_get_ptr(newpkg, PKG_REPLACES));
		pkg_set_ptr(newpkg, PKG_REPLACES, NULL);
	}

	if (!pkg_get_string(oldpkg, PKG_FILENAME))
		pkg_set_string(oldpkg, PKG_FILENAME, pkg_get_string(newpkg, PKG_FILENAME));
	if (!pkg_get_string(oldpkg, PKG_LOCAL_FILENAME))
		pkg_set_string(oldpkg, PKG_LOCAL_FILENAME, pkg_get_string(newpkg, PKG_LOCAL_FILENAME));
	if (!pkg_get_string(oldpkg, PKG_TMP_UNPACK_DIR))
		pkg_set_string(oldpkg, PKG_TMP_UNPACK_DIR, pkg_get_string(newpkg, PKG_TMP_UNPACK_DIR));
	if (!pkg_get_string(oldpkg, PKG_MD5SUM))
		pkg_set_string(oldpkg, PKG_MD5SUM, pkg_get_string(newpkg, PKG_MD5SUM));
	if (!pkg_get_string(oldpkg, PKG_SHA256SUM))
		pkg_set_string(oldpkg, PKG_SHA256SUM, pkg_get_string(newpkg, PKG_SHA256SUM));
	if (!pkg_get_int(oldpkg, PKG_SIZE))
		pkg_set_int(oldpkg, PKG_SIZE, pkg_get_int(newpkg, PKG_SIZE));
	if (!pkg_get_int(oldpkg, PKG_INSTALLED_SIZE))
		pkg_set_int(oldpkg, PKG_INSTALLED_SIZE, pkg_get_int(newpkg, PKG_INSTALLED_SIZE));
	if (!pkg_get_string(oldpkg, PKG_PRIORITY))
		pkg_set_string(oldpkg, PKG_PRIORITY, pkg_get_string(newpkg, PKG_PRIORITY));
	if (!pkg_get_string(oldpkg, PKG_SOURCE))
		pkg_set_string(oldpkg, PKG_SOURCE, pkg_get_string(newpkg, PKG_SOURCE));

	if (!pkg_get_ptr(oldpkg, PKG_CONFFILES)) {
		cf = pkg_get_ptr(newpkg, PKG_CONFFILES);
		if (cf) {
			conffile_list_init(&head);
			list_splice_init(&cf->head, &head.head);
			pkg_set_raw(oldpkg, PKG_CONFFILES, &head, sizeof(head));
		}
	}

	if (!oldpkg->installed_files) {
		oldpkg->installed_files = newpkg->installed_files;
		oldpkg->installed_files_ref_cnt =
		    newpkg->installed_files_ref_cnt;
		newpkg->installed_files = NULL;
	}

	if (!oldpkg->essential)
		oldpkg->essential = newpkg->essential;

	return 0;
}

static void abstract_pkg_init(abstract_pkg_t * ab_pkg)
{
	ab_pkg->provided_by = abstract_pkg_vec_alloc();
	ab_pkg->dependencies_checked = 0;
	ab_pkg->state_status = SS_NOT_INSTALLED;
}

abstract_pkg_t *abstract_pkg_new(void)
{
	abstract_pkg_t *ab_pkg;

	ab_pkg = xcalloc(1, sizeof(abstract_pkg_t));
	abstract_pkg_init(ab_pkg);

	return ab_pkg;
}

void set_flags_from_control(pkg_t * pkg)
{
	char *file_name;
	FILE *fp;

	sprintf_alloc(&file_name, "%s/%s.control", pkg->dest->info_dir,
		      pkg->name);

	fp = fopen(file_name, "r");
	if (fp == NULL) {
		opkg_perror(ERROR, "Failed to open %s", file_name);
		free(file_name);
		return;
	}

	free(file_name);

	if (pkg_parse_from_stream(pkg, fp, PFM_ALL ^ PFM_ESSENTIAL)) {
		opkg_msg(DEBUG,
			 "Unable to read control file for %s. May be empty.\n",
			 pkg->name);
	}

	fclose(fp);

	return;
}

static const char *pkg_state_want_to_str(pkg_state_want_t sw)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pkg_state_want_map); i++) {
		if (pkg_state_want_map[i].value == sw) {
			return pkg_state_want_map[i].str;
		}
	}

	opkg_msg(ERROR, "Internal error: state_want=%d\n", sw);
	return "<STATE_WANT_UNKNOWN>";
}

pkg_state_want_t pkg_state_want_from_str(char *str)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pkg_state_want_map); i++) {
		if (strcmp(str, pkg_state_want_map[i].str) == 0) {
			return pkg_state_want_map[i].value;
		}
	}

	opkg_msg(ERROR, "Internal error: state_want=%s\n", str);
	return SW_UNKNOWN;
}

static char *pkg_state_flag_to_str(pkg_state_flag_t sf)
{
	int i;
	unsigned int len;
	char *str;

	/* clear the temporary flags before converting to string */
	sf &= SF_NONVOLATILE_FLAGS;

	if (sf == 0)
		return xstrdup("ok");

	len = 0;
	for (i = 0; i < ARRAY_SIZE(pkg_state_flag_map); i++) {
		if (sf & pkg_state_flag_map[i].value)
			len += strlen(pkg_state_flag_map[i].str) + 1;
	}

	str = xmalloc(len + 1);
	str[0] = '\0';

	for (i = 0; i < ARRAY_SIZE(pkg_state_flag_map); i++) {
		if (sf & pkg_state_flag_map[i].value) {
			strncat(str, pkg_state_flag_map[i].str, len);
			strncat(str, ",", len);
		}
	}

	len = strlen(str);
	str[len - 1] = '\0';	/* squash last comma */

	return str;
}

pkg_state_flag_t pkg_state_flag_from_str(const char *str)
{
	int i;
	int sf = SF_OK;
	const char *sfname;
	unsigned int sfname_len;

	if (strcmp(str, "ok") == 0) {
		return SF_OK;
	}
	for (i = 0; i < ARRAY_SIZE(pkg_state_flag_map); i++) {
		sfname = pkg_state_flag_map[i].str;
		sfname_len = strlen(sfname);
		if (strncmp(str, sfname, sfname_len) == 0) {
			sf |= pkg_state_flag_map[i].value;
			str += sfname_len;
			if (str[0] == ',') {
				str++;
			} else {
				break;
			}
		}
	}

	return sf;
}

static const char *pkg_state_status_to_str(pkg_state_status_t ss)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pkg_state_status_map); i++) {
		if (pkg_state_status_map[i].value == ss) {
			return pkg_state_status_map[i].str;
		}
	}

	opkg_msg(ERROR, "Internal error: state_status=%d\n", ss);
	return "<STATE_STATUS_UNKNOWN>";
}

pkg_state_status_t pkg_state_status_from_str(const char *str)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pkg_state_status_map); i++) {
		if (strcmp(str, pkg_state_status_map[i].str) == 0) {
			return pkg_state_status_map[i].value;
		}
	}

	opkg_msg(ERROR, "Internal error: state_status=%s\n", str);
	return SS_NOT_INSTALLED;
}

void pkg_formatted_field(FILE * fp, pkg_t * pkg, const char *field)
{
	int i, j;
	char *str;
	const char *p;
	compound_depend_t *dep;
	abstract_pkg_t **ab_pkg;

	if (strlen(field) < PKG_MINIMUM_FIELD_NAME_LEN) {
		goto UNKNOWN_FMT_FIELD;
	}

	switch (field[0]) {
	case 'a':
	case 'A':
		if (strcasecmp(field, "Architecture") == 0) {
			p = pkg_get_string(pkg, PKG_ARCHITECTURE);
			if (p) {
				fprintf(fp, "Architecture: %s\n",
					p);
			}
		} else if (strcasecmp(field, "Auto-Installed") == 0) {
			if (pkg->auto_installed)
				fprintf(fp, "Auto-Installed: yes\n");
		} else {
			goto UNKNOWN_FMT_FIELD;
		}
		break;
	case 'c':
	case 'C':
		if (strcasecmp(field, "Conffiles") == 0) {
			conffile_list_t *cl;
			conffile_list_elt_t *iter;

			cl = pkg_get_ptr(pkg, PKG_CONFFILES);

			if (!cl || nv_pair_list_empty(cl))
				return;

			fprintf(fp, "Conffiles:\n");
			for (iter = nv_pair_list_first(cl); iter;
			     iter = nv_pair_list_next(cl, iter)) {
				if (((conffile_t *) iter->data)->name
				    && ((conffile_t *) iter->data)->value) {
					fprintf(fp, " %s %s\n",
						((conffile_t *) iter->data)->
						name,
						((conffile_t *) iter->data)->
						value);
				}
			}
		} else if (strcasecmp(field, "Conflicts") == 0) {
			struct depend *cdep;
			compound_depend_t *deps, *dep;
			deps = pkg_get_ptr(pkg, PKG_CONFLICTS);
			if (deps) {
				fprintf(fp, "Conflicts:");
				for (i = 0, dep = deps; dep->type; dep++, i++) {
					cdep = dep->possibilities[0];
					fprintf(fp, "%s %s", i == 0 ? "" : ",",
						cdep->pkg->name);
					if (cdep->version) {
						fprintf(fp, " (%s%s)",
							constraint_to_str(cdep->
									  constraint),
							cdep->version);
					}
				}
				fprintf(fp, "\n");
			}
		} else {
			goto UNKNOWN_FMT_FIELD;
		}
		break;
	case 'd':
	case 'D':
		if (strcasecmp(field, "Depends") == 0) {
			dep = pkg_get_depends(pkg, DEPEND);
			if (dep) {
				fprintf(fp, "Depends:");
				for (i = 0, j = 0; dep && dep->type; i++, dep++) {
					if (dep->type != DEPEND)
						continue;
					str = pkg_depend_str(pkg, i);
					fprintf(fp, "%s %s", j == 0 ? "" : ",",
						str);
					free(str);
					j++;
				}
				fprintf(fp, "\n");
			}
		} else if (strcasecmp(field, "Description") == 0) {
			p = pkg_get_string(pkg, PKG_DESCRIPTION);
			if (p) {
				fprintf(fp, "Description: %s\n",
					p);
			}
		} else {
			goto UNKNOWN_FMT_FIELD;
		}
		break;
	case 'e':
	case 'E':
		if (pkg->essential) {
			fprintf(fp, "Essential: yes\n");
		}
		break;
	case 'f':
	case 'F':
		p = pkg_get_string(pkg, PKG_FILENAME);
		if (p) {
			fprintf(fp, "Filename: %s\n", p);
		}
		break;
	case 'i':
	case 'I':
		if (strcasecmp(field, "Installed-Size") == 0) {
			fprintf(fp, "Installed-Size: %lu\n",
				(unsigned long) pkg_get_int(pkg, PKG_INSTALLED_SIZE));
		} else if (strcasecmp(field, "Installed-Time") == 0) {
			i = pkg_get_int(pkg, PKG_INSTALLED_TIME);
			if (i) {
				fprintf(fp, "Installed-Time: %lu\n",
					(unsigned long) i);
			}
		}
		break;
	case 'm':
	case 'M':
		if (strcasecmp(field, "Maintainer") == 0) {
			p = pkg_get_string(pkg, PKG_MAINTAINER);
			if (p) {
				fprintf(fp, "Maintainer: %s\n", p);
			}
		} else if (strcasecmp(field, "MD5sum") == 0) {
			p = pkg_get_string(pkg, PKG_MD5SUM);
			if (p) {
				fprintf(fp, "MD5Sum: %s\n", p);
			}
		} else {
			goto UNKNOWN_FMT_FIELD;
		}
		break;
	case 'p':
	case 'P':
		if (strcasecmp(field, "Package") == 0) {
			fprintf(fp, "Package: %s\n", pkg->name);
		} else if (strcasecmp(field, "Priority") == 0) {
			fprintf(fp, "Priority: %s\n", pkg_get_string(pkg, PKG_PRIORITY));
		} else if (strcasecmp(field, "Provides") == 0) {
			ab_pkg = pkg_get_ptr(pkg, PKG_PROVIDES);
			if (ab_pkg && ab_pkg[0] && ab_pkg[1]) {
				fprintf(fp, "Provides:");
				for (i = 0, ab_pkg++; *ab_pkg; i++, ab_pkg++) {
					fprintf(fp, "%s %s", i == 0 ? "" : ",",
						(*ab_pkg)->name);
					ab_pkg++;
				}
				fprintf(fp, "\n");
			}
		} else {
			goto UNKNOWN_FMT_FIELD;
		}
		break;
	case 'r':
	case 'R':
		if (strcasecmp(field, "Replaces") == 0) {
			ab_pkg = pkg_get_ptr(pkg, PKG_REPLACES);
			if (ab_pkg && *ab_pkg) {
				fprintf(fp, "Replaces:");
				for (i = 0; *ab_pkg; i++, ab_pkg++) {
					fprintf(fp, "%s %s", i == 0 ? "" : ",",
						(*ab_pkg)->name);
				}
				fprintf(fp, "\n");
			}
		} else if (strcasecmp(field, "Recommends") == 0) {
			dep = pkg_get_depends(pkg, RECOMMEND);
			if (dep) {
				fprintf(fp, "Recommends:");
				for (j = 0, i = 0; dep && dep->type; i++, dep++) {
					if (dep->type != RECOMMEND)
						continue;
					str = pkg_depend_str(pkg, i);
					fprintf(fp, "%s %s", j == 0 ? "" : ",",
						str);
					free(str);
					j++;
				}
				fprintf(fp, "\n");
			}
		} else {
			goto UNKNOWN_FMT_FIELD;
		}
		break;
	case 's':
	case 'S':
		if (strcasecmp(field, "Section") == 0) {
			p = pkg_get_string(pkg, PKG_SECTION);
			if (p) {
				fprintf(fp, "Section: %s\n", p);
			}
#if defined HAVE_SHA256
		} else if (strcasecmp(field, "SHA256sum") == 0) {
			p = pkg_get_string(pkg, PKG_SHA256SUM);
			if (p) {
				fprintf(fp, "SHA256sum: %s\n", p);
			}
#endif
		} else if (strcasecmp(field, "Size") == 0) {
			i = pkg_get_int(pkg, PKG_SIZE);
			if (i) {
				fprintf(fp, "Size: %lu\n", (unsigned long) i);
			}
		} else if (strcasecmp(field, "Source") == 0) {
			p = pkg_get_string(pkg, PKG_SOURCE);
			if (p) {
				fprintf(fp, "Source: %s\n", p);
			}
		} else if (strcasecmp(field, "Status") == 0) {
			char *pflag = pkg_state_flag_to_str(pkg->state_flag);
			fprintf(fp, "Status: %s %s %s\n",
				pkg_state_want_to_str(pkg->state_want),
				pflag,
				pkg_state_status_to_str(pkg->state_status));
			free(pflag);
		} else if (strcasecmp(field, "Suggests") == 0) {
			dep = pkg_get_depends(pkg, SUGGEST);
			if (dep) {
				fprintf(fp, "Suggests:");
				for (j = 0, i = 0; dep && dep->type; i++, dep++) {
					if (dep->type != SUGGEST)
						continue;
					str = pkg_depend_str(pkg, i);
					fprintf(fp, "%s %s", j == 0 ? "" : ",",
						str);
					free(str);
					j++;
				}
				fprintf(fp, "\n");
			}
		} else {
			goto UNKNOWN_FMT_FIELD;
		}
		break;
	case 't':
	case 'T':
		if (strcasecmp(field, "Tags") == 0) {
			p = pkg_get_string(pkg, PKG_TAGS);
			if (p) {
				fprintf(fp, "Tags: %s\n", p);
			}
		}
		break;
	case 'v':
	case 'V':
		{
			char *version = pkg_version_str_alloc(pkg);
			if (version == NULL)
				return;
			fprintf(fp, "Version: %s\n", version);
			free(version);
		}
		break;
	default:
		goto UNKNOWN_FMT_FIELD;
	}

	return;

UNKNOWN_FMT_FIELD:
	opkg_msg(ERROR, "Internal error: field=%s\n", field);
}

void pkg_formatted_info(FILE * fp, pkg_t * pkg)
{
	pkg_formatted_field(fp, pkg, "Package");
	pkg_formatted_field(fp, pkg, "Version");
	pkg_formatted_field(fp, pkg, "Depends");
	pkg_formatted_field(fp, pkg, "Recommends");
	pkg_formatted_field(fp, pkg, "Suggests");
	pkg_formatted_field(fp, pkg, "Provides");
	pkg_formatted_field(fp, pkg, "Replaces");
	pkg_formatted_field(fp, pkg, "Conflicts");
	pkg_formatted_field(fp, pkg, "Status");
	pkg_formatted_field(fp, pkg, "Section");
	pkg_formatted_field(fp, pkg, "Essential");
	pkg_formatted_field(fp, pkg, "Architecture");
	pkg_formatted_field(fp, pkg, "Maintainer");
	pkg_formatted_field(fp, pkg, "MD5sum");
	pkg_formatted_field(fp, pkg, "Size");
	pkg_formatted_field(fp, pkg, "Filename");
	pkg_formatted_field(fp, pkg, "Conffiles");
	pkg_formatted_field(fp, pkg, "Source");
	pkg_formatted_field(fp, pkg, "Description");
	pkg_formatted_field(fp, pkg, "Installed-Time");
	pkg_formatted_field(fp, pkg, "Tags");
	fputs("\n", fp);
}

void pkg_print_status(pkg_t * pkg, FILE * file)
{
	if (pkg == NULL) {
		return;
	}

	pkg_formatted_field(file, pkg, "Package");
	pkg_formatted_field(file, pkg, "Version");
	pkg_formatted_field(file, pkg, "Depends");
	pkg_formatted_field(file, pkg, "Recommends");
	pkg_formatted_field(file, pkg, "Suggests");
	pkg_formatted_field(file, pkg, "Provides");
	pkg_formatted_field(file, pkg, "Replaces");
	pkg_formatted_field(file, pkg, "Conflicts");
	pkg_formatted_field(file, pkg, "Status");
	pkg_formatted_field(file, pkg, "Essential");
	pkg_formatted_field(file, pkg, "Architecture");
	pkg_formatted_field(file, pkg, "Conffiles");
	pkg_formatted_field(file, pkg, "Installed-Time");
	pkg_formatted_field(file, pkg, "Auto-Installed");
	fputs("\n", file);
}

/*
 * libdpkg - Debian packaging suite library routines
 * vercmp.c - comparison of version numbers
 *
 * Copyright (C) 1995 Ian Jackson <iwj10@cus.cam.ac.uk>
 */

/* assume ascii; warning: evaluates x multiple times! */
#define order(x) ((x) == '~' ? -1 \
		: isdigit((x)) ? 0 \
		: !(x) ? 0 \
		: isalpha((x)) ? (x) \
		: (x) + 256)

static int verrevcmp(const char *val, const char *ref)
{
	if (!val)
		val = "";
	if (!ref)
		ref = "";

	while (*val || *ref) {
		int first_diff = 0;

		while ((*val && !isdigit(*val)) || (*ref && !isdigit(*ref))) {
			int vc = order(*val), rc = order(*ref);
			if (vc != rc)
				return vc - rc;
			val++;
			ref++;
		}

		while (*val == '0')
			val++;
		while (*ref == '0')
			ref++;
		while (isdigit(*val) && isdigit(*ref)) {
			if (!first_diff)
				first_diff = *val - *ref;
			val++;
			ref++;
		}
		if (isdigit(*val))
			return 1;
		if (isdigit(*ref))
			return -1;
		if (first_diff)
			return first_diff;
	}
	return 0;
}

int pkg_compare_versions(const pkg_t * pkg, const pkg_t * ref_pkg)
{
	unsigned int epoch1 = (unsigned int) pkg_get_int(pkg, PKG_EPOCH);
	unsigned int epoch2 = (unsigned int) pkg_get_int(ref_pkg, PKG_EPOCH);
	char *revision1 = pkg_get_string(pkg, PKG_REVISION);
	char *revision2 = pkg_get_string(ref_pkg, PKG_REVISION);
	const char *version1 = pkg_get_string(pkg, PKG_VERSION);
	const char *version2 = pkg_get_string(ref_pkg, PKG_VERSION);
	int r;

	if (epoch1 > epoch2) {
		return 1;
	}

	if (epoch1 < epoch2) {
		return -1;
	}

	r = verrevcmp(version1, version2);
	if (r) {
		return r;
	}

	r = verrevcmp(revision1, revision2);
	if (r) {
		return r;
	}

	return r;
}

int pkg_version_satisfied(pkg_t * it, pkg_t * ref, const char *op)
{
	int r;

	r = pkg_compare_versions(it, ref);

	if (strcmp(op, "<=") == 0 || strcmp(op, "<") == 0) {
		return r <= 0;
	}

	if (strcmp(op, ">=") == 0 || strcmp(op, ">") == 0) {
		return r >= 0;
	}

	if (strcmp(op, "<<") == 0) {
		return r < 0;
	}

	if (strcmp(op, ">>") == 0) {
		return r > 0;
	}

	if (strcmp(op, "=") == 0) {
		return r == 0;
	}

	opkg_msg(ERROR, "Unknown operator: %s.\n", op);
	return 0;
}

int pkg_name_version_and_architecture_compare(const void *p1, const void *p2)
{
	const pkg_t * a = *(const pkg_t **)p1;
	const pkg_t * b = *(const pkg_t **)p2;
	int namecmp;
	int vercmp;
	int arch_prio1, arch_prio2;
	if (!a->name || !b->name) {
		opkg_msg(ERROR, "Internal error: a->name=%p, b->name=%p.\n",
			 a->name, b->name);
		return 0;
	}

	namecmp = strcmp(a->name, b->name);
	if (namecmp)
		return namecmp;
	vercmp = pkg_compare_versions(a, b);
	if (vercmp)
		return vercmp;
	arch_prio1 = pkg_get_int(a, PKG_ARCH_PRIORITY);
	arch_prio2 = pkg_get_int(b, PKG_ARCH_PRIORITY);
	if (!arch_prio1 || !arch_prio2) {
		opkg_msg(ERROR,
			 "Internal error: a->arch_priority=%i b->arch_priority=%i.\n",
			 arch_prio1, arch_prio2);
		return 0;
	}
	if (arch_prio1 > arch_prio2)
		return 1;
	if (arch_prio1 < arch_prio2)
		return -1;
	return 0;
}

int abstract_pkg_name_compare(const void *p1, const void *p2)
{
	const abstract_pkg_t *a = *(const abstract_pkg_t **)p1;
	const abstract_pkg_t *b = *(const abstract_pkg_t **)p2;
	if (!a->name || !b->name) {
		opkg_msg(ERROR, "Internal error: a->name=%p b->name=%p.\n",
			 a->name, b->name);
		return 0;
	}
	return strcmp(a->name, b->name);
}

char *pkg_version_str_alloc(pkg_t * pkg)
{
	const char *verstr;
	char *version, *revptr;
	unsigned int epoch = (unsigned int) pkg_get_int(pkg, PKG_EPOCH);

	revptr = pkg_get_string(pkg, PKG_REVISION);
	verstr = pkg_get_string(pkg, PKG_VERSION);

	if (epoch) {
		if (revptr)
			sprintf_alloc(&version, "%d:%s-%s",
				      epoch, verstr, revptr);
		else
			sprintf_alloc(&version, "%d:%s",
				      epoch, verstr);
	} else {
		if (revptr)
			sprintf_alloc(&version, "%s-%s",
				      verstr, revptr);
		else
			version = xstrdup(verstr);
	}

	return version;
}

/*
 * XXX: this should be broken into two functions
 */
str_list_t *pkg_get_installed_files(pkg_t * pkg)
{
	int err, fd;
	char *list_file_name = NULL;
	FILE *list_file = NULL;
	char *line;
	char *installed_file_name;
	unsigned int rootdirlen = 0;
	int list_from_package;
	const char *local_filename;

	pkg->installed_files_ref_cnt++;

	if (pkg->installed_files) {
		return pkg->installed_files;
	}

	pkg->installed_files = str_list_alloc();

	/*
	 * For installed packages, look at the package.list file in the database.
	 * For uninstalled packages, get the file list directly from the package.
	 */
	if (pkg->state_status == SS_NOT_INSTALLED || pkg->dest == NULL)
		list_from_package = 1;
	else
		list_from_package = 0;

	if (list_from_package) {
		local_filename = pkg_get_string(pkg, PKG_LOCAL_FILENAME);

		if (!local_filename) {
			return pkg->installed_files;
		}
		/* XXX: CLEANUP: Maybe rewrite this to avoid using a temporary
		   file. In other words, change deb_extract so that it can
		   simply return the file list as a char *[] rather than
		   insisting on writing it to a FILE * as it does now. */
		sprintf_alloc(&list_file_name, "%s/%s.list.XXXXXX",
			      conf->tmp_dir, pkg->name);
		fd = mkstemp(list_file_name);
		if (fd == -1) {
			opkg_perror(ERROR, "Failed to make temp file %s.",
				    list_file_name);
			free(list_file_name);
			return pkg->installed_files;
		}
		list_file = fdopen(fd, "r+");
		if (list_file == NULL) {
			opkg_perror(ERROR, "Failed to fdopen temp file %s.",
				    list_file_name);
			close(fd);
			unlink(list_file_name);
			free(list_file_name);
			return pkg->installed_files;
		}
		err = pkg_extract_data_file_names_to_stream(pkg, list_file);
		if (err) {
			opkg_msg(ERROR, "Error extracting file list from %s.\n",
				 local_filename);
			fclose(list_file);
			unlink(list_file_name);
			free(list_file_name);
			str_list_deinit(pkg->installed_files);
			pkg->installed_files = NULL;
			return NULL;
		}
		rewind(list_file);
	} else {
		sprintf_alloc(&list_file_name, "%s/%s.list",
			      pkg->dest->info_dir, pkg->name);
		list_file = fopen(list_file_name, "r");
		if (list_file == NULL) {
			opkg_perror(ERROR, "Failed to open %s", list_file_name);
			free(list_file_name);
			return pkg->installed_files;
		}
		free(list_file_name);
	}

	if (conf->offline_root)
		rootdirlen = strlen(conf->offline_root);

	while (1) {
		char *file_name;

		line = file_read_line_alloc(list_file);
		if (line == NULL) {
			break;
		}
		file_name = line;

		if (list_from_package) {
			if (*file_name == '.') {
				file_name++;
			}
			if (*file_name == '/') {
				file_name++;
			}
			sprintf_alloc(&installed_file_name, "%s%s",
				      pkg->dest->root_dir, file_name);
		} else {
			if (conf->offline_root &&
			    strncmp(conf->offline_root, file_name,
				    rootdirlen)) {
				sprintf_alloc(&installed_file_name, "%s%s",
					      conf->offline_root, file_name);
			} else {
				// already contains root_dir as header -> ABSOLUTE
				sprintf_alloc(&installed_file_name, "%s",
					      file_name);
			}
		}
		str_list_append(pkg->installed_files, installed_file_name);
		free(installed_file_name);
		free(line);
	}

	fclose(list_file);

	if (list_from_package) {
		unlink(list_file_name);
		free(list_file_name);
	}

	return pkg->installed_files;
}

/* XXX: CLEANUP: This function and it's counterpart,
   (pkg_get_installed_files), do not match our init/deinit naming
   convention. Nor the alloc/free convention. But, then again, neither
   of these conventions currrently fit the way these two functions
   work. */
void pkg_free_installed_files(pkg_t * pkg)
{
	pkg->installed_files_ref_cnt--;

	if (pkg->installed_files_ref_cnt > 0)
		return;

	if (pkg->installed_files) {
		str_list_purge(pkg->installed_files);
	}

	pkg->installed_files = NULL;
}

void pkg_remove_installed_files_list(pkg_t * pkg)
{
	char *list_file_name;

	sprintf_alloc(&list_file_name, "%s/%s.list",
		      pkg->dest->info_dir, pkg->name);

	if (!conf->noaction)
		(void)unlink(list_file_name);

	free(list_file_name);
}

conffile_t *pkg_get_conffile(pkg_t * pkg, const char *file_name)
{
	conffile_list_elt_t *iter;
	conffile_list_t *cl;
	conffile_t *conffile;

	if (pkg == NULL) {
		return NULL;
	}

	cl = pkg_get_ptr(pkg, PKG_CONFFILES);

	for (iter = cl ? nv_pair_list_first(cl) : NULL; iter;
	     iter = nv_pair_list_next(cl, iter)) {
		conffile = (conffile_t *) iter->data;

		if (strcmp(conffile->name, file_name) == 0) {
			return conffile;
		}
	}

	return NULL;
}

int pkg_run_script(pkg_t * pkg, const char *script, const char *args)
{
	int err;
	char *path;
	char *cmd;
	char *tmp_unpack_dir;

	if (conf->noaction)
		return 0;

	/* XXX: FEATURE: When conf->offline_root is set, we should run the
	   maintainer script within a chroot environment. */
	if (conf->offline_root && !conf->force_postinstall) {
		opkg_msg(INFO, "Offline root mode: not running %s.%s.\n",
			 pkg->name, script);
		return 0;
	}

	/* Installed packages have scripts in pkg->dest->info_dir, uninstalled packages
	   have scripts in tmp_unpack_dir. */
	if (pkg->state_status == SS_INSTALLED
	    || pkg->state_status == SS_UNPACKED) {
		if (pkg->dest == NULL) {
			opkg_msg(ERROR, "Internal error: %s has a NULL dest.\n",
				 pkg->name);
			return -1;
		}
		sprintf_alloc(&path, "%s/%s.%s", pkg->dest->info_dir, pkg->name,
			      script);
	} else {
		tmp_unpack_dir = pkg_get_string(pkg, PKG_TMP_UNPACK_DIR);
		if (tmp_unpack_dir == NULL) {
			opkg_msg(ERROR,
				 "Internal error: %s has a NULL tmp_unpack_dir.\n",
				 pkg->name);
			return -1;
		}
		sprintf_alloc(&path, "%s/%s", tmp_unpack_dir, script);
	}

	opkg_msg(INFO, "Running script %s.\n", path);

	setenv("PKG_ROOT",
	       pkg->dest ? pkg->dest->root_dir : conf->default_dest->root_dir,
	       1);

	if (pkg->is_upgrade)
		setenv("PKG_UPGRADE", "1", 1);
	else
		setenv("PKG_UPGRADE", "0", 1);

	if (!file_exists(path)) {
		free(path);
		return 0;
	}

	sprintf_alloc(&cmd, "%s %s", path, args);
	free(path);
	{
		const char *argv[] = { "sh", "-c", cmd, NULL };
		err = xsystem(argv);
	}
	free(cmd);

	if (err) {
		opkg_msg(ERROR,
			 "package \"%s\" %s script returned status %d.\n",
			 pkg->name, script, err);
		return err;
	}

	return 0;
}

int pkg_arch_supported(pkg_t * pkg)
{
	nv_pair_list_elt_t *l;
	char *architecture = pkg_get_string(pkg, PKG_ARCHITECTURE);

	if (!architecture)
		return 1;

	list_for_each_entry(l, &conf->arch_list.head, node) {
		nv_pair_t *nv = (nv_pair_t *) l->data;
		if (strcmp(nv->name, architecture) == 0) {
			opkg_msg(DEBUG,
				 "Arch %s (priority %s) supported for pkg %s.\n",
				 nv->name, nv->value, pkg->name);
			return 1;
		}
	}

	opkg_msg(DEBUG, "Arch %s unsupported for pkg %s.\n",
		 architecture, pkg->name);
	return 0;
}

void pkg_info_preinstall_check(void)
{
	int i;
	pkg_vec_t *installed_pkgs = pkg_vec_alloc();

	/* update the file owner data structure */
	opkg_msg(INFO, "Updating file owner list.\n");
	pkg_hash_fetch_all_installed(installed_pkgs);
	for (i = 0; i < installed_pkgs->len; i++) {
		pkg_t *pkg = installed_pkgs->pkgs[i];
		str_list_t *installed_files = pkg_get_installed_files(pkg);	/* this causes installed_files to be cached */
		str_list_elt_t *iter, *niter;
		if (installed_files == NULL) {
			opkg_msg(ERROR, "Failed to determine installed "
				 "files for pkg %s.\n", pkg->name);
			break;
		}
		for (iter = str_list_first(installed_files), niter =
		     str_list_next(installed_files, iter); iter;
		     iter = niter, niter =
		     str_list_next(installed_files, iter)) {
			char *installed_file = (char *)iter->data;
			file_hash_set_file_owner(installed_file, pkg);
		}
		pkg_free_installed_files(pkg);
	}
	pkg_vec_free(installed_pkgs);
}

struct pkg_write_filelist_data {
	pkg_t *pkg;
	FILE *stream;
};

static void
pkg_write_filelist_helper(const char *key, void *entry_, void *data_)
{
	struct pkg_write_filelist_data *data = data_;
	pkg_t *entry = entry_;
	if (entry == data->pkg) {
		fprintf(data->stream, "%s\n", key);
	}
}

int pkg_write_filelist(pkg_t * pkg)
{
	struct pkg_write_filelist_data data;
	char *list_file_name;

	sprintf_alloc(&list_file_name, "%s/%s.list",
		      pkg->dest->info_dir, pkg->name);

	opkg_msg(INFO, "Creating %s file for pkg %s.\n",
		 list_file_name, pkg->name);

	data.stream = fopen(list_file_name, "w");
	if (!data.stream) {
		opkg_perror(ERROR, "Failed to open %s", list_file_name);
		free(list_file_name);
		return -1;
	}

	data.pkg = pkg;
	hash_table_foreach(&conf->file_hash, pkg_write_filelist_helper, &data);
	fclose(data.stream);
	free(list_file_name);

	pkg->state_flag &= ~SF_FILELIST_CHANGED;

	return 0;
}

int pkg_write_changed_filelists(void)
{
	pkg_vec_t *installed_pkgs = pkg_vec_alloc();
	int i, err, ret = 0;

	if (conf->noaction)
		return 0;

	opkg_msg(INFO, "Saving changed filelists.\n");

	pkg_hash_fetch_all_installed(installed_pkgs);
	for (i = 0; i < installed_pkgs->len; i++) {
		pkg_t *pkg = installed_pkgs->pkgs[i];
		if (pkg->state_flag & SF_FILELIST_CHANGED) {
			err = pkg_write_filelist(pkg);
			if (err)
				ret = -1;
		}
	}

	pkg_vec_free(installed_pkgs);

	return ret;
}
