/*
 *  Copyright (C) 2016 Jo-Philipp Wich <jo@mein.io>
 *
 *  Zlib decrompression utility routines.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <signal.h>
#include <pthread.h>

struct gzip_handle {
	FILE *file;
	struct gzip_handle *gzip;

	pid_t pid;
	int rfd, wfd;
	struct sigaction pipe_sa;
	pthread_t thread;
};

int gzip_exec(struct gzip_handle *zh, const char *filename);
ssize_t gzip_read(struct gzip_handle *zh, char *buf, ssize_t len);
ssize_t gzip_copy(struct gzip_handle *zh, FILE *out, ssize_t len);
int gzip_close(struct gzip_handle *zh);
FILE *gzip_fdopen(struct gzip_handle *zh, const char *filename);

#define gzip_seek(zh, len) gzip_copy(zh, NULL, len)
