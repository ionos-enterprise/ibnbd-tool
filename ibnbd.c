// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>	/* for isatty() */
#include <stdbool.h>

#include "levenshtein.h"
#include "table.h"
#include "misc.h"
#include "list.h"

#include "ibnbd-sysfs.h"
#include "ibnbd-clms.h"

#define INF(fmt, ...) \
	do { \
		if (ctx.verbose_set) \
			printf(fmt, ##__VA_ARGS__); \
	} while (0)

static struct ibnbd_sess_dev **sds_clt;
static struct ibnbd_sess_dev **sds_srv;
static struct ibnbd_sess **sess_clt;
static struct ibnbd_sess **sess_srv;
static struct ibnbd_path **paths_clt;
static struct ibnbd_path **paths_srv;
static int sds_clt_cnt, sds_srv_cnt,
	   sess_clt_cnt, sess_srv_cnt,
	   paths_clt_cnt, paths_srv_cnt;

static struct ibnbd_ctx ctx;

struct sarg {
	const char *str;
	const char *descr;
	int (*parse)(int argc, const char *argv[], int i,
		     const struct sarg *sarg, struct ibnbd_ctx *ctx);
	size_t offset;
	int dist;
};

static int parse_fmt(int argc, const char *argv[], int i, 
		     const struct sarg *sarg, struct ibnbd_ctx *ctx)
{
	if (!strcasecmp(argv[i], "csv"))
		ctx->fmt = FMT_CSV;
	else if (!strcasecmp(argv[i], "json"))
		ctx->fmt = FMT_JSON;
	else if (!strcasecmp(argv[i], "xml"))
		ctx->fmt = FMT_XML;
	else if (!strcasecmp(argv[i], "term"))
		ctx->fmt = FMT_TERM;
	else
		return i;

	ctx->fmt_set = true;

	return i + 1;
}

static int parse_io_mode(int argc, const char *argv[], int i, 
		     const struct sarg *sarg, struct ibnbd_ctx *ctx)
{
	if (strcasecmp(argv[i], "blockio") &&
	    strcasecmp(argv[i], "fileio"))
		return i;

	strcpy(ctx->io_mode, argv[i]);

	ctx->io_mode_set = true;

	return i + 1;
}

enum lstmode {
	LST_DEVICES,
	LST_SESSIONS,
	LST_PATHS
};

static int parse_lst(int argc, const char *argv[], int i, 
		     const struct sarg *sarg, struct ibnbd_ctx *ctx)
{
	if (!strcasecmp(argv[i], "devices") ||
	    !strcasecmp(argv[i], "device") ||
	    !strcasecmp(argv[i], "devs") ||
	    !strcasecmp(argv[i], "dev"))
		ctx->lstmode = LST_DEVICES;
	else if (!strcasecmp(argv[i], "sessions") ||
		 !strcasecmp(argv[i], "session") ||
		 !strcasecmp(argv[i], "sess"))
		ctx->lstmode = LST_SESSIONS;
	else if (!strcasecmp(argv[i], "paths") ||
		 !strcasecmp(argv[i], "path"))
		ctx->lstmode = LST_PATHS;
	else
		return i;

	ctx->lstmode_set = true;

	return i + 1;
}

enum ibnbdmode {
	IBNBD_NONE = 0,
	IBNBD_CLIENT = 1,
	IBNBD_SERVER = 1 << 1,
	IBNBD_BOTH = IBNBD_CLIENT | IBNBD_SERVER,
};

static int parse_from(int argc, const char *argv[], int i, 
		     const struct sarg *sarg, struct ibnbd_ctx *ctx)
{
	int j = i + 1;

	if (j >= argc) {
		ERR(ctx->trm, "Please specify the destionation to map from\n");
		return i;
	}

	ctx->from = argv[j];
	ctx->from_set = 1;

	return j + 1;
}

static int parse_mode(int argc, const char *argv[], int i, 
		     const struct sarg *sarg, struct ibnbd_ctx *ctx)
{
	if (!strcasecmp(argv[i], "client") || !strcasecmp(argv[i], "clt"))
		ctx->ibnbdmode = IBNBD_CLIENT;
	else if (!strcasecmp(argv[i], "server") || !strcasecmp(argv[i], "srv"))
		ctx->ibnbdmode = IBNBD_SERVER;
	else if (!strcasecmp(argv[i], "both"))
		ctx->ibnbdmode = IBNBD_BOTH;
	else
		return i;

	ctx->ibnbdmode_set = true;

	return i + 1;
}

static int parse_rw(int argc, const char *argv[], int i, 
		     const struct sarg *sarg, struct ibnbd_ctx *ctx)
{
	if (strcasecmp(argv[i], "ro") &&
	    strcasecmp(argv[i], "rw") &&
	    strcasecmp(argv[i], "migration"))
		return i;

	ctx->access_mode = argv[i];
	ctx->access_mode_set = true;

	return i + 1;
}

static int clm_set_hdr_unit(struct table_column *clm, char const *unit)
{
	size_t len;

	len = strlen(clm->m_header);

	clm->m_width = len + snprintf(clm->m_header + len,
				      sizeof(clm->m_header) - len, " (%s)",
				      unit);

	return 0;
}

static int parse_unit(int argc, const char *argv[], int i, 
		     const struct sarg *sarg, struct ibnbd_ctx *ctx)
{
	int rc;

	rc = get_unit_index(sarg->str, &ctx->unit_id);
	if (rc < 0)
		return i;

	clm_set_hdr_unit(&clm_ibnbd_dev_rx_sect, sarg->descr);
	clm_set_hdr_unit(&clm_ibnbd_dev_tx_sect, sarg->descr);
	clm_set_hdr_unit(&clm_ibnbd_sess_rx_bytes, sarg->descr);
	clm_set_hdr_unit(&clm_ibnbd_sess_tx_bytes, sarg->descr);
	clm_set_hdr_unit(&clm_ibnbd_path_rx_bytes, sarg->descr);
	clm_set_hdr_unit(&clm_ibnbd_path_tx_bytes, sarg->descr);

	ctx->unit_set = true;
	return i + 1;
}

static int parse_all(int argc, const char *argv[], int i, 
		     const struct sarg *sarg, struct ibnbd_ctx *ctx)
{
	memcpy(&ctx->clms_devices_clt, &all_clms_devices_clt,
	       ARRSIZE(all_clms_devices_clt) * sizeof(all_clms_devices[0]));
	memcpy(&ctx->clms_devices_srv, &all_clms_devices_srv,
	       ARRSIZE(all_clms_devices_srv) * sizeof(all_clms_devices[0]));
	memcpy(&ctx->clms_sessions_clt, &all_clms_sessions_clt,
	       ARRSIZE(all_clms_sessions_clt) * sizeof(all_clms_sessions[0]));
	memcpy(&ctx->clms_sessions_srv, &all_clms_sessions_srv,
	       ARRSIZE(all_clms_sessions_srv) * sizeof(all_clms_sessions[0]));
	memcpy(&ctx->clms_paths_clt, &all_clms_paths_clt,
	       ARRSIZE(all_clms_paths_clt) * sizeof(all_clms_paths[0]));
	memcpy(&ctx->clms_paths_srv, &all_clms_paths_srv,
	       ARRSIZE(all_clms_paths_srv) * sizeof(all_clms_paths[0]));

	return i + 1;
}

static int parse_flag(int argc, const char *argv[], int i, 
		     const struct sarg *sarg, struct ibnbd_ctx *ctx)
{
	*(short *)(((char*)ctx)+(sarg->offset)) = 1;

	return i + 1;
}

static struct sarg sargs[] = {
	{"from", "Destination to map a device from", parse_from, 0},
	{"client", "Information for client", parse_mode, 0},
	{"clt", "Information for client", parse_mode, 0},
	{"server", "Information for server", parse_mode, 0},
	{"srv", "Information for server", parse_mode, 0},
	{"both", "Information for both", parse_mode, 0},
	{"devices", "List mapped devices", parse_lst, 0},
	{"device", "", parse_lst, 0},
	{"devs", "", parse_lst, 0},
	{"dev", "", parse_lst, 0},
	{"sessions", "List sessions", parse_lst, 0},
	{"session", "", parse_lst, 0},
	{"sess", "", parse_lst, 0},
	{"paths", "List paths", parse_lst, 0},
	{"path", "", parse_lst, 0},
	{"notree", "Don't display paths for each sessions", parse_flag,
	 offsetof(struct ibnbd_ctx, notree_set)},
	{"xml", "Print in XML format", parse_fmt, 0},
	{"csv", "Print in CSV format", parse_fmt, 0},
	{"json", "Print in JSON format", parse_fmt, 0},
	{"term", "Print for terminal", parse_fmt, 0},
	{"ro", "Readonly", parse_rw, 0},
	{"rw", "Writable", parse_rw, 0},
	{"migration", "Writable (migration)", parse_rw, 0},
	{"blockio", "Block IO mode", parse_io_mode, 0},
	{"fileio", "File IO mode", parse_io_mode, 0},
	{"help", "Display help and exit", parse_flag,
	 offsetof(struct ibnbd_ctx, help_set)},
	{"verbose", "Verbose output", parse_flag,
	 offsetof(struct ibnbd_ctx, verbose_set)},
	{"-v", "Verbose output", parse_flag,
	 offsetof(struct ibnbd_ctx, verbose_set)},
	{"B", "Byte", parse_unit, 0},
	{"K", "KiB", parse_unit, 0},
	{"M", "MiB", parse_unit, 0},
	{"G", "GiB", parse_unit, 0},
	{"T", "TiB", parse_unit, 0},
	{"P", "PiB", parse_unit, 0},
	{"E", "EiB", parse_unit, 0},
	{"noheaders", "Don't print headers", parse_flag,
	 offsetof(struct ibnbd_ctx, noheaders_set)},
	{"nototals", "Don't print totals", parse_flag,
	 offsetof(struct ibnbd_ctx, nototals_set)},
	{"force", "Force operation", parse_flag,
	 offsetof(struct ibnbd_ctx, force_set)},
	{"noterm", "Non-interactive mode", parse_flag,
	 offsetof(struct ibnbd_ctx, noterm_set)},
	{"-f", "", parse_flag,
	 offsetof(struct ibnbd_ctx, force_set)},
	{"all", "Print all columns", parse_all, 0},
	{0}
};

static const struct sarg *find_sarg(const char *str, const struct sarg *sargs)
{
	do {
		if (!strcasecmp(str, (*sargs).str))
			return sargs;
	} while ((*++sargs).str);

	return NULL;
}

#define HP "    "
#define HPRE HP "                "
#define HOPT HP "%-16s%s\n"

static void print_opt(const char *opt, const char *descr)
{
	printf(HOPT, opt, descr);
}

static void print_sarg_descr(char *str)
{
	const struct sarg *s;

	s = find_sarg(str, sargs);
	if (s)
		print_opt(s->str, s->descr);
}

struct cmd {
	const char *cmd;
	const char *short_d;
	const char *long_d;
	int (*func)(void);
	int (*parse_args)(int argc, const char *argv[], int i,
			  struct ibnbd_ctx *ctx);
	void (*help)(const struct cmd *cmd);
	int dist;
};

static const struct cmd *find_cmd(const char *cmd, const struct cmd *cmds)
{
	do {
		if (!strcmp(cmd, (*cmds).cmd))
			return cmds;
	} while ((*++cmds).cmd);

	return NULL;
}

static void print_usage(const char *program_name, const struct cmd *cmds)
{
	printf("Usage: %s%s%s {", CLR(ctx.trm, CBLD, program_name));

	clr_print(ctx.trm, CBLD, "%s", (*cmds).cmd);

	while ((*++cmds).cmd)
		printf("|%s%s%s", CLR(ctx.trm, CBLD, (*cmds).cmd));

	printf("} [ARGUMENTS]\n");
}

static void print_help(const char *program_name, const struct cmd *cmds)
{
	print_usage(program_name, cmds);
	printf("\nIBNBD command line utility.\n");
	printf("\nSubcommands:\n");
	do
		printf("     %-*s%s\n", 20, (*cmds).cmd, (*cmds).short_d);
	while ((*++cmds).cmd);

	printf("\nOptions:\n");
	print_sarg_descr("verbose");
	print_sarg_descr("help");
}

static int cmd_help(void);

static void cmd_print_usage(const struct cmd *cmd, const char *a)
{
	printf("Usage: %s%s%s %s%s%s %s[OPTIONS]\n",
	       CLR(ctx.trm, CBLD, ctx.pname), CLR(ctx.trm, CBLD, cmd->cmd), a);
	printf("\n%s\n", cmd->long_d);
}

static void print_clms_list(struct table_column **clms)
{
	if (*clms)
		printf("%s", (*clms)->m_name);

	while (*++clms)
		printf(",%s", (*clms)->m_name);

	printf("\n");
}

static void help_fields(void)
{
	print_opt("{fields}",
		  "Comma separated list of fields to be printed. The list can be");
	print_opt("",
		  "prefixed with '+' or '-' to add or remove fields from the ");
	print_opt("", "default selection.\n");
}

static void print_fields(struct table_column **def_clt,
			 struct table_column **def_srv,
			 struct table_column **all)
{
	table_tbl_print_term(HPRE, all, ctx.trm, &ctx);
	printf("\n%sDefault client: ", HPRE);
	print_clms_list(def_clt);
	printf("%sDefault server: ", HPRE);
	print_clms_list(def_srv);
	printf("\n");
}

static void help_list(const struct cmd *cmd)
{
	cmd_print_usage(cmd, "");

	printf("\nOptions:\n");
	print_opt("{mode}", "Information to print: devices|sessions|paths.");
	print_opt("", "Default: devices.");
	help_fields();

	printf("%s%s%s%s\n", HPRE, CLR(ctx.trm, CDIM, "Device Fields"));
	print_fields(def_clms_devices_clt, def_clms_devices_srv,
		     all_clms_devices);

	printf("%s%s%s%s\n", HPRE, CLR(ctx.trm, CDIM, "Session Fields"));
	print_fields(def_clms_sessions_clt, def_clms_sessions_srv,
		     all_clms_sessions);

	printf("%s%s%s%s\n", HPRE, CLR(ctx.trm, CDIM, "Path Fields"));
	print_fields(def_clms_paths_clt, def_clms_paths_srv, all_clms_paths);

	printf("%sProvide 'all' to print all available fields\n", HPRE);

	print_opt("{format}", "Output format: csv|json|xml");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");
	print_sarg_descr("notree");
	print_sarg_descr("noheaders");
	print_sarg_descr("nototals");
	print_sarg_descr("help");
}

static int list_devices(struct ibnbd_sess_dev **d_clt, int d_clt_cnt,
			struct ibnbd_sess_dev **d_srv, int d_srv_cnt)
{
	if (!(ctx.ibnbdmode & IBNBD_CLIENT))
		d_clt_cnt = 0;
	if (!(ctx.ibnbdmode & IBNBD_SERVER))
		d_srv_cnt = 0;

	switch (ctx.fmt) {
	case FMT_CSV:
		if (d_clt_cnt && d_srv_cnt)
			printf("Imports:\n");

		if (d_clt_cnt)
			list_devices_csv(d_clt, ctx.clms_devices_clt, &ctx);

		if (d_clt_cnt && d_srv_cnt)
			printf("Exports:\n");

		if (d_srv_cnt)
			list_devices_csv(d_srv, ctx.clms_devices_srv, &ctx);

		break;
	case FMT_JSON:
		printf("{\n");

		if (d_clt_cnt) {
			printf("\t\"imports\": ");
			list_devices_json(d_clt, ctx.clms_devices_clt, &ctx);
		}

		if (d_clt_cnt && d_srv_cnt)
			printf(",");

		printf("\n");

		if (d_srv_cnt) {
			printf("\t\"exports\": ");
			list_devices_json(d_srv, ctx.clms_devices_srv, &ctx);
		}

		printf("\n}\n");

		break;
	case FMT_XML:
		if (d_clt_cnt) {
			printf("<imports>\n");
			list_devices_xml(d_clt, ctx.clms_devices_clt, &ctx);
			printf("</imports>\n");
		}
		if (d_srv_cnt) {
			printf("<exports>\n");
			list_devices_xml(d_srv, ctx.clms_devices_srv, &ctx);
			printf("</exports>\n");
		}

		break;
	case FMT_TERM:
	default:
		if (d_clt_cnt && d_srv_cnt && !ctx.noheaders_set)
			printf("%s%s%s\n", CLR(ctx.trm, CDIM, "Imported devices"));

		if (d_clt_cnt)
			list_devices_term(d_clt, ctx.clms_devices_clt, &ctx);

		if (d_clt_cnt && d_srv_cnt && !ctx.noheaders_set)
			printf("%s%s%s\n", CLR(ctx.trm, CDIM, "Exported devices"));

		if (d_srv_cnt)
			list_devices_term(d_srv, ctx.clms_devices_srv, &ctx);

		break;
	}

	return 0;
}

static int list_sessions(struct ibnbd_sess **s_clt, int clt_s_num,
			 struct ibnbd_sess **s_srv, int srv_s_num)
{
	if (!(ctx.ibnbdmode & IBNBD_CLIENT))
		clt_s_num = 0;
	if (!(ctx.ibnbdmode & IBNBD_SERVER))
		srv_s_num = 0;

	switch (ctx.fmt) {
	case FMT_CSV:
		if (clt_s_num && srv_s_num)
			printf("Outgoing:\n");

		if (clt_s_num)
			list_sessions_csv(s_clt, ctx.clms_sessions_clt, &ctx);

		if (clt_s_num && srv_s_num)
			printf("Incoming:\n");

		if (srv_s_num)
			list_sessions_csv(s_srv, ctx.clms_sessions_srv, &ctx);
		break;
	case FMT_JSON:
		printf("{\n");

		if (clt_s_num) {
			printf("\t\"outgoing\": ");
			list_sessions_json(s_clt, ctx.clms_sessions_clt, &ctx);
		}

		if (clt_s_num && srv_s_num)
			printf(",");

		printf("\n");

		if (srv_s_num) {
			printf("\t\"incoming\": ");
			list_sessions_json(s_srv, ctx.clms_sessions_srv, &ctx);
		}

		printf("\n}\n");

		break;
	case FMT_XML:
		if (clt_s_num) {
			printf("\t\"outgoing\": ");
			printf("<outgoing>\n");
			list_sessions_xml(s_clt, ctx.clms_sessions_clt, &ctx);
			printf("</outgoing>\n");
		}
		if (srv_s_num) {
			printf("\t\"outgoing\": ");
			printf("<incoming>\n");
			list_sessions_xml(s_srv, ctx.clms_sessions_srv, &ctx);
			printf("</incoming>\n");
		}

		break;
	case FMT_TERM:
	default:
		if (clt_s_num && srv_s_num && !ctx.noheaders_set)
			printf("%s%s%s\n", CLR(ctx.trm, CDIM, "Outgoing sessions"));

		if (clt_s_num)
			list_sessions_term(s_clt, ctx.clms_sessions_clt, &ctx);

		if (clt_s_num && srv_s_num && !ctx.noheaders_set)
			printf("%s%s%s\n", CLR(ctx.trm, CDIM, "Incoming sessions"));

		if (srv_s_num)
			list_sessions_term(s_srv, ctx.clms_sessions_srv, &ctx);
		break;
	}

	return 0;
}

static int list_paths(struct ibnbd_path **p_clt, int clt_p_num,
		      struct ibnbd_path **p_srv, int srv_p_num)
{
	if (!(ctx.ibnbdmode & IBNBD_CLIENT))
		clt_p_num = 0;
	if (!(ctx.ibnbdmode & IBNBD_SERVER))
		srv_p_num = 0;

	switch (ctx.fmt) {
	case FMT_CSV:
		if (clt_p_num && srv_p_num)
			printf("Outgoing paths:\n");

		if (clt_p_num)
			list_paths_csv(p_clt, ctx.clms_paths_clt, &ctx);

		if (clt_p_num && srv_p_num)
			printf("Incoming paths:\n");

		if (srv_p_num)
			list_paths_csv(p_srv, ctx.clms_paths_srv, &ctx);
		break;
	case FMT_JSON:
		printf("{\n");

		if (clt_p_num) {
			printf("\t\"outgoing paths\": ");
			list_paths_json(p_clt, ctx.clms_paths_clt, &ctx);
		}

		if (clt_p_num && srv_p_num)
			printf(",");

		printf("\n");

		if (srv_p_num) {
			printf("\t\"incoming paths\": ");
			list_paths_json(p_srv, ctx.clms_paths_srv, &ctx);
		}

		printf("\n}\n");

		break;
	case FMT_XML:
		if (clt_p_num) {
			printf("<outgoing paths>\n");
			list_paths_xml(p_clt, ctx.clms_paths_clt, &ctx);
			printf("</outgoing paths>\n");
		}
		if (srv_p_num) {
			printf("<incoming paths>\n");
			list_paths_xml(p_srv, ctx.clms_paths_srv, &ctx);
			printf("</incoming paths>\n");
		}

		break;
	case FMT_TERM:
	default:
		if (clt_p_num && srv_p_num && !ctx.noheaders_set)
			printf("%s%s%s\n", CLR(ctx.trm, CDIM, "Outgoing paths"));

		if (clt_p_num)
			list_paths_term(p_clt, clt_p_num,
					ctx.clms_paths_clt, 0, &ctx);

		if (clt_p_num && srv_p_num && !ctx.noheaders_set)
			printf("%s%s%s\n", CLR(ctx.trm, CDIM, "Incoming paths"));

		if (srv_p_num)
			list_paths_term(p_srv, srv_p_num,
					ctx.clms_paths_srv, 0, &ctx);
		break;
	}

	return 0;
}

static int cmd_list(void)
{
	int rc;

	switch (ctx.lstmode) {
	case LST_DEVICES:
	default:
		rc = list_devices(sds_clt, sds_clt_cnt - 1, sds_srv,
				  sds_srv_cnt - 1);
		break;
	case LST_SESSIONS:
		rc = list_sessions(sess_clt, sess_clt_cnt - 1, sess_srv,
				   sess_srv_cnt - 1);
		break;
	case LST_PATHS:
		rc = list_paths(paths_clt, paths_clt_cnt - 1, paths_srv,
				paths_srv_cnt - 1);
		break;
	}

	return rc;
}

static bool match_device(struct ibnbd_sess_dev *d, const char *name)
{
	if (!strcmp(d->mapping_path, name) ||
	    !strcmp(d->dev->devname, name) ||
	    !strcmp(d->dev->devpath, name))
		return true;

	return false;
}

static int find_devices(const char *name, struct ibnbd_sess_dev **devs,
			struct ibnbd_sess_dev **res)
{
	int i, cnt = 0;

	for (i = 0; devs[i]; i++)
		if (match_device(devs[i], name))
			res[cnt++] = devs[i];

	res[cnt] = NULL;

	return cnt;
}

/*
 * Find all ibnbd devices by device name, path or mapping path
 */
static int find_devs_all(const char *name, struct ibnbd_sess_dev **ds_imp,
			    int *ds_imp_cnt, struct ibnbd_sess_dev **ds_exp,
			    int *ds_exp_cnt)
{
	int cnt_imp = 0, cnt_exp = 0;

	if (ctx.ibnbdmode & IBNBD_CLIENT)
		cnt_imp = find_devices(name, sds_clt, ds_imp);
	if (ctx.ibnbdmode & IBNBD_SERVER)
		cnt_exp = find_devices(name, sds_srv, ds_exp);

	*ds_imp_cnt = cnt_imp;
	*ds_exp_cnt = cnt_exp;

	return cnt_imp + cnt_exp;
}

static int show_device(struct ibnbd_sess_dev **clt, struct ibnbd_sess_dev **srv)
{
	struct table_fld flds[CLM_MAX_CNT];
	struct ibnbd_sess_dev **ds;
	struct table_column **cs;

	if (clt[0]) {
		ds = clt;
		cs = ctx.clms_devices_clt;
	} else {
		ds = srv;
		cs = ctx.clms_devices_srv;
	}

	switch (ctx.fmt) {
	case FMT_CSV:
		list_devices_csv(ds, cs, &ctx);
		break;
	case FMT_JSON:
		list_devices_json(ds, cs, &ctx);
		printf("\n");
		break;
	case FMT_XML:
		list_devices_xml(ds, cs, &ctx);
		break;
	case FMT_TERM:
	default:
		table_row_stringify(ds[0], flds, cs, &ctx, true, 0);
		table_entry_print_term("", flds, cs, table_get_max_h_width(cs),
				       ctx.trm);
		break;
	}

	return 0;
}

static bool match_sess(struct ibnbd_sess *s, const char *name)
{
	char *at;

	if (!strcmp(name, s->sessname))
		return true;

	at = strchr(s->sessname, '@');
	if (at && (!strcmp(name, at + 1) ||
		   !strncmp(name, s->sessname, at - s->sessname)))
		return true;
	return false;
}

static struct ibnbd_sess *find_session(const char *name,
				       struct ibnbd_sess **ss)
{
	int i;

	for (i = 0; ss[i]; i++)
		if (!strcmp(name, ss[i]->sessname))
			return ss[i];

	return NULL;
}

static int find_sessions_match(const char *name, struct ibnbd_sess **ss,
			       struct ibnbd_sess **res)
{
	int i, cnt = 0;

	for (i = 0; ss[i]; i++)
		if (match_sess(ss[i], name))
			res[cnt++] = ss[i];

	res[cnt] = NULL;

	return cnt;
}

static int find_sess_all(const char *name, struct ibnbd_sess **ss_clt,
			     int *ss_clt_cnt, struct ibnbd_sess **ss_srv,
			     int *ss_srv_cnt)
{
	int cnt_srv = 0, cnt_clt = 0;

	if (ctx.ibnbdmode & IBNBD_CLIENT)
		cnt_clt = find_sessions_match(name, sess_clt, ss_clt);
	if (ctx.ibnbdmode & IBNBD_SERVER)
		cnt_srv = find_sessions_match(name, sess_srv, ss_srv);

	*ss_clt_cnt = cnt_clt;
	*ss_srv_cnt = cnt_srv;

	return cnt_clt + cnt_srv;
}

static bool match_path(struct ibnbd_path *p, const char *name)
{
	int port;
	char *at;

	if (!strcmp(p->pathname, name) ||
	    !strcmp(name, p->src_addr) ||
	    !strcmp(name, p->dst_addr) ||
	    (sscanf(name, "%d\n", &port) == 1 &&
	     p->hca_port == port) ||
	    !strcmp(name, p->hca_name))
		return true;

	at = strrchr(name, ':');
	if (!at)
		return false;

	if (strncmp(p->sess->sessname, name,
		    strlen(p->sess->sessname)))
		return false;

	if ((sscanf(at + 1, "%d\n", &port) == 1 &&
	     p->hca_port == port) ||
	    !strcmp(at + 1, p->dst_addr) ||
	    !strcmp(at + 1, p->src_addr) ||
	    !strcmp(at + 1, p->hca_name))
		return true;

	return false;
}

static int find_paths(const char *name, struct ibnbd_path **pp,
		      struct ibnbd_path **res)
{
	int i, cnt = 0;

	for (i = 0; pp[i]; i++)
		if (match_path(pp[i], name))
			res[cnt++] = pp[i];

	res[cnt] = NULL;

	return cnt;
}

static int find_paths_all(const char *name, struct ibnbd_path **pp_clt,
			  int *pp_clt_cnt, struct ibnbd_path **pp_srv,
			  int *pp_srv_cnt)
{
	int cnt_clt = 0, cnt_srv = 0;

	if (ctx.ibnbdmode & IBNBD_CLIENT)
		cnt_clt = find_paths(name, paths_clt, pp_clt);
	if (ctx.ibnbdmode & IBNBD_SERVER)
		cnt_srv = find_paths(name, paths_srv, pp_srv);

	*pp_clt_cnt = cnt_clt;
	*pp_srv_cnt = cnt_srv;

	return cnt_clt + cnt_srv;
}

static int show_path(struct ibnbd_path **pp_clt, struct ibnbd_path **pp_srv)
{
	struct table_fld flds[CLM_MAX_CNT];
	struct table_column **cs;
	struct ibnbd_path **pp;

	if (pp_clt[0]) {
		pp = pp_clt;
		cs = ctx.clms_paths_clt;
	} else {
		pp = pp_srv;
		cs = ctx.clms_paths_srv;
	}

	switch (ctx.fmt) {
	case FMT_CSV:
		list_paths_csv(pp, cs, &ctx);
		break;
	case FMT_JSON:
		list_paths_json(pp, cs, &ctx);
		printf("\n");
		break;
	case FMT_XML:
		list_paths_xml(pp, cs, &ctx);
		break;
	case FMT_TERM:
	default:
		table_row_stringify(pp[0], flds, cs, &ctx, true, 0);
		table_entry_print_term("", flds, cs,
				       table_get_max_h_width(cs), ctx.trm);
		break;
	}

	return 0;
}

static int show_session(struct ibnbd_sess **ss_clt, struct ibnbd_sess **ss_srv)
{
	struct table_fld flds[CLM_MAX_CNT];
	struct table_column **cs, **ps;
	struct ibnbd_sess **ss;

	if (ss_clt[0]) {
		ss = ss_clt;
		cs = ctx.clms_sessions_clt;
		ps = clms_paths_sess_clt;
	} else {
		ss = ss_srv;
		cs = ctx.clms_sessions_srv;
		ps = clms_paths_sess_srv;
	}

	switch (ctx.fmt) {
	case FMT_CSV:
		list_sessions_csv(ss, cs, &ctx);
		break;
	case FMT_JSON:
		list_sessions_json(ss, cs, &ctx);
		printf("\n");
		break;
	case FMT_XML:
		list_sessions_xml(ss, cs, &ctx);
		break;
	case FMT_TERM:
	default:
		table_row_stringify(ss[0], flds, cs, &ctx, true, 0);
		table_entry_print_term("", flds, cs,
				       table_get_max_h_width(cs), ctx.trm);
		printf("%s%s%s", CLR(ctx.trm, CBLD, ss[0]->sessname));
		if (ss[0]->side == IBNBD_CLT)
			printf(" %s(%s)%s", CLR(ctx.trm, CBLD, ss[0]->mp_short));
		printf("\n");
		list_paths_term(ss[0]->paths, ss[0]->path_cnt, ps, 1, &ctx);

		break;
	}

	return 0;
}

static int cmd_show(void)
{
	struct ibnbd_sess_dev **ds_clt, **ds_srv;
	struct ibnbd_path **pp_clt, **pp_srv;
	struct ibnbd_sess **ss_clt, **ss_srv;
	int c_ds_clt, c_ds_srv, c_ds = 0,
	    c_pp_clt, c_pp_srv, c_pp = 0,
	    c_ss_clt, c_ss_srv, c_ss = 0, ret;

	pp_clt = calloc(paths_clt_cnt, sizeof(*pp_clt));
	pp_srv = calloc(paths_srv_cnt, sizeof(*pp_srv));
	ss_clt = calloc(sess_clt_cnt, sizeof(*ss_clt));
	ss_srv = calloc(sess_srv_cnt, sizeof(*ss_srv));
	ds_clt = calloc(sds_clt_cnt, sizeof(*ds_clt));
	ds_srv = calloc(sds_srv_cnt, sizeof(*ds_srv));

	if ((paths_clt_cnt && !pp_clt) ||
	    (paths_srv_cnt && !pp_srv) ||
	    (sess_clt_cnt && !ss_clt) ||
	    (sess_srv_cnt && !ss_srv) ||
	    (sds_clt_cnt && !ds_clt) ||
	    (sds_srv_cnt && !ds_srv)) {
		ERR(ctx.trm, "Failed to alloc memory\n");
		ret = -ENOMEM;
		goto out;
	}
	if (!ctx.lstmode_set || ctx.lstmode == LST_PATHS)
		c_pp = find_paths_all(ctx.name, pp_clt, &c_pp_clt, pp_srv,
				      &c_pp_srv);
	if (!ctx.lstmode_set || ctx.lstmode == LST_SESSIONS)
		c_ss = find_sess_all(ctx.name, ss_clt, &c_ss_clt, ss_srv,
				     &c_ss_srv);
	if (!ctx.lstmode_set || ctx.lstmode == LST_DEVICES)
		c_ds = find_devs_all(ctx.name, ds_clt, &c_ds_clt, ds_srv,
				     &c_ds_srv);
	if (c_pp + c_ss + c_ds > 1) {
		ERR(ctx.trm, "Multiple entries match '%s'\n", ctx.name);
		if (c_pp) {
			printf("Paths:\n");
			list_paths(pp_clt, c_pp_clt, pp_srv, c_pp_srv);
		}
		if (c_ss) {
			printf("Sessions:\n");
			list_sessions(ss_clt, c_ss_clt, ss_srv, c_ss_srv);
		}
		if (c_ds) {
			printf("Devices:\n");
			list_devices(ds_clt, c_ds_clt, ds_srv, c_ds_srv);
		}
		ret = -EINVAL;
		goto out;
	}

	if (c_ds)
		ret = show_device(ds_clt, ds_srv);
	else if (c_ss)
		ret = show_session(ss_clt, ss_srv);
	else if (c_pp)
		ret = show_path(pp_clt, pp_srv);
	else {
		ERR(ctx.trm, "There is no entry matching '%s'\n", ctx.name);
		ret = -ENOENT;
	}
out:
	free(ds_clt);
	free(ds_srv);
	free(pp_clt);
	free(pp_srv);
	free(ss_clt);
	free(ss_srv);
	return ret;
}

static int parse_name(int argc, const char *argv[], int i,
		      struct ibnbd_ctx *ctx)
{
	int j = i + 1;

	if (j >= argc) {
		ERR(ctx->trm, "Please specify the <name> argument\n");
		return i;
	}

	ctx->name = argv[j];

	return j + 1;
}

static void help_show(const struct cmd *cmd)
{
	cmd_print_usage(cmd, "<name> [path] ");

	printf("\nArguments:\n");
	print_opt("<name>",
		  "Name of a local or a remote block device, session name, path name or remote hostname.");
	print_opt("",
		  "I.e. ibnbd0, /dev/ibnbd0, d12aef94-4110-4321-9373-3be8494a557b, ps401a-1@st401b-2, st401b-2, <ip1>@<ip2>, etc.");
	print_opt("",
		  "In order to display path information, path name or identifier");
	print_opt("", "has to be provided, i.e. st401b-2:1.");

	printf("\nOptions:\n");
	help_fields();

	printf("%s%s%s%s\n", HPRE, CLR(ctx.trm, CDIM, "Device Fields"));
	print_fields(def_clms_devices_clt, def_clms_devices_srv,
		     all_clms_devices);

	printf("%s%s%s%s\n", HPRE, CLR(ctx.trm, CDIM, "Sessions Fields"));
	print_fields(def_clms_sessions_clt, def_clms_sessions_srv,
		     all_clms_sessions);

	printf("%s%s%s%s\n", HPRE, CLR(ctx.trm, CDIM, "Paths Fields"));
	print_fields(def_clms_paths_clt, def_clms_paths_srv, all_clms_paths);

	printf("%sProvide 'all' to print all available fields\n", HPRE);

	print_opt("{format}", "Output format: csv|json|xml");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");
	print_opt("{mode}",
		  "Information to print: device|session|path. Default: device.");

	print_sarg_descr("help");
}

static void help_map(const struct cmd *cmd)
{
	cmd_print_usage(cmd, "<path> from <server> ");

	printf("\nArguments:\n");
	print_opt("<device>", "Path to the device to be mapped on server side");
	print_opt("from <server>",
		  "Address, hostname or session name of the server");

	printf("\nOptions:\n");
	print_opt("<path>", "Path(s) to establish: [src_addr@]dst_addr");
	print_opt("", "Address is [ip:]<ipv4>, [ip:]<ipv6> or gid:<gid>");

	print_opt("{io_mode}",
		  "IO Mode to use on server side: fileio|blockio. Default: blockio");
	print_opt("{rw}",
		  "Access permission on server side: ro|rw|migration. Default: rw");
	print_sarg_descr("verbose");
	print_sarg_descr("help");
}

static bool is_ip4(const char *arg)
{
	/* TODO */
	return false;
}

static bool is_ip6(const char *arg)
{
	/* TODO */
	return false;
}

static bool is_gid(const char *arg)
{
	/* TODO */
	return is_ip6(arg);
}

static int parse_path(const char *arg)
{
	const char *src, *dst;
	char *d;

	d = strchr(arg, '@');
	if (d) {
		src = arg;
		dst = d + 1;
	} else {
		src = NULL;
		dst = arg;
	}

	if (src && !is_ip4(src) && !is_ip6(src) && !is_gid(src))
		return -EINVAL;

	if (!is_ip4(dst) && !is_ip6(dst) && !is_gid(dst))
		return -EINVAL;

	ctx.paths[ctx.path_cnt].src = src;
	ctx.paths[ctx.path_cnt].dst = dst;

	ctx.path_cnt++;

	return 0;
}

static int cmd_map(void)
{
	char cmd[4096], sessname[NAME_MAX];
	struct ibnbd_sess *sess;
	int i, cnt = 0, ret;

	if (!parse_path(ctx.from)) {
		/* user provided only paths to establish
		 * -> generate sessname
		 */
		strcpy(sessname, "clt@srv"); /* TODO */
	} else
		strcpy(sessname, ctx.from);

	sess = find_session(sessname, sess_clt);

	if (!sess && !ctx.path_cnt) {
		ERR(ctx.trm, 
		    "Client session '%s' not found. Please provide at least one path to establish a new one.\n",
		    ctx.from);
		return -EINVAL;
	}

	if (sess && ctx.path_cnt)
		INF(
		    "Session '%s' exists. Provided paths will be ignored by the driver. Please use addpath to add a path to an existsing sesion.\n",
		    ctx.from);

	cnt = snprintf(cmd, sizeof(cmd), "sessname=%s", sessname);
	cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt, " device_path=%s",
			ctx.name);

	for (i = 0; i < ctx.path_cnt; i++)
		cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt, " path=%s@%s",
				ctx.paths[i].src, ctx.paths[i].dst);

	if (sess)
		for (i = 0; i < sess->path_cnt; i++)
			cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt,
					" path=%s@%s", sess->paths[i]->src_addr,
					sess->paths[i]->dst_addr);

	if (ctx.io_mode_set)
		cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt, " io_mode=%s",
				ctx.io_mode);

	if (ctx.access_mode_set)
		cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt, " access_mode=%s",
				ctx.access_mode);

	errno = 0;
	ret = printf_sysfs(PATH_IBNBD_CLT, "map_device", "%s", cmd);
	ret = (ret < 0 ? ret : errno);
	if (ret)
		ERR(ctx.trm, "Failed to map device: %m (%d)\n", ret);

	return ret;
}

static struct ibnbd_sess_dev *find_single_device(const char *name,
						 struct ibnbd_sess_dev **devs)
{
	struct ibnbd_sess_dev *ds = NULL, **res;
	int cnt;

	if (!sds_clt_cnt) {
		ERR(ctx.trm, "Device '%s' not found: no devices mapped\n", name);
		return NULL;
	}

	res = calloc(sds_clt_cnt, sizeof(*res));
	if (!res) {
		ERR(ctx.trm, "Failed to allocate memory\n");
		return NULL;
	}

	cnt = find_devices(name, devs, res);
	if (!cnt) {
		ERR(ctx.trm, "Device '%s' not found\n", name);
		goto free;
	}

	if (cnt > 1) {
		ERR(ctx.trm, 
		"Please specify an exact path. There are multiple devices matching '%s':\n",
		name);
		list_devices(devs, cnt, &ds, 0);
		goto free;
	}

	ds = res[0];

free:
	free(res);
	return ds;
}

static int cmd_resize(void)
{
	struct ibnbd_sess_dev *ds;
	char tmp[PATH_MAX];
	int ret;

	ds = find_single_device(ctx.name, sds_clt);
	if (!ds)
		return -EINVAL;

	if (!ctx.size_set) {
		ERR(ctx.trm, "Please provide the size of the device to configure\n");
		return -EINVAL;
	}

	sprintf(tmp, "/sys/block/%s/ibnbd/", ds->dev->devname);
	errno = 0;
	ret = printf_sysfs(tmp, "resize", "%s", ctx.size_sect);
	ret = (ret < 0 ? ret : errno);
	if (ret)
		ERR(ctx.trm, "Failed to resize %s: %m (%d)\n",
		    ds->dev->devname, ret);

	return ret;
}

static void help_resize(const struct cmd *cmd)
{
	cmd_print_usage(cmd, "<device name or path or mapping path> ");

	printf("\nArguments:\n");
	print_opt("<device>", "Name of the device to be unmapped");
	print_opt("<size>", "New size of the device in bytes");

	printf("\nOptions:\n");
	print_sarg_descr("verbose");
	print_sarg_descr("help");
}

static void help_unmap(const struct cmd *cmd)
{
	cmd_print_usage(cmd, "<device name or path or mapping path> ");

	printf("\nArguments:\n");
	print_opt("<device>", "Name of the device to be unmapped");

	printf("\nOptions:\n");
	print_sarg_descr("force");
	print_sarg_descr("verbose");
	print_sarg_descr("help");
}

static int cmd_unmap(void)
{
	struct ibnbd_sess_dev *ds;
	char tmp[PATH_MAX];
	int ret;

	ds = find_single_device(ctx.name, sds_clt);
	if (!ds)
		return -EINVAL;

	sprintf(tmp, "/sys/block/%s/ibnbd/", ds->dev->devname);
	errno = 0;
	ret = printf_sysfs(tmp, "unmap_device", "%s",
			   ctx.force_set ? "force" : "normal");
	ret = (ret < 0 ? ret : errno);
	if (ret)
		ERR(ctx.trm, "Failed to %sunmap '%s': %m (%d)\n",
		    ctx.force_set ? "force-" : "",
		    ds->dev->devname, ret);

	return ret;
}

static void help_remap(const struct cmd *cmd)
{
	cmd_print_usage(cmd, "<devname|sessname> ");

	printf("\nArguments:\n");
	print_opt("<identifier>",
		  "Identifier of a device to be remapped. Or identifier of a session to remap all devices on.");

	printf("\nOptions:\n");
	print_sarg_descr("force");
	print_sarg_descr("verbose");
	print_sarg_descr("help");
}

static int cmd_remap(void)
{
	printf("TODO\n");
	return 0;
}

static void help_reconnect(const struct cmd *cmd)
{
	cmd_print_usage(cmd, "<path or session> ");

	printf("\nArguments:\n");
	print_opt("<identifier>",
		  "Name or identifier of a session or of a path:");
	print_opt("", "[sessname], [pathname], [sessname:port], etc.");

	printf("\nOptions:\n");
	print_sarg_descr("verbose");
	print_sarg_descr("help");
}

static int cmd_reconnect(void)
{
	printf("TODO\n");
	return 0;
}

static void help_disconnect(const struct cmd *cmd)
{
	cmd_print_usage(cmd, "<path or session> ");

	printf("\nArguments:\n");
	print_opt("<identifier>",
		  "Name or identifier of a session or of a path:");
	print_opt("", "[sessname], [pathname], [sessname:port], etc.");

	printf("\nOptions:\n");
	print_sarg_descr("verbose");
	print_sarg_descr("help");
}

static int cmd_disconnect(void)
{
	printf("TODO\n");
	return 0;
}

static void help_addpath(const struct cmd *cmd)
{
	cmd_print_usage(cmd, "<session> <path> ");

	printf("\nArguments:\n");
	print_opt("<session>",
		  "Name of the session to add the new path to");
	print_opt("<path>",
		  "Path to be added: [src_addr,]dst_addr");
	print_opt("",
		  "Address is of the form ip:<ipv4>, ip:<ipv6> or gid:<gid>");

	printf("\nOptions:\n");
	print_sarg_descr("verbose");
	print_sarg_descr("help");
}

static int cmd_addpath(void)
{
	printf("TODO\n");
	return 0;
}

static void help_help(const struct cmd *cmd);

static void help_delpath(const struct cmd *cmd)
{
	cmd_print_usage(cmd, "<path> ");

	printf("\nArguments:\n");
	print_opt("<path>",
		  "Name or any unique identifier of a path:");
	print_opt("", "[pathname], [sessname:port], etc.");

	printf("\nOptions:\n");
	print_sarg_descr("verbose");
	print_sarg_descr("help");
}

static int cmd_delpath(void)
{
	printf("TODO\n");
	return 0;
}

static struct cmd cmds[] = {
	{"list",
		"List block device or transport related information",
		"List block device or transport related information: devices, sessions, paths, etc.",
		cmd_list, NULL, help_list},
	{"show",
		"Show information about a device, a session or a path",
		"Show information about an ibnbd block- or transport- item: device, session or path.",
		cmd_show, parse_name, help_show},
	{"map",
		"Map a device from a given server",
		"Map a device from a given server",
		 cmd_map, parse_name, help_map},
	{"resize",
		"Resize a mapped device",
		"Change size of a mapped device",
		 cmd_resize, parse_name, help_resize},
	{"unmap",
		"Unmap an imported device",
		"Umap a given imported device",
		cmd_unmap, parse_name, help_unmap},
	{"remap",
		"Remap a device or all devices on a session",
		"Unmap and map again an imported device or do this for all devices of a given session",
		 cmd_remap, parse_name, help_remap},
	{"disconnect",
		"Disconnect a path or a session",
		"Disconnect a path or all paths on a given session",
		cmd_disconnect, parse_name, help_disconnect},
	{"reconnect",
		"Reconnect a path or a session",
		"Disconnect and connect again a path or a whole session",
		 cmd_reconnect, parse_name, help_reconnect},
	{"addpath",
		"Add a path to an existing session",
		"Add a new path to an existing session",
		 cmd_addpath, parse_name, help_addpath},
	{"delpath",
		"Delete a path",
		"Delete a given path from the corresponding session",
		 cmd_delpath, parse_name, help_delpath},
	{"help",
		"Display help",
		"Display help message and exit.",
		cmd_help, NULL, help_help},
	{ 0 }
};

static void help_help(const struct cmd *cmd)
{
	print_help(ctx.pname, cmds);
}

static int cmd_help(void)
{
	print_help(ctx.pname, cmds);
	return 0;
}

static int levenstein_compare(int d1, int d2, const char *s1, const char *s2)
{
	return d1 != d2 ? d1 - d2 : strcmp(s1, s2);
}

static int cmd_compare(const void *p1, const void *p2)
{
	const struct cmd *c1 = p1;
	const struct cmd *c2 = p2;

	return levenstein_compare(c1->dist, c2->dist, c1->cmd, c2->cmd);
}

static int sarg_compare(const void *p1, const void *p2)
{
	const struct sarg *c1 = p1;
	const struct sarg *c2 = p2;

	return levenstein_compare(c1->dist, c2->dist, c1->str, c2->str);
}

static void handle_unknown_cmd(const char *cmd, struct cmd *cmds)
{
	struct cmd *cs;
	size_t len = 0, cnt = 0;

	for (cs = cmds; cs->cmd; cs++) {
		cs->dist = levenshtein(cs->cmd, cmd, 0, 2, 1, 3) + 1;
		if (strlen(cs->cmd) < 2)
			cs->dist += 3;
		len++;
		if (cs->dist < 7)
			cnt++;
	}

	if (!cnt)
		return;

	qsort(cmds, len, sizeof(*cmds), cmd_compare);

	printf("Did you mean:\n");

	for (len = 0; len < cnt; len++)
		printf("\t%s\n", cmds[len].cmd);
}

static void handle_unknown_sarg(const char *sarg, struct sarg *sargs)
{
	struct sarg *cs;
	size_t len = 0, cnt = 0, i;

	for (cs = sargs; cs->str; cs++) {
		cs->dist = levenshtein(cs->str, sarg, 0, 2, 1, 3) + 1;
		if (strlen(cs->str) < 2)
			cs->dist += 10;
		len++;
		if (cs->dist < 5)
			cnt++;
	}

	if (!cnt)
		return;

	qsort(sargs, len, sizeof(*sargs), sarg_compare);

	printf("Did you mean:\n");

	for (i = 0; i < cnt; i++)
		printf("\t%s\n", sargs[i].str);
}

static int parse_precision(const char *str,
			   struct ibnbd_ctx *ctx)
{
	unsigned int prec;
	char e;

	if (strncmp(str, "prec", 4))
		return -EINVAL;

	if (sscanf(str + 4, "%u%c\n", &prec, &e) != 1)
		return -EINVAL;

	ctx->prec = prec;
	ctx->prec_set = true;

	return 0;
}

static const char *comma = ",";

static inline int parse_clt_devices_clms(const char *arg, struct ibnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_devices_clt,
				    ctx->clms_devices_clt, CLM_MAX_CNT);
}

static inline int parse_srv_devices_clms(const char *arg, struct ibnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_devices_srv,
				    ctx->clms_devices_srv, CLM_MAX_CNT);
}

static inline int parse_clt_sessions_clms(const char *arg, struct ibnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_sessions_clt,
				    ctx->clms_sessions_clt, CLM_MAX_CNT);
}

static inline int parse_srv_sessions_clms(const char *arg, struct ibnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_sessions_srv,
				    ctx->clms_sessions_srv, CLM_MAX_CNT);
}

static inline int parse_clt_paths_clms(const char *arg, struct ibnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_paths_clt,
				    ctx->clms_paths_clt, CLM_MAX_CNT);
}

static inline int parse_srv_paths_clms(const char *arg, struct ibnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_paths_srv,
				    ctx->clms_paths_srv, CLM_MAX_CNT);
}

static int parse_sign(char s)
{
	if (s == '+')
		ctx.sign = 1;
	else if (s == '-')
		ctx.sign = -1;
	else
		ctx.sign = 0;

	return ctx.sign;
}

static int parse_size(const char *str)
{
	uint64_t size;

	if (parse_sign(*str))
		str++;

	if (str_to_size(str, &size))
		return -EINVAL;

	ctx.size_sect = size >> 9;
	ctx.size_set = 1;

	return 0;
}

static void init_ibnbd_ctx(struct ibnbd_ctx *ctx)
{
	memcpy(&(ctx->clms_devices_clt), &def_clms_devices_clt,
	       ARRSIZE(def_clms_devices_clt) * sizeof(all_clms_devices[0]));
	memcpy(&(ctx->clms_devices_srv), &def_clms_devices_srv,
	       ARRSIZE(def_clms_devices_srv) * sizeof(all_clms_devices[0]));
	
	memcpy(&(ctx->clms_sessions_clt), &def_clms_sessions_clt,
	       ARRSIZE(def_clms_sessions_clt) * sizeof(all_clms_sessions[0]));
	memcpy(&(ctx->clms_sessions_srv), &def_clms_sessions_srv,
	       ARRSIZE(def_clms_sessions_srv) * sizeof(all_clms_sessions[0]));
	
	memcpy(&(ctx->clms_paths_clt), &def_clms_paths_clt,
	       ARRSIZE(def_clms_paths_clt) * sizeof(all_clms_paths[0]));
	memcpy(&(ctx->clms_paths_srv), &def_clms_paths_srv,
	       ARRSIZE(def_clms_paths_srv) * sizeof(all_clms_paths[0]));
}

static void ibnbd_ctx_default(struct ibnbd_ctx *ctx)
{
	if (!ctx->lstmode_set)
		ctx->lstmode = LST_DEVICES;

	if (!ctx->fmt_set)
		ctx->fmt = FMT_TERM;

	if (!ctx->prec_set)
		ctx->prec = 3;

	if (!ctx->ibnbdmode_set) {
		if (sess_clt[0])
			ctx->ibnbdmode |= IBNBD_CLIENT;
		if (sess_srv[0])
			ctx->ibnbdmode |= IBNBD_SERVER;
	}
}

int main(int argc, const char *argv[])
{
	int ret = 0, i, rc_cd, rc_cs, rc_cp, rc_sd, rc_ss, rc_sp;
	const struct sarg *sarg;
	const struct cmd *cmd;

	ctx.trm = (isatty(STDOUT_FILENO) == 1);

	init_ibnbd_ctx(&ctx);

	ctx.pname = argv[0];

	if (argc < 2) {
		ERR(ctx.trm, "no command specified\n");
		print_usage(argv[0], cmds);
		ret = -EINVAL;
		goto out;
	}

	i = 1;

	/*
	 * try finding sess/devs/paths preceding the command
	 * (for those who is used to type ibnbd dev map or ibnbd session list)
	 */
	i = parse_lst(argc, argv, i, NULL, &ctx);
	/*
	 * try finding clt/srv preceding the command
	 * (for those who is used to type ibnbd clt list or ibnbd srv sess list)
	 */
	i = parse_mode(argc, argv, i, NULL, &ctx);

	cmd = find_cmd(argv[i], cmds);
	if (!cmd) {
		printf("'%s' is not a valid command. Try '%s%s%s %s%s%s'\n",
		       argv[i], CLR(ctx.trm, CBLD, argv[0]),
		       CLR(ctx.trm, CBLD, "help"));
		handle_unknown_cmd(argv[i], cmds);
		ret = -EINVAL;
		goto out;
	}
	if (cmd == find_cmd("help", cmds))
		ctx.help_set = true;

	if (i + 1 < argc && cmd->help &&
	    (!strcmp(argv[i + 1], "help") ||
	     !strcmp(argv[i + 1], "--help") ||
	     !strcmp(argv[i + 1], "-h"))) {
		cmd->help(cmd);
		goto out;
	}

	if (cmd->parse_args) {
		ret = cmd->parse_args(argc, argv, i, &ctx);
		if (ret == i) {
			if (cmd->help)
				cmd->help(cmd);
			ret = -EINVAL;
			goto out;
		}
		i = ret;
	} else {
		i++;
	}

	while (i < argc) {
		sarg = find_sarg(argv[i], sargs);
		if (!sarg) {
			rc_cd = parse_clt_devices_clms(argv[i], &ctx);
			rc_sd = parse_srv_devices_clms(argv[i], &ctx);
			rc_cs = parse_clt_sessions_clms(argv[i], &ctx);
			rc_ss = parse_srv_sessions_clms(argv[i], &ctx);
			rc_cp = parse_clt_paths_clms(argv[i], &ctx);
			rc_sp = parse_srv_paths_clms(argv[i], &ctx);
			if (!parse_precision(argv[i], &ctx) ||
			    !(rc_cd && rc_cs && rc_cp && rc_sd && rc_ss && rc_sp) ||
			    !parse_path(argv[i]) ||
			    !parse_size(argv[i])) {
				i++;
				continue;
			}

			printf("'%s' is not a valid argument. Try '", argv[i]);
			printf("%s%s%s %s%s%s %s%s%s",
			       CLR(ctx.trm, CBLD, ctx.pname),
			       CLR(ctx.trm, CBLD, cmd->cmd),
			       CLR(ctx.trm, CBLD, "help"));
			printf("'\n");

			handle_unknown_sarg(argv[i], sargs);

			ret = -EINVAL;
			goto out;
		}
		ret = sarg->parse(argc, argv, i, sarg, &ctx);
		if (i == ret) {
			ret = -EINVAL;
			goto out;
		}
		i = ret;
	}

	ret = ibnbd_sysfs_alloc_all(&sds_clt, &sds_srv,
				    &sess_clt, &sess_srv,
				    &paths_clt, &paths_srv,
				    &sds_clt_cnt, &sds_srv_cnt,
				    &sess_clt_cnt, &sess_srv_cnt,
				    &paths_clt_cnt, &paths_srv_cnt);
	if (ret) {
		ERR(ctx.trm, "Failed to alloc memory for sysfs entries: %d\n", ret);
		goto out;
	}

	ret = ibnbd_sysfs_read_all(sds_clt, sds_srv, sess_clt, sess_srv,
				   paths_clt, paths_srv);
	if (ret) {
		ERR(ctx.trm, "Failed to read sysfs entries: %d\n", ret);
		goto free;
	}

	ibnbd_ctx_default(&ctx);

	ret = 0;

	if (ctx.help_set && cmd->help)
		cmd->help(cmd);
	else if (cmd->func) {
		/*
		 * if (args.ibnbdmode == IBNBD_NONE) {
		 *	ERR("ibnbd modules not loaded\n");
		 *	ret = -ENOENT;
		 *	goto free;
		 * }
		 */
		ret = cmd->func();
	}
free:
	ibnbd_sysfs_free_all(sds_clt, sds_srv, sess_clt, sess_srv,
			     paths_clt, paths_srv);
out:
	return ret;
}
