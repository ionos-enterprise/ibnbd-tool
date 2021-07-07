// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Configuration tool for RNBD driver and RTRS library.
 *
 * Copyright (c) 2019 1&1 IONOS SE. All rights reserved.
 * Authors: Danil Kipnis <danil.kipnis@cloud.ionos.com>
 *          Lutz Pogrell <lutz.pogrell@cloud.ionos.com>
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "list.h"

#include "table.h"
#include "misc.h"
#include "rnbd-sysfs.h"

extern struct table_column *clms_paths_shortdesc[];
extern bool trm;

int list_devices_term(struct rnbd_sess_dev **sds,
		      struct table_column **cs,
		      const struct rnbd_ctx *ctx)
{
	struct rnbd_dev d_total = {
		.rx_sect = 0,
		.tx_sect = 0
	};
	struct rnbd_sess_dev total = {
		.dev = &d_total,
		.mapping_path = ""
	};
	struct table_fld *flds;
	int i, cs_cnt, dev_num;
	bool nototals_set = ctx->nototals_set;

	cs_cnt = table_clm_cnt(cs);

	for (dev_num = 0; sds[dev_num]; dev_num++)
		;

	if (!table_has_num(cs))
		nototals_set = true;

	flds = calloc((dev_num + 1) * cs_cnt, sizeof(*flds));
	if (!flds) {
		ERR(trm, "not enough memory\n");
		return -ENOMEM;
	}

	for (i = 0; sds[i]; i++) {
		table_row_stringify(sds[i], flds + i * cs_cnt, cs, ctx, true, 0);
		total.dev->rx_sect += sds[i]->dev->rx_sect;
		total.dev->tx_sect += sds[i]->dev->tx_sect;
	}

	if (!nototals_set)
		table_row_stringify(&total, flds + i * cs_cnt,
				    cs, ctx, true, 0);

	if (!ctx->noheaders_set)
		table_header_print_term("", cs, trm);

	for (i = 0; i < dev_num; i++)
		table_flds_print_term("", flds + i * cs_cnt,
				      cs, trm, 0);

	if (!nototals_set) {
		table_row_print_line("", cs, trm, 0);
		table_flds_del_not_num(flds + i * cs_cnt, cs);
		table_flds_print_term("", flds + i * cs_cnt,
				      cs, trm, 0);
	}

	free(flds);

	return 0;
}

void list_devices_csv(struct rnbd_sess_dev **sds,
		      struct table_column **cs,
		      const struct rnbd_ctx *ctx)
{
	int i;

	if (!sds[0])
		return;

	if (!ctx->noheaders_set)
		table_header_print_csv(cs);

	for (i = 0; sds[i]; i++)
		table_row_print(sds[i], FMT_CSV, "", cs, false, ctx, false, 0);
}

void list_devices_json(struct rnbd_sess_dev **sds,
		       struct table_column **cs,
		       const struct rnbd_ctx *ctx)
{
	int i;

	if (!sds[0])
		return;

	printf("[\n");

	for (i = 0; sds[i]; i++) {
		if (i)
			printf(",\n");
		table_row_print(sds[i], FMT_JSON, "\t\t", cs, false, ctx, false, 0);
	}

	printf("\n\t]");
}

void list_devices_xml(struct rnbd_sess_dev **sds,
		      struct table_column **cs,
		      const struct rnbd_ctx *ctx)
{
	int i;

	for (i = 0; sds[i]; i++) {
		printf("\t<device>\n");
		table_row_print(sds[i], FMT_XML, "\t\t", cs, false, ctx, false, 0);
		printf("\t</device>\n");
	}
}

static int compar_sess_sessname(const void *p1, const void *p2)
{
	const struct rnbd_sess *const *sess1 = p1, *const *sess2 = p2;

	return strcmp((*sess1)->sessname, (*sess2)->sessname);
}

int list_sessions_term(struct rnbd_sess **sessions,
		       struct table_column **cs,
		       const struct rnbd_ctx *ctx)
{
	struct rnbd_sess total = {
		.act_path_cnt = 0,
		.path_cnt = 0,
		.rx_bytes = 0,
		.tx_bytes = 0,
		.inflights = 0,
		.reconnects = 0
	};
	int i, cs_cnt, sess_num;
	struct table_fld *flds;
	struct rnbd_sess **sorted_sessions;

	cs_cnt = table_clm_cnt(cs);
	for (sess_num = 0; sessions[sess_num]; sess_num++)
		;

	sorted_sessions = calloc(sess_num + 1, sizeof(*sessions));
	if (!sorted_sessions) {
		ERR(trm, "not enough memory\n");
		return -ENOMEM;
	}
	memcpy(sorted_sessions, sessions, sizeof(*sessions) * sess_num);
	qsort(sorted_sessions, sess_num, sizeof(*sorted_sessions), compar_sess_sessname);

	flds = calloc((sess_num + 1) * cs_cnt, sizeof(*flds));
	if (!flds) {
		free(sorted_sessions);
		ERR(trm, "not enough memory\n");
		return -ENOMEM;
	}

	for (i = 0; sorted_sessions[i]; i++) {
		table_row_stringify(sorted_sessions[i], flds + i * cs_cnt, cs,
				    ctx, true, 0);

		total.act_path_cnt += sorted_sessions[i]->act_path_cnt;
		total.path_cnt += sorted_sessions[i]->path_cnt;
		total.rx_bytes += sorted_sessions[i]->rx_bytes;
		total.tx_bytes += sorted_sessions[i]->tx_bytes;
		total.inflights += sorted_sessions[i]->inflights;
		total.reconnects += sorted_sessions[i]->reconnects;
	}

	if (!ctx->nototals_set) {
		table_row_stringify(&total, flds + sess_num * cs_cnt,
				    cs, ctx, true, 0);
	}

	if (!ctx->noheaders_set)
		table_header_print_term("", cs, trm);

	for (i = 0; sorted_sessions[i]; i++) {
		table_flds_print_term("", flds + i * cs_cnt,
				      cs, trm, 0);
		if (!ctx->notree_set)
			list_paths_term(sorted_sessions[i]->paths,
					sorted_sessions[i]->path_cnt,
					clms_paths_shortdesc, 1, ctx,
					compar_paths_hca_src);
	}

	if (!ctx->nototals_set && table_has_num(cs)) {
		table_row_print_line("", cs, trm, 0);
		table_flds_del_not_num(flds + sess_num * cs_cnt, cs);
		table_flds_print_term("", flds + sess_num * cs_cnt,
				      cs, trm, 0);
	}

	free(sorted_sessions);
	free(flds);

	return 0;
}

void list_sessions_csv(struct rnbd_sess **sessions,
		       struct table_column **cs,
		       const struct rnbd_ctx *ctx)
{
	int i;

	if (!ctx->noheaders_set)
		table_header_print_csv(cs);

	for (i = 0; sessions[i]; i++)
		table_row_print(sessions[i], FMT_CSV, "", cs, false, ctx, false, 0);
}

void list_sessions_json(struct rnbd_sess **sessions,
			struct table_column **cs,
			const struct rnbd_ctx *ctx)
{
	int i;

	printf("[\n");

	for (i = 0; sessions[i]; i++) {
		if (i)
			printf(",\n");
		table_row_print(sessions[i], FMT_JSON, "\t\t", cs,
				false, ctx, false, 0);
	}

	printf("\n\t]");
}

void list_sessions_xml(struct rnbd_sess **sessions,
		       struct table_column **cs,
		       const struct rnbd_ctx *ctx)
{
	int i;

	for (i = 0; sessions[i]; i++) {
		printf("\t<session>\n");
		table_row_print(sessions[i], FMT_XML, "\t\t", cs,
				false, ctx, false, 0);
		printf("\t</session>\n");
	}
}

int compar_paths_hca_src(const void *p1, const void *p2)
{
	const struct rnbd_path *const *path1 = p1, *const *path2 = p2;

	return strcmp((*path1)->hca_name, (*path2)->hca_name) ?
		: strcmp((*path1)->src_addr, (*path2)->src_addr);
}

int compar_paths_sessname(const void *p1, const void *p2)
{
	const struct rnbd_path *const *path1 = p1, *const *path2 = p2;

	return strcmp((*path1)->sess->sessname, (*path2)->sess->sessname);
}

static struct rnbd_path **alloc_sorted_paths(struct rnbd_path **paths,
					     int path_cnt,
					     int (*comp)(const void *p1, const void *p2))
{
	struct rnbd_path **sorted_paths;

	sorted_paths = calloc(path_cnt, sizeof(*paths));
	if (!sorted_paths)
		return NULL;
	memcpy(sorted_paths, paths, sizeof(*paths) * path_cnt);

	if (comp) {
		/* sort the list according to the first column */
		qsort(sorted_paths, path_cnt, sizeof(*paths), comp);
	}
	return sorted_paths;
}

static void free_sorted_paths(struct rnbd_path **sorted_paths)
{
	free(sorted_paths);
}

int list_paths_term(struct rnbd_path **paths, int path_cnt,
		    struct table_column **cs, int tree,
		    const struct rnbd_ctx *ctx,
		    int (*comp)(const void *p1, const void *p2))
{
	struct rnbd_path total = {
		.rx_bytes = 0,
		.tx_bytes = 0,
		.inflights = 0,
		.reconnects = 0
	};
	int i, cs_cnt, fld_cnt = 0;
	struct table_fld *flds;
	struct rnbd_path **sorted_paths;

	cs_cnt = table_clm_cnt(cs);

	for (i = 0; i < path_cnt; i++) {
		if (!paths[i]) {
			ERR(trm, "inconsistent internal data path_cnt <-> paths\n");
			return -EFAULT;
		}
	}
	flds = calloc((path_cnt + 1) * cs_cnt, sizeof(*flds));
	if (!flds) {
		ERR(trm, "not enough memory\n");
		return -ENOMEM;
	}

	sorted_paths = alloc_sorted_paths(paths, path_cnt, comp);
	if (!sorted_paths) {
		free(flds);
		ERR(trm, "not enough memory\n");
		return -EFAULT;
	}

	for (i = 0; i < path_cnt; i++) {
		if (!sorted_paths[i]) {
			free_sorted_paths(sorted_paths);
			free(flds);
			ERR(trm, "inconsistent internal data path_cnt <-> paths\n");
			return -EFAULT;
		}
		table_row_stringify(sorted_paths[i], flds + fld_cnt, cs, ctx, true, 0);

		fld_cnt += cs_cnt;

		total.rx_bytes += sorted_paths[i]->rx_bytes;
		total.tx_bytes += sorted_paths[i]->tx_bytes;
		total.inflights += sorted_paths[i]->inflights;
		total.reconnects += sorted_paths[i]->reconnects;
	}

	if (!ctx->nototals_set)
		table_row_stringify(&total, flds + fld_cnt, cs, ctx, true, 0);

	if (!ctx->noheaders_set && !tree)
		table_header_print_term("", cs, trm);

	fld_cnt = 0;
	for (i = 0; i < path_cnt; i++) {
		table_flds_print_term(
			!tree ? "" : i < path_cnt - 1 ?
			"├─ " : "└─ ", flds + fld_cnt, cs, trm, 0);
		fld_cnt += cs_cnt;
	}

	if (!ctx->nototals_set && table_has_num(cs) && !tree) {
		table_row_print_line("", cs, trm, 0);
		table_flds_del_not_num(flds + fld_cnt, cs);
		table_flds_print_term("", flds + fld_cnt, cs, trm, 0);
	}

	free_sorted_paths(sorted_paths);
	free(flds);

	return 0;
}

void list_paths_csv(struct rnbd_path **paths,
		    struct table_column **cs,
		    const struct rnbd_ctx *ctx)
{
	int i;

	if (!ctx->noheaders_set)
		table_header_print_csv(cs);

	for (i = 0; paths[i]; i++)
		table_row_print(paths[i], FMT_CSV, "", cs,
				false, ctx, false, 0);
}

void list_paths_json(struct rnbd_path **paths,
		     struct table_column **cs,
		     const struct rnbd_ctx *ctx)
{
	int i;

	printf("\n\t[\n");

	for (i = 0; paths[i]; i++) {
		if (i)
			printf(",\n");
		table_row_print(paths[i], FMT_JSON, "\t\t", cs,
				false, ctx, false, 0);
	}

	printf("\n\t]");
}

void list_paths_xml(struct rnbd_path **paths,
		    struct table_column **cs,
		    const struct rnbd_ctx *ctx)
{
	int i;

	for (i = 0; paths[i]; i++) {
		printf("\t<path>\n");
		table_row_print(paths[i], FMT_XML, "\t\t", cs,
				false, ctx, false, 0);
		printf("\t</path>\n");
	}
}

