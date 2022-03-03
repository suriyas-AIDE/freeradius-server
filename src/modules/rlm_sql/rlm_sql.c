/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_sql.c
 * @brief Implements SQL 'users' file, and SQL accounting.
 *
 * @copyright 2012-2014 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 * @copyright 2000,2006 The FreeRADIUS server project
 * @copyright 2000 Mike Machado (mike@innercite.com)
 * @copyright 2000 Alan DeKok (aland@freeradius.org)
 */
RCSID("$Id$")

#define LOG_PREFIX mctx->inst->name

#include <ctype.h>

#include <freeradius-devel/server/base.h>
#include <freeradius-devel/server/exfile.h>
#include <freeradius-devel/server/map_proc.h>
#include <freeradius-devel/server/module_rlm.h>
#include <freeradius-devel/server/pairmove.h>
#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/table.h>

#include <sys/stat.h>

#include "rlm_sql.h"

extern module_rlm_t rlm_sql;

static int driver_parse(UNUSED TALLOC_CTX *ctx, void *out, void *parent,
			CONF_ITEM *ci, UNUSED CONF_PARSER const *rule);

/*
 *	So we can do pass2 xlat checks on the queries.
 */
static const CONF_PARSER query_config[] = {
	{ FR_CONF_OFFSET("query", FR_TYPE_STRING | FR_TYPE_XLAT | FR_TYPE_MULTI, rlm_sql_config_t, accounting.query) },
	CONF_PARSER_TERMINATOR
};

/*
 *	For now hard-code the subsections.  This isn't perfect, but it
 *	helps the average case.
 */
static const CONF_PARSER type_config[] = {
	{ FR_CONF_POINTER("accounting-on", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) query_config },
	{ FR_CONF_POINTER("accounting-off", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) query_config },
	{ FR_CONF_POINTER("start", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) query_config },
	{ FR_CONF_POINTER("interim-update", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) query_config },
	{ FR_CONF_POINTER("stop", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) query_config },
	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER acct_config[] = {
	{ FR_CONF_OFFSET("reference", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_sql_config_t, accounting.reference), .dflt = ".query" },
	{ FR_CONF_OFFSET("logfile", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_sql_config_t, accounting.logfile) },

	{ FR_CONF_POINTER("type", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) type_config },
	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER postauth_config[] = {
	{ FR_CONF_OFFSET("reference", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_sql_config_t, postauth.reference), .dflt = ".query" },
	{ FR_CONF_OFFSET("logfile", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_sql_config_t, postauth.logfile) },

	{ FR_CONF_OFFSET("query", FR_TYPE_STRING | FR_TYPE_XLAT | FR_TYPE_MULTI, rlm_sql_config_t, postauth.query) },
	CONF_PARSER_TERMINATOR
};

static const CONF_PARSER module_config[] = {
	{ FR_CONF_OFFSET("driver", FR_TYPE_VOID, rlm_sql_t, driver_submodule), .dflt = "rlm_sql_null",
			 .func = driver_parse },
	{ FR_CONF_OFFSET("server", FR_TYPE_STRING, rlm_sql_config_t, sql_server), .dflt = "" },	/* Must be zero length so drivers can determine if it was set */
	{ FR_CONF_OFFSET("port", FR_TYPE_UINT32, rlm_sql_config_t, sql_port), .dflt = "0" },
	{ FR_CONF_OFFSET("login", FR_TYPE_STRING, rlm_sql_config_t, sql_login), .dflt = "" },
	{ FR_CONF_OFFSET("password", FR_TYPE_STRING | FR_TYPE_SECRET, rlm_sql_config_t, sql_password), .dflt = "" },
	{ FR_CONF_OFFSET("radius_db", FR_TYPE_STRING, rlm_sql_config_t, sql_db), .dflt = "radius" },
	{ FR_CONF_OFFSET("read_groups", FR_TYPE_BOOL, rlm_sql_config_t, read_groups), .dflt = "yes" },
	{ FR_CONF_OFFSET("read_profiles", FR_TYPE_BOOL, rlm_sql_config_t, read_profiles), .dflt = "yes" },
	{ FR_CONF_OFFSET("sql_user_name", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_sql_config_t, query_user), .dflt = "" },
	{ FR_CONF_OFFSET("group_attribute", FR_TYPE_STRING, rlm_sql_config_t, group_attribute) },
	{ FR_CONF_OFFSET("logfile", FR_TYPE_STRING | FR_TYPE_XLAT, rlm_sql_config_t, logfile) },
	{ FR_CONF_OFFSET("default_user_profile", FR_TYPE_STRING, rlm_sql_config_t, default_profile), .dflt = "" },
	{ FR_CONF_OFFSET("open_query", FR_TYPE_STRING, rlm_sql_config_t, connect_query) },

	{ FR_CONF_OFFSET("authorize_check_query", FR_TYPE_STRING | FR_TYPE_XLAT | FR_TYPE_NOT_EMPTY, rlm_sql_config_t, authorize_check_query) },
	{ FR_CONF_OFFSET("authorize_reply_query", FR_TYPE_STRING | FR_TYPE_XLAT | FR_TYPE_NOT_EMPTY, rlm_sql_config_t, authorize_reply_query) },

	{ FR_CONF_OFFSET("authorize_group_check_query", FR_TYPE_STRING | FR_TYPE_XLAT | FR_TYPE_NOT_EMPTY, rlm_sql_config_t, authorize_group_check_query) },
	{ FR_CONF_OFFSET("authorize_group_reply_query", FR_TYPE_STRING | FR_TYPE_XLAT | FR_TYPE_NOT_EMPTY, rlm_sql_config_t, authorize_group_reply_query) },
	{ FR_CONF_OFFSET("group_membership_query", FR_TYPE_STRING | FR_TYPE_XLAT | FR_TYPE_NOT_EMPTY, rlm_sql_config_t, groupmemb_query) },
	{ FR_CONF_OFFSET("safe_characters", FR_TYPE_STRING, rlm_sql_config_t, allowed_chars), .dflt = "@abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_: /" },

	/*
	 *	This only works for a few drivers.
	 */
	{ FR_CONF_OFFSET("query_timeout", FR_TYPE_TIME_DELTA, rlm_sql_config_t, query_timeout) },

	{ FR_CONF_POINTER("accounting", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) acct_config },

	{ FR_CONF_POINTER("post-auth", FR_TYPE_SUBSECTION, NULL), .subcs = (void const *) postauth_config },
	CONF_PARSER_TERMINATOR
};

static fr_dict_t const *dict_freeradius;
static fr_dict_t const *dict_radius;

extern fr_dict_autoload_t rlm_sql_dict[];
fr_dict_autoload_t rlm_sql_dict[] = {
	{ .out = &dict_freeradius, .proto = "freeradius" },
	{ .out = &dict_radius, .proto = "radius" },
	{ NULL }
};

static fr_dict_attr_t const *attr_fall_through;
static fr_dict_attr_t const *attr_sql_user_name;
static fr_dict_attr_t const *attr_user_profile;
static fr_dict_attr_t const *attr_user_name;

extern fr_dict_attr_autoload_t rlm_sql_dict_attr[];
fr_dict_attr_autoload_t rlm_sql_dict_attr[] = {
	{ .out = &attr_fall_through, .name = "Fall-Through", .type = FR_TYPE_BOOL, .dict = &dict_freeradius },
	{ .out = &attr_sql_user_name, .name = "SQL-User-Name", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_user_profile, .name = "User-Profile", .type = FR_TYPE_STRING, .dict = &dict_freeradius },
	{ .out = &attr_user_name, .name = "User-Name", .type = FR_TYPE_STRING, .dict = &dict_radius },
	{ NULL }
};

static int driver_parse(UNUSED TALLOC_CTX *ctx, void *out, void *parent,
			CONF_ITEM *ci, UNUSED CONF_PARSER const *rule)
{
	rlm_sql_t		*inst = talloc_get_type_abort(parent, rlm_sql_t);
	char const		*name = cf_pair_value(cf_item_to_pair(ci));
	CONF_SECTION		*cs = cf_item_to_section(cf_parent(ci));
	CONF_SECTION		*driver_cs;
	module_instance_t	*mi;

	fr_assert(out == &inst->driver_submodule);	/* Make sure we're being told to write in the right place */

	driver_cs = cf_section_find(cs, name, NULL);

	/*
	 *	Allocate an empty section if one doesn't exist
	 *	this is so defaults get parsed.
	 */
	if (!driver_cs) driver_cs = cf_section_alloc(cs, cs, name, NULL);

	mi = module_bootstrap(DL_MODULE_TYPE_SUBMODULE, module_by_data(parent), driver_cs);
	if (unlikely(mi == NULL)) {
		cf_log_err(driver_cs, "Failed loading SQL driver");
		return -1;
	}

	*((module_instance_t **)out) = mi;

	inst->driver = (rlm_sql_driver_t const *)inst->driver_submodule->module; /* Public symbol exported by the submodule */

	return 0;
}

/*
 *	Fall-Through checking function from rlm_files.c
 */
static sql_fall_through_t fall_through(fr_pair_list_t *vps)
{
	fr_pair_t *tmp;
	tmp = fr_pair_find_by_da_idx(vps, attr_fall_through, 0);

	return tmp ? tmp->vp_uint32 : FALL_THROUGH_DEFAULT;
}

/*
 *	Yucky prototype.
 */
static size_t sql_escape_func(request_t *, char *out, size_t outlen, char const *in, void *arg);

/** Escape a tainted VB used as an xlat argument
 *
 */
static int sql_xlat_escape(request_t *request, fr_value_box_t *vb, void *uctx)
{
	fr_sbuff_t		sbuff;
	fr_sbuff_uctx_talloc_t	sbuff_ctx;

	size_t			len;
	rlm_sql_handle_t	*handle;
	rlm_sql_t		*inst = talloc_get_type_abort(uctx, rlm_sql_t);
	fr_dlist_t		entry;

	handle = fr_pool_connection_get(inst->pool, request);
	if (!handle) {
		fr_value_box_clear_value(vb);
		return -1;
	}

	/*
	 *	Maximum escaped length is 3 * original - if every character needs escaping
	 */
	if (!fr_sbuff_init_talloc(vb, &sbuff, &sbuff_ctx, vb->length * 3, vb->length * 3)) {
		fr_strerror_printf_push("Failed to allocate buffer for escaped sql argument");
		return -1;
	}

	len = inst->sql_escape_func(request, fr_sbuff_buff(&sbuff), vb->length * 3 + 1, vb->vb_strvalue, handle);

	/*
	 *	fr_value_box_strdup_shallow resets the dlist entries - take a copy
	 */
	entry = vb->entry;
	fr_sbuff_trim_talloc(&sbuff, len);
	fr_value_box_clear_value(vb);
	fr_value_box_strdup_shallow(vb, NULL, fr_sbuff_buff(&sbuff), vb->tainted);
	vb->entry = entry;

	fr_pool_connection_release(inst->pool, request, handle);
	return 0;
}

/** Execute an arbitrary SQL query
 *
 * For SELECTs, the values of the first column will be returned.
 * For INSERTS, UPDATEs and DELETEs, the number of rows affected will
 * be returned instead.
 *
@verbatim
%{sql:<sql statement>}
@endverbatim
 *
 * @ingroup xlat_functions
 */
static xlat_action_t sql_xlat(TALLOC_CTX *ctx, fr_dcursor_t *out,
			      xlat_ctx_t const *xctx,
			      request_t *request, fr_value_box_list_t *in)
{
	rlm_sql_handle_t	*handle = NULL;
	rlm_sql_row_t		row;
	rlm_sql_t const		*inst = talloc_get_type_abort(xctx->mctx->inst->data, rlm_sql_t);
	sql_rcode_t		rcode;
	xlat_action_t		ret = XLAT_ACTION_DONE;
	char const		*p;
	fr_value_box_t		*arg = fr_dlist_head(in);
	fr_value_box_t		*vb = NULL;
	bool			fetched = false;

	handle = fr_pool_connection_get(inst->pool, request);	/* connection pool should produce error */
	if (!handle) return XLAT_ACTION_FAIL;

	rlm_sql_query_log(inst, request, NULL, arg->vb_strvalue);

	p = arg->vb_strvalue;

	/*
	 *	Trim whitespace for the prefix check
	 */
	fr_skip_whitespace(p);

	/*
	 *	If the query starts with any of the following prefixes,
	 *	then return the number of rows affected
	 */
	if ((strncasecmp(p, "insert", 6) == 0) ||
	    (strncasecmp(p, "update", 6) == 0) ||
	    (strncasecmp(p, "delete", 6) == 0)) {
		int numaffected;

		rcode = rlm_sql_query(inst, request, &handle, arg->vb_strvalue);
		if (rcode != RLM_SQL_OK) {
		query_error:
			RERROR("SQL query failed: %s", fr_table_str_by_value(sql_rcode_description_table, rcode, "<INVALID>"));

			ret = XLAT_ACTION_FAIL;
			goto finish;
		}

		numaffected = (inst->driver->sql_affected_rows)(handle, &inst->config);
		if (numaffected < 1) {
			RDEBUG2("SQL query affected no rows");
			(inst->driver->sql_finish_query)(handle, &inst->config);

			goto finish;
		}

		MEM(vb = fr_value_box_alloc_null(ctx));
		fr_value_box_uint32(vb, NULL, (uint32_t)numaffected, false);
		fr_dcursor_append(out, vb);

		(inst->driver->sql_finish_query)(handle, &inst->config);

		goto finish;
	} /* else it's a SELECT statement */

	rcode = rlm_sql_select_query(inst, request, &handle, arg->vb_strvalue);
	if (rcode != RLM_SQL_OK) goto query_error;

	do {
		rcode = rlm_sql_fetch_row(&row, inst, request, &handle);
		switch (rcode) {
		case RLM_SQL_OK:
			if (row[0]) break;

			RDEBUG2("NULL value in first column of result");
			ret = XLAT_ACTION_FAIL;

			goto finish_query;

		case RLM_SQL_NO_MORE_ROWS:
			if (!fetched) {
				RDEBUG2("SQL query returned no results");
				ret = XLAT_ACTION_FAIL;
			}

			goto finish_query;

		default:
			(inst->driver->sql_finish_select_query)(handle, &inst->config);
			goto query_error;
		}

		fetched = true;

		MEM(vb = fr_value_box_alloc_null(ctx));
		fr_value_box_strdup(vb, vb, NULL, row[0], false);
		fr_dcursor_append(out, vb);

	} while (1);

finish_query:
	(inst->driver->sql_finish_select_query)(handle, &inst->config);

finish:
	fr_pool_connection_release(inst->pool, request, handle);

	return ret;
}

/** Converts a string value into a #fr_pair_t
 *
 * @param[in,out] ctx to allocate #fr_pair_t (s).
 * @param[out] out where to write the resulting #fr_pair_t.
 * @param[in] request The current request.
 * @param[in] map to process.
 * @param[in] uctx The value to parse.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int _sql_map_proc_get_value(TALLOC_CTX *ctx, fr_pair_list_t *out,
				   request_t *request, map_t const *map, void *uctx)
{
	fr_pair_t	*vp;
	char const	*value = uctx;

	vp = fr_pair_afrom_da(ctx, tmpl_da(map->lhs));
	if (!vp) return -1;

	/*
	 *	Buffer not always talloced, sometimes it's
	 *	just a pointer to a field in a result struct.
	 */
	if (fr_pair_value_from_str(vp, value, strlen(value), NULL, true) < 0) {
		RPEDEBUG("Failed parsing value \"%pV\" for attribute %s",
			 fr_box_strvalue_buffer(value), tmpl_da(map->lhs)->name);
		talloc_free(vp);

		return -1;
	}

	vp->op = map->op;
	fr_pair_append(out, vp);

	return 0;
}

/*
 *	Verify the result of the map.
 */
static int sql_map_verify(CONF_SECTION *cs, UNUSED void *mod_inst, UNUSED void *proc_inst,
			  tmpl_t const *src, UNUSED map_list_t const *maps)
{
	if (!src) {
		cf_log_err(cs, "Missing SQL query");

		return -1;
	}

	return 0;
}

/** Executes a SELECT query and maps the result to server attributes
 *
 * @param mod_inst #rlm_sql_t instance.
 * @param proc_inst Instance data for this specific mod_proc call (unused).
 * @param request The current request.
 * @param query string to execute.
 * @param maps Head of the map list.
 * @return
 *	- #RLM_MODULE_NOOP no rows were returned or columns matched.
 *	- #RLM_MODULE_UPDATED if one or more #fr_pair_t were added to the #request_t.
 *	- #RLM_MODULE_FAIL if a fault occurred.
 */
static rlm_rcode_t mod_map_proc(void *mod_inst, UNUSED void *proc_inst, request_t *request,
				fr_value_box_list_t *query, map_list_t const *maps)
{
	rlm_sql_t		*inst = talloc_get_type_abort(mod_inst, rlm_sql_t);
	rlm_sql_handle_t	*handle = NULL;

	int			i, j;

	rlm_rcode_t		rcode = RLM_MODULE_UPDATED;
	sql_rcode_t		ret;

	map_t const		*map;

	rlm_sql_row_t		row;

	int			rows = 0;
	int			field_cnt;
	char const		**fields = NULL, *map_rhs;
	char			map_rhs_buff[128];

	char const		*query_str = NULL;
	fr_value_box_t		*query_head = fr_dlist_head(query);

#define MAX_SQL_FIELD_INDEX (64)

	int			field_index[MAX_SQL_FIELD_INDEX];
	bool			found_field = false;	/* Did we find any matching fields in the result set ? */

	fr_assert(inst->driver->sql_fields);		/* Should have been caught during validation... */

	if (!query_head) {
		REDEBUG("Query cannot be (null)");
		return RLM_MODULE_FAIL;
	}

	if (fr_value_box_list_concat_in_place(request,
					      query_head, query, FR_TYPE_STRING,
					      FR_VALUE_BOX_LIST_FREE, true,
					      SIZE_MAX) < 0) {
		RPEDEBUG("Failed concatenating input string");
		return RLM_MODULE_FAIL;
	}
	query_str = query_head->vb_strvalue;

	for (i = 0; i < MAX_SQL_FIELD_INDEX; i++) field_index[i] = -1;

	/*
	 *	Add SQL-User-Name attribute just in case it is needed
	 *	We could search the string fmt for SQL-User-Name to see if this is
	 * 	needed or not
	 */
	sql_set_user(inst, request, NULL);

	handle = fr_pool_connection_get(inst->pool, request);		/* connection pool should produce error */
	if (!handle) {
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	rlm_sql_query_log(inst, request, NULL, query_str);

	ret = rlm_sql_select_query(inst, request, &handle, query_str);
	if (ret != RLM_SQL_OK) {
		RERROR("SQL query failed: %s", fr_table_str_by_value(sql_rcode_description_table, ret, "<INVALID>"));
		rcode = RLM_MODULE_FAIL;
		goto finish;
	}

	/*
	 *	Not every driver provides an sql_num_rows function
	 */
	if (inst->driver->sql_num_rows) {
		ret = inst->driver->sql_num_rows(handle, &inst->config);
		if (ret == 0) {
			RDEBUG2("Server returned an empty result");
			rcode = RLM_MODULE_NOOP;
			(inst->driver->sql_finish_select_query)(handle, &inst->config);
			goto finish;
		}

		if (ret < 0) {
			RERROR("Failed retrieving row count");
		error:
			rcode = RLM_MODULE_FAIL;
			(inst->driver->sql_finish_select_query)(handle, &inst->config);
			goto finish;
		}
	}

	/*
	 *	Map proc only registered if driver provides an sql_fields function
	 */
	ret = (inst->driver->sql_fields)(&fields, handle, &inst->config);
	if (ret != RLM_SQL_OK) {
		RERROR("Failed retrieving field names: %s", fr_table_str_by_value(sql_rcode_description_table, ret, "<INVALID>"));
		goto error;
	}
	fr_assert(fields);
	field_cnt = talloc_array_length(fields);

	if (RDEBUG_ENABLED3) for (j = 0; j < field_cnt; j++) RDEBUG3("Got field: %s", fields[j]);

	/*
	 *	Iterate over the maps, it's O(N2)ish but probably
	 *	faster than building a radix tree each time the
	 *	map set is evaluated (map->rhs can be dynamic).
	 */
	for (map = map_list_head(maps), i = 0;
	     map && (i < MAX_SQL_FIELD_INDEX);
	     map = map_list_next(maps, map), i++) {
		/*
		 *	Expand the RHS to get the name of the SQL field
		 */
		if (tmpl_expand(&map_rhs, map_rhs_buff, sizeof(map_rhs_buff),
				request, map->rhs, NULL, NULL) < 0) {
			RPERROR("Failed getting field name");
			goto error;
		}

		for (j = 0; j < field_cnt; j++) {
			if (strcmp(fields[j], map_rhs) != 0) continue;
			field_index[i] = j;
			found_field = true;
		}
	}

	/*
	 *	Couldn't resolve any map RHS values to fields
	 *	in the result set.
	 */
	if (!found_field) {
		RDEBUG2("No fields matching map found in query result");
		rcode = RLM_MODULE_NOOP;
		(inst->driver->sql_finish_select_query)(handle, &inst->config);
		goto finish;
	}

	/*
	 *	We've resolved all the maps to result indexes, now convert
	 *	the values at those indexes into fr_pair_ts.
	 *
	 *	Note: Not all SQL client libraries provide a row count,
	 *	so we have to do the count here.
	 */
	while (((ret = rlm_sql_fetch_row(&row, inst, request, &handle)) == RLM_SQL_OK)) {
		rows++;
		for (map = map_list_head(maps), j = 0;
		     map && (j < MAX_SQL_FIELD_INDEX);
		     map = map_list_next(maps, map), j++) {
			if (field_index[j] < 0) continue;	/* We didn't find the map RHS in the field set */
			if (map_to_request(request, map, _sql_map_proc_get_value, row[field_index[j]]) < 0) goto error;
		}
	}

	if (ret == RLM_SQL_ERROR) goto error;

	if (rows == 0) {
		RDEBUG2("SQL query returned no results");
		rcode = RLM_MODULE_NOOP;
	}

	(inst->driver->sql_finish_select_query)(handle, &inst->config);

finish:
	talloc_free(fields);
	fr_pool_connection_release(inst->pool, request, handle);

	return rcode;
}


/** xlat escape function for drivers which do not provide their own
 *
 */
static size_t sql_escape_func(UNUSED request_t *request, char *out, size_t outlen, char const *in, void *arg)
{
	rlm_sql_handle_t	*handle = arg;
	rlm_sql_t const		*inst = talloc_get_type_abort_const(handle->inst, rlm_sql_t);
	size_t			len = 0;

	while (in[0]) {
		size_t utf8_len;

		/*
		 *	Allow all multi-byte UTF8 characters.
		 */
		utf8_len = fr_utf8_char((uint8_t const *) in, -1);
		if (utf8_len > 1) {
			if (outlen <= utf8_len) break;

			memcpy(out, in, utf8_len);
			in += utf8_len;
			out += utf8_len;

			outlen -= utf8_len;
			len += utf8_len;
			continue;
		}

		/*
		 *	Because we register our own escape function
		 *	we're now responsible for escaping all special
		 *	chars in an xlat expansion or attribute value.
		 */
		switch (in[0]) {
		case '\n':
			if (outlen <= 2) break;
			out[0] = '\\';
			out[1] = 'n';

			in++;
			out += 2;
			outlen -= 2;
			len += 2;
			break;

		case '\r':
			if (outlen <= 2) break;
			out[0] = '\\';
			out[1] = 'r';

			in++;
			out += 2;
			outlen -= 2;
			len += 2;
			break;

		case '\t':
			if (outlen <= 2) break;
			out[0] = '\\';
			out[1] = 't';

			in++;
			out += 2;
			outlen -= 2;
			len += 2;
			break;
		}

		/*
		 *	Non-printable characters get replaced with their
		 *	mime-encoded equivalents.
		 */
		if ((in[0] < 32) ||
		    strchr(inst->config.allowed_chars, *in) == NULL) {
			/*
			 *	Only 3 or less bytes available.
			 */
			if (outlen <= 3) {
				break;
			}

			snprintf(out, outlen, "=%02X", (unsigned char) in[0]);
			in++;
			out += 3;
			outlen -= 3;
			len += 3;
			continue;
		}

		/*
		 *	Only one byte left.
		 */
		if (outlen <= 1) {
			break;
		}

		/*
		 *	Allowed character.
		 */
		*out = *in;
		out++;
		in++;
		outlen--;
		len++;
	}
	*out = '\0';
	return len;
}

/*
 *	Set the SQL user name.
 *
 *	We don't call the escape function here. The resulting string
 *	will be escaped later in the queries xlat so we don't need to
 *	escape it twice. (it will make things wrong if we have an
 *	escape candidate character in the username)
 */
int sql_set_user(rlm_sql_t const *inst, request_t *request, char const *username)
{
	char *expanded = NULL;
	fr_pair_t *vp = NULL;
	char const *sqluser;
	ssize_t len;

	fr_assert(request->packet != NULL);

	if (username != NULL) {
		sqluser = username;
	} else if (inst->config.query_user[0] != '\0') {
		sqluser = inst->config.query_user;
	} else {
		return 0;
	}

	MEM(pair_update_request(&vp, inst->sql_user) >= 0);
	len = xlat_aeval(vp, &expanded, request, sqluser, NULL, NULL);
	if (len < 0) {
		pair_delete_request(vp);
		return -1;
	}

	/*
	 *	Replace any existing SQL-User-Name with outs
	 */
	fr_pair_value_bstrdup_buffer_shallow(vp, expanded, true);
	MEM(fr_pair_value_bstr_realloc(vp, NULL, len) == 0);
	RDEBUG2("SQL-User-Name set to '%pV'", &vp->data);

	return 0;
}

/*
 *	Do a set/unset user, so it's a bit clearer what's going on.
 */
#define sql_unset_user(_i, _r) fr_pair_delete_by_da(&_r->request_pairs, _i->sql_user)

static int sql_get_grouplist(rlm_sql_t const *inst, rlm_sql_handle_t **handle, request_t *request,
			     rlm_sql_grouplist_t **phead)
{
	char			*expanded = NULL;
	int     		num_groups = 0;
	rlm_sql_row_t		row;
	rlm_sql_grouplist_t	*entry;
	int			ret;

	/* NOTE: sql_set_user should have been run before calling this function */

	entry = *phead = NULL;

	if (!inst->config.groupmemb_query || !*inst->config.groupmemb_query) return 0;
	if (xlat_aeval(request, &expanded, request, inst->config.groupmemb_query,
			 inst->sql_escape_func, *handle) < 0) return -1;

	ret = rlm_sql_select_query(inst, request, handle, expanded);
	talloc_free(expanded);
	if (ret != RLM_SQL_OK) return -1;

	while (rlm_sql_fetch_row(&row, inst, request, handle) == RLM_SQL_OK) {
		if (!row[0]){
			RDEBUG2("row[0] returned NULL");
			(inst->driver->sql_finish_select_query)(*handle, &inst->config);
			talloc_free(entry);
			return -1;
		}

		if (!*phead) {
			*phead = talloc_zero(*handle, rlm_sql_grouplist_t);
			entry = *phead;
		} else {
			entry->next = talloc_zero(*phead, rlm_sql_grouplist_t);
			entry = entry->next;
		}
		entry->next = NULL;
		entry->name = talloc_typed_strdup(entry, row[0]);

		num_groups++;
	}

	(inst->driver->sql_finish_select_query)(*handle, &inst->config);

	return num_groups;
}


/*
 * sql groupcmp function. That way we can do group comparisons (in the users file for example)
 * with the group memberships reciding in sql
 * The group membership query should only return one element which is the username. The returned
 * username will then be checked with the passed check string.
 */
static int sql_groupcmp(void *instance, request_t *request, UNUSED fr_pair_list_t *request_list,
			fr_pair_t const *check) CC_HINT(nonnull (1, 2, 4));

static int sql_groupcmp(void *instance, request_t *request, UNUSED fr_pair_list_t *request_list,
			fr_pair_t const *check)
{
	rlm_sql_handle_t	*handle;
	rlm_sql_t const		*inst = talloc_get_type_abort_const(instance, rlm_sql_t);
	rlm_sql_grouplist_t	*head, *entry;

	/*
	 *	No group queries, don't do group comparisons.
	 */
	if (!inst->config.groupmemb_query) {
		RWARN("Cannot do group comparison when group_membership_query is not set");
		return 1;
	}

	RDEBUG2("sql_groupcmp");

	if (check->vp_length == 0){
		RDEBUG2("sql_groupcmp: Illegal group name");
		return 1;
	}

	/*
	 *	Set, escape, and check the user attr here
	 */
	if (sql_set_user(inst, request, NULL) < 0)
		return 1;

	/*
	 *	Get a socket for this lookup
	 */
	handle = fr_pool_connection_get(inst->pool, request);
	if (!handle) {
		return 1;
	}

	/*
	 *	Get the list of groups this user is a member of
	 */
	if (sql_get_grouplist(inst, &handle, request, &head) < 0) {
		REDEBUG("Error getting group membership");
		fr_pool_connection_release(inst->pool, request, handle);
		return 1;
	}

	for (entry = head; entry != NULL; entry = entry->next) {
		if (strcmp(entry->name, check->vp_strvalue) == 0){
			RDEBUG2("sql_groupcmp finished: User is a member of group %s",
			       check->vp_strvalue);
			talloc_free(head);
			fr_pool_connection_release(inst->pool, request, handle);
			return 0;
		}
	}

	/* Free the grouplist */
	talloc_free(head);
	fr_pool_connection_release(inst->pool, request, handle);

	RDEBUG2("sql_groupcmp finished: User is NOT a member of group %pV", &check->data);

	return 1;
}

static unlang_action_t rlm_sql_process_groups(rlm_rcode_t *p_result,
					      rlm_sql_t const *inst, request_t *request, rlm_sql_handle_t **handle,
					      sql_fall_through_t *do_fall_through)
{
	rlm_rcode_t		rcode = RLM_MODULE_NOOP;
	fr_pair_list_t		check_tmp, reply_tmp;
	fr_pair_t		*sql_group = NULL;
	rlm_sql_grouplist_t	*head = NULL, *entry = NULL;

	char			*expanded = NULL;
	int			rows;

	fr_assert(request->packet != NULL);
	fr_pair_list_init(&check_tmp);
	fr_pair_list_init(&reply_tmp);

	if (!inst->config.groupmemb_query) {
		RWARN("Cannot do check groups when group_membership_query is not set");

	do_nothing:
		*do_fall_through = FALL_THROUGH_DEFAULT;

		/*
		 *	Didn't add group attributes or allocate
		 *	memory, so don't do anything else.
		 */
		RETURN_MODULE_NOTFOUND;
	}

	/*
	 *	Get the list of groups this user is a member of
	 */
	rows = sql_get_grouplist(inst, handle, request, &head);
	if (rows < 0) {
		REDEBUG("Error retrieving group list");

		RETURN_MODULE_FAIL;
	}
	if (rows == 0) {
		RDEBUG2("User not found in any groups");
		goto do_nothing;
	}
	fr_assert(head);

	RDEBUG2("User found in the group table");

	/*
	 *	Add the Sql-Group attribute to the request list so we know
	 *	which group we're retrieving attributes for
	 */
	MEM(pair_update_request(&sql_group, inst->group_da) >= 0);

	entry = head;
	do {
	next:
		fr_assert(entry != NULL);
		fr_pair_value_strdup(sql_group, entry->name, true);

		if (inst->config.authorize_group_check_query) {
			fr_pair_t	*vp;

			/*
			 *	Expand the group query
			 */
			if (xlat_aeval(request, &expanded, request, inst->config.authorize_group_check_query,
					 inst->sql_escape_func, *handle) < 0) {
				REDEBUG("Error generating query");
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}

			rows = sql_getvpdata(request->control_ctx, inst, request, handle, &check_tmp, expanded);
			TALLOC_FREE(expanded);
			if (rows < 0) {
				REDEBUG("Error retrieving check pairs for group %s", entry->name);
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}

			/*
			 *	If we got check rows we need to process them before we decide to
			 *	process the reply rows
			 */
			if ((rows > 0) &&
			    (paircmp(request, &request->request_pairs, &check_tmp) != 0)) {
				fr_pair_list_free(&check_tmp);
				entry = entry->next;

				if (!entry) break;

				goto next;	/* != continue */
			}

			RDEBUG2("Group \"%s\": Conditional check items matched", entry->name);
			rcode = RLM_MODULE_OK;

			RDEBUG2("Group \"%s\": Merging assignment check items", entry->name);
			RINDENT();
			for (vp = fr_pair_list_head(&check_tmp);
			     vp;
			     vp = fr_pair_list_next(&check_tmp, vp)) {
			 	if (!fr_assignment_op[vp->op]) continue;

				rcode = RLM_MODULE_UPDATED;
			 	RDEBUG2("&%pP", vp);
			}
			REXDENT();
			radius_pairmove(request, &request->control_pairs, &check_tmp);

			fr_pair_list_free(&check_tmp);
		}

		if (inst->config.authorize_group_reply_query) {

			/*
			 *	Now get the reply pairs since the paircmp matched
			 */
			if (xlat_aeval(request, &expanded, request, inst->config.authorize_group_reply_query,
					 inst->sql_escape_func, *handle) < 0) {
				REDEBUG("Error generating query");
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}

			rows = sql_getvpdata(request->reply_ctx, inst, request, handle, &reply_tmp, expanded);
			TALLOC_FREE(expanded);
			if (rows < 0) {
				REDEBUG("Error retrieving reply pairs for group %s", entry->name);
				rcode = RLM_MODULE_FAIL;
				goto finish;
			}

			if (rows == 0) {
				*do_fall_through = FALL_THROUGH_DEFAULT;
				continue;
			}

			fr_assert(!fr_pair_list_empty(&reply_tmp)); /* coverity, among others */
			*do_fall_through = fall_through(&reply_tmp);

			RDEBUG2("Group \"%s\": Merging reply items", entry->name);
			if (rcode == RLM_MODULE_NOOP) rcode = RLM_MODULE_UPDATED;

			log_request_pair_list(L_DBG_LVL_2, request, NULL, &reply_tmp, NULL);

			radius_pairmove(request, &request->reply_pairs, &reply_tmp);
			fr_pair_list_free(&reply_tmp);
		/*
		 *	If there's no reply query configured, then we assume
		 *	FALL_THROUGH_NO, which is the same as the users file if you
		 *	had no reply attributes.
		 */
		} else {
			*do_fall_through = FALL_THROUGH_DEFAULT;
		}

		entry = entry->next;
	} while (entry != NULL && (*do_fall_through == FALL_THROUGH_YES));

finish:
	talloc_free(head);
	pair_delete_request(inst->group_da);

	RETURN_MODULE_RCODE(rcode);
}

static int mod_detach(module_detach_ctx_t const *mctx)
{
	rlm_sql_t	*inst = talloc_get_type_abort(mctx->inst->data, rlm_sql_t);

	if (inst->pool) fr_pool_free(inst->pool);

	/*
	 *	We need to explicitly free all children, so if the driver
	 *	parented any memory off the instance, their destructors
	 *	run before we unload the bytecode for them.
	 *
	 *	If we don't do this, we get a SEGV deep inside the talloc code
	 *	when it tries to call a destructor that no longer exists.
	 */
	talloc_free_children(inst);

	return 0;
}

static int mod_bootstrap(module_inst_ctx_t const *mctx)
{
	rlm_sql_t		*inst = talloc_get_type_abort(mctx->inst->data, rlm_sql_t);
	CONF_SECTION		*conf = mctx->inst->conf;
	xlat_t			*xlat;
	xlat_arg_parser_t	*sql_xlat_arg;

	inst->name = mctx->inst->name;	/* Need this for functions in sql.c */

	/*
	 *	Register the group comparison attribute
	 */
	if (inst->config.groupmemb_query) {
		char buffer[256];

		char const *group_attribute;

		if (inst->config.group_attribute) {
			group_attribute = inst->config.group_attribute;
		} else if (cf_section_name2(conf)) {
			snprintf(buffer, sizeof(buffer), "%s-SQL-Group", mctx->inst->name);
			group_attribute = buffer;
		} else {
			group_attribute = "SQL-Group";
		}

		/*
		 *	Checks if attribute already exists.
		 */
		if (paircmp_register_by_name(group_attribute, attr_user_name,
						false, sql_groupcmp, inst) < 0) {
			PERROR("Failed registering group comparison");
		error:
			return -1;
		}

		inst->group_da = fr_dict_attr_search_by_qualified_oid(NULL, dict_freeradius, group_attribute,
								     false, false);
		if (!inst->group_da) {
			PERROR("Failed resolving group attribute");
			goto error;
		}
	}

	/*
	 *	Register the SQL xlat function
	 */
	xlat = xlat_register_module(inst, mctx, mctx->inst->name, sql_xlat, NULL);

	/*
	 *	The xlat escape function needs access to inst - so
	 *	argument parser details need to be defined here
	 */
	sql_xlat_arg = talloc_zero(inst, xlat_arg_parser_t);
	sql_xlat_arg->type = FR_TYPE_STRING;
	sql_xlat_arg->required = true;
	sql_xlat_arg->concat = true;
	sql_xlat_arg->func = sql_xlat_escape;
	sql_xlat_arg->uctx = inst;
	xlat_func_mono(xlat, sql_xlat_arg);

	/*
	 *	Register the SQL map processor function
	 */
	if (inst->driver->sql_fields) map_proc_register(inst, mctx->inst->name, mod_map_proc, sql_map_verify, 0);

	return 0;
}


static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	rlm_sql_t	*inst = talloc_get_type_abort(mctx->inst->data, rlm_sql_t);
	CONF_SECTION	*conf = mctx->inst->conf;

	/*
	 *	We need authorize_group_check_query or authorize_group_reply_query
	 *	if group_membership_query is set.
	 *
	 *	Or we need group_membership_query if authorize_group_check_query or
	 *	authorize_group_reply_query is set.
	 */
	if (!inst->config.groupmemb_query) {
		if (inst->config.authorize_group_check_query) {
			WARN("Ignoring authorize_group_reply_query as group_membership_query is not configured");
		}

		if (inst->config.authorize_group_reply_query) {
			WARN("Ignoring authorize_group_check_query as group_membership_query is not configured");
		}

		if (!inst->config.read_groups) {
			WARN("Ignoring read_groups as group_membership_query is not configured");
			inst->config.read_groups = false;
		}
	} /* allow the group check / reply queries to be NULL */

	/*
	 *	This will always exist, as cf_section_parse_init()
	 *	will create it if it doesn't exist.  However, the
	 *	"reference" config item won't exist in an auto-created
	 *	configuration.  So if that doesn't exist, we ignore
	 *	the whole subsection.
	 */
	inst->config.accounting.cs = cf_section_find(conf, "accounting", NULL);
	inst->config.accounting.reference_cp = (cf_pair_find(inst->config.accounting.cs, "reference") != NULL);

	inst->config.postauth.cs = cf_section_find(conf, "post-auth", NULL);
	inst->config.postauth.reference_cp = (cf_pair_find(inst->config.postauth.cs, "reference") != NULL);

	/*
	 *	Cache the SQL-User-Name fr_dict_attr_t, so we can be slightly
	 *	more efficient about creating SQL-User-Name attributes.
	 */
	inst->sql_user = attr_sql_user_name;

	/*
	 *	Export these methods, too.  This avoids RTDL_GLOBAL.
	 */
	inst->sql_set_user		= sql_set_user;
	inst->sql_query			= rlm_sql_query;
	inst->sql_select_query		= rlm_sql_select_query;
	inst->sql_fetch_row		= rlm_sql_fetch_row;

	/*
	 *	Either use the module specific escape function
	 *	or our default one.
	 */
	inst->sql_escape_func = inst->driver->sql_escape_func ?
				inst->driver->sql_escape_func :
				sql_escape_func;

	inst->ef = module_rlm_exfile_init(inst, conf, 256, fr_time_delta_from_sec(30), true, NULL, NULL);
	if (!inst->ef) {
		cf_log_err(conf, "Failed creating log file context");
		return -1;
	}

	/*
	 *	Initialise the connection pool for this instance
	 */
	INFO("Attempting to connect to database \"%s\"", inst->config.sql_db);

	inst->pool = module_rlm_connection_pool_init(conf, inst, sql_mod_conn_create, NULL, NULL, NULL, NULL);
	if (!inst->pool) return -1;

	return 0;
}

static unlang_action_t CC_HINT(nonnull) mod_authorize(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_rcode_t		rcode = RLM_MODULE_NOOP;

	rlm_sql_t const		*inst = talloc_get_type_abort_const(mctx->inst->data, rlm_sql_t);
	rlm_sql_handle_t	*handle;

	fr_pair_list_t		check_tmp;
	fr_pair_list_t		reply_tmp;
	fr_pair_t 		*user_profile = NULL;

	bool			user_found = false;

	sql_fall_through_t	do_fall_through = FALL_THROUGH_DEFAULT;

	int			rows;

	char			*expanded = NULL;

	fr_pair_list_init(&check_tmp);
	fr_pair_list_init(&reply_tmp);
	fr_assert(request->packet != NULL);
	fr_assert(request->reply != NULL);

	if (!inst->config.authorize_check_query && !inst->config.authorize_reply_query &&
	    !inst->config.read_groups && !inst->config.read_profiles) {
		RWDEBUG("No authorization checks configured, returning noop");

		RETURN_MODULE_NOOP;
	}

	/*
	 *	Set, escape, and check the user attr here
	 */
	if (sql_set_user(inst, request, NULL) < 0) RETURN_MODULE_FAIL;

	/*
	 *	Reserve a socket
	 *
	 *	After this point use goto error or goto release to cleanup socket temporary pairlists and
	 *	temporary attributes.
	 */
	handle = fr_pool_connection_get(inst->pool, request);
	if (!handle) {
		sql_unset_user(inst, request);
		RETURN_MODULE_FAIL;
	}

	/*
	 *	Query the check table to find any conditions associated with this user/realm/whatever...
	 */
	if (inst->config.authorize_check_query) {
		fr_pair_t	*vp;

		if (xlat_aeval(request, &expanded, request, inst->config.authorize_check_query,
				 inst->sql_escape_func, handle) < 0) {
			REDEBUG("Failed generating query");
			rcode = RLM_MODULE_FAIL;

		error:
			fr_pair_list_free(&check_tmp);
			fr_pair_list_free(&reply_tmp);
			sql_unset_user(inst, request);

			fr_pool_connection_release(inst->pool, request, handle);

			RETURN_MODULE_RCODE(rcode);
		}

		rows = sql_getvpdata(request->control_ctx, inst, request, &handle, &check_tmp, expanded);
		TALLOC_FREE(expanded);
		if (rows < 0) {
			REDEBUG("Failed getting check attributes");
			rcode = RLM_MODULE_FAIL;
			goto error;
		}

		if (rows == 0) goto skip_reply;	/* Don't need to free VPs we don't have */

		/*
		 *	Only do this if *some* check pairs were returned
		 */
		RDEBUG2("User found in radcheck table");
		user_found = true;
		if (paircmp(request, &request->request_pairs, &check_tmp) != 0) {
			fr_pair_list_free(&check_tmp);
			goto skip_reply;
		}

		RDEBUG2("Conditional check items matched, merging assignment check items");
		RINDENT();
		for (vp = fr_pair_list_head(&check_tmp);
		     vp;
		     vp = fr_pair_list_next(&check_tmp, vp)) {
			if (!fr_assignment_op[vp->op]) continue;
			RDEBUG2("&%pP", vp);
		}
		REXDENT();
		radius_pairmove(request, &request->control_pairs, &check_tmp);

		rcode = RLM_MODULE_OK;
		fr_pair_list_free(&check_tmp);
	}

	if (inst->config.authorize_reply_query) {

		/*
		 *	Now get the reply pairs since the paircmp matched
		 */
		if (xlat_aeval(request, &expanded, request, inst->config.authorize_reply_query,
				 inst->sql_escape_func, handle) < 0) {
			REDEBUG("Error generating query");
			rcode = RLM_MODULE_FAIL;
			goto error;
		}

		rows = sql_getvpdata(request->reply_ctx, inst, request, &handle, &reply_tmp, expanded);
		TALLOC_FREE(expanded);
		if (rows < 0) {
			REDEBUG("SQL query error getting reply attributes");
			rcode = RLM_MODULE_FAIL;
			goto error;
		}

		if (rows == 0) goto skip_reply;

		do_fall_through = fall_through(&reply_tmp);

		RDEBUG2("User found in radreply table, merging reply items");
		user_found = true;

		log_request_pair_list(L_DBG_LVL_2, request, NULL, &reply_tmp, NULL);

		radius_pairmove(request, &request->reply_pairs, &reply_tmp);

		rcode = RLM_MODULE_OK;
		fr_pair_list_free(&reply_tmp);
	}

	/*
	 *	Neither group checks or profiles will work without
	 *	a group membership query.
	 */
	if (!inst->config.groupmemb_query) goto release;

skip_reply:
	if ((do_fall_through == FALL_THROUGH_YES) ||
	    (inst->config.read_groups && (do_fall_through == FALL_THROUGH_DEFAULT))) {
		rlm_rcode_t ret;

		RDEBUG3("... falling-through to group processing");
		rlm_sql_process_groups(&ret, inst, request, &handle, &do_fall_through);
		switch (ret) {

		/*
		 *	Nothing bad happened, continue...
		 */
		case RLM_MODULE_UPDATED:
			rcode = RLM_MODULE_UPDATED;
			FALL_THROUGH;

		case RLM_MODULE_OK:
			if (rcode != RLM_MODULE_UPDATED) rcode = RLM_MODULE_OK;
			FALL_THROUGH;

		case RLM_MODULE_NOOP:
			user_found = true;
			break;

		case RLM_MODULE_NOTFOUND:
			break;

		default:
			rcode = ret;
			goto release;
		}
	}

	/*
	 *	Repeat the above process with the default profile or User-Profile
	 */
	if ((do_fall_through == FALL_THROUGH_YES) ||
	    (inst->config.read_profiles && (do_fall_through == FALL_THROUGH_DEFAULT))) {
		rlm_rcode_t	ret;
		char const	*profile;

		/*
		 *  Check for a default_profile or for a User-Profile.
		 */
		RDEBUG3("... falling-through to profile processing");
		user_profile = fr_pair_find_by_da_idx(&request->control_pairs, attr_user_profile, 0);

		profile = user_profile ?
				      user_profile->vp_strvalue :
				      inst->config.default_profile;

		if (!profile || !*profile) goto release;

		RDEBUG2("Checking profile %s", profile);

		if (sql_set_user(inst, request, profile) < 0) {
			REDEBUG("Error setting profile");
			rcode = RLM_MODULE_FAIL;
			goto error;
		}

		rlm_sql_process_groups(&ret, inst, request, &handle, &do_fall_through);
		switch (ret) {
		/*
		 *	Nothing bad happened, continue...
		 */
		case RLM_MODULE_UPDATED:
			rcode = RLM_MODULE_UPDATED;
			FALL_THROUGH;

		case RLM_MODULE_OK:
			if (rcode != RLM_MODULE_UPDATED) rcode = RLM_MODULE_OK;
			FALL_THROUGH;

		case RLM_MODULE_NOOP:
			user_found = true;
			break;

		case RLM_MODULE_NOTFOUND:
			break;

		default:
			rcode = ret;
			goto release;
		}
	}

	/*
	 *	At this point the key (user) hasn't be found in the check table, the reply table
	 *	or the group mapping table, and there was no matching profile.
	 */
release:
	if (!user_found) rcode = RLM_MODULE_NOTFOUND;

	fr_pool_connection_release(inst->pool, request, handle);
	sql_unset_user(inst, request);

	RETURN_MODULE_RCODE(rcode);
}

/*
 *	Generic function for failing between a bunch of queries.
 *
 *	Uses the same principle as rlm_linelog, expanding the 'reference' config
 *	item using xlat to figure out what query it should execute.
 *
 *	If the reference matches multiple config items, and a query fails or
 *	doesn't update any rows, the next matching config item is used.
 *
 */
static unlang_action_t acct_redundant(rlm_rcode_t *p_result, rlm_sql_t const *inst, request_t *request, sql_acct_section_t const *section)
{
	rlm_rcode_t		rcode = RLM_MODULE_OK;

	rlm_sql_handle_t	*handle = NULL;
	int			sql_ret;
	int			numaffected = 0;

	CONF_ITEM		*item;
	CONF_PAIR 		*pair;
	char const		*attr = NULL;
	char const		*value;

	char			path[FR_MAX_STRING_LEN];
	char			*p = path;
	char			*expanded = NULL;

	fr_assert(section);

	if (section->reference[0] != '.') *p++ = '.';

	if (xlat_eval(p, sizeof(path) - (p - path), request, section->reference, NULL, NULL) < 0) {
		rcode = RLM_MODULE_FAIL;

		goto finish;
	}

	/*
	 *	If we can't find a matching config item we do
	 *	nothing so return RLM_MODULE_NOOP.
	 */
	item = cf_reference_item(NULL, section->cs, path);
	if (!item) {
		RWDEBUG("No such configuration item %s", path);
		rcode = RLM_MODULE_NOOP;

		goto finish;
	}
	if (cf_item_is_section(item)){
		RWDEBUG("Sections are not supported as references");
		rcode = RLM_MODULE_NOOP;

		goto finish;
	}

	pair = cf_item_to_pair(item);
	attr = cf_pair_attr(pair);

	RDEBUG2("Using query template '%s'", attr);

	handle = fr_pool_connection_get(inst->pool, request);
	if (!handle) {
		rcode = RLM_MODULE_FAIL;

		goto finish;
	}

	sql_set_user(inst, request, NULL);

	while (true) {
		value = cf_pair_value(pair);
		if (!value) {
			RDEBUG2("Ignoring null query");
			rcode = RLM_MODULE_NOOP;

			goto finish;
		}

		if (xlat_aeval(request, &expanded, request, value, inst->sql_escape_func, handle) < 0) {
			rcode = RLM_MODULE_FAIL;

			goto finish;
		}

		if (!*expanded) {
			RDEBUG2("Ignoring null query");
			rcode = RLM_MODULE_NOOP;

			goto finish;
		}

		rlm_sql_query_log(inst, request, section, expanded);

		sql_ret = rlm_sql_query(inst, request, &handle, expanded);
		TALLOC_FREE(expanded);
		RDEBUG2("SQL query returned: %s", fr_table_str_by_value(sql_rcode_description_table, sql_ret, "<INVALID>"));

		switch (sql_ret) {
		/*
		 *  Query was a success! Now we just need to check if it did anything.
		 */
		case RLM_SQL_OK:
			break;

		/*
		 *  A general, unrecoverable server fault.
		 */
		case RLM_SQL_ERROR:
		/*
		 *  If we get RLM_SQL_RECONNECT it means all connections in the pool
		 *  were exhausted, and we couldn't create a new connection,
		 *  so we do not need to call fr_pool_connection_release.
		 */
		case RLM_SQL_RECONNECT:
			rcode = RLM_MODULE_FAIL;
			goto finish;

		/*
		 *  Query was invalid, this is a terminal error, but we still need
		 *  to do cleanup, as the connection handle is still valid.
		 */
		case RLM_SQL_QUERY_INVALID:
			rcode = RLM_MODULE_INVALID;
			goto finish;

		/*
		 *  Driver found an error (like a unique key constraint violation)
		 *  that hinted it might be a good idea to try an alternative query.
		 */
		case RLM_SQL_ALT_QUERY:
			goto next;
		}
		fr_assert(handle);

		/*
		 *  We need to have updated something for the query to have been
		 *  counted as successful.
		 */
		numaffected = (inst->driver->sql_affected_rows)(handle, &inst->config);
		(inst->driver->sql_finish_query)(handle, &inst->config);
		RDEBUG2("%i record(s) updated", numaffected);

		if (numaffected > 0) break;	/* A query succeeded, were done! */
	next:
		/*
		 *  We assume all entries with the same name form a redundant
		 *  set of queries.
		 */
		pair = cf_pair_find_next(section->cs, pair, attr);

		if (!pair) {
			RDEBUG2("No additional queries configured");
			rcode = RLM_MODULE_NOOP;

			goto finish;
		}

		RDEBUG2("Trying next query...");
	}

finish:
	talloc_free(expanded);
	fr_pool_connection_release(inst->pool, request, handle);
	sql_unset_user(inst, request);

	RETURN_MODULE_RCODE(rcode);
}

/*
 *	Accounting: Insert or update session data in our sql table
 */
static unlang_action_t CC_HINT(nonnull) mod_accounting(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_sql_t const *inst = talloc_get_type_abort_const(mctx->inst->data, rlm_sql_t);

	if (inst->config.accounting.reference_cp) {
		return acct_redundant(p_result, inst, request, &inst->config.accounting);
	}

	RETURN_MODULE_NOOP;
}

/*
 *	Postauth: Write a record of the authentication attempt
 */
static unlang_action_t CC_HINT(nonnull) mod_post_auth(rlm_rcode_t *p_result, module_ctx_t const *mctx, request_t *request)
{
	rlm_sql_t const *inst = talloc_get_type_abort_const(mctx->inst->data, rlm_sql_t);

	if (inst->config.postauth.reference_cp) {
		return acct_redundant(p_result, inst, request, &inst->config.postauth);
	}

	RETURN_MODULE_NOOP;
}

/*
 *	Execute postauth_query after authentication
 */


/* globally exported name */
module_rlm_t rlm_sql = {
	.common = {
		.magic		= MODULE_MAGIC_INIT,
		.name		= "sql",
		.type		= MODULE_TYPE_THREAD_SAFE,
		.inst_size	= sizeof(rlm_sql_t),
		.config		= module_config,
		.bootstrap	= mod_bootstrap,
		.instantiate	= mod_instantiate,
		.detach		= mod_detach
	},
	.methods = {
		[MOD_AUTHORIZE]		= mod_authorize,
		[MOD_ACCOUNTING]	= mod_accounting,
		[MOD_POST_AUTH]		= mod_post_auth
	},
};
