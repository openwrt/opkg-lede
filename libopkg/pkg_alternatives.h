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

#ifndef OPKG_ALTERNATIVES_H
#define OPKG_ALTERNATIVES_H

#include "pkg.h"

int pkg_alternatives_update(pkg_t * pkg);

#endif
