/* conffile.c - the opkg package management system

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

#include <stdio.h>
#include <stdlib.h>

#include "opkg_message.h"
#include "conffile.h"
#include "file_util.h"
#include "sprintf_alloc.h"
#include "opkg_conf.h"

int conffile_init(conffile_t *conffile, const char *file_name, const char *md5sum)
{
    return nv_pair_init(conffile, file_name, md5sum);
}

void conffile_deinit(conffile_t *conffile)
{
    nv_pair_deinit(conffile);
}

int conffile_has_been_modified(conffile_t *conffile)
{
    char *chksum;
    char *filename = conffile->name;
    char *root_filename;
    int ret = 1;

    if (conffile->value == NULL) {
	 opkg_msg(NOTICE, "Conffile %s has no md5sum.\n", conffile->name);
	 return 1;
    }

    root_filename = root_filename_alloc(filename);

#ifdef HAVE_MD5
    if(conffile->value && strlen(conffile->value) > 33) {
        chksum = file_sha256sum_alloc(root_filename);
    } else {
        chksum = file_md5sum_alloc(root_filename);
    }
#else
    chksum = file_sha256sum_alloc(root_filename);
#endif
    if (chksum && (ret = strcmp(chksum, conffile->value))) {
        opkg_msg(INFO, "Conffile %s:\n\told chk=%s\n\tnew chk=%s\n",
		conffile->name, chksum, conffile->value);
    }

    free(root_filename);
    if (chksum)
        free(chksum);

    return ret;
}
