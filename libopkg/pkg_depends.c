/* pkg_depends.c - the opkg package management system

   Steven M. Ayer

   Copyright (C) 2002 Compaq Computer Corporation

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
#include <ctype.h>

#include "pkg.h"
#include "opkg_utils.h"
#include "pkg_hash.h"
#include "opkg_message.h"
#include "pkg_parse.h"
#include "hash_table.h"
#include "libbb/libbb.h"

static int parseDepends(compound_depend_t * compound_depend, char *depend_str, enum depend_type type);
static depend_t *depend_init(void);
static char **add_unresolved_dep(pkg_t * pkg, char **the_lost, int ref_ndx);
static char **merge_unresolved(char **oldstuff, char **newstuff);
static int is_pkg_in_pkg_vec(pkg_vec_t * vec, pkg_t * pkg);

static int pkg_installed_and_constraint_satisfied(pkg_t * pkg, void *cdata)
{
	depend_t *depend = (depend_t *) cdata;
	if ((pkg->state_status == SS_INSTALLED
	     || pkg->state_status == SS_UNPACKED)
	    && version_constraints_satisfied(depend, pkg))
		return 1;
	else
		return 0;
}

static int pkg_constraint_satisfied(pkg_t * pkg, void *cdata)
{
	depend_t *depend = (depend_t *) cdata;
	if (version_constraints_satisfied(depend, pkg))
		return 1;
	else
		return 0;
}

/* returns ndependencies or negative error value */
int
pkg_hash_fetch_unsatisfied_dependencies(pkg_t * pkg, pkg_vec_t * unsatisfied,
					char ***unresolved)
{
	pkg_t *satisfier_entry_pkg;
	int i, j, k;
	int found;
	char **the_lost;
	abstract_pkg_t *ab_pkg;
	compound_depend_t *compound_depend;

	/*
	 * this is a setup to check for redundant/cyclic dependency checks,
	 * which are marked at the abstract_pkg level
	 */
	if (!(ab_pkg = pkg->parent)) {
		opkg_msg(ERROR, "Internal error, with pkg %s.\n", pkg->name);
		*unresolved = NULL;
		return 0;
	}
	if (ab_pkg->dependencies_checked) {	/* avoid duplicate or cyclic checks */
		*unresolved = NULL;
		return 0;
	} else {
		ab_pkg->dependencies_checked = 1;	/* mark it for subsequent visits */
	}

	compound_depend = pkg_get_ptr(pkg, PKG_DEPENDS);

	if (!compound_depend || !compound_depend->type) {
		*unresolved = NULL;
		return 0;
	}

	the_lost = NULL;

	/* foreach dependency */
	for (i = 0; compound_depend && compound_depend->type; compound_depend++, i++) {
		depend_t **possible_satisfiers =
		    compound_depend->possibilities;;
		found = 0;
		satisfier_entry_pkg = NULL;

		if (compound_depend->type == GREEDY_DEPEND) {
			/* foreach possible satisfier */
			for (j = 0; j < compound_depend->possibility_count; j++) {
				/* foreach provided_by, which includes the abstract_pkg itself */
				abstract_pkg_t *abpkg = possible_satisfiers[j]->pkg;
				abstract_pkg_vec_t *ab_provider_vec = abpkg->provided_by;
				int nposs = ab_provider_vec->len;
				abstract_pkg_t **ab_providers = ab_provider_vec->pkgs;
				int l;
				for (l = 0; l < nposs; l++) {
					pkg_vec_t *test_vec = ab_providers[l]->pkgs;
					/* if no depends on this one, try the first package that Provides this one */
					if (!test_vec) {	/* no pkg_vec hooked up to the abstract_pkg!  (need another feed?) */
						continue;
					}

					/* cruise this possiblity's pkg_vec looking for an installed version */
					for (k = 0; k < test_vec->len; k++) {
						pkg_t *pkg_scout = test_vec->pkgs[k];
						/* not installed, and not already known about? */
						if ((pkg_scout->state_want != SW_INSTALL)
						    && !pkg_scout->parent->dependencies_checked
						    && !is_pkg_in_pkg_vec(unsatisfied, pkg_scout)) {
							char **newstuff = NULL;
							int rc;
							pkg_vec_t *tmp_vec = pkg_vec_alloc();
							/* check for not-already-installed dependencies */
							rc = pkg_hash_fetch_unsatisfied_dependencies(
								pkg_scout, tmp_vec, &newstuff);
							if (newstuff == NULL) {
								int m;
								int ok = 1;
								for (m = 0; m < rc; m++) {
									pkg_t *p = tmp_vec->pkgs[m];
									if (p->state_want == SW_INSTALL)
										continue;
									opkg_msg(DEBUG,
									     "Not installing %s due"
									     " to requirement for %s.\n",
									     pkg_scout->name, p->name);
									ok = 0;
									break;
								}
								pkg_vec_free(tmp_vec);
								if (ok) {
									/* mark this one for installation */
									opkg_msg(NOTICE,
									     "Adding satisfier for greedy"
									     " dependence %s.\n",
									     pkg_scout->name);
									pkg_vec_insert(unsatisfied, pkg_scout);
								}
							} else {
								opkg_msg(DEBUG,
									 "Not installing %s due to "
									 "broken depends.\n",
									 pkg_scout->name);
								free(newstuff);
							}
						}
					}
				}
			}

			continue;
		}

		/* foreach possible satisfier, look for installed package  */
		for (j = 0; j < compound_depend->possibility_count; j++) {
			/* foreach provided_by, which includes the abstract_pkg itself */
			depend_t *dependence_to_satisfy = possible_satisfiers[j];
			abstract_pkg_t *satisfying_apkg = possible_satisfiers[j]->pkg;
			pkg_t *satisfying_pkg =
				pkg_hash_fetch_best_installation_candidate(satisfying_apkg,
					pkg_installed_and_constraint_satisfied,
					dependence_to_satisfy, 1);
			/* Being that I can't test constraing in pkg_hash, I will test it here */
			if (satisfying_pkg != NULL) {
				if (!pkg_installed_and_constraint_satisfied
				    (satisfying_pkg, dependence_to_satisfy)) {
					satisfying_pkg = NULL;
				}
			}
			opkg_msg(DEBUG, "satisfying_pkg=%p\n", satisfying_pkg);
			if (satisfying_pkg != NULL) {
				found = 1;
				break;
			}

		}
		/* if nothing installed matches, then look for uninstalled satisfier */
		if (!found) {
			/* foreach possible satisfier, look for installed package  */
			for (j = 0; j < compound_depend->possibility_count; j++) {
				/* foreach provided_by, which includes the abstract_pkg itself */
				depend_t *dependence_to_satisfy = possible_satisfiers[j];
				abstract_pkg_t *satisfying_apkg = possible_satisfiers[j]->pkg;
				pkg_t *satisfying_pkg =
					pkg_hash_fetch_best_installation_candidate(satisfying_apkg,
						pkg_constraint_satisfied, dependence_to_satisfy, 1);
				/* Being that I can't test constraing in pkg_hash, I will test it here too */
				if (satisfying_pkg != NULL) {
					if (!pkg_constraint_satisfied(satisfying_pkg,
							dependence_to_satisfy)) {
						satisfying_pkg = NULL;
					}
				}

				/* user request overrides package recommendation */
				if (satisfying_pkg != NULL
				    && (compound_depend->type == RECOMMEND
					    || compound_depend->type == SUGGEST)
				    && (satisfying_pkg->state_want == SW_DEINSTALL
					    || satisfying_pkg->state_want == SW_PURGE)) {
					opkg_msg(NOTICE,
						 "%s: ignoring recommendation for "
						 "%s at user request\n",
						 pkg->name, satisfying_pkg->name);
					continue;
				}

				opkg_msg(DEBUG, "satisfying_pkg=%p\n",
					 satisfying_pkg);
				if (satisfying_pkg != NULL) {
					satisfier_entry_pkg = satisfying_pkg;
					break;
				}
			}
		}

		/* we didn't find one, add something to the unsatisfied vector */
		if (!found) {
			if (!satisfier_entry_pkg) {
				/* failure to meet recommendations is not an error */
				if (compound_depend->type != RECOMMEND
				    && compound_depend->type != SUGGEST)
					the_lost = add_unresolved_dep(pkg, the_lost, i);
				else
					opkg_msg(NOTICE,
						"%s: unsatisfied recommendation for %s\n",
						pkg->name,
						compound_depend->possibilities[0]->pkg->name);
			} else {
				if (compound_depend->type == SUGGEST) {
					/* just mention it politely */
					opkg_msg(NOTICE,
						"package %s suggests installing %s\n",
						pkg->name, satisfier_entry_pkg->name);
				} else {
					char **newstuff = NULL;

					if (satisfier_entry_pkg != pkg &&
					    !is_pkg_in_pkg_vec(unsatisfied, satisfier_entry_pkg))
					{
						pkg_hash_fetch_unsatisfied_dependencies(
							satisfier_entry_pkg, unsatisfied, &newstuff);
						pkg_vec_insert(unsatisfied, satisfier_entry_pkg);
						the_lost = merge_unresolved(the_lost, newstuff);
						if (newstuff)
							free(newstuff);
					}
				}
			}
		}
	}
	*unresolved = the_lost;

	return unsatisfied->len;
}

/*checking for conflicts !in replaces
  If a packages conflicts with another but is also replacing it, I should not consider it a
  really conflicts
  returns 0 if conflicts <> replaces or 1 if conflicts == replaces
*/
static int is_pkg_a_replaces(pkg_t * pkg_scout, pkg_t * pkg)
{
	abstract_pkg_t **replaces = pkg_get_ptr(pkg, PKG_REPLACES);

	if (!replaces || !*replaces)
		return 0;

	while (*replaces) {
		if (strcmp(pkg_scout->name, (*replaces)->name) == 0) {	// Found
			opkg_msg(DEBUG2, "Seems I've found a replace %s %s\n",
				 pkg_scout->name, (*replaces)->name);
			return 1;
		}
		replaces++;
	}

	return 0;
}

pkg_vec_t *pkg_hash_fetch_conflicts(pkg_t * pkg)
{
	pkg_vec_t *installed_conflicts, *test_vec;
	compound_depend_t *conflicts, *conflict;
	depend_t **possible_satisfiers;
	depend_t *possible_satisfier;
	int j, k;
	abstract_pkg_t *ab_pkg;
	pkg_t **pkg_scouts;
	pkg_t *pkg_scout;

	/*
	 * this is a setup to check for redundant/cyclic dependency checks,
	 * which are marked at the abstract_pkg level
	 */
	if (!(ab_pkg = pkg->parent)) {
		opkg_msg(ERROR, "Internal error: %s not in hash table\n",
			 pkg->name);
		return (pkg_vec_t *) NULL;
	}

	conflicts = pkg_get_ptr(pkg, PKG_CONFLICTS);
	if (!conflicts) {
		return (pkg_vec_t *) NULL;
	}
	installed_conflicts = pkg_vec_alloc();

	/* foreach conflict */
	for (conflict = conflicts; conflict->type; conflict++ ) {
		possible_satisfiers = conflicts->possibilities;

		/* foreach possible satisfier */
		for (j = 0; j < conflicts->possibility_count; j++) {
			possible_satisfier = possible_satisfiers[j];
			if (!possible_satisfier)
				opkg_msg(ERROR,
					 "Internal error: possible_satisfier=NULL\n");
			if (!possible_satisfier->pkg)
				opkg_msg(ERROR,
					 "Internal error: possible_satisfier->pkg=NULL\n");
			test_vec = possible_satisfier->pkg->pkgs;
			if (test_vec) {
				/* pkg_vec found, it is an actual package conflict
				 * cruise this possiblity's pkg_vec looking for an installed version */
				pkg_scouts = test_vec->pkgs;
				for (k = 0; k < test_vec->len; k++) {
					pkg_scout = pkg_scouts[k];
					if (!pkg_scout) {
						opkg_msg(ERROR,
							 "Internal error: pkg_scout=NULL\n");
						continue;
					}
					if ((pkg_scout->state_status == SS_INSTALLED
						 || pkg_scout->state_want == SW_INSTALL)
						&& version_constraints_satisfied(possible_satisfier,
							pkg_scout)
						&& !is_pkg_a_replaces(pkg_scout, pkg)) {
						if (!is_pkg_in_pkg_vec(installed_conflicts,
							pkg_scout)) {
							pkg_vec_insert(installed_conflicts, pkg_scout);
						}
					}
				}
			}
		}
		conflicts++;
	}

	if (installed_conflicts->len)
		return installed_conflicts;
	pkg_vec_free(installed_conflicts);
	return (pkg_vec_t *) NULL;
}

int version_constraints_satisfied(depend_t * depends, pkg_t * pkg)
{
	pkg_t *temp;
	int comparison;

	if (depends->constraint == NONE)
		return 1;

	temp = pkg_new();

	parse_version(temp, depends->version);

	comparison = pkg_compare_versions(pkg, temp);

	pkg_deinit(temp);
	free(temp);

	if ((depends->constraint == EARLIER) && (comparison < 0))
		return 1;
	else if ((depends->constraint == LATER) && (comparison > 0))
		return 1;
	else if (comparison == 0)
		return 1;
	else if ((depends->constraint == LATER_EQUAL) && (comparison >= 0))
		return 1;
	else if ((depends->constraint == EARLIER_EQUAL) && (comparison <= 0))
		return 1;

	return 0;
}

int pkg_dependence_satisfiable(depend_t * depend)
{
	abstract_pkg_t *apkg = depend->pkg;
	abstract_pkg_vec_t *provider_apkgs = apkg->provided_by;
	int n_providers = provider_apkgs->len;
	abstract_pkg_t **apkgs = provider_apkgs->pkgs;
	pkg_vec_t *pkg_vec;
	int n_pkgs;
	int i;
	int j;

	for (i = 0; i < n_providers; i++) {
		abstract_pkg_t *papkg = apkgs[i];
		pkg_vec = papkg->pkgs;
		if (pkg_vec) {
			n_pkgs = pkg_vec->len;
			for (j = 0; j < n_pkgs; j++) {
				pkg_t *pkg = pkg_vec->pkgs[j];
				if (version_constraints_satisfied(depend, pkg)) {
					return 1;
				}
			}
		}
	}
	return 0;
}

static int is_pkg_in_pkg_vec(pkg_vec_t * vec, pkg_t * pkg)
{
	int i;
	char *arch1, *arch2;
	pkg_t **pkgs = vec->pkgs;
	arch1 = pkg_get_architecture(pkg);

	for (i = 0; i < vec->len; i++) {
		arch2 = pkg_get_architecture(*(pkgs + i));

		if ((strcmp(pkg->name, (*(pkgs + i))->name) == 0)
		    && (pkg_compare_versions(pkg, *(pkgs + i)) == 0)
		    && (strcmp(arch1, arch2) == 0))
			return 1;
	}
	return 0;
}

/**
 * pkg_replaces returns 1 if pkg->replaces contains one of replacee's provides and 0
 * otherwise.
 */
int pkg_replaces(pkg_t * pkg, pkg_t * replacee)
{
	abstract_pkg_t **replaces = pkg_get_ptr(pkg, PKG_REPLACES);
	abstract_pkg_t **provides = pkg_get_ptr(replacee, PKG_PROVIDES);
	abstract_pkg_t **r, **p;

	for (r = replaces; r && *r; r++)
		for (p = provides; p && *p; p++)
			if (*r == *p)
				return 1;

	return 0;
}

/**
 * pkg_conflicts_abstract returns 1 if pkg->conflicts contains conflictee and 0
 * otherwise.
 */
int pkg_conflicts_abstract(pkg_t * pkg, abstract_pkg_t * conflictee)
{
	compound_depend_t *conflicts, *conflict;

	conflicts = pkg_get_ptr(pkg, PKG_CONFLICTS);

	int j;
	for (conflict = conflicts; conflict->type; conflict++) {
		int possibility_count = conflict->possibility_count;
		struct depend **possibilities = conflict->possibilities;
		for (j = 0; j < possibility_count; j++) {
			if (possibilities[j]->pkg == conflictee) {
				return 1;
			}
		}
	}
	return 0;
}

/**
 * pkg_conflicts returns 1 if pkg->conflicts contains one of
 * conflictee's provides and 0 otherwise.
 */
int pkg_conflicts(pkg_t * pkg, pkg_t * conflictee)
{
	int j;
	int possibility_count;
	struct depend **possibilities;
	compound_depend_t *conflicts, *conflict;
	abstract_pkg_t **conflictee_provides, **provider, *possibility;

	conflicts = pkg_get_ptr(pkg, PKG_CONFLICTS);
	conflictee_provides = pkg_get_ptr(conflictee, PKG_PROVIDES);

	for (conflict = conflicts; conflict->type; conflict++) {
		possibility_count = conflict->possibility_count;
		possibilities = conflict->possibilities;
		for (j = 0; j < possibility_count; j++) {
			possibility = possibilities[j]->pkg;
			for (provider = conflictee_provides; provider && *provider; provider++) {
				if (possibility == *provider) {
					return 1;
				}
			}
		}
	}
	return 0;
}

static char **merge_unresolved(char **oldstuff, char **newstuff)
{
	int oldlen = 0, newlen = 0;
	char **result;
	int i, j;

	if (!newstuff)
		return oldstuff;

	while (oldstuff && oldstuff[oldlen])
		oldlen++;
	while (newstuff && newstuff[newlen])
		newlen++;

	result = xrealloc(oldstuff, sizeof(char *) * (oldlen + newlen + 1));

	for (i = oldlen, j = 0; i < (oldlen + newlen); i++, j++)
		*(result + i) = *(newstuff + j);

	*(result + i) = NULL;

	return result;
}

/*
 * a kinda kludgy way to back out depends str from two different arrays (reg'l'r 'n pre)
 * this is null terminated, no count is carried around
 */
char **add_unresolved_dep(pkg_t * pkg, char **the_lost, int ref_ndx)
{
	int count;
	char **resized;

	count = 0;
	while (the_lost && the_lost[count])
		count++;

	count++;		/* need one to hold the null */
	resized = xrealloc(the_lost, sizeof(char *) * (count + 1));
	resized[count - 1] = pkg_depend_str(pkg, ref_ndx);
	resized[count] = NULL;

	return resized;
}

static void flag_related_packages(pkg_t *pkg, int state_flags)
{
	int i, j;
	compound_depend_t *deps;

	for (deps = pkg_get_ptr(pkg, PKG_DEPENDS), i = 0; deps && deps[i].type; i++)
		for (j = 0; j < deps[i].possibility_count; j++) {
			if ((deps[i].possibilities[j]->pkg->state_flag & state_flags) != state_flags) {
				opkg_msg(DEBUG, "propagating pkg flag to dependent abpkg %s\n",
				         deps[i].possibilities[j]->pkg->name);
				deps[i].possibilities[j]->pkg->state_flag |= state_flags;
			}
		}

	for (deps = pkg_get_ptr(pkg, PKG_CONFLICTS), i = 0; deps && deps[i].type; i++)
		for (j = 0; j < deps[i].possibility_count; j++) {
			if ((deps[i].possibilities[j]->pkg->state_flag & state_flags) != state_flags) {
				opkg_msg(DEBUG, "propagating pkg flag to conflicting abpkg %s\n",
				         deps[i].possibilities[j]->pkg->name);
				deps[i].possibilities[j]->pkg->state_flag |= state_flags;
			}
		}
}

abstract_pkg_t **init_providelist(pkg_t *pkg, int *count)
{
	abstract_pkg_t *ab_pkg;
	abstract_pkg_t **provides = pkg_get_ptr(pkg, PKG_PROVIDES);

	if (!provides) {
		provides = calloc(2, sizeof(abstract_pkg_t *));

		if (!provides) {
			if (count)
				*count = 0;

			return NULL;
		}

		ab_pkg = ensure_abstract_pkg_by_name(pkg->name);

		if (!ab_pkg->pkgs)
			ab_pkg->pkgs = pkg_vec_alloc();

		if (!abstract_pkg_vec_contains(ab_pkg->provided_by, ab_pkg))
			abstract_pkg_vec_insert(ab_pkg->provided_by, ab_pkg);

		provides[0] = ab_pkg;
		provides[1] = NULL;

		if (count)
			*count = 2;

		pkg_set_ptr(pkg, PKG_PROVIDES, provides);
	}
	else if (count) {
		for (*count = 1; *provides; provides++) {
			if (pkg->state_flag & SF_NEED_DETAIL) {
				if (!((*provides)->state_flag & SF_NEED_DETAIL)) {
					opkg_msg(DEBUG, "propagating pkg flag to provided abpkg %s\n",
					         (*provides)->name);
					(*provides)->state_flag |= SF_NEED_DETAIL;
				}
			}
			(*count)++;
		}
	}

	flag_related_packages(pkg, SF_NEED_DETAIL);

	return provides;
}

void parse_providelist(pkg_t *pkg, char *list)
{
	int count = 0;
	char *item, *tok;
	abstract_pkg_t *ab_pkg, *provided_abpkg, **tmp, **provides;

	provides = init_providelist(pkg, &count);
	ab_pkg = ensure_abstract_pkg_by_name(pkg->name);

	if (!provides || !ab_pkg)
		return;

	for (item = strtok_r(list, ", ", &tok); item;
	     count++, item = strtok_r(NULL, ", ", &tok)) {
		tmp = realloc(provides, sizeof(abstract_pkg_t *) * (count + 1));

		if (!tmp)
			break;

		provided_abpkg = ensure_abstract_pkg_by_name(item);

		if (provided_abpkg->state_flag & SF_NEED_DETAIL) {
			if (!(ab_pkg->state_flag & SF_NEED_DETAIL)) {
				opkg_msg(DEBUG, "propagating provided abpkg flag to "
				                "provider abpkg %s\n", ab_pkg->name);
				ab_pkg->state_flag |= SF_NEED_DETAIL;
			}
		}

		if (!abstract_pkg_vec_contains(provided_abpkg->provided_by, ab_pkg))
			abstract_pkg_vec_insert(provided_abpkg->provided_by, ab_pkg);

		provides = tmp;
		provides[count - 1] = provided_abpkg;
	}

	provides[count - 1] = NULL;

	pkg_set_ptr(pkg, PKG_PROVIDES, provides);
}

void parse_replacelist(pkg_t *pkg, char *list)
{
	int count;
	char *item, *tok;
	abstract_pkg_t *ab_pkg, *old_abpkg, **tmp, **replaces = NULL;

	ab_pkg = ensure_abstract_pkg_by_name(pkg->name);

	if (!ab_pkg->pkgs)
		ab_pkg->pkgs = pkg_vec_alloc();

	abstract_pkg_vec_insert(ab_pkg->provided_by, ab_pkg);

	for (count = 1, item = strtok_r(list, ", ", &tok);
	     item;
	     count++, item = strtok_r(NULL, ", ", &tok), count++) {
		tmp = realloc(replaces, sizeof(abstract_pkg_t *) * (count + 1));

		if (!tmp)
			break;

		old_abpkg = ensure_abstract_pkg_by_name(item);

		if (pkg->state_flag & SF_NEED_DETAIL) {
			if (!(old_abpkg->state_flag & SF_NEED_DETAIL)) {
				opkg_msg(DEBUG, "propagating pkg flag to replaced abpkg %s\n",
				         old_abpkg->name);
				old_abpkg->state_flag |= SF_NEED_DETAIL;
			}
		}

		if (!old_abpkg->replaced_by)
			old_abpkg->replaced_by = abstract_pkg_vec_alloc();

		/* if a package pkg both replaces and conflicts old_abpkg,
		 * then add it to the replaced_by vector so that old_abpkg
		 * will be upgraded to ab_pkg automatically */
		if (pkg_conflicts_abstract(pkg, old_abpkg))
			abstract_pkg_vec_insert(old_abpkg->replaced_by, ab_pkg);

		replaces = tmp;
		replaces[count - 1] = old_abpkg;
	}

	if (!replaces)
		return;

	replaces[count - 1] = NULL;

	pkg_set_ptr(pkg, PKG_REPLACES, replaces);
}

void buildProvides(abstract_pkg_t * ab_pkg, pkg_t * pkg)
{
#if 0
	int i;

	/* every pkg provides itself */
	pkg->provides_count++;
	abstract_pkg_vec_insert(ab_pkg->provided_by, ab_pkg);
	pkg->provides = xcalloc(pkg->provides_count, sizeof(abstract_pkg_t *));
	pkg->provides[0] = ab_pkg;
	for (i = 1; i < pkg->provides_count; i++) {
		abstract_pkg_t *provided_abpkg =
		    ensure_abstract_pkg_by_name(pkg->provides_str[i - 1]);
		free(pkg->provides_str[i - 1]);

		pkg->provides[i] = provided_abpkg;

		abstract_pkg_vec_insert(provided_abpkg->provided_by, ab_pkg);
	}
	if (pkg->provides_str)
		free(pkg->provides_str);
#endif
}

void buildConflicts(pkg_t * pkg)
{
	/*
	int i;
	compound_depend_t *conflicts, *conflict;

	if (!pkg->conflicts_count)
		return;

	conflicts = pkg->conflicts =
	    xcalloc(pkg->conflicts_count, sizeof(compound_depend_t));
	for (i = 0; i < pkg->conflicts_count; i++) {
		conflicts->type = CONFLICTS;
		parseDepends(conflicts, pkg->conflicts_str[i]);
		free(pkg->conflicts_str[i]);
		conflicts++;
	}
	if (pkg->conflicts_str)
		free(pkg->conflicts_str);
		*/
}

void buildReplaces(abstract_pkg_t * ab_pkg, pkg_t * pkg)
{
#if 0
	int i;

	if (!pkg->replaces_count)
		return;

	pkg->replaces = xcalloc(pkg->replaces_count, sizeof(abstract_pkg_t *));

	for (i = 0; i < pkg->replaces_count; i++) {
		abstract_pkg_t *old_abpkg =
		    ensure_abstract_pkg_by_name(pkg->replaces_str[i]);

		pkg->replaces[i] = old_abpkg;
		free(pkg->replaces_str[i]);

		if (!old_abpkg->replaced_by)
			old_abpkg->replaced_by = abstract_pkg_vec_alloc();
		/* if a package pkg both replaces and conflicts old_abpkg,
		 * then add it to the replaced_by vector so that old_abpkg
		 * will be upgraded to ab_pkg automatically */
		if (pkg_conflicts_abstract(pkg, old_abpkg))
			abstract_pkg_vec_insert(old_abpkg->replaced_by, ab_pkg);
	}

	if (pkg->replaces_str)
		free(pkg->replaces_str);
#endif
}

void parse_deplist(pkg_t *pkg, enum depend_type type, char *list)
{
	int id, count;
	char *item, *tok;
	compound_depend_t *tmp, *deps;

	switch (type)
	{
	case DEPEND:
	case PREDEPEND:
	case RECOMMEND:
	case SUGGEST:
	case GREEDY_DEPEND:
		id = PKG_DEPENDS;
		break;

	case CONFLICTS:
		id = PKG_CONFLICTS;
		break;

	default:
		return;
	}

	deps = pkg_get_ptr(pkg, id);

	for (tmp = deps, count = 1; tmp && tmp->type; tmp++)
		count++;

	for (item = strtok_r(list, ",", &tok); item; item = strtok_r(NULL, ",", &tok), count++) {
		tmp = realloc(deps, sizeof(compound_depend_t) * (count + 1));

		if (!tmp)
			break;

		deps = tmp;

		memset(deps + count - 1, 0, sizeof(compound_depend_t));
		parseDepends(deps + count - 1, item, type);
	}

	if (!deps)
		return;

	memset(deps + count - 1, 0, sizeof(compound_depend_t));
	pkg_set_ptr(pkg, id, deps);
}

void buildDepends(pkg_t * pkg)
{
#if 0
	unsigned int count;
	int i;
	compound_depend_t *depends;

	if (!
	    (count =
	     pkg->pre_depends_count + pkg->depends_count +
	     pkg->recommends_count + pkg->suggests_count))
		return;

	depends = pkg->depends = xcalloc(count, sizeof(compound_depend_t));

	for (i = 0; i < pkg->pre_depends_count; i++) {
		parseDepends(depends, pkg->pre_depends_str[i]);
		free(pkg->pre_depends_str[i]);
		depends->type = PREDEPEND;
		depends++;
	}
	if (pkg->pre_depends_str)
		free(pkg->pre_depends_str);

	for (i = 0; i < pkg->depends_count; i++) {
		parseDepends(depends, pkg->depends_str[i]);
		free(pkg->depends_str[i]);
		depends++;
	}
	if (pkg->depends_str)
		free(pkg->depends_str);

	for (i = 0; i < pkg->recommends_count; i++) {
		parseDepends(depends, pkg->recommends_str[i]);
		free(pkg->recommends_str[i]);
		depends->type = RECOMMEND;
		depends++;
	}
	if (pkg->recommends_str)
		free(pkg->recommends_str);

	for (i = 0; i < pkg->suggests_count; i++) {
		parseDepends(depends, pkg->suggests_str[i]);
		free(pkg->suggests_str[i]);
		depends->type = SUGGEST;
		depends++;
	}
	if (pkg->suggests_str)
		free(pkg->suggests_str);

#endif
}

const char *constraint_to_str(enum version_constraint c)
{
	switch (c) {
	case NONE:
		return "";
	case EARLIER:
		return "< ";
	case EARLIER_EQUAL:
		return "<= ";
	case EQUAL:
		return "= ";
	case LATER_EQUAL:
		return ">= ";
	case LATER:
		return "> ";
	}

	return "";
}

/*
 * Returns a printable string for pkg's dependency at the specified idx. The
 * resultant string must be passed to free() by the caller.
 */
char *pkg_depend_str(pkg_t * pkg, int idx)
{
	int i;
	unsigned int len;
	char *str;
	compound_depend_t *cdep = NULL, *p;
	depend_t *dep;

	for (i = 0, p = pkg_get_ptr(pkg, PKG_DEPENDS); p && p->type; i++, p++)
		if (i == idx) {
			cdep = p;
			break;
		}

	if (!cdep)
		return NULL;

	len = 0;

	/* calculate string length */
	for (i = 0; i < cdep->possibility_count; i++) {
		dep = cdep->possibilities[i];

		if (i != 0)
			len += 3;	/* space, pipe, space */

		len += strlen(dep->pkg->name);

		if (dep->version) {
			len += 2;	/* space, left parenthesis */
			len += 3;	/* constraint string (<=, >=, etc), space */
			len += strlen(dep->version);
			len += 1;	/* right parenthesis */
		}
	}

	str = xmalloc(len + 1);	/* +1 for the NULL terminator */
	str[0] = '\0';

	for (i = 0; i < cdep->possibility_count; i++) {
		dep = cdep->possibilities[i];

		if (i != 0)
			strncat(str, " | ", len);

		strncat(str, dep->pkg->name, len);

		if (dep->version) {
			strncat(str, " (", len);
			strncat(str, constraint_to_str(dep->constraint), len);
			strncat(str, dep->version, len);
			strncat(str, ")", len);
		}
	}

	return str;
}

void buildDependedUponBy(pkg_t * pkg, abstract_pkg_t * ab_pkg)
{
	compound_depend_t *depends;
	int othercount;
	int j;
	abstract_pkg_t *ab_depend;
	abstract_pkg_t **temp;

	for (depends = pkg_get_ptr(pkg, PKG_DEPENDS); depends && depends->type; depends++) {
		if (depends->type != PREDEPEND
		    && depends->type != DEPEND && depends->type != RECOMMEND)
			continue;
		for (j = 0; j < depends->possibility_count; j++) {
			ab_depend = depends->possibilities[j]->pkg;
			if (!ab_depend->depended_upon_by) {
				ab_depend->depended_upon_by =
				    xcalloc(1, sizeof(abstract_pkg_t *));
			}

			temp = ab_depend->depended_upon_by;
			othercount = 1;
			while (*temp) {
				temp++;
				othercount++;
			}
			*temp = ab_pkg;

			ab_depend->depended_upon_by =
			    xrealloc(ab_depend->depended_upon_by,
				     (othercount +
				      1) * sizeof(abstract_pkg_t *));

			/* the array may have been moved by realloc */
			temp = ab_depend->depended_upon_by + othercount;
			*temp = NULL;
		}
	}
}

static depend_t *depend_init(void)
{
	depend_t *d = xcalloc(1, sizeof(depend_t));
	d->constraint = NONE;
	d->version = NULL;
	d->pkg = NULL;

	return d;
}

static int parseDepends(compound_depend_t * compound_depend, char *depend_str, enum depend_type type)
{
	int i;
	char *depend, *name, *vstr, *rest, *tok = NULL;
	depend_t **possibilities = NULL, **tmp;

	compound_depend->type = type;

	for (i = 0, depend = strtok_r(depend_str, "|", &tok); depend; i++, depend = strtok_r(NULL, "|", &tok)) {
		name = strtok(depend, " ");
		rest = strtok(NULL, "\n");

		tmp = realloc(possibilities, sizeof(tmp) * (i + 1));

		if (!tmp)
			return -1;

		possibilities = tmp;
		possibilities[i] = depend_init();
		possibilities[i]->pkg = ensure_abstract_pkg_by_name(name);

		if (rest && *rest == '(') {
			vstr = strtok(rest + 1, ")");

			if (!strncmp(vstr, "<<", 2)) {
				possibilities[i]->constraint = EARLIER;
				vstr += 2;
			} else if (!strncmp(vstr, "<=", 2)) {
				possibilities[i]->constraint = EARLIER_EQUAL;
				vstr += 2;
			} else if (!strncmp(vstr, ">=", 2)) {
				possibilities[i]->constraint = LATER_EQUAL;
				vstr += 2;
			} else if (!strncmp(vstr, ">>", 2)) {
				possibilities[i]->constraint = LATER;
				vstr += 2;
			} else if (!strncmp(vstr, "=", 1)) {
				possibilities[i]->constraint = EQUAL;
				vstr++;
			}
			/* should these be here to support deprecated designations; dpkg does */
			else if (!strncmp(vstr, "<", 1)) {
				possibilities[i]->constraint = EARLIER_EQUAL;
				vstr++;
			} else if (!strncmp(vstr, ">", 1)) {
				possibilities[i]->constraint = LATER_EQUAL;
				vstr++;
			}

			possibilities[i]->version = trim_xstrdup(vstr);
			rest = strtok(NULL, " ");
		}
		else {
			rest = strtok(rest, " ");
		}

		if (rest && *rest == '*')
			compound_depend->type = GREEDY_DEPEND;
	}

	compound_depend->possibility_count = i;
	compound_depend->possibilities = possibilities;

	return 0;
}

compound_depend_t *pkg_get_depends(pkg_t *pkg, enum depend_type type)
{
	compound_depend_t *dep;

	for (dep = pkg_get_ptr(pkg, PKG_DEPENDS); dep && dep->type; dep++)
		if (type == UNSPEC || dep->type == type)
			return dep;

	return NULL;
}
