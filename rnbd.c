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
#include <inttypes.h>
#include <string.h>
#include <unistd.h>	/* for isatty() */
#include <stdbool.h>

#include "levenshtein.h"
#include "table.h"
#include "misc.h"
#include "list.h"

#include "rnbd-sysfs.h"
#include "rnbd-clms.h"

#define INF(verbose_set, fmt, ...)		\
	do { \
		if (verbose_set) \
			printf(fmt, ##__VA_ARGS__); \
	} while (0)

bool trm;

static struct rnbd_sess_dev **sds_clt;
static struct rnbd_sess_dev **sds_srv;
static struct rnbd_sess **sess_clt;
static struct rnbd_sess **sess_srv;
static struct rnbd_path **paths_clt;
static struct rnbd_path **paths_srv;
static int sds_clt_cnt, sds_srv_cnt,
	   sess_clt_cnt, sess_srv_cnt,
	   paths_clt_cnt, paths_srv_cnt;

struct param {
	enum rnbd_token tok;
	const char *param_str;
	const char *short_d;
	const char *short_d2;
	const char *descr;
	const char *params;
	int (*parse)(int argc, const char *argv[],
		     const struct param *param, struct rnbd_ctx *ctx);
	void (*help)(const char *program_name,
		     const struct param *cmd,
		     const struct rnbd_ctx *ctx);
	size_t offset;
	int dist;
};

static int parse_fmt(int argc, const char *argv[],
		     const struct param *param, struct rnbd_ctx *ctx)
{
	if (!strcasecmp(*argv, "csv"))
		ctx->fmt = FMT_CSV;
	else if (!strcasecmp(*argv, "json"))
		ctx->fmt = FMT_JSON;
	else if (!strcasecmp(*argv, "xml"))
		ctx->fmt = FMT_XML;
	else if (!strcasecmp(*argv, "term"))
		ctx->fmt = FMT_TERM;
	else
		return 0;

	ctx->fmt_set = true;

	return 1;
}

static bool is_hca(const char *name, int len, const struct rnbd_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->port_cnt; i++)
		if (strncmp(name, ctx->port_descs[i].hca, len) == 0)
			return true;
	return false;
}

static int parse_port_desc(const char *arg,
			   struct rnbd_ctx *ctx)
{
	int port;
	char *at;

	at = strrchr(arg, ':');

	if (at) {
		/* <hca_name>:<port> */
		if ((sscanf(at+1, "%d\n", &port) == 1)
		    && is_hca(arg, at-arg, ctx)) {
			strncpy(ctx->port_desc_arg.hca, arg,
				at-arg > NAME_MAX ? NAME_MAX : at-arg);
			strncpy(ctx->port_desc_arg.port, at+1, NAME_MAX);
		} else {
			return -EINVAL;
		}
	} else {
		/* either a hca name or a port number */
		if (is_hca(arg, strlen(arg), ctx))
			strncpy(ctx->port_desc_arg.hca, arg, NAME_MAX);
		else if (sscanf(arg, "%d\n", &port) == 1)
			strncpy(ctx->port_desc_arg.port, arg, NAME_MAX);
		else
			return -EINVAL;
	}
	ctx->port_desc_set = true;

	return 0;
}

enum lstmode {
	LST_DEVICES,
	LST_SESSIONS,
	LST_PATHS,
	LST_ALL
};

static int parse_lst(int argc, const char *argv[],
		     const struct param *param, struct rnbd_ctx *ctx)
{
	if (!strcasecmp(*argv, "devices") ||
	    !strcasecmp(*argv, "device") ||
	    !strcasecmp(*argv, "devs") ||
	    !strcasecmp(*argv, "dev"))
		ctx->lstmode = LST_DEVICES;
	else if (!strcasecmp(*argv, "sessions") ||
		 !strcasecmp(*argv, "session") ||
		 !strcasecmp(*argv, "sess"))
		ctx->lstmode = LST_SESSIONS;
	else if (!strcasecmp(*argv, "paths") ||
		 !strcasecmp(*argv, "path"))
		ctx->lstmode = LST_PATHS;
	else
		return 0;

	ctx->lstmode_set = true;

	return 1;
}

static int parse_from(int argc, const char *argv[],
		      const struct param *param, struct rnbd_ctx *ctx)
{
	if (argc < 2) {
		ERR(trm, "Please specify the destination to map from\n");
		return 0;
	}

	ctx->from = argv[1];
	ctx->from_set = 1;

	return 2;
}

static int parse_help(int argc, const char *argv[],
		      const struct param *param, struct rnbd_ctx *ctx)
{
	ctx->help_set = true;

	if (argc > 1) {

		ctx->help_arg = argv[1];
		ctx->help_arg_set = 1;
	}
	return 1;
}

static void parse_argv0(const char *argv0, struct rnbd_ctx *ctx)
{
	const char *prog_name = strrchr(argv0, '/');

	if (!prog_name)
		prog_name = argv0;
	else
		prog_name++;

	ctx->pname = prog_name;
}

static int parse_mode(int argc, const char *argv[],
		     const struct param *param, struct rnbd_ctx *ctx)
{
	if (!strcasecmp(*argv, "client") || !strcasecmp(*argv, "clt") || !strcasecmp(*argv, "cli"))
		ctx->rnbdmode = RNBD_CLIENT;
	else if (!strcasecmp(*argv, "server") || !strcasecmp(*argv, "serv") || !strcasecmp(*argv, "srv"))
		ctx->rnbdmode = RNBD_SERVER;
	else if (!strcasecmp(*argv, "both"))
		ctx->rnbdmode = RNBD_BOTH;
	else
		return 0;

	ctx->rnbdmode_set = true;

	return 1;
}

static int parse_rw(int argc, const char *argv[],
		     const struct param *param, struct rnbd_ctx *ctx)
{
	if (strcasecmp(*argv, "ro") &&
	    strcasecmp(*argv, "rw") &&
	    strcasecmp(*argv, "migration"))
		return 0;

	ctx->access_mode = *argv;
	ctx->access_mode_set = true;

	return 1;
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

static int parse_apply_unit(const char *str, struct rnbd_ctx *ctx)
{
	int rc, shift;

	if (ctx->size_state != size_number)
		return -EINVAL;

	rc = get_unit_shift(str, &shift);
	if (rc < 0)
		return rc;

	INF(ctx->debug_set,
	    "unit %s accepted left shift size_sect by %d.\n",
	    str, shift);

	if (shift > 9)
		ctx->size_sect <<= shift-9;
	else
		ctx->size_sect >>= 9-shift;

	ctx->size_state = size_unit;

	return 0;
}

static int parse_unit(int argc, const char *argv[],
		      const struct param *param, struct rnbd_ctx *ctx)
{
	int rc;

	rc = get_unit_index(param->param_str, &ctx->unit_id);
	if (rc < 0)
		return 0;

	clm_set_hdr_unit(&clm_rnbd_dev_rx_sect, param->descr);
	clm_set_hdr_unit(&clm_rnbd_dev_tx_sect, param->descr);
	clm_set_hdr_unit(&clm_rnbd_sess_rx_bytes, param->descr);
	clm_set_hdr_unit(&clm_rnbd_sess_tx_bytes, param->descr);
	clm_set_hdr_unit(&clm_rnbd_path_rx_bytes, param->descr);
	clm_set_hdr_unit(&clm_rnbd_path_tx_bytes, param->descr);

	ctx->unit_set = true;
	return 1;
}

static int parse_all(int argc, const char *argv[],
		     const struct param *param, struct rnbd_ctx *ctx)
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

	return 1;
}

static int parse_flag(int argc, const char *argv[],
		     const struct param *param, struct rnbd_ctx *ctx)
{
	*(bool *)(((char *)ctx)+(param->offset)) = 1;

	return 1;
}

static void print_version(const struct rnbd_ctx *ctx)
{
	printf("%s version %s%s%s\n", ctx->pname,
	       CLR(trm, CBLD, VERSION));
}

static int parse_version(int argc, const char *argv[],
			 const struct param *param, struct rnbd_ctx *ctx)
{
	print_version(ctx);

	return -EAGAIN;
}

static int parse_debug(int argc, const char *argv[],
		     const struct param *param, struct rnbd_ctx *ctx)
{
	ctx->debug_set = true;
	ctx->verbose_set = true;

	return 1;
}

static struct param _params_from =
	{TOK_FROM, "from", "", "", "Destination to map a device from",
	 NULL, parse_from, 0};
static struct param _params_client =
	{TOK_CLIENT, "client", "", "", "Operations of client",
	 NULL, parse_mode, 0};
static struct param _params_clt =
	{TOK_CLIENT, "clt", "", "", "Operations of client",
	 NULL, parse_mode, 0};
static struct param _params_cli =
	{TOK_CLIENT, "cli", "", "", "Operations of client",
	 NULL, parse_mode, 0};
static struct param _params_server =
	{TOK_SERVER, "server", "", "", "Operations of server",
	 NULL, parse_mode, 0};
static struct param _params_serv =
	{TOK_SERVER, "serv", "", "", "Operations of server",
	 NULL, parse_mode, 0};
static struct param _params_srv =
	{TOK_SERVER, "srv", "", "", "Operations of server",
	 NULL, parse_mode, 0};
static struct param _params_both =
	{TOK_BOTH, "both", "", "", "Operations of both client and server",
	 NULL, parse_mode, 0};
static struct param _params_devices_client =
	{TOK_DEVICES, "devices", "", "", "Map/unmapped/modify devices",
	 NULL, parse_lst, 0};
static struct param _params_devices =
	{TOK_DEVICES, "devices", "", "", "Operate on devices", NULL, parse_lst, 0};
static struct param _params_device =
	{TOK_DEVICES, "device", "", "", "", NULL, parse_lst, 0};
static struct param _params_devs =
	{TOK_DEVICES, "devs", "", "", "", NULL, parse_lst, 0};
static struct param _params_dev =
	{TOK_DEVICES, "dev", "", "", "", NULL, parse_lst, 0};
static struct param _params_sessions =
	{TOK_SESSIONS, "sessions", "", "", "Operate on sessions",
	 NULL, parse_lst, 0};
static struct param _params_session =
	{TOK_SESSIONS, "session", "", "", "", NULL, parse_lst, 0};
static struct param _params_sess =
	{TOK_SESSIONS, "sess", "", "", "", NULL, parse_lst, 0};
static struct param _params_paths =
	{TOK_PATHS, "paths", "", "", "Operate on paths", NULL, parse_lst, 0};
static struct param _params_path =
	{TOK_PATHS, "path", "", "", "", NULL, parse_lst, 0};
static struct param _params_path_param =
	{TOK_PATHS, "<path>", "", "",
	 "Path to use (i.e. gid:fe80::1@gid:fe80::2)",
	 NULL, NULL, 0};
static struct param _params_path_param_gid =
	{TOK_PATHS, "gid:*", "", "",
	 "Path to use (i.e. gid:fe80::1@gid:fe80::2)",
	 NULL, NULL, 0};
static struct param _params_path_param_ip =
	{TOK_PATHS, "ip:*", "", "",
	 "Path to use (i.e. ip:192.168.42.42@ip:192.168.53.53)",
	 NULL, NULL, 0};
static struct param _params_notree =
	{TOK_NOTREE, "notree", "", "", "Don't display paths for each sessions",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, notree_set)};
static struct param _params_xml =
	{TOK_XML, "xml", "", "", "Print in XML format", NULL, parse_fmt, 0};
static struct param _params_cvs =
	{TOK_CSV, "csv", "", "", "Print in CSV format", NULL, parse_fmt, 0};
static struct param _params_json =
	{TOK_JSON, "json", "", "", "Print in JSON format", NULL, parse_fmt, 0};
static struct param _params_term =
	{TOK_TERM, "term", "", "", "Print for terminal", NULL, parse_fmt, 0};
static struct param _params_ro =
	{TOK_RO, "ro", "", "", "Readonly", NULL, parse_rw, 0};
static struct param _params_rw =
	{TOK_RW, "rw", "", "", "Writable", NULL, parse_rw, 0};
static struct param _params_migration =
	{TOK_MIGRATION, "migration", "", "", "Writable (migration)",
	 NULL, parse_rw, 0};
static struct param _params_help =
	{TOK_HELP, "help", "", "", "Display help and exit",
	 NULL, parse_help, NULL, offsetof(struct rnbd_ctx, help_set)};
static struct param _params_version =
	{TOK_VERSION, "version", "", "",
	 "Display version of this tool and exit",
	 NULL, NULL, NULL, 0};
static struct param _params_minus_minus_version =
	{TOK_VERSION, "--version", "", "",
	 "Display version of this tool and exit",
	 NULL, parse_version, NULL, 0};
static struct param _params_verbose =
	{TOK_VERBOSE, "verbose", "", "", "Verbose output",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, verbose_set)};
static struct param _params_minus_h =
	{TOK_HELP, "-h", "", "", "Help output",
	 NULL, parse_help, NULL, offsetof(struct rnbd_ctx, help_set)};
static struct param _params_minus_minus_help =
	{TOK_HELP, "--help", "", "", "Help output",
	 NULL, parse_help, NULL, offsetof(struct rnbd_ctx, help_set)};
static struct param _params_minus_v =
	{TOK_VERBOSE, "-v", "", "", "Verbose output",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, verbose_set)};
static struct param _params_minus_minus_verbose =
	{TOK_VERBOSE, "--verbose", "", "", "Verbose output",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, verbose_set)};
static struct param _params_minus_d =
	{TOK_VERBOSE, "-d", "", "", "Debug mode",
	 NULL, parse_debug, NULL, offsetof(struct rnbd_ctx, debug_set)};
static struct param _params_minus_minus_debug =
	{TOK_VERBOSE, "--debug", "", "", "Debug mode", NULL, parse_debug, 0};
static struct param _params_minus_s =
	{TOK_VERBOSE, "-s", "", "", "Simulate",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, simulate_set)};
static struct param _params_minus_minus_simulate =
	{TOK_VERBOSE, "--simulate", "", "",
	 "Only print modifying operations, do not execute",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, simulate_set)};
static struct param _params_minus_c =
	{TOK_VERBOSE, "-c", "", "", "Complete",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, complete_set)};
static struct param _params_minus_minus_complete =
	{TOK_VERBOSE, "--complete", "", "", "Complete",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, complete_set)};
static struct param _params_byte =
	{TOK_BYTE, "B", "", "", "Byte", NULL, parse_unit, 0};
static struct param _params_kib =
	{TOK_KIB, "K", "", "", "KiB", NULL, parse_unit, 0};
static struct param _params_mib =
	{TOK_MIB, "M", "", "", "MiB", NULL, parse_unit, 0};
static struct param _params_gib =
	{TOK_GIB, "G", "", "", "GiB", NULL, parse_unit, 0};
static struct param _params_tib =
	{TOK_TIB, "T", "", "", "TiB", NULL, parse_unit, 0};
static struct param _params_pib =
	{TOK_PIB, "P", "", "", "PiB", NULL, parse_unit, 0};
static struct param _params_eib =
	{TOK_EIB, "E", "", "", "EiB", NULL, parse_unit, 0};
static struct param _params_noheaders =
	{TOK_NOHEADERS, "noheaders", "", "", "Don't print headers",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, noheaders_set)};
static struct param _params_nototals =
	{TOK_NOTOTALS, "nototals", "", "", "Don't print totals",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, nototals_set)};
static struct param _params_force =
	{TOK_FORCE, "force", "", "", "Force operation",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, force_set)};
static struct param _params_noterm =
	{TOK_NOTERM, "noterm", "", "", "Non-interactive mode",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, noterm_set)};
#if 0
static struct param _params_minus_f =
	{TOK_FORCE, "-f", "", "", "",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, force_set)};
#endif
static struct param _params_all =
	{TOK_ALL, "all", "", "", "Print all columns", NULL, parse_all, 0};
static struct param _params_all_recover =
	{TOK_ALL, "all", "", "", "Recover all",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, all_set)};
static struct param _params_recover_add_missing =
	{TOK_ALL, "add-missing", "", "", "Add missing paths",
	 NULL, parse_flag, NULL, offsetof(struct rnbd_ctx, add_missing_set)};

static struct param _params_null =
	{TOK_NONE, 0};

static struct param *params_options[] = {
	&_params_noheaders,
	&_params_nototals,
	&_params_notree,
	&_params_force,
	&_params_help,
	&_params_verbose,
	&_params_all_recover,
	&_params_recover_add_missing,
	&_params_null
};

static const struct param *find_param(const char *str,
				    struct param *const params[])
{
	if (str) {
		do {
			if (!strcasecmp(str, (*params)->param_str))
				return *params;
		} while ((*++params)->param_str);
	}
	return NULL;
}

static  void usage_param(const char *str, struct param *const params[],
			 const struct rnbd_ctx *ctx)
{
	if (ctx->complete_set) {
		for (; (*params)->param_str; params++)
			printf("%s ", (*params)->param_str);
		printf("\n");
	} else {
		printf("Usage: %s%s%s ", CLR(trm, CBLD, str));

		clr_print(trm, CBLD, "%s", (*params)->param_str);

		while ((*++params)->param_str)
			printf("|%s%s%s",
			       CLR(trm, CBLD, (*params)->param_str));

		printf(" [SUBCOMMANDS]\n\n");
	}
}

#define HP "    "
#define HPRE HP "                "

static void print_opt(const char *opt, const char *descr)
{
	if (trm)
		printf(HP "\x1B[1m%-16s\x1B[0m%s\n", opt, descr);
	else
		printf(HP "%-16s%s\n", opt, descr);
}

static void print_param_descr(char *str)
{
	const struct param *s;

	s = find_param(str, params_options);
	if (s)
		print_opt(s->param_str, s->descr);
}

static void print_param(struct param *const params[])
{
	do {
		print_opt((*params)->param_str, (*params)->descr);
	} while ((*++params)->param_str);
}

static void help_param(const char *str, struct param *const params[],
		       const struct rnbd_ctx *ctx)
{
	usage_param(str, params, ctx);
	printf("Subcommands:\n");
	print_param(params);
}

static void print_usage(const char *sub_name, struct param * const cmds[],
		        const struct rnbd_ctx *ctx)
{
	if (ctx->complete_set) {
		for (; (*cmds)->param_str; cmds++)
			if (strcmp("help", (*cmds)->param_str))
				printf("%s ", (*cmds)->param_str);
		printf("\n");
	} else {
		if (sub_name)
			printf("Usage: %s%s%s %s {",
			       CLR(trm, CBLD, ctx->pname), sub_name);
		else
			printf("Usage: %s%s%s {",
			       CLR(trm, CBLD, ctx->pname));

		clr_print(trm, CBLD, "%s", (*cmds)->param_str);

		while ((*++cmds)->param_str)
			printf("|%s%s%s",
			       CLR(trm, CBLD, (*cmds)->param_str));

		printf("} [ARGUMENTS]\n\n");
	}
}

static bool help_print_flags(const struct rnbd_ctx *ctx)
{
	if (ctx->help_arg_set && strncmp(ctx->help_arg, "flags", 3) == 0)
		return true;
	else
		return false;
}

static void cmd_print_usage_short(const struct param *cmd, const char *a,
				  const struct rnbd_ctx *ctx)
{
	if (a && *a)
		printf("Usage: %s%s%s %s%s%s %s%s%s",
		       CLR(trm, CBLD, ctx->pname),
		       CLR(trm, CBLD, a),
		       CLR(trm, CBLD, cmd->param_str));
	else
		printf("Usage: %s%s%s %s%s%s",
		       CLR(trm, CBLD, ctx->pname),
		       CLR(trm, CBLD, cmd->param_str));

	if (cmd->params)
		printf(" %s", cmd->params);
	printf(" [OPTIONS]\n\n");
}

static void cmd_print_usage_descr(const struct param *cmd, const char *a,
				  const struct rnbd_ctx *ctx)
{
	cmd_print_usage_short(cmd, a, ctx);
	printf("%s\n", cmd->descr);
}
static void print_help(const char *program_name,
		       const struct param *cmd,
		       struct param *const sub_cmds[],
		       const struct rnbd_ctx *ctx)
{
	print_usage(program_name, sub_cmds, ctx);

	cmd->help(program_name, cmd, ctx);

	printf("\nSubcommands:\n");
	print_param(sub_cmds);
}

static void print_clms_list(struct table_column **clms)
{
	if (*clms)
		printf("%s", (*clms)->m_name);

	while (*++clms)
		printf(",%s", (*clms)->m_name);

	printf("\n");
}

static void help_object(const char *program_name,
			const struct param *cmd,
			const struct rnbd_ctx *ctx)
{
	const char *word;

	word = strchr(program_name, ' ');
	if (word)
		word++; /* skip space */
	else
		word = program_name;

	printf("Execute operations on %ss.\n", word);
}

static void help_fields(void)
{
	print_opt("{fields}",
		  "Comma separated list of fields to be printed.");
	print_opt("",
		  "The list can be prefixed with '+' or '-' to add or remove");
	print_opt("",
		  "fields from the default selection.\n");
}

static void print_fields(const struct rnbd_ctx *ctx,
			 struct table_column **def_clt,
			 struct table_column **def_srv,
			 struct table_column **clt_all,
			 struct table_column **srv_all,
			 struct table_column **all,
			 enum rnbdmode mode)
{
	switch (mode) {
	case RNBD_SERVER:
		table_tbl_print_term(HPRE, srv_all, trm, ctx);
		break;
	case RNBD_CLIENT:
		table_tbl_print_term(HPRE, clt_all, trm, ctx);
		break;
	case RNBD_BOTH:
	case RNBD_NONE:
		table_tbl_print_term(HPRE, all, trm, ctx);
		break;
	}
	if (mode != RNBD_SERVER) {
		printf("\n%sDefault%s: ",
		       HPRE, mode == RNBD_BOTH ? " client" : "");
		print_clms_list(def_clt);
	}
	if (mode != RNBD_CLIENT) {
		printf("%sDefault%s: ",
		       HPRE, mode == RNBD_BOTH ? " server" : "");
		print_clms_list(def_srv);
	}
	printf("\n");
}

static void help_dump_all(const char *program_name,
			  const struct param *cmd,
			  const struct rnbd_ctx *ctx)
{
	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nOptions:\n");
	help_fields();

	printf("%s%s%s%s\n", HPRE,
	       CLR(trm, CDIM, "Device Fields"));
	print_fields(ctx, def_clms_devices_clt,
		     def_clms_devices_srv,
		     all_clms_devices_clt,
		     all_clms_devices_srv,
		     all_clms_devices, RNBD_BOTH);

	printf("%s%s%s%s\n", HPRE,
	       CLR(trm, CDIM, "Session Fields"));
	print_fields(ctx, def_clms_sessions_clt,
		     def_clms_sessions_srv,
		     all_clms_sessions_clt,
		     all_clms_sessions_srv,
		     all_clms_sessions, RNBD_BOTH);

	printf("%s%s%s%s\n", HPRE,
	       CLR(trm, CDIM, "Path Fields"));
	print_fields(ctx, def_clms_paths_clt,
		     def_clms_paths_srv,
		     all_clms_paths_clt,
		     all_clms_paths_srv,
		     all_clms_paths, RNBD_BOTH);

	print_opt("{format}", "Output format: csv|json|xml");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");
	print_param_descr("notree");
	print_param_descr("noheaders");
	print_param_descr("nototals");
	print_param_descr("help");
}

static void help_list_devices(const char *program_name,
			      const struct param *cmd,
			      const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "devices";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nOptions:\n");

	help_fields();

	print_fields(ctx, def_clms_devices_clt,
		     def_clms_devices_srv,
		     all_clms_devices_clt,
		     all_clms_devices_srv,
		     all_clms_devices, ctx->rnbdmode);

	print_opt("{format}", "Output format: csv|json|xml");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");
	print_param_descr("notree");
	print_param_descr("noheaders");
	print_param_descr("nototals");
	print_opt("help", "Display help and exit. [fields|all]");
}

static void help_list_sessions(const char *program_name,
			       const struct param *cmd,
			       const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "sessions";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nOptions:\n");

	help_fields();

	print_fields(ctx, def_clms_sessions_clt,
		     def_clms_sessions_srv,
		     all_clms_sessions_clt,
		     all_clms_sessions_srv,
		     all_clms_sessions, ctx->rnbdmode);

	print_opt("{format}", "Output format: csv|json|xml");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");
	print_param_descr("notree");
	print_param_descr("noheaders");
	print_param_descr("nototals");
	print_opt("help", "Display help and exit. [fields|all]");
}

static void help_list_paths(const char *program_name,
			    const struct param *cmd,
			    const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "paths";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nOptions:\n");

	help_fields();

	print_fields(ctx, def_clms_paths_clt,
		     def_clms_paths_srv,
		     all_clms_paths_clt,
		     all_clms_paths_srv,
		     all_clms_paths, ctx->rnbdmode);

	print_opt("{format}", "Output format: csv|json|xml");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");
	print_param_descr("notree");
	print_param_descr("noheaders");
	print_param_descr("nototals");
	print_opt("help", "Display help and exit. [fields|all]");
}

static int list_devices(struct rnbd_sess_dev **d_clt, int d_clt_cnt,
			struct rnbd_sess_dev **d_srv, int d_srv_cnt,
			bool is_dump, struct rnbd_ctx *ctx)
{
	if (!(ctx->rnbdmode & RNBD_CLIENT))
		d_clt_cnt = 0;
	if (!(ctx->rnbdmode & RNBD_SERVER))
		d_srv_cnt = 0;

	switch (ctx->fmt) {
	case FMT_CSV:
		if ((d_clt_cnt && d_srv_cnt) || ctx->rnbdmode == RNBD_BOTH)
			printf("Imports:\n");

		if (d_clt_cnt)
			list_devices_csv(d_clt, ctx->clms_devices_clt, ctx);

		if ((d_clt_cnt && d_srv_cnt) || ctx->rnbdmode == RNBD_BOTH)
			printf("Exports:\n");

		if (d_srv_cnt)
			list_devices_csv(d_srv, ctx->clms_devices_srv, ctx);

		break;
	case FMT_JSON:
		printf("{\n");

		printf("\t\"imports\": ");

		if (d_clt_cnt)
			list_devices_json(d_clt, ctx->clms_devices_clt, ctx);
		else
			printf("null");

		printf(",\n\t\"exports\": ");

		if (d_srv_cnt)
			list_devices_json(d_srv, ctx->clms_devices_srv, ctx);
		else
			printf("null");

		if (!is_dump)
			printf("\n}\n");
		else
			printf(",\n");

		break;
	case FMT_XML:
		if (d_clt_cnt) {
			printf("<imports>\n");
			list_devices_xml(d_clt, ctx->clms_devices_clt, ctx);
			printf("</imports>\n");
		}
		if (d_srv_cnt) {
			printf("<exports>\n");
			list_devices_xml(d_srv, ctx->clms_devices_srv, ctx);
			printf("</exports>\n");
		}

		break;
	case FMT_TERM:
	default:
		if ((d_clt_cnt && d_srv_cnt && !ctx->noheaders_set)
		    || (d_clt_cnt && is_dump))
			printf("%s%s%s\n",
			       CLR(trm, CDIM, "Imported devices"));

		if (d_clt_cnt)
			list_devices_term(d_clt, ctx->clms_devices_clt, ctx);

		if (d_clt_cnt && d_srv_cnt && is_dump)
			printf("\n");

		if ((d_clt_cnt && d_srv_cnt && !ctx->noheaders_set)
		    || (d_srv_cnt && is_dump))
			printf("%s%s%s\n",
			       CLR(trm, CDIM, "Exported devices"));

		if (d_srv_cnt)
			list_devices_term(d_srv, ctx->clms_devices_srv, ctx);

		break;
	}
	return 0;
}

static int list_sessions(struct rnbd_sess **s_clt, int clt_s_num,
			 struct rnbd_sess **s_srv, int srv_s_num,
			 bool is_dump, struct rnbd_ctx *ctx)
{
	if (!(ctx->rnbdmode & RNBD_CLIENT))
		clt_s_num = 0;
	if (!(ctx->rnbdmode & RNBD_SERVER))
		srv_s_num = 0;

	switch (ctx->fmt) {
	case FMT_CSV:
		if (clt_s_num && srv_s_num)
			printf("Outgoing sessions:\n");

		if (clt_s_num)
			list_sessions_csv(s_clt, ctx->clms_sessions_clt, ctx);

		if (clt_s_num && srv_s_num)
			printf("Incoming sessions:\n");

		if (srv_s_num)
			list_sessions_csv(s_srv, ctx->clms_sessions_srv, ctx);
		break;
	case FMT_JSON:
		if (!is_dump)
			printf("{\n");

		printf("\t\"outgoing sessions\": ");
		if (clt_s_num)
			list_sessions_json(s_clt, ctx->clms_sessions_clt, ctx);
		else
			printf("null,\n");

		printf("\t\"incoming sessions\": ");
		if (srv_s_num)
			list_sessions_json(s_srv, ctx->clms_sessions_srv, ctx);
		else
			printf("null");

		if (!is_dump)
			printf("\n}\n");
		else
			printf(",\n");

		break;
	case FMT_XML:
		if (clt_s_num) {
			printf("<outgoing-sessions>\n");
			list_sessions_xml(s_clt, ctx->clms_sessions_clt, ctx);
			printf("</outgoing-sessions>\n");
		}
		if (srv_s_num) {
			printf("<incoming-sessions>\n");
			list_sessions_xml(s_srv, ctx->clms_sessions_srv, ctx);
			printf("</incoming-sessions>\n");
		}

		break;
	case FMT_TERM:
	default:
		if ((clt_s_num && srv_s_num && !ctx->noheaders_set)
		    || (clt_s_num && is_dump))
			printf("%s%s%s\n",
			       CLR(trm, CDIM, "Outgoing sessions"));

		if (clt_s_num)
			list_sessions_term(s_clt, ctx->clms_sessions_clt, ctx);

		if (clt_s_num && srv_s_num && is_dump)
			printf("\n");

		if ((clt_s_num && srv_s_num && !ctx->noheaders_set)
		    || (srv_s_num && is_dump))
			printf("%s%s%s\n",
			       CLR(trm, CDIM, "Incoming sessions"));

		if (srv_s_num)
			list_sessions_term(s_srv, ctx->clms_sessions_srv, ctx);
		break;
	}

	return 0;
}

static int list_paths(struct rnbd_path **p_clt, int clt_p_num,
		      struct rnbd_path **p_srv, int srv_p_num,
		      bool is_dump, struct rnbd_ctx *ctx)
{
	if (!(ctx->rnbdmode & RNBD_CLIENT))
		clt_p_num = 0;
	if (!(ctx->rnbdmode & RNBD_SERVER))
		srv_p_num = 0;

	switch (ctx->fmt) {
	case FMT_CSV:
		if (clt_p_num && srv_p_num)
			printf("Outgoing paths:\n");

		if (clt_p_num)
			list_paths_csv(p_clt, ctx->clms_paths_clt, ctx);

		if (clt_p_num && srv_p_num)
			printf("Incoming paths:\n");

		if (srv_p_num)
			list_paths_csv(p_srv, ctx->clms_paths_srv, ctx);
		break;
	case FMT_JSON:
		if (!is_dump)
			printf("{\n");

		printf("\t\"outgoing paths\": ");
		if (clt_p_num)
			list_paths_json(p_clt, ctx->clms_paths_clt, ctx);
		else
			printf("null,\n");

		printf("\t\"incoming paths\": ");
		if (srv_p_num)
			list_paths_json(p_srv, ctx->clms_paths_srv, ctx);
		else
			printf("null");

		printf("\n}\n");

		break;
	case FMT_XML:
		if (clt_p_num) {
			printf("<outgoing-paths>\n");
			list_paths_xml(p_clt, ctx->clms_paths_clt, ctx);
			printf("</outgoing-paths>\n");
		}
		if (srv_p_num) {
			printf("<incoming-paths>\n");
			list_paths_xml(p_srv, ctx->clms_paths_srv, ctx);
			printf("</incoming-paths>\n");
		}

		break;
	case FMT_TERM:
	default:
		if ((clt_p_num && srv_p_num && !ctx->noheaders_set)
		    || (clt_p_num && is_dump))
			printf("%s%s%s\n",
			       CLR(trm, CDIM, "Outgoing paths"));

		if (clt_p_num)
			list_paths_term(p_clt, clt_p_num,
					ctx->clms_paths_clt, 0, ctx,
					compar_paths_sessname);

		if (clt_p_num && srv_p_num && is_dump)
			printf("\n");

		if ((clt_p_num && srv_p_num && !ctx->noheaders_set)
		    || (srv_p_num && is_dump))
			printf("%s%s%s\n",
			       CLR(trm, CDIM, "Incoming paths"));

		if (srv_p_num)
			list_paths_term(p_srv, srv_p_num,
					ctx->clms_paths_srv, 0, ctx,
					compar_paths_sessname);
		break;
	}

	return 0;
}

static bool match_device(struct rnbd_sess_dev *d, const char *name)
{
	if (!strcmp(d->mapping_path, name) ||
	    !strcmp(d->dev->devname, name) ||
	    !strcmp(d->dev->devpath, name))
		return true;

	return false;
}

static int find_devices(const char *name, struct rnbd_sess_dev **devs,
			struct rnbd_sess_dev **res)
{
	int i, cnt = 0;

	for (i = 0; devs[i]; i++)
		if (match_device(devs[i], name))
			res[cnt++] = devs[i];

	res[cnt] = NULL;

	return cnt;
}

/*
 * Find all rnbd devices by device name, device path or mapping path
 */
static int find_devs_all(const char *name, enum rnbdmode rnbdmode,
			 struct rnbd_sess_dev **ds_imp,
			 int *ds_imp_cnt, struct rnbd_sess_dev **ds_exp,
			 int *ds_exp_cnt)
{
	int cnt_imp = 0, cnt_exp = 0;

	if (rnbdmode & RNBD_CLIENT)
		cnt_imp = find_devices(name, sds_clt, ds_imp);
	if (rnbdmode & RNBD_SERVER)
		cnt_exp = find_devices(name, sds_srv, ds_exp);

	*ds_imp_cnt = cnt_imp;
	*ds_exp_cnt = cnt_exp;

	return cnt_imp + cnt_exp;
}

static int show_device(struct rnbd_sess_dev **clt, struct rnbd_sess_dev **srv,
		       struct rnbd_ctx *ctx)
{
	struct table_fld flds[CLM_MAX_CNT];
	struct rnbd_sess_dev **ds;
	struct table_column **cs;

	if (clt[0]) {
		ds = clt;
		cs = ctx->clms_devices_clt;
	} else {
		ds = srv;
		cs = ctx->clms_devices_srv;
	}

	switch (ctx->fmt) {
	case FMT_CSV:
		list_devices_csv(ds, cs, ctx);
		break;
	case FMT_JSON:
		list_devices_json(ds, cs, ctx);
		printf("\n");
		break;
	case FMT_XML:
		list_devices_xml(ds, cs, ctx);
		break;
	case FMT_TERM:
	default:
		table_row_stringify(ds[0], flds, cs, ctx, true, 0);
		table_entry_print_term("", flds, cs,
				       table_get_max_h_width(cs), trm);
		break;
	}

	return 0;
}

static struct rnbd_sess *find_sess(const char *name,
				   struct rnbd_sess **ss)
{
	int i;

	for (i = 0; ss[i]; i++)
		if (!strcmp(name, ss[i]->sessname))
			return ss[i];

	return NULL;
}

static int find_sess_match(const char *name, enum rnbdmode rnbdmode,
			   struct rnbd_sess **ss, struct rnbd_sess **res)
{
	int i, cnt = 0;

	for (i = 0; ss[i]; i++)
		if (!strcmp(ss[i]->sessname, name) ||
		    !strcmp(ss[i]->hostname, name))
			res[cnt++] = ss[i];

	res[cnt] = NULL;

	return cnt;
}

static int find_sess_match_all(const char *name, enum rnbdmode rnbdmode,
			 struct rnbd_sess **ss_clt, int *ss_clt_cnt,
			 struct rnbd_sess **ss_srv, int *ss_srv_cnt)
{
	int cnt_srv = 0, cnt_clt = 0;

	if (rnbdmode & RNBD_CLIENT)
		cnt_clt = find_sess_match(name, rnbdmode, sess_clt, ss_clt);
	if (rnbdmode & RNBD_SERVER)
		cnt_srv = find_sess_match(name, rnbdmode, sess_srv, ss_srv);

	*ss_clt_cnt = cnt_clt;
	*ss_srv_cnt = cnt_srv;

	return cnt_clt + cnt_srv;
}

static int parse_path1(const char *arg,
		       struct path *path)
{
	char *src, *dst;
	const char *d; int d_pos;

	d = strchr(arg, ',');
	if (!d)
		d = strchr(arg, '@');
	if (d) {
		d_pos = d - arg;
		src = strndup(arg, d_pos+1);
		src[d_pos] = 0;
		dst = strdup(d + 1);
	} else {
		src = NULL;
		dst = strdup(arg);
	}

	if (src && !is_path_addr(src))
		goto free_error;

	if (!is_path_addr(dst))
		goto free_error;

	path->provided = strdup(arg);
	path->src = src;
	path->dst = dst;

	return 1;

free_error:
	if (src)
		free(src);
	if (dst)
		free(dst);

	return 0;
}

static bool match_path(struct rnbd_path *p, const char *name)
{
	struct path other = {NULL, NULL};
	int port;
	char *at;

	if (!strcmp(p->pathname, name) ||
	    !strcmp(name, p->src_addr) ||
	    !strcmp(name, p->dst_addr))
		return true;

	if (parse_path1(name, &other)) {
		if ((!other.src
		     || match_path_addr(p->src_addr, other.src))
		    && match_path_addr(p->dst_addr, other.dst))
			return true;
		if (!other.src
		     && match_path_addr(p->src_addr, other.dst))
			return true;
	}

	if ((sscanf(name, "%d\n", &port) == 1 &&
	     p->hca_port == port) ||
	    !strcmp(name, p->hca_name))
		return true;

	at = strrchr(name, ':');
	if (!at)
		return false;

	if (strncmp(p->sess->sessname, name,
		    strlen(p->sess->sessname)) &&
	    strncmp(p->hca_name, name,
		    strlen(p->hca_name)))
		return false;

	if ((sscanf(at + 1, "%d\n", &port) == 1 &&
	     p->hca_port == port) ||
	    !strcmp(at + 1, p->dst_addr) ||
	    !strcmp(at + 1, p->src_addr) ||
	    !strcmp(at + 1, p->hca_name))
		return true;

	return false;
}

static int find_paths(const char *session_name, const char *path_name,
		      struct rnbd_ctx *ctx,
		      struct rnbd_path **pp, struct rnbd_path **res)
{
	int i, port, cnt = 0;

	for (i = 0; pp[i]; i++) {
		if (session_name && path_name && ctx->port_desc_set) {
			if (!strcmp(session_name, pp[i]->sess->sessname)) {
				if (ctx->port_desc_arg.hca[0]
				    && strcmp(ctx->port_desc_arg.hca, pp[i]->hca_name))
					continue;
				if (ctx->port_desc_arg.port[0])
				{
					sscanf(ctx->port_desc_arg.port, "%d\n", &port);
					if (pp[i]->hca_port != port)
						continue;
				}
				if (!match_path(pp[i], path_name))
					continue;

				res[cnt++] = pp[i];
			}
		} else if (path_name != NULL && !ctx->port_desc_set) {
			if (match_path(pp[i], path_name)
			    && (session_name == NULL
				|| !strcmp(session_name, pp[i]->sess->sessname)))
				res[cnt++] = pp[i];
		} else {
			if (!strcmp(session_name, pp[i]->sess->sessname)) {
				if (ctx->port_desc_arg.hca[0]
				    && strcmp(ctx->port_desc_arg.hca, pp[i]->hca_name))
					continue;
				if (ctx->port_desc_arg.port[0])
				{
					sscanf(ctx->port_desc_arg.port, "%d\n", &port);
					if (pp[i]->hca_port != port)
						continue;
				}
				res[cnt++] = pp[i];
			}
		}
	}
	res[cnt] = NULL;

	return cnt;
}

static int find_paths_for_session(const char *session_name,
				  struct rnbd_path **pp, struct rnbd_path **res)
{
	int i, cnt = 0;

	for (i = 0; pp[i]; i++)
		if (!strcmp(session_name, pp[i]->sess->sessname))
			res[cnt++] = pp[i];

	res[cnt] = NULL;

	return cnt;
}

static int find_paths_all(const char *session_name,
			  const char *path_name,
			  struct rnbd_ctx *ctx,
			  struct rnbd_path **pp_clt,
			  int *pp_clt_cnt, struct rnbd_path **pp_srv,
			  int *pp_srv_cnt)
{
	int cnt_clt = 0, cnt_srv = 0;
	char *base_path_name;

	if (ctx->rnbdmode & RNBD_CLIENT)
		cnt_clt = find_paths(session_name, path_name, ctx, paths_clt, pp_clt);
	if (ctx->rnbdmode & RNBD_SERVER)
		cnt_srv = find_paths(session_name, path_name, ctx, paths_srv, pp_srv);
	if (cnt_clt + cnt_srv == 0 && path_name && strchr(path_name, '%') != NULL) {
		INF(ctx->debug_set,
		    "Retry match for path name %s ignoring interface name.\n",
		    path_name);
		base_path_name = strdup(path_name);
		if (base_path_name) {
			*strchr(base_path_name, '%') = '\0';
			if (ctx->rnbdmode & RNBD_CLIENT)
				cnt_clt = find_paths(session_name,base_path_name,
						     ctx, paths_clt, pp_clt);
			if (ctx->rnbdmode & RNBD_SERVER)
				cnt_srv = find_paths(session_name, base_path_name,
						     ctx, paths_srv, pp_srv);
			free(base_path_name);
		}
	}
	*pp_clt_cnt = cnt_clt;
	*pp_srv_cnt = cnt_srv;

	return cnt_clt + cnt_srv;
}

static void list_default(struct rnbd_ctx *ctx)
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

static int show_path(struct rnbd_path **pp_clt, struct rnbd_path **pp_srv,
		     struct rnbd_ctx *ctx)
{
	struct table_fld flds[CLM_MAX_CNT];
	struct table_column **cs;
	struct rnbd_path **pp;

	if (pp_clt[0]) {
		pp = pp_clt;
		cs = ctx->clms_paths_clt;
	} else {
		pp = pp_srv;
		cs = ctx->clms_paths_srv;
	}

	switch (ctx->fmt) {
	case FMT_CSV:
		list_paths_csv(pp, cs, ctx);
		break;
	case FMT_JSON:
		list_paths_json(pp, cs, ctx);
		printf("\n");
		break;
	case FMT_XML:
		list_paths_xml(pp, cs, ctx);
		break;
	case FMT_TERM:
	default:
		table_row_stringify(pp[0], flds, cs, ctx, true, 0);
		table_entry_print_term("", flds, cs,
				       table_get_max_h_width(cs), trm);
		break;
	}

	return 0;
}

static int show_session(struct rnbd_sess **ss_clt, struct rnbd_sess **ss_srv,
			struct rnbd_ctx *ctx)
{
	struct table_fld flds[CLM_MAX_CNT];
	struct table_column **cs, **ps;
	struct rnbd_sess **ss;

	if (ss_clt && ss_clt[0]) {
		ss = ss_clt;
		cs = ctx->clms_sessions_clt;
		ps = clms_paths_sess_clt;
	} else {
		ss = ss_srv;
		cs = ctx->clms_sessions_srv;
		ps = clms_paths_sess_srv;
	}

	switch (ctx->fmt) {
	case FMT_CSV:
		list_sessions_csv(ss, cs, ctx);
		break;
	case FMT_JSON:
		list_sessions_json(ss, cs, ctx);
		printf("\n");
		break;
	case FMT_XML:
		list_sessions_xml(ss, cs, ctx);
		break;
	case FMT_TERM:
	default:
		table_row_stringify(ss[0], flds, cs, ctx, true, 0);
		table_entry_print_term("", flds, cs,
				       table_get_max_h_width(cs), trm);

		/* when notree is set explicitly or if exactly one collumn */
		/* is requested */
		/* no information on paths of the session will be shown */
		if (ctx->notree_set || table_clm_cnt(cs) == 1)
			break;

		printf("%s%s%s", CLR(trm, CBLD, ss[0]->sessname));
		if (ss[0]->side == RNBD_CLIENT)
			printf(" %s(%s)%s",
			       CLR(trm, CBLD, ss[0]->mp_short));
		printf("\n");
		list_paths_term(ss[0]->paths, ss[0]->path_cnt, ps, 1, ctx,
				compar_paths_hca_src);

		break;
	}

	return 0;
}

static int show_all(const char *name, struct rnbd_ctx *ctx)
{
	struct rnbd_sess_dev **ds_clt, **ds_srv;
	struct rnbd_path **pp_clt, **pp_srv;
	struct rnbd_sess **ss_clt, **ss_srv;
	const char *session_name; const char *path_name;
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
		ERR(trm, "Failed to alloc memory\n");
		ret = -ENOMEM;
		goto out;
	}
	if (ctx->path_cnt == 1) {
		session_name = name;
		path_name = ctx->paths[0].provided;
	} else {
		session_name = NULL;
		path_name = name;
		if (ctx->path_cnt > 1)
			ERR(trm, "Multiple paths specified\n");
	}
	c_pp = find_paths_all(session_name, path_name, ctx,
			      pp_clt, &c_pp_clt, pp_srv,
			      &c_pp_srv);
	if (!(c_pp && ctx->path_cnt == 1))
		c_ss = find_sess_match_all(name, ctx->rnbdmode, ss_clt,
					   &c_ss_clt, ss_srv, &c_ss_srv);
	if (!(c_pp && ctx->path_cnt == 1))
		c_ds = find_devs_all(name, ctx->rnbdmode, ds_clt,
				     &c_ds_clt, ds_srv, &c_ds_srv);
	if ((ctx->path_cnt == 1 && c_pp > 1)
	    || (ctx->path_cnt != 1 && c_pp + c_ss + c_ds > 1)) {
		ERR(trm, "Multiple entries match '%s'\n", name);

		list_default(ctx);

		if (c_pp) {
			printf("Paths:\n");
			list_paths(pp_clt, c_pp_clt,
				   pp_srv, c_pp_srv, false, ctx);
		}
		if (c_ss) {
			printf("Sessions:\n");
			list_sessions(ss_clt, c_ss_clt, ss_srv,
				      c_ss_srv, false, ctx);
		}
		if (c_ds) {
			printf("Devices:\n");
			list_devices(ds_clt, c_ds_clt, ds_srv,
				     c_ds_srv, false, ctx);
		}
		ret = -EINVAL;
		goto out;
	}

	if (ctx->path_cnt == 1 && c_ss + c_ds > 0)
		ERR(trm, "Provided path ignored!\n");

	if (c_ds)
		ret = show_device(ds_clt, ds_srv, ctx);
	else if (c_ss)
		ret = show_session(ss_clt, ss_srv, ctx);
	else if (c_pp)
		ret = show_path(pp_clt, pp_srv, ctx);
	else {
		ERR(trm, "There is no entry matching '%s'\n", name);
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

static int show_devices(const char *name, struct rnbd_ctx *ctx)
{
	struct rnbd_sess_dev **ds_clt, **ds_srv;
	int c_ds_clt, c_ds_srv, c_ds = 0, ret;

	ds_clt = calloc(sds_clt_cnt, sizeof(*ds_clt));
	ds_srv = calloc(sds_srv_cnt, sizeof(*ds_srv));

	if ((sds_clt_cnt && !ds_clt) ||
	    (sds_srv_cnt && !ds_srv)) {
		ERR(trm, "Failed to alloc memory\n");
		ret = -ENOMEM;
		goto out;
	}
	c_ds = find_devs_all(name, ctx->rnbdmode, ds_clt,
			     &c_ds_clt, ds_srv, &c_ds_srv);
	if (c_ds > 1) {
		ERR(trm, "Multiple devices match '%s'\n", name);

		list_default(ctx);

		printf("Devices:\n");
		list_devices(ds_clt, c_ds_clt, ds_srv, c_ds_srv, false, ctx);

		ret = -EINVAL;
		goto out;
	}

	if (c_ds) {
		ret = show_device(ds_clt, ds_srv, ctx);
	} else {
		ERR(trm, "There is no device matching '%s'\n", name);
		ret = -ENOENT;
	}
out:
	free(ds_clt);
	free(ds_srv);
	return ret;
}

static int show_client_sessions(const char *name, struct rnbd_ctx *ctx)
{
	struct rnbd_sess **ss_clt;
	int c_ss_clt = 0, ret;

	ss_clt = calloc(sess_clt_cnt, sizeof(*ss_clt));

	if (sess_clt_cnt && !ss_clt) {
		ERR(trm, "Failed to alloc memory\n");
		ret = -ENOMEM;
		goto out;
	}
	ss_clt[0] = find_sess(name, sess_clt);
	if (ss_clt[0])
		c_ss_clt = 1;
	else
		c_ss_clt = find_sess_match(name, ctx->rnbdmode, sess_clt, ss_clt);

	if (c_ss_clt > 1) {
		ERR(trm, "Multiple sessions match '%s'\n", name);

		list_default(ctx);

		printf("Sessions:\n");
		list_sessions(ss_clt, c_ss_clt, NULL, 0, false, ctx);

		ret = -EINVAL;
		goto out;
	}

	if (c_ss_clt) {
		ret = show_session(ss_clt, NULL, ctx);
	} else {
		ERR(trm, "There is no session matching '%s'\n", name);
		ret = -ENOENT;
	}
out:
	free(ss_clt);
	return ret;
}

static int show_server_sessions(const char *name, struct rnbd_ctx *ctx)
{
	struct rnbd_sess **ss_srv;
	int c_ss_srv = 0, ret;

	ss_srv = calloc(sess_srv_cnt, sizeof(*ss_srv));

	if (sess_srv_cnt && !ss_srv) {
		ERR(trm, "Failed to alloc memory\n");
		ret = -ENOMEM;
		goto out;
	}
	ss_srv[0] = find_sess(name, sess_srv);
	if (ss_srv[0])
		c_ss_srv = 1;
	else
		c_ss_srv = find_sess_match(name, ctx->rnbdmode, sess_srv, ss_srv);

	if (c_ss_srv > 1) {
		ERR(trm, "Multiple sessions match '%s'\n", name);

		list_default(ctx);

		printf("Sessions:\n");
		list_sessions(NULL, 0, ss_srv, c_ss_srv, false, ctx);

		ret = -EINVAL;
		goto out;
	}

	if (c_ss_srv) {
		ret = show_session(NULL, ss_srv, ctx);
	} else {
		ERR(trm, "There is no session matching '%s'\n", name);
		ret = -ENOENT;
	}
out:
	free(ss_srv);
	return ret;
}

static int show_both_sessions(const char *name, struct rnbd_ctx *ctx)
{
	struct rnbd_sess **ss_clt;
	struct rnbd_sess **ss_srv;
	int c_ss_clt = 0;
	int c_ss_srv = 0, ret;

	ss_srv = calloc(sess_srv_cnt, sizeof(*ss_srv));
	ss_clt = calloc(sess_clt_cnt, sizeof(*ss_clt));

	if ((sess_clt_cnt && !ss_clt)
	    || (sess_srv_cnt && !ss_srv)) {
		ERR(trm, "Failed to alloc memory\n");
		ret = -ENOMEM;
		goto out;
	}

	ss_clt[0] = find_sess(name, sess_clt);
	ss_srv[0] = find_sess(name, sess_srv);
	if (ss_clt[0])
		c_ss_clt = 1;
	if (ss_srv[0])
		c_ss_srv = 1;

	if (!ss_clt[0] && !ss_srv[0]) {
		c_ss_clt = find_sess_match(name, ctx->rnbdmode, sess_clt, ss_clt);
		c_ss_srv = find_sess_match(name, ctx->rnbdmode, sess_srv, ss_srv);
	}

	if (c_ss_clt + c_ss_srv > 1) {
		ERR(trm, "Multiple sessions match '%s'\n", name);

		list_default(ctx);

		printf("Sessions:\n");
		list_sessions(ss_clt, c_ss_clt, ss_srv, c_ss_srv, false, ctx);

		ret = -EINVAL;
		goto out;
	}

	if (c_ss_clt) {
		ret = show_session(ss_clt, NULL, ctx);
	} else if (c_ss_srv) {
		ret = show_session(NULL, ss_srv, ctx);
	} else {
		ERR(trm, "There is no session matching '%s'\n", name);
		ret = -ENOENT;
	}
out:
	free(ss_clt);
	free(ss_srv);
	return ret;
}

static int show_paths(const char *name, struct rnbd_ctx *ctx)
{
	struct rnbd_path **pp_clt, **pp_srv;
	int c_pp_clt, c_pp_srv, c_pp = 0, ret;
	const char *session_name; const char *path_name;

	pp_clt = calloc(paths_clt_cnt, sizeof(*pp_clt));
	pp_srv = calloc(paths_srv_cnt, sizeof(*pp_srv));

	if ((paths_clt_cnt && !pp_clt) ||
	    (paths_srv_cnt && !pp_srv)) {
		ERR(trm, "Failed to alloc memory\n");
		ret = -ENOMEM;
		goto out;
	}
	if (ctx->path_cnt == 1) {
		session_name = name;
		path_name = ctx->paths[0].provided;
	} else if (ctx->port_desc_set) {
		session_name = name;
		path_name = NULL;
	} else {
		session_name = NULL;
		path_name = name;
		if (ctx->path_cnt > 1)
			ERR(trm, "Multiple paths specified\n");
	}
	c_pp = find_paths_all(session_name, path_name, ctx, pp_clt,
			      &c_pp_clt, pp_srv, &c_pp_srv);

	if (c_pp > 1) {

		ERR(trm, "Multiple paths match '%s'\n", name);

		list_default(ctx);

		printf("Paths:\n");
		list_paths(pp_clt, c_pp_clt, pp_srv, c_pp_srv, false, ctx);

		ret = -EINVAL;
		goto out;
	}

	if (c_pp) {
		ret = show_path(pp_clt, pp_srv, ctx);
		if (ret == 0 && ctx->path_cnt > 1)
			ret = -EINVAL;
	} else {
		if (session_name)
			ERR(trm, "There is no path matching '%s %s'\n", session_name, path_name);
		else
			ERR(trm, "There is no path matching '%s'\n", path_name);
		ret = -ENOENT;
	}
out:
	free(pp_clt);
	free(pp_srv);
	return ret;
}

static int parse_name_help(int argc, const char *argv[], const char *what,
			   const struct param *cmd, struct rnbd_ctx *ctx)
{
	const char *word;

	if (argc <= 0) {
		word = strchr(what, ' ');
		if (word)
			word++; /* skip space */
		else
			word = what;
		cmd_print_usage_short(cmd, what, ctx);
		ERR(trm, "Please specify the %s argument\n", word);
		return -EINVAL;
	}
	if (!strcmp(*argv, "help")) {
		parse_help(argc, argv, NULL, ctx);

		cmd->help(what, cmd, ctx);
		return -EAGAIN;
	}
	ctx->name = *argv;

	return 0;
}

static int parse_option_name(int argc, const char *argv[],
			     const struct param *cmd, struct rnbd_ctx *ctx)
{
	if (argc <= 0)
		return 0;

	ctx->name = *argv;

	return 1;
}

static void help_show(const char *program_name,
		      const struct param *cmd,
		      const struct rnbd_ctx *ctx)
{
	cmd_print_usage_descr(cmd, "", ctx);

	printf("\nArguments:\n");
	print_opt("<name>",
		  "Name of an rnbd device, session, or path.");

	printf("\nOptions:\n");

	help_fields();

	print_fields(ctx, def_clms_devices_clt,
		     def_clms_devices_srv,
		     all_clms_devices_clt,
		     all_clms_devices_srv,
		     all_clms_devices, ctx->rnbdmode);
	print_fields(ctx, def_clms_sessions_clt,
		     def_clms_sessions_srv,
		     all_clms_sessions_clt,
		     all_clms_sessions_srv,
		     all_clms_sessions, ctx->rnbdmode);
	print_fields(ctx, def_clms_paths_clt,
		     def_clms_paths_srv,
		     all_clms_paths_clt,
		     all_clms_paths_srv,
		     all_clms_paths, ctx->rnbdmode);

	if (!ctx->help_set)
		printf("%sProvide 'all' to print all available fields\n\n",
		       HPRE);

	print_opt("{format}", "Output format: csv|json|xml");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");

	print_opt("help", "Display help and exit. [fields|all]");
}

static void help_show_devices(const char *program_name,
			      const struct param *cmd,
			      const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "devices";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<device>",
		  "Name of a local or a remote block device.");
	print_opt("",
		  "I.e. rnbd0, /dev/rnbd0, d12aef94-4110-4321-9373-3be8494a557b.");

	printf("\nOptions:\n");

	help_fields();

	print_fields(ctx, def_clms_devices_clt,
		     def_clms_devices_srv,
		     all_clms_devices_clt,
		     all_clms_devices_srv,
		     all_clms_devices, ctx->rnbdmode);

	print_opt("{format}", "Output format: csv|json|xml");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");

	print_opt("help", "Display help and exit. [fields|all]");
}

static void help_show_sessions(const char *program_name,
			       const struct param *cmd,
			       const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "sessions";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<session>",
		  "Session name or remote hostname.");
	print_opt("",
		  "I.e. ps401a-1@st401b-2, st401b-2, <ip1>@<ip2>, etc.");

	printf("\nOptions:\n");

	help_fields();

	print_fields(ctx, def_clms_sessions_clt,
		     def_clms_sessions_srv,
		     all_clms_sessions_clt,
		     all_clms_sessions_srv,
		     all_clms_sessions, ctx->rnbdmode);

	if (!ctx->help_set)
		printf("%sProvide 'all' to print all available fields\n\n",
		       HPRE);

	print_opt("{format}", "Output format: csv|json|xml");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");

	print_opt("help", "Display help and exit. [fields|all]");
}

static void help_default_paths(const char *program_name,
			       const struct param *cmd,
			       const struct rnbd_ctx *ctx)
{
	printf("\nArguments:\n");
	print_opt("[session]",
		  "Optional session name to select a path in the case paths");
	print_opt("", "with same addresses are used in multiple sessions.");
	print_opt("<path>",
		  "Name or identifier of a path:");
	print_opt("", "[pathname], [sessname:port]");
	print_opt("", "");
	print_opt("<hca_name>:<port>", "");
	print_opt("<hca_name>", "");
	print_opt("<port>", "alternative to path a hca/port specification");
	print_opt("", "might be provided.");
	print_opt("", "This requires that session name has been provided.");
}

static void help_show_paths(const char *program_name,
			    const struct param *cmd,
			    const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "paths";

	cmd_print_usage_descr(cmd, program_name, ctx);

	help_default_paths(program_name, cmd, ctx);

	printf("\nOptions:\n");

	help_fields();

	print_fields(ctx, def_clms_paths_clt,
		     def_clms_paths_srv,
		     all_clms_paths_clt,
		     all_clms_paths_srv,
		     all_clms_paths, ctx->rnbdmode);

	if (!ctx->help_set)
		printf("%sProvide 'all' to print all available fields\n\n",
		       HPRE);

	print_opt("{format}", "Output format: csv|json|xml");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");

	print_opt("help", "Display help and exit. [fields|all]");
}

static void help_map(const char *program_name,
		     const struct param *cmd,
		     const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "device ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<device>", "Path to the device to be mapped on server side");
	print_opt("from <server>",
		  "Address, hostname or session name of the server");

	printf("\nOptions:\n");
	print_opt("<path>", "Path(s) to establish: [src_addr@]dst_addr");
	print_opt("", "Address is [ip:]<ipv4>, [ip:]<ipv6> or gid:<gid>");

	print_opt("{rw}",
		  "Access permission on server side: ro|rw|migration. Default: rw");
	print_param_descr("verbose");
	print_param_descr("help");

	printf("\nExample:\n");
	print_opt("", "rnbd map 600144f0-e284-4932-8853-e86d54aaefe7 from st401a-8");
}

static int parse_path(const char *arg,
		      struct rnbd_ctx *ctx)
{
	if (!parse_path1(arg, ctx->paths+ctx->path_cnt))
		return -EINVAL;

	ctx->path_cnt++;

	return 0;
}

static struct rnbd_sess *find_single_session(const char *session_name,
					      struct rnbd_ctx *ctx,
					      struct rnbd_sess **sessions,
					      int sess_cnt, bool print_err)
{
	struct rnbd_sess **matching_sess, *res = NULL;
	int match_count;

	if (!sess_cnt) {
		ERR(trm, "Session '%s' not found: no sessions open\n",
		    session_name);
		return NULL;
	}

	res = find_sess(session_name, sessions);
	if (res)
		/* of there is an exact match, that's the one we want */
		return res;

	matching_sess = calloc(sess_cnt, sizeof(*matching_sess));

	if (sess_cnt && !matching_sess) {
		ERR(trm, "Failed to alloc memory\n");
		return NULL;
	}
	match_count = find_sess_match(session_name, ctx->rnbdmode, sessions,
				      matching_sess);

	if (match_count == 1) {
		res = *matching_sess;
	} else if (print_err) {
		ERR(trm, "%s '%s'.\n",
		    (match_count > 1)  ?
			"Please specify the session uniquely. There are multiple sessions matching"
			: "No session found matching",
		    session_name);
	}

	free(matching_sess);
	return res;
}

static struct rnbd_path *find_single_path(const char *session_name,
					  const char *path_name,
					  struct rnbd_ctx *ctx,
					  struct rnbd_path **paths,
					  int path_cnt, bool print_err)
{
	struct rnbd_path **matching_paths, *res = NULL;
	int match_count;
	char *base_path_name;

	if (!path_cnt) {
		if (print_err)
			ERR(trm, "Path '%s' not found: there exists no paths\n",
			    path_name);
		return NULL;
	}

	matching_paths = calloc(path_cnt, sizeof(*matching_paths));

	if (path_cnt && !matching_paths) {
		ERR(trm, "Failed to alloc memory\n");
		return NULL;
	}
	match_count = find_paths(session_name, path_name, ctx, paths, matching_paths);
	if (match_count == 0 && path_name && strchr(path_name, '%') != NULL) {
		INF(ctx->debug_set,
		    "Retry to find path for name %s ignoring interface.\n",
		    path_name);
		base_path_name = strdup(path_name);
		if (base_path_name) {
			*strchr(base_path_name, '%') = '\0';
			match_count = find_paths(session_name, base_path_name, ctx, paths, matching_paths);
			free(base_path_name);
		}
	}
	if (match_count == 1) {
		res = *matching_paths;
	} else {
		if (print_err) {
			if (path_name)
				ERR(trm, "%s '%s'.\n",
				    (match_count > 1)  ?
				    "Please specify the path uniquely. There are multiple paths matching"
				    : "No path found matching",
				    path_name);
			else
				ERR(trm, "%s '%s'.\n",
				    "Please specify the path for session",
				    session_name);
		}
	}

	free(matching_paths);
	return res;
}

static struct rnbd_path *find_first_path_for_session(const char *session_name,
						     struct rnbd_path **paths,
						     int path_cnt)
{
	struct rnbd_path **matching_paths, *res = NULL;
	int match_count;

	if (!path_cnt) {
		ERR(trm, "Path '%s' not found: there exists no paths\n",
		    session_name);
		return NULL;
	}

	matching_paths = calloc(path_cnt, sizeof(*matching_paths));

	if (path_cnt && !matching_paths) {
		ERR(trm, "Failed to alloc memory\n");
		return NULL;
	}
	match_count = find_paths_for_session(session_name, paths, matching_paths);

	if (match_count > 0)
		res = *matching_paths;

	free(matching_paths);

	return res;
}

static int client_devices_map(const char *from_name, const char *device_name,
			      struct rnbd_ctx *ctx)
{
	char cmd[4096], sessname[NAME_MAX];
	struct rnbd_sess *sess = NULL;
	struct rnbd_path *path;
	int i, cnt = 0, ret;

	if (!from_name && ctx->path_cnt) {

		/* User provided only a path to designate a session to use. */

		path = find_single_path(NULL, ctx->paths[0].dst, ctx,
					paths_clt, paths_clt_cnt, true);
		if (path) {
			sess = path->sess;
			INF(ctx->debug_set,
			    "map matched session %s for path name %s.\n",
			    sess->sessname, ctx->paths[0].dst);
			strcpy(sessname, sess->sessname);
		} else {
			ERR(trm,
			    "Client session for path '%s' not found. Please provide a session name to establish a new session.\n",
			    ctx->paths[0].dst);
			return -EINVAL;
		}
	}
	if (!sess) {

		/* Try to match a session in any case */
		sess = find_single_session(from_name, ctx, sess_clt,
					   sds_clt_cnt, false);

		if (sess) {
			INF(ctx->debug_set,
			    "map matched session %s for name %s.\n",
			    sess->sessname, from_name);
			strcpy(sessname, sess->sessname);
		} else {

			INF(ctx->debug_set,
			    "map found no matching session for %s.\n",
			    from_name);

			if (!ctx->path_cnt) {
				/* if still not found, generate the session name */
				ret = sessname_from_host(from_name,
							 sessname, sizeof(sessname));
				if (ret) {
					ERR(trm,
					    "Failed to generate session name for %s: %s (%d)\n",
					    from_name, strerror(-ret), ret);
					return ret;
				}

				/* if no path provided by user, */
				/* try to resolve from_name as host name */
				ret = resolve_host(from_name, ctx->paths, ctx);
				if (ret < 0) {
					INF(ctx->debug_set,
					    "Failed to resolve host name for %s: %s (%d)\n",
					    from_name, strerror(-ret), ret);
					ret = 0;
				} else if (ret == 0) {
					INF(ctx->debug_set,
					    "Found no paths to host %s.\n",
					    from_name);
				} else {
					ctx->path_cnt = ret;
				}
			} else {
				/* we are going to create a new session for an path address */
				/* use session name as given by user                        */
				strncpy(sessname, from_name, sizeof(sessname));
			}
		}
	}
	if (sess && ctx->path_cnt && from_name) {
		INF(ctx->verbose_set,
		    "Session '%s' exists. Provided paths will be ignored by the driver. Please use addpath to add a path to an existsing sesion.\n",
		    from_name);
	}
	if (!sess && !ctx->path_cnt) {
		ERR(trm,
		    "No client session '%s' found and '%s' can not be resolved as host name.\n"
		    "Please provide a host name or at least one path to establish a new session.\n",
		    from_name, from_name);
		return -EINVAL;
	}

	cnt = snprintf(cmd, sizeof(cmd), "sessname=%s", sessname);
	cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt, " device_path=%s",
			device_name);

	for (i = 0; i < ctx->path_cnt; i++)
		if (ctx->paths[i].src)
			cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt,
					" path=%s@%s",
					ctx->paths[i].src, ctx->paths[i].dst);
		else
			cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt,
					" path=%s",
					ctx->paths[i].dst);

	if (sess)
		for (i = 0; i < sess->path_cnt; i++)
			cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt,
					" path=%s@%s", sess->paths[i]->src_addr,
					sess->paths[i]->dst_addr);

	if (ctx->access_mode_set)
		cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt, " access_mode=%s",
				ctx->access_mode);

	ret = printf_sysfs(get_sysfs_info(ctx)->path_dev_clt,
			   "map_device", ctx, "%s", cmd);
	if (ret)
		if (ctx->sysfs_avail)
			ERR(trm, "Failed to map device: %s (%d)\n",
			    strerror(-ret), ret);
		else
			ERR(trm, "Failed to map device: modules not loaded.\n");
	else
		INF(ctx->verbose_set, "Successfully mapped '%s' from '%s'.\n",
		    device_name, from_name ? from_name : sessname);

	return ret;
}

static struct rnbd_sess_dev *find_single_device(const char *name,
						struct rnbd_ctx *ctx,
						struct rnbd_sess_dev **devs,
						int dev_cnt, bool print_err)
{
	struct rnbd_sess_dev *res = NULL, **matching_devs;
	int match_count;

	if (!dev_cnt) {
		ERR(trm,
		    "Device '%s' not found: no devices mapped\n", name);
		return NULL;
	}

	matching_devs = calloc(dev_cnt, sizeof(*matching_devs));
	if (!matching_devs) {
		ERR(trm, "Failed to allocate memory\n");
		return NULL;
	}

	match_count = find_devices(name, devs, matching_devs);
	if (match_count == 1) {

		res = matching_devs[0];
	} else if (print_err) {
		ERR(trm, "%s '%s'.\n",
		    (match_count > 1)  ?
			"Please specify an unique device. There are multiple devices matching"
			: "Did not found device",
		    name);
	}

	free(matching_devs);
	return res;
}

static int client_devices_resize(const char *device_name, uint64_t size_sect,
				 struct rnbd_ctx *ctx)
{
	const struct rnbd_sess_dev *ds;
	char tmp[PATH_MAX];
	int ret;

	ds = find_single_device(device_name, ctx, sds_clt, sds_clt_cnt, true/*print_err*/);
	if (!ds)
		return -EINVAL;

	sprintf(tmp, "/sys/block/%s/%s", ds->dev->devname,
		get_sysfs_info(ctx)->path_dev_name);
	ret = printf_sysfs(tmp, "resize", ctx, "%" PRIu64, size_sect);
	if (ret)
		ERR(trm, "Failed to resize %s to %" PRIu64 ": %s (%d)\n",
		    ds->dev->devname, size_sect, strerror(-ret), ret);
	else
		INF(ctx->verbose_set,
		    "Device '%s' resized sucessfully to %" PRIu64 " sectors.\n",
		    ds->dev->devname, size_sect);

	return ret;
}

static void help_resize(const char *program_name,
			const struct param *cmd,
			const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<device name or path or mapping path> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<device>", "Name of the device to be resized");
	print_opt("<size>", "New size of the device in bytes");
	print_opt("{unit}", "Units to use for size (in binary): B|K|M|G|T|P|E");

	printf("\nOptions:\n");
	print_param_descr("verbose");
	print_param_descr("help");
}

static void help_unmap(const char *program_name,
		       const struct param *cmd,
		       const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<device name or path or mapping path> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<device>", "Name of the device to be unmapped");

	printf("\nOptions:\n");
	print_param_descr("force");
	print_param_descr("verbose");
	print_param_descr("help");
}

static int _client_devices_unmap(const struct rnbd_sess_dev *ds, bool force,
				struct rnbd_ctx *ctx)
{
	char tmp[PATH_MAX];
	int ret;

	sprintf(tmp, "/sys/block/%s/%s", ds->dev->devname,
		get_sysfs_info(ctx)->path_dev_name);

	ret = printf_sysfs(tmp, "unmap_device", ctx, "%s",
			   force ? "force" : "normal");
	if (ret)
		ERR(trm, "Failed to %sunmap '%s': %s (%d)\n",
		    force ? "force-" : "",
		    ds->dev->devname, strerror(-ret), ret);
	else
		INF(ctx->verbose_set, "Device '%s' sucessfully unmapped.\n",
		    ds->dev->devname);

	return ret;
}

static int client_devices_unmap(const char *device_name, bool force,
				struct rnbd_ctx *ctx)
{
	const struct rnbd_sess_dev *ds;

	ds = find_single_device(device_name, ctx, sds_clt, sds_clt_cnt, true/*print_err*/);
	if (!ds)
		return -EINVAL;

	return _client_devices_unmap(ds, force, ctx);
}

static int client_device_remap(const struct rnbd_dev *dev,
			       struct rnbd_ctx *ctx)
{
	char tmp[PATH_MAX];
	int ret;

	sprintf(tmp, "/sys/block/%s/%s", dev->devname,
		get_sysfs_info(ctx)->path_dev_name);

	ret = printf_sysfs(tmp, "remap_device", ctx, "1");
	if (ret == -EALREADY) {
		INF(ctx->verbose_set,
		    "Device '%s' does not need to be remapped.\n",
		    dev->devname);
		ret = 0;
	} else if (ret) {
		ERR(trm, "Failed to remap %s: %s (%d)\n",
		    dev->devname, strerror(-ret), ret);
	} else {
		INF(ctx->verbose_set,
		    "Device '%s' sucessfully remapped.\n",
		    dev->devname);
	}
	return ret;
}

static int client_devices_remap(const char *device_name, struct rnbd_ctx *ctx)
{
	const struct rnbd_sess_dev *ds;

	ds = find_single_device(device_name, ctx, sds_clt, sds_clt_cnt, true/*print_err*/);
	if (!ds)
		return -EINVAL;

	return client_device_remap(ds->dev, ctx);
}

static int client_session_remap(const char *session_name,
				struct rnbd_ctx *ctx)
{
	int tmp_err, err = 0;
	int cnt, i;
	struct rnbd_sess_dev *const *sds_iter;
	const struct rnbd_sess *sess;
	char cmd[4096];

	if (!ctx->sysfs_avail)
		ERR(trm, "Not possible to remap devices: modules not loaded.\n");

	if (!sds_clt_cnt) {
		ERR(trm,
		    "No devices mapped. Nothing to be done!\n");
		return -EINVAL;
	}
	sess = find_single_session(session_name, ctx, sess_clt,
				   sds_clt_cnt, true);
	if (!sess)
		return -EINVAL;

	if (!ctx->force_set) {
		for (sds_iter = sds_clt; *sds_iter; sds_iter++) {

			if ((*sds_iter)->sess == sess) {
				tmp_err = client_device_remap((*sds_iter)->dev, ctx);
				/*  intentional continue on error */
				if (!err && tmp_err)
					err = tmp_err;
			}
		}
		return err;
	}
	for (sds_iter = sds_clt; *sds_iter; sds_iter++) {

		if ((*sds_iter)->sess == sess) {
			tmp_err = _client_devices_unmap((*sds_iter), 0, ctx);
			/*  intentional continue on error */
			if (tmp_err)
				ERR(trm, "Failed to unmap device: %s, %s (%d)\n",
				    (*sds_iter)->dev->devname, strerror(-tmp_err), tmp_err);
		}
	}
	/* All devices should be unmapped now and */
	/* therefor session should be closed.     */
	/* We have a race condition here with     */
	/* simultanous map/unmap commands         */
	/* which involve the same hosts.          */
	for (sds_iter = sds_clt; *sds_iter; sds_iter++) {

		if ((*sds_iter)->sess == sess) {
			cnt = snprintf(cmd, sizeof(cmd), "sessname=%s", sess->sessname);
			cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt, " device_path=%s",
					(*sds_iter)->mapping_path);
			for (i = 0; i < sess->path_cnt; i++)
				cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt,
						" path=%s@%s", sess->paths[i]->src_addr,
						sess->paths[i]->dst_addr);
			cnt += snprintf(cmd + cnt, sizeof(cmd) - cnt, " access_mode=%s",
					(*sds_iter)->access_mode);
			tmp_err = printf_sysfs(get_sysfs_info(ctx)->path_dev_clt,
					       "map_device", ctx, "%s", cmd);
			
			if (tmp_err) {
				ERR(trm, "Failed to map device: %s (%d)\n",
				    strerror(-tmp_err), tmp_err);
				if (!err)
					err = tmp_err;
			}  else { 
				INF(ctx->verbose_set, "Successfully mapped '%s' from '%s'.\n",
				    (*sds_iter)->dev->devname, sess->sessname);
			}
		}
	}
	return err;
}

static int session_do_all_paths(enum rnbdmode mode,
				const char *session_name,
				int (*do_it)(const char *session_name,
					     const char *path_name,
					     struct rnbd_ctx *ctx),
				struct rnbd_ctx *ctx)
{
	int err = 0;
	struct rnbd_path *const *paths_iter;
	const struct rnbd_sess *sess;

	if (!(mode == RNBD_CLIENT ?
	      sess_clt_cnt
	      : sess_srv_cnt)) {
		ERR(trm,
		    "No sessions opened!\n");
		return -EINVAL;
	}

	if (mode == RNBD_CLIENT)
		sess = find_single_session(session_name, ctx,
					   sess_clt, sess_clt_cnt,
					   true);
	else
		sess = find_single_session(session_name, ctx,
					   sess_srv, sess_srv_cnt,
					   true);

	if (!sess)
		/*find_single_session has printed an error message*/
		return -EINVAL;

	for (paths_iter = (mode == RNBD_CLIENT ? paths_clt : paths_srv);
	     *paths_iter && !err; paths_iter++) {

		if ((*paths_iter)->sess == sess)
			err = do_it(session_name, (*paths_iter)->pathname, ctx);
	}
	return err;
}

static void help_remap(const char *program_name,
		       const struct param *cmd,
		       const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<device name> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<identifier>", "Identifier of a device to be remapped.");

	printf("\nOptions:\n");
	print_param_descr("force");
	print_param_descr("verbose");
	print_param_descr("help");
}

static void help_remap_device_or_session(const char *program_name,
					 const struct param *cmd,
					 const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<name> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<identifier>",
		  "Identifier of a device or session to be remapped.");
	print_opt("", "If identifier designates a session,");
	print_opt("",
		  "all devices of this particular session will be remapped.");

	printf("\nOptions:\n");
	print_param_descr("force");
	print_opt("",
		  "When provided, all devices will be unmapped and mapped again.");
	printf("\n");
	print_param_descr("verbose");
	print_param_descr("help");
}

static void help_remap_session(const char *program_name,
			       const struct param *cmd,
			       const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<session name> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<session>",
		  "Identifier of a session to remap all devices on.");

	printf("\nOptions:\n");
	print_param_descr("force");
	print_opt("",
		  "When provided, all devices will be unmapped and mapped again.");
	printf("\n");
	print_param_descr("verbose");
	print_param_descr("help");

}

static void help_close_device(const char *program_name,
			      const struct param *cmd,
			      const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<device name> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<device>",
		  "Identifier of a device to be closed.");

	printf("\nOptions:\n");
	print_opt("<session>",
		  "Identifier of a session for which the device is to be closed.");
	print_param_descr("force");
	print_param_descr("verbose");
	print_param_descr("help");

	printf("\nExample:\n");
	print_opt("", "rnbd close 600144f0-e284-4932-8853-e86d54aaefe7");
}

static void help_reconnect_session(const char *program_name,
				   const struct param *cmd,
				   const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<session> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<session>", "Name or identifier of a session.");

	printf("\nOptions:\n");
	print_param_descr("verbose");
	print_param_descr("help");
}

static void help_recover_device(const char *program_name,
				   const struct param *cmd,
				   const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<device> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<device>", "Name or identifier of a device.");

	printf("\nOptions:\n");
	print_param_descr("all");
	print_param_descr("verbose");
	print_param_descr("help");

	printf("\nExample:\n");
	print_opt("", "rnbd recover 600144f0-e284-4932-8853-e86d54aaefe7");
}

static void help_recover_session(const char *program_name,
				   const struct param *cmd,
				   const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<session>|all ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<session>|all", "Name or identifier of a session.");
	print_opt("", "All recovers all sessions.");

	printf("\nOptions:\n");
	print_param_descr("add-missing");
	print_param_descr("verbose");
	print_param_descr("help");

	printf("\nExample:\n");
	print_opt("", "rnbd recover ps402a-905@st401a-8");
}

static void help_recover_path(const char *program_name,
			      const struct param *cmd,
			      const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<path> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	help_default_paths(program_name, cmd, ctx);

	printf("\nOptions:\n");
	print_param_descr("verbose");
	print_param_descr("help");
}

static void help_recover_device_session_or_path(const char *program_name,
						const struct param *cmd,
						const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<path> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<device>|<session>|<path>|all", "");
	print_opt("",
		  "Name of device, session, or path to recover");
	print_opt("", "'all' will recover all sessions and devices.");

	printf("\nOptions:\n");
	print_opt("<path>",
		  "Optional argument to identify a path in the context of a session");
	print_param_descr("add-missing");
	print_param_descr("verbose");
	print_param_descr("help");

	printf("\nExample:\n");
	print_opt("", "rnbd recover all");
	print_opt("recover a device:", "");
	print_opt("", "rnbd recover 600144f0-e284-4932-8853-e86d54aaefe7");
	print_opt("recover a session:", "");
	print_opt("", "rnbd recover ps402a-905@st401a-8");
}

static void help_reconnect_path(const char *program_name,
				const struct param *cmd,
				const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<path> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	help_default_paths(program_name, cmd, ctx);

	printf("\nOptions:\n");
	print_param_descr("verbose");
	print_param_descr("help");
}

static void help_disconnect_session(const char *program_name,
				    const struct param *cmd,
				    const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<session> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<session>", "Name or identifier of a session.");

	printf("\nOptions:\n");
	print_param_descr("verbose");
	print_param_descr("help");
}

static void help_disconnect_path(const char *program_name,
				 const struct param *cmd,
				 const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<path> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	help_default_paths(program_name, cmd, ctx);

	printf("\nOptions:\n");
	print_param_descr("verbose");
	print_param_descr("help");
}

static int client_session_add(const char *session_name,
			      const struct path *path,
			      struct rnbd_ctx *ctx)
{
	char address_string[4096];
	char sysfs_path[4096];
	struct rnbd_sess *sess;
	int ret;

	sess = find_sess(session_name, sess_clt);

	if (!sess) {
		ERR(trm,
		    "Session '%s' does not exists.\n", session_name);
		return -EINVAL;
	}

	snprintf(sysfs_path, sizeof(sysfs_path), "%s%s",
		 get_sysfs_info(ctx)->path_sess_clt, sess->sessname);
	if (path->src)
		snprintf(address_string, sizeof(address_string),
			 "%s,%s", path->src, path->dst);
	else
		snprintf(address_string, sizeof(address_string),
			 "%s", path->dst);

	ret = printf_sysfs(sysfs_path, "add_path", ctx, "%s", address_string);
	if (ret)
		ERR(trm,
		    "Failed to add path '%s' to session '%s': %s (%d)\n",
		    address_string, sess->sessname, strerror(-ret), ret);
	else
		INF(ctx->verbose_set, "Successfully added path '%s' to '%s'.\n",
		    address_string, sess->sessname);
	return ret;
}

static void help_addpath(const char *program_name,
			 const struct param *cmd,
			 const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<session> <path> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	printf("\nArguments:\n");
	print_opt("<session>",
		  "Name of the session to add the new path to");
	print_opt("<path>",
		  "Path to be added: [src_addr@]dst_addr");
	print_opt("",
		  "Address is of the form ip:<ipv4>, ip:<ipv6> or gid:<gid>");

	printf("\nOptions:\n");
	print_param_descr("verbose");
	print_param_descr("help");
}

static void help_delpath(const char *program_name,
			 const struct param *cmd,
			 const struct rnbd_ctx *ctx)
{
	if (!program_name)
		program_name = "<path> ";

	cmd_print_usage_descr(cmd, program_name, ctx);

	help_default_paths(program_name, cmd, ctx);

	printf("\nOptions:\n");
	print_param_descr("verbose");
	print_param_descr("help");
}

static int client_path_do(const char *session_name,
			  const char *path_name,
			  const char *sysfs_entry,
			  const char *message_success,
			  const char *message_fail, struct rnbd_ctx *ctx)
{
	char sysfs_path[4096];
	struct rnbd_path *path;
	int ret;

	path = find_single_path(session_name, path_name,
				ctx, paths_clt, paths_clt_cnt, true);

	if (!path)
		return -EINVAL;

	snprintf(sysfs_path, sizeof(sysfs_path), "%s%s/paths/%s",
		 get_sysfs_info(ctx)->path_sess_clt,
		 path->sess->sessname, path->pathname);

	ret = printf_sysfs(sysfs_path, sysfs_entry, ctx, "1");
	if (ret)
		ERR(trm, message_fail, path->pathname,
		    path->sess->sessname, strerror(-ret), ret);
	else
		INF(ctx->verbose_set, message_success,
		    path->pathname, path->sess->sessname);
	return ret;
}

static int client_path_delete(const char *session_name,
			      const char *path_name,
			      struct rnbd_ctx *ctx)
{
	return client_path_do(session_name, path_name, "remove_path",
			      "Successfully removed path '%s' from '%s'.\n",
			      "Failed to remove path '%s' from session '%s': %s (%d)\n",
			      ctx);
}

static int client_path_reconnect(const char *session_name,
				 const char *path_name,
				 struct rnbd_ctx *ctx)
{
	return client_path_do(session_name, path_name, "reconnect",
			      "Successfully reconnected path '%s' of session '%s'.\n",
			      "Failed to reconnect path '%s' from session '%s': %s (%d)\n",
			      ctx);
}

static int client_path_recover(const char *session_name,
			       const char *path_name,
			       struct rnbd_ctx *ctx)
{
	struct rnbd_path *path;

	if (!session_name && !strcmp(path_name, "all")) {
		ERR(trm, "Please provide session to recover all paths for\n");
		return -EINVAL;
	} else if (session_name && !strcmp(path_name, "all")) {
		path = find_single_path(session_name, path_name, ctx, paths_clt,
					paths_clt_cnt, false);
		if (!path)
			return session_do_all_paths(RNBD_CLIENT, session_name,
						    client_path_recover, ctx);
	} else {
		path = find_single_path(session_name, path_name, ctx, paths_clt,
					paths_clt_cnt, true);
	}

	if (!path)
		return -EINVAL;

	/*
	 * Return success for connected paths
	 */
	if (!strcmp(path->state, "connected")) {
		INF(ctx->debug_set,
		    "Path '%s' is connected, skipping.\n",
		    path_name);
		return 0;
	}

	INF(ctx->debug_set, "Path '%s' is '%s', recovering.\n",
		    path_name, path->state);

	return client_path_do(session_name, path_name, "reconnect",
			      "Successfully reconnected path '%s' of session '%s'.\n",
			      "Failed to reconnect path '%s' from session '%s': %s (%d)\n",
			      ctx);
}

static int client_path_disconnect(const char *session_name,
				  const char *path_name,
				  struct rnbd_ctx *ctx)
{
	return client_path_do(session_name, path_name, "disconnect",
			      "Successfully disconnected path '%s' from session '%s'.\n",
			      "Failed to disconnect path '%s' of session '%s': %s (%d)\n",
			      ctx);
}

static int client_path_readd(const char *session_name,
			     const char *path_name,
			     struct rnbd_ctx *ctx)
{
	char sysfs_path[4096];
	struct rnbd_path *path;
	int ret;

	path = find_single_path(session_name, path_name, ctx, paths_clt,
				paths_clt_cnt, true);

	if (!path)
		return -EINVAL;

	snprintf(sysfs_path, sizeof(sysfs_path), "%s%s/paths/%s",
		 get_sysfs_info(ctx)->path_sess_clt,
		 path->sess->sessname, path->pathname);

	ret = printf_sysfs(sysfs_path, "remove_path", ctx, "1");
	if (ret) {
		ERR(trm,
		    "Failed to remove path '%s' from session '%s': %s (%d)\n",
		    path->pathname,
		    path->sess->sessname, strerror(-ret), ret);
		return ret;
	}
	INF(ctx->verbose_set, "Successfully removed path '%s' from '%s'.\n",
	    path->pathname, path->sess->sessname);

	snprintf(sysfs_path, sizeof(sysfs_path), "%s%s",
		 get_sysfs_info(ctx)->path_sess_clt,
		 path->sess->sessname);

	if (strncmp("ip:fe80:", path_name, 8) == 0 && strchr(path_name, '%') != NULL) {
		/* for a link local IPv6 address use the original address string */
		/* only here not for remove_path */
		ret = printf_sysfs(sysfs_path, "add_path", ctx, "%s", path_name);
	} else {
		ret = printf_sysfs(sysfs_path, "add_path", ctx, "%s@%s", path->src_addr, path->dst_addr);
	}
	if (ret)
		ERR(trm,
		    "Failed to readd path '%s' to session '%s': %s (%d)\n",
		    path->sess->sessname, path->pathname, strerror(-ret), ret);
	else
		INF(ctx->verbose_set,
		    "Successfully readded path '%s' to '%s'.\n",
		    path->pathname, path->sess->sessname);
	return ret;
}

static int server_path_disconnect(const char *session_name,
				  const char *path_name,
				  struct rnbd_ctx *ctx)
{
	char sysfs_path[4096];
	struct rnbd_path *path;
	int ret;

	path = find_single_path(session_name, path_name, ctx, paths_srv,
				paths_srv_cnt, true);

	if (!path)
		return -EINVAL;

	snprintf(sysfs_path, sizeof(sysfs_path), "%s%s/paths/%s",
		 get_sysfs_info(ctx)->path_sess_srv,
		 path->sess->sessname, path->pathname);

	ret = printf_sysfs(sysfs_path, "disconnect", ctx, "1");
	if (ret)
		ERR(trm,
		    "Failed to disconnect path '%s' of session '%s': %s (%d)\n",
		    path->pathname,
		    path->sess->sessname, strerror(-ret), ret);
	else
		INF(ctx->verbose_set,
		    "Successfully disconnected path '%s' from session '%s'.\n",
		    path->pathname, path->sess->sessname);
	return ret;
}

static int server_devices_force_close(const char *device_name,
				      const char *session_name,
				      struct rnbd_ctx *ctx)
{
	char sysfs_path[4096];
	int ret;

	snprintf(sysfs_path, sizeof(sysfs_path), "%s/devices/%s/sessions/%s",
		 get_sysfs_info(ctx)->path_dev_srv,
		 device_name, session_name);

	ret = printf_sysfs(sysfs_path, "force_close", ctx, "1");
	if (ret)
		ERR(trm,
		    "Failed to close device '%s' for session '%s': %s (%d)\n",
		    device_name, session_name,
		    strerror(-ret), ret);
	else
		INF(ctx->verbose_set,
		    "Successfully closed device '%s' for session '%s'.\n",
		    device_name, session_name);

	return ret;
}

static struct param _cmd_dump_all =
	{TOK_DUMP, "dump",
		"Dump information about all",
		"",
		"Dump information about all rnbd objects.",
		NULL, NULL, help_dump_all};
static struct param _cmd_list_devices =
	{TOK_LIST, "list",
		"List information on all",
		"s",
		"List information on devices.",
		NULL, NULL, help_list_devices};
static struct param _cmd_list_sessions =
	{TOK_LIST, "list",
		"List information on all",
		"s",
		"List information on sessions.",
		NULL, NULL, help_list_sessions};
static struct param _cmd_list_paths =
	{TOK_LIST, "list",
		"List information on all",
		"s",
		"List information on paths.",
		NULL, NULL, help_list_paths};
static struct param _cmd_show =
	{TOK_SHOW, "show",
		"Show information about the object that is designated by <name>",
		"",
		"Show information about an rnbd device, session, or path.",
	 "<name>",
		NULL, help_show};
static struct param _cmd_show_devices =
	{TOK_SHOW, "show",
		"Show information about a",
		"",
		"Show information about an rnbd block device.",
		"<device>",
		NULL, help_show_devices};
static struct param _cmd_show_sessions =
	{TOK_SHOW, "show",
		"Show information about a",
		"",
		"Show information about an rnbd session.",
		"<session>",
		NULL, help_show_sessions};
static struct param _cmd_show_paths =
	{TOK_SHOW, "show",
		"Show information about a",
		"",
		"Show information about an rnbd transport path.",
		"[session] <path>",
		NULL, help_show_paths};
static struct param _cmd_map =
	{TOK_MAP, "map",
		"Map a",
		" from a given server",
		"Map a device from a given server",
		"<device> from <server>",
		 NULL, help_map};
static struct param _cmd_resize =
	{TOK_RESIZE, "resize",
		"Resize a mapped",
		"",
		"Change size of a mapped device",
		"<device> <size>",
		 NULL, help_resize};
static struct param _cmd_unmap =
	{TOK_UNMAP, "unmap",
		"Unmap an imported",
		"",
		"Unmap a given imported device",
		"<device>",
		NULL, help_unmap};
static struct param _cmd_remap =
	{TOK_REMAP, "remap",
		"Remap a",
		"",
		"Remap an imported device",
		"<device>",
		 NULL, help_remap};
static struct param _cmd_remap_device_or_session =
	{TOK_REMAP, "remap",
		"Remap a",
		"",
		"Remap a devices or all devices of a given session",
		"<device|session>",
		 NULL, help_remap_device_or_session};
static struct param _cmd_remap_session =
	{TOK_REMAP, "remap",
		"Remap all devicess on a",
		"",
		"Remap all devices of a given session",
		"<session>",
		 NULL, help_remap_session};
static struct param _cmd_close_device =
	{TOK_CLOSE, "close",
		"Close a",
		" for a session",
		"Close a particular device for a given session",
		"<device>",
		 NULL, help_close_device};
static struct param _cmd_client_recover_device =
	{TOK_RECOVER, "recover",
		"Recover a",
		"",
		"Recover a device: recover a device when it is not open.",
		"<device>|all",
		 NULL, help_recover_device};
static struct param _cmd_recover_device_session_or_path =
	{TOK_RECOVER, "recover",
		"Recover a",
		"",
		"Recover a device, session, or path.",
		"<device>|<session>|<path>|all",
		 NULL, help_recover_device_session_or_path};
static struct param _cmd_disconnect_session =
	{TOK_DISCONNECT, "disconnect",
		"Disconnect a",
		"",
		"Disconnect all paths on a given session",
		"<session>",
		NULL, help_disconnect_session};
static struct param _cmd_dis_session =
	{TOK_DISCONNECT, "dis",
		"Disconnect a",
		"",
		"Disconnect all paths on a given session",
		"<session>",
		NULL, help_disconnect_session};
static struct param _cmd_disconnect_path =
	{TOK_DISCONNECT, "disconnect",
		"Disconnect a",
		"",
		"Disconnect a path of a given session",
		"[session] <path>",
		NULL, help_disconnect_path};
static struct param _cmd_dis_path =
	{TOK_DISCONNECT, "dis",
		"Disconnect a",
		"",
		"Disconnect a path of a given session",
		"[session] <path>",
		NULL, help_disconnect_path};
static struct param _cmd_reconnect_session =
	{TOK_RECONNECT, "reconnect",
		"Reconnect a",
		"",
		"Disconnect and connect again a whole session",
		"<session>",
		 NULL, help_reconnect_session};
static struct param _cmd_recover_session =
	{TOK_RECOVER, "recover",
		"Recover a",
		"",
		"Recover a session: reconnect disconnected paths.",
		"<session>|all [add-missing]",
		 NULL, help_recover_session};
static struct param _cmd_reconnect_path =
	{TOK_RECONNECT, "reconnect",
		"Reconnect a",
		"",
		"Disconnect and connect again a single path of a session",
		"[session] <path>",
		 NULL, help_reconnect_path};
static struct param _cmd_recover_path =
	{TOK_RECOVER, "recover",
		"Recover a",
		"",
		"Recover a path: reconnect if not in connected state.",
		"[session] <path>|all",
		 NULL, help_recover_path};
static struct param _cmd_add =
	{TOK_ADD, "add",
		"Add a",
		" to an existing session",
		"Add a new path to an existing session",
		"<session> <path>",
		 NULL, help_addpath};
static struct param _cmd_delete =
	{TOK_DELETE, "delete",
		"Delete a",
		"",
		"Delete a given path from the corresponding session",
		"[session] <path>",
		 NULL, help_delpath};
static struct param _cmd_del =
	{TOK_DELETE, "del",
		"Delete a",
		"",
		"Delete a given path from the corresponding session",
		"[session] <path>",
		 NULL, help_delpath};
static struct param _cmd_readd =
	{TOK_READD, "readd",
		"Readd a",
		"",
		"Delete and add again a given path to the corresponding session",
		"[session] <path>",
		 NULL, help_delpath};
static struct param _cmd_help =
	{TOK_HELP, "help",
		"Display help on",
		"s",
		"Display help message and exit.",
		NULL, NULL, help_object};
static struct param _cmd_null =
		{ 0 };

static struct param *params_flags[] = {
	&_params_minus_minus_help,
	&_params_minus_h,
	&_params_minus_minus_verbose,
	&_params_minus_v,
	&_params_minus_minus_debug,
	&_params_minus_d,
	&_params_minus_minus_simulate,
	&_params_minus_s,
	&_params_minus_c,
	&_params_minus_minus_complete,
	&_params_minus_minus_version,
	&_params_null
};

static struct param *params_flags_help[] = {
	&_params_minus_minus_help,
	&_params_minus_minus_verbose,
	&_params_minus_minus_debug,
	&_params_minus_minus_simulate,
	&_params_null
};

static struct param *params_default[] = {
	&_params_help,
	&_params_verbose,
	&_params_minus_v,
	&_params_null
};

static struct param *params_mode[] = {
	&_params_client,
	&_params_clt,
	&_params_cli,
	&_params_server,
	&_params_serv,
	&_params_srv,
	&_params_both,
	&_params_help,
	&_params_version,
	&_params_minus_minus_version,
	&_params_null
};

static struct param *params_mode_help[] = {
	&_params_client,
	&_params_server,
	&_params_help,
	&_params_version,
	&_params_null
};

static struct param *params_both[] = {
	&_params_devices,
	&_params_device,
	&_params_devs,
	&_params_dev,
	&_params_sessions,
	&_params_session,
	&_params_sess,
	&_params_paths,
	&_params_path,
	&_cmd_list_devices,
	&_cmd_dump_all,
	&_cmd_show,
	&_cmd_map,
	&_cmd_resize,
	&_cmd_unmap,
	&_cmd_remap,
	&_cmd_recover_device_session_or_path,
	&_params_help,
	&_params_null
};

static struct param *params_both_help[] = {
	&_params_devices,
	&_params_sessions,
	&_params_paths,
	&_params_help,
	&_params_null
};

static struct param *params_object_type_client[] = {
	&_params_devices,
	&_params_device,
	&_params_devs,
	&_params_dev,
	&_params_sessions,
	&_params_session,
	&_params_sess,
	&_params_paths,
	&_params_path,
	&_cmd_dump_all,
	&_cmd_list_devices,
	&_cmd_show,
	&_cmd_map,
	&_cmd_resize,
	&_cmd_unmap,
	&_cmd_remap_device_or_session,
	&_cmd_recover_device_session_or_path,
	&_params_help,
	&_params_null
};

static struct param *params_object_type_server[] = {
	&_params_devices,
	&_params_device,
	&_params_devs,
	&_params_dev,
	&_params_sessions,
	&_params_session,
	&_params_sess,
	&_params_paths,
	&_params_path,
	&_cmd_close_device,
	&_cmd_dump_all,
	&_cmd_list_devices,
	&_cmd_show,
	&_params_help,
	&_params_null
};

static struct param *params_object_type_help_client[] = {
	&_params_devices_client,
	&_params_sessions,
	&_params_paths,
	&_params_help,
	&_params_null
};

static struct param *params_object_type_help_server[] = {
	&_params_devices,
	&_params_sessions,
	&_params_paths,
	&_params_help,
	&_params_null
};

static struct param *params_list_parameters[] = {
	&_params_xml,
	&_params_cvs,
	&_params_json,
	&_params_term,
	&_params_byte,
	&_params_kib,
	&_params_mib,
	&_params_gib,
	&_params_tib,
	&_params_pib,
	&_params_eib,
	&_params_notree,
	&_params_noheaders,
	&_params_nototals,
	&_params_noterm,
	&_params_all,
	&_params_verbose,
	&_params_help,
	&_params_null
};

static struct param *params_fmt_parameters[] = {
	&_params_xml,
	&_params_cvs,
	&_params_json,
	&_params_term,
	&_params_null
};

static struct param *params_map_parameters[] = {
	&_params_from,
	&_params_ro,
	&_params_rw,
	&_params_migration,
	&_params_help,
	&_params_verbose,
	&_params_minus_v,
	&_params_null
};

/* help when somthing is given after from <host> that can not be parsed */
static struct param *params_map_parameters_help_tail[] = {
	&_params_path_param_ip,
	&_params_path_param_gid,
	&_params_ro,
	&_params_rw,
	&_params_migration,
	&_params_help,
	&_params_verbose,
	&_params_null
};

static struct param *params_unmap_parameters[] = {
	&_params_help,
	&_params_force,
	&_params_verbose,
	&_params_minus_v,
	&_params_null
};

static struct param *params_add_path_parameters[] = {
	&_params_help,
	&_params_path_param,
	&_params_verbose,
	&_params_minus_v,
	&_params_null
};

static struct param *params_recover_session_parameters[] = {
	&_params_help,
	&_params_verbose,
	&_params_minus_v,
	&_params_recover_add_missing,
	&_params_null
};

static struct param *params_recover_session_parameters_help[] = {
	&_params_help,
	&_params_verbose,
	&_params_minus_v,
	&_params_all_recover,
	&_params_recover_add_missing,
	&_params_null
};

static struct param *params_add_path_help[] = {
	&_params_help,
	&_params_path_param,
	&_params_verbose,
	&_params_null
};

static struct param *cmds_client_sessions[] = {
	&_cmd_list_sessions,
	&_cmd_show_sessions,
	&_cmd_reconnect_session,
	&_cmd_recover_session,
	&_cmd_remap_session,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_client_sessions_help[] = {
	&_cmd_list_sessions,
	&_cmd_show_sessions,
	&_cmd_reconnect_session,
	&_cmd_recover_session,
	&_cmd_remap_session,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_client_devices[] = {
	&_cmd_list_devices,
	&_cmd_show_devices,
	&_cmd_map,
	&_cmd_resize,
	&_cmd_unmap,
	&_cmd_remap,
	&_cmd_client_recover_device,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_client_paths[] = {
	&_cmd_list_paths,
	&_cmd_show_paths,
	&_cmd_disconnect_path,
	&_cmd_dis_path,
	&_cmd_reconnect_path,
	&_cmd_recover_path,
	&_cmd_add,
	&_cmd_delete,
	&_cmd_del,
	&_cmd_readd,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_client_paths_help[] = {
	&_cmd_list_paths,
	&_cmd_show_paths,
	&_cmd_disconnect_path,
	&_cmd_reconnect_path,
	&_cmd_recover_path,
	&_cmd_add,
	&_cmd_delete,
	&_cmd_readd,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_server_sessions[] = {
	&_cmd_list_sessions,
	&_cmd_show_sessions,
	&_cmd_disconnect_session,
	&_cmd_dis_session,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_server_sessions_help[] = {
	&_cmd_list_sessions,
	&_cmd_show_sessions,
	&_cmd_disconnect_session,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_server_devices[] = {
	&_cmd_list_devices,
	&_cmd_show_devices,
	&_cmd_close_device,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_server_paths[] = {
	&_cmd_list_paths,
	&_cmd_show_paths,
	&_cmd_disconnect_path,
	&_cmd_dis_path,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_server_paths_help[] = {
	&_cmd_list_paths,
	&_cmd_show_paths,
	&_cmd_disconnect_path,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_both_sessions[] = {
	&_cmd_list_sessions,
	&_cmd_show_sessions,
	&_cmd_remap_session,
	&_cmd_disconnect_session,
	&_cmd_dis_session,
	&_cmd_reconnect_session,
	&_cmd_recover_session,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_both_sessions_help[] = {
	&_cmd_list_sessions,
	&_cmd_show_sessions,
	&_cmd_remap_session,
	&_cmd_recover_session,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_both_paths[] = {
	&_cmd_list_paths,
	&_cmd_show_paths,
	&_cmd_disconnect_session,
	&_cmd_dis_session,
	&_cmd_reconnect_session,
	&_cmd_add,
	&_cmd_delete,
	&_cmd_del,
	&_cmd_readd,
	&_cmd_help,
	&_cmd_null
};

static struct param *cmds_both_paths_help[] = {
	&_cmd_list_paths,
	&_cmd_show_paths,
	&_cmd_add,
	&_cmd_delete,
	&_cmd_readd,
	&_cmd_help,
	&_cmd_null
};

static int levenstein_compare(int d1, int d2, const char *s1, const char *s2)
{
	return d1 != d2 ? d1 - d2 : strcmp(s1, s2);
}

static int param_compare(const void *p1, const void *p2)
{
	const struct param *const*c1 = p1;
	const struct param *const*c2 = p2;

	return levenstein_compare((*c1)->dist, (*c2)->dist,
				  (*c1)->param_str, (*c2)->param_str);
}

static void handle_unknown_param(const char *param, struct param *params[])
{
	struct param **cs;
	size_t len = 0, cnt = 0, i;

	ERR(trm, "Unknown '%s'\n", param);

	for (cs = params; (*cs)->param_str; cs++) {
		(*cs)->dist = levenshtein((*cs)->param_str, param, 1, 2, 1, 0)
				+ 1;
		if (strlen((*cs)->param_str) < 2)
			(*cs)->dist += 3;
		len++;
		if ((*cs)->dist < 4)
			cnt++;
	}

	if (!cnt || cnt == len) {

		if (len > 7)
			return;

		cnt = len;
	} else {

		qsort(params, len, sizeof(*params), param_compare);
	}
	printf("Did you mean:\n");
	if (cnt > 3)
		cnt = 3;

	for (i = 0; i < cnt; i++)
		printf("\t%s\n", params[i]->param_str);
}

static int parse_precision(const char *str,
			   struct rnbd_ctx *ctx)
{
	unsigned int prec;
	char e;

	if (strncmp(str, "prec", 4))
		return -EINVAL;

	if (str[4] == '=')
		str += 5;
	else
		str += 4;
	if (sscanf(str, "%u%c\n", &prec, &e) != 2)
		return -EINVAL;

	ctx->prec = prec;
	ctx->prec_set = true;

	return 0;
}

static const char *comma = ",";

static int parse_clt_devices_clms(const char *arg, struct rnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_devices_clt,
				    ctx->clms_devices_clt, CLM_MAX_CNT);
}

static int parse_srv_devices_clms(const char *arg, struct rnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_devices_srv,
				    ctx->clms_devices_srv, CLM_MAX_CNT);
}

static int parse_clt_sessions_clms(const char *arg, struct rnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_sessions_clt,
				    ctx->clms_sessions_clt, CLM_MAX_CNT);
}

static int parse_srv_sessions_clms(const char *arg, struct rnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_sessions_srv,
				    ctx->clms_sessions_srv, CLM_MAX_CNT);
}

static int parse_both_sessions_clms(const char *arg, struct rnbd_ctx *ctx)
{
	int tmp_err, err;

	err = table_extend_columns(arg, comma, all_clms_sessions_clt,
				   ctx->clms_sessions_clt, CLM_MAX_CNT);
	tmp_err = table_extend_columns(arg, comma, all_clms_sessions_srv,
				       ctx->clms_sessions_srv, CLM_MAX_CNT);
	if (!tmp_err && err)
		err = tmp_err;

	/* if any of the parse commands succeed return success */
	return err;
}

static int parse_clt_paths_clms(const char *arg, struct rnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_paths_clt,
				    ctx->clms_paths_clt, CLM_MAX_CNT);
}

static int parse_srv_paths_clms(const char *arg, struct rnbd_ctx *ctx)
{
	return table_extend_columns(arg, comma, all_clms_paths_srv,
				    ctx->clms_paths_srv, CLM_MAX_CNT);
}

static int parse_both_paths_clms(const char *arg, struct rnbd_ctx *ctx)
{
	int tmp_err, err;

	err = table_extend_columns(arg, comma, all_clms_paths_clt,
				   ctx->clms_paths_clt, CLM_MAX_CNT);
	tmp_err = table_extend_columns(arg, comma, all_clms_paths_srv,
				       ctx->clms_paths_srv, CLM_MAX_CNT);
	if (!tmp_err && err)
		err = tmp_err;

	/* if any of the parse commands succeed return success */
	return err;
}

static int parse_clt_clms(const char *arg, struct rnbd_ctx *ctx)
{
	int tmp_err, err;

	err = table_extend_columns(arg, comma, all_clms_devices_clt,
				   ctx->clms_devices_clt, CLM_MAX_CNT);
	tmp_err = table_extend_columns(arg, comma, all_clms_sessions_clt,
				       ctx->clms_sessions_clt, CLM_MAX_CNT);
	if (!tmp_err && err)
		err = tmp_err;

	tmp_err = table_extend_columns(arg, comma, all_clms_paths_clt,
				       ctx->clms_paths_clt, CLM_MAX_CNT);
	if (!tmp_err && err)
		err = tmp_err;

	/* if any of the parse commands succeed return success */
	return err;
}

static int parse_srv_clms(const char *arg, struct rnbd_ctx *ctx)
{
	int tmp_err, err;

	err = table_extend_columns(arg, comma, all_clms_devices_srv,
				   ctx->clms_devices_srv, CLM_MAX_CNT);
	tmp_err = table_extend_columns(arg, comma, all_clms_sessions_srv,
				       ctx->clms_sessions_srv, CLM_MAX_CNT);
	if (!tmp_err && err)
		err = tmp_err;

	tmp_err = table_extend_columns(arg, comma, all_clms_paths_srv,
				       ctx->clms_paths_srv, CLM_MAX_CNT);
	if (!tmp_err && err)
		err = tmp_err;

	/* if any of the parse commands succeed return success */
	return err;
}

static int parse_both_devices_clms(const char *arg, struct rnbd_ctx *ctx)
{
	int tmp_err, err;

	err = table_extend_columns(arg, comma, all_clms_devices_clt,
				    ctx->clms_devices_clt, CLM_MAX_CNT);
	tmp_err = table_extend_columns(arg, comma, all_clms_devices_srv,
				    ctx->clms_devices_srv, CLM_MAX_CNT);
	if (!tmp_err && err)
		err = tmp_err;

	/* if any of the parse commands succeed return success */
	return err;
}

static int parse_both_clms(const char *arg, struct rnbd_ctx *ctx)
{
	int tmp_err, err;

	err =  parse_clt_clms(arg, ctx);
	tmp_err = parse_srv_clms(arg, ctx);

	if (!tmp_err && err)
		err = tmp_err;

	/* if any of the parse commands succeed return success */
	return err;
}

static int parse_sign(char s,
		      struct rnbd_ctx *ctx)
{
	if (s == '+')
		ctx->sign = 1;
	else if (s == '-')
		ctx->sign = -1;
	else
		ctx->sign = 0;

	return ctx->sign;
}

static int parse_size(const char *str,
		      struct rnbd_ctx *ctx)
{
	if (parse_sign(*str, ctx))
		str++;

	return str_to_size(str, ctx);
}

static void init_rnbd_ctx(struct rnbd_ctx *ctx)
{
	memset(ctx, 0, sizeof(struct rnbd_ctx));

	list_default(ctx);

	trm = (isatty(STDOUT_FILENO) == 1);
}

static void deinit_rnbd_ctx(struct rnbd_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->path_cnt; i++) {
		if (ctx->paths[i].src)
			free((char *)ctx->paths[i].src);
		if (ctx->paths[i].dst)
			free((char *)ctx->paths[i].dst);
		if (ctx->paths[i].provided)
			free((char *)ctx->paths[i].provided);
	}
}

static void rnbd_ctx_default(struct rnbd_ctx *ctx)
{
	if (!ctx->lstmode_set)
		ctx->lstmode = LST_DEVICES;

	if (!ctx->fmt_set)
		ctx->fmt = FMT_TERM;

	if (!ctx->prec_set)
		ctx->prec = 3;

	if (!ctx->rnbdmode_set) {
		if (sess_clt[0])
			ctx->rnbdmode |= RNBD_CLIENT;
		if (sess_srv[0])
			ctx->rnbdmode |= RNBD_SERVER;
	}
}

static void help_mode(const char *mode, struct param *const params[],
		      const struct rnbd_ctx *ctx)
{
	char buff[PATH_MAX];

	if (ctx->pname_with_mode) {

		help_param(ctx->pname, params, ctx);
	} else {

		snprintf(buff, sizeof(buff), "%s %s", ctx->pname, mode);
		help_param(buff, params, ctx);
	}
}

static void help_start(const struct rnbd_ctx *ctx)
{
	if (help_print_flags(ctx)) {
		help_param(ctx->pname, params_flags_help, ctx);
		printf("\n\n");
	}

	help_param(ctx->pname, params_mode_help, ctx);
}

static int init_show(enum rnbdmode rnbdmode,
		     enum lstmode object_type,
		     struct rnbd_ctx *ctx)
{
	(void) parse_all(0, NULL, NULL, ctx);
	switch (object_type) {
	case LST_PATHS:
	case LST_ALL:
		if (rnbdmode & RNBD_CLIENT) {
			parse_clt_paths_clms("-src_addr", ctx);
			parse_clt_paths_clms("-dst_addr", ctx);
		}
		if (rnbdmode & RNBD_SERVER) {
			parse_srv_paths_clms("-src_addr", ctx);
			parse_srv_paths_clms("-dst_addr", ctx);
		}
		break;
	default:
		;
	}
	return 0;
}

/**
 * Parse all the possible parameters to list or show commands.
 * The results are collected in the rnbd_ctx struct
 *
 *  Assumtions:
 * * all types  accept the same parameters (except for collums)
 * * it is always the end of a command line, so a left over argument
 *   that can not be interpreded is an error
 *
 * Returns:
 * > 0 number of arguments accepted
 * < 0 error code
 * == 0 there are no arguments to the list command
 */
int parse_list_parameters(int argc, const char *argv[], struct rnbd_ctx *ctx,
		int (*parse_clms)(const char *arg, struct rnbd_ctx *ctx),
			  const struct param *cmd, const char *program_name, int allow_paths)
{
	int err = 0; int start_argc = argc;
	const struct param *param;

	while (argc && err >= 0) {
		/* parse the list flags */
		param = find_param(*argv, params_list_parameters);
		if (param) {
			err = param->parse(argc, argv, param, ctx);
			if (err > 0) {
				argc -= err; argv += err;
				continue;
			}
		}
		/* parse collumn parameters */
		err = (*parse_clms)(*argv, ctx);
		if (err == 0) {

			argc--; argv++;
			continue;
		}
		if (err == -EINVAL) {

			err = parse_precision(*argv, ctx);
			if (err == 0) {
				argc--; argv++;
				continue;
			}
		}
		if (err == -EINVAL && allow_paths)
			err = parse_path(*argv, ctx);

		if (err == 0) {
			argc--; argv++;
			continue;
		}
		if (err == -EINVAL && allow_paths)
			err = parse_port_desc(*argv, ctx);

		if (err == 0) {
			argc--; argv++;
		}
	}
	if (ctx->help_set) {
		cmd->help(program_name, cmd, ctx);
		err = -EAGAIN;
	} else if (err == -EINVAL) {
		handle_unknown_param(*argv, params_list_parameters);
	}
	return err < 0 ? err : start_argc - argc;
}

/**
 * Parse parameters for a command as described by cmd.
 */
int parse_cmd_parameters(int argc, const char *argv[],
			 struct param *const params[], struct rnbd_ctx *ctx,
			 const struct param *cmd, const char *program_name,
			 int allow_option_name)
{
	int err = 0; int start_argc = argc;
	const struct param *param;

	while (argc && err >= 0) {
		param = find_param(*argv, params);
		if (param)
			err = param->parse(argc, argv, param, ctx);
		if (!param || err <= 0) {
			if (allow_option_name
			    && parse_option_name(argc, argv, cmd, ctx)) {
				err = 1;
			} else {
				break;
			}
		}
		argc -= err; argv += err;
	}
	if (ctx->help_set && cmd) {
		cmd->help(program_name, cmd, ctx);
		err = -EAGAIN;
	}
	return err < 0 ? err : start_argc - argc;
}

int parse_all_parameters(int argc, const char *argv[],
			 struct param *const params[], struct rnbd_ctx *ctx,
			 const struct param *cmd, const char *program_name)
{
	int err = 0;
	const struct param *param;

	while (argc && err >= 0) {
		param = find_param(*argv, params);
		if (param)
			err = param->parse(argc, argv, param, ctx);
		else
			err = 1; /* skip unknown parameter */

		argc -= err; argv += err;
	}
	return err;
}

/**
 * Parse parameters for the map command as described by cmd.
 */
int parse_map_parameters(int argc, const char *argv[], int *accepted,
			 struct param *const params[], struct rnbd_ctx *ctx,
			 const struct param *cmd, const char *program_name,
			 int allow_port_desc)
{
	int err = 0; int start_argc = argc;
	const struct param *param;

	while (argc && err >= 0) {
		param = find_param(*argv, params);
		if (param) {
			err = param->parse(argc, argv, param, ctx);
		} else {
			err = parse_path(*argv, ctx);
			if (err == 0) {
				err = 1;
			} else if (allow_port_desc) {
				err = parse_port_desc(*argv, ctx);
				if (err == 0)
					err = 1;
			}
		}
		if (err <= 0)
			break;

		argc -= err; argv += err;
	}
	if (ctx->help_set) {
		cmd->help(program_name, cmd, ctx);
		err = -EAGAIN;
	}
	*accepted = start_argc - argc;
	return err;
}

int cmd_dump_all(int argc, const char *argv[], const struct param *cmd,
		 const char *help_context, struct rnbd_ctx *ctx)

{
	int err, tmp_err;

	err = parse_all_parameters(argc, argv, params_fmt_parameters,
				   ctx, cmd, "");
	if (err < 0)
		return err;

	/* default format for dump is all except when output to terminal */
	if (ctx->fmt == FMT_TERM) {
		ctx->notree_set = true;
		ctx->nototals_set = true;
	} else	{
		(void) parse_all(0, NULL, NULL, ctx);
	}
	/* parse from beginning again! */
	err = parse_list_parameters(argc, argv, ctx,
				    parse_both_clms,
				    cmd, "", 0);
	if (err < 0)
		return err;

	ctx->rnbdmode = RNBD_BOTH;

	err = list_devices(sds_clt, sds_clt_cnt - 1, sds_srv,
			   sds_srv_cnt - 1, true, ctx);

	if ((sds_clt_cnt - 1 + sds_srv_cnt - 1)
	    && (sess_clt_cnt - 1 + sess_srv_cnt - 1))
		printf("\n");

	tmp_err = list_sessions(sess_clt, sess_clt_cnt - 1, sess_srv,
				sess_srv_cnt - 1, true, ctx);

	if (!err && tmp_err)
		err = tmp_err;

	if ((sds_clt_cnt - 1 + sds_srv_cnt - 1
	     + sess_clt_cnt - 1 + sess_srv_cnt - 1)
	    && (paths_clt_cnt - 1 + paths_srv_cnt - 1))
		printf("\n");

	tmp_err = list_paths(paths_clt, paths_clt_cnt - 1, paths_srv,
			     paths_srv_cnt - 1, true, ctx);

	if (!err && tmp_err)
		err = tmp_err;

	return err;
}

int check_root(const struct rnbd_ctx *ctx)
{
	int err = 0;
	uid_t uid;

	uid = geteuid();
	/* according to man page "this function is always successful" */
	if (ctx->simulate_set) {
		INF((uid != 0 && ctx->verbose_set),
		    "warning: Modifying operations require root permission.\n");
		return err;
	}
	if (uid != 0) {
		ERR(trm, "Modifying operations require root permission.\n");
		err = -EPERM;
	}
	return err;
}

int cmd_map(int argc, const char *argv[], const struct param *cmd,
	    const char *help_context, struct rnbd_ctx *ctx)
{
	int accepted = 0, err = 0;

	err = parse_name_help(argc--, argv++,
			      help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_map_parameters(argc, argv, &accepted,
				   params_map_parameters,
				   ctx, cmd, help_context,
				   0 /* allow_port_desc */);
	if (!ctx->from_set && ctx->path_cnt == 0) {

		cmd_print_usage_short(cmd, help_context, ctx);
		ERR(trm,
		    "Please specify the destination to map from\n");

		return -EINVAL;
	}
	argc -= accepted; argv += accepted;

	if (argc > 0 || (err < 0 && err != -EAGAIN)) {

		handle_unknown_param(*argv, params_map_parameters_help_tail);

		return -EINVAL;
	}
	err = check_root(ctx);
	if (err < 0)
		return err;

	return err = client_devices_map(ctx->from, ctx->name, ctx);
}

int cmd_resize(int argc, const char *argv[], const struct param *cmd,
	       const char *help_context, struct rnbd_ctx *ctx)
{
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	if (argc > 0)
		err = parse_size(*argv, ctx);
	else
		err = -EINVAL;
	if (err < 0 || ctx->sign < 0) {

		cmd_print_usage_short(cmd, help_context, ctx);

		if (err == -ENOENT)
			ERR(trm,
			    "Please provide a valid unit for size of the device to be configured\n");
		else if (ctx->sign < 0)
			ERR(trm,
			    "Please provide a positiv value for size of the device to be configured\n");
		else
			ERR(trm,
			    "Please provide the size of the device to be configured\n");

		return err == 0 ? -EINVAL : err;
	}
	argc--; argv++;

	if (argc > 0) {
		err = parse_apply_unit(*argv, ctx);
		if (err < 0) {
			if (!strcmp(*argv, "help")) {
				parse_help(argc, argv, NULL, ctx);

				cmd->help(*argv, cmd, ctx);
				return -EAGAIN;
			} else {
				cmd_print_usage_short(cmd, help_context, ctx);

				ERR(trm,
				    "Please provide a valid unit for size of device to be configured\n");
				return err;
			}
		}
	} else if (ctx->size_state == size_number) {

		ctx->size_sect >>= 9;
		ctx->size_state = size_unit;
	}
	argc--; argv++;

	if (argc > 0) {

		handle_unknown_param(*argv, params_default);
		return -EINVAL;
	}
	err = check_root(ctx);
	if (err < 0)
		return err;

	return client_devices_resize(ctx->name, ctx->size_sect, ctx);
}

int cmd_unmap(int argc, const char *argv[], const struct param *cmd,
	      const char *help_context, struct rnbd_ctx *ctx)
{
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_cmd_parameters(argc, argv,
				   params_unmap_parameters,
				   ctx, cmd, help_context, 0);
	if (err < 0)
		return err;

	argc -= err; argv += err;

	if (argc > 0) {

		handle_unknown_param(*argv, params_unmap_parameters);
		return -EINVAL;
	}
	err = check_root(ctx);
	if (err < 0)
		return err;

	return client_devices_unmap(ctx->name, ctx->force_set, ctx);
}

int cmd_remap(int argc, const char *argv[], const struct param *cmd,
	      bool allowSession, const char *help_context, struct rnbd_ctx *ctx)
{
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_cmd_parameters(argc, argv, params_unmap_parameters,
				   ctx, cmd, help_context, 0);
	if (err < 0)
		return err;

	argc -= err; argv += err;

	if (argc > 0) {

		handle_unknown_param(*argv, params_default);
		return -EINVAL;
	}
	err = check_root(ctx);
	if (err < 0)
		return err;

	if (allowSession
	    && find_single_session(ctx->name, ctx, sess_clt,
				   sds_clt_cnt, false))
		return client_session_remap(ctx->name, ctx);

	return client_devices_remap(ctx->name, ctx);
}

int cmd_close_device(int argc, const char *argv[], const struct param *cmd,
		     const char *help_context, struct rnbd_ctx *ctx)
{
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_cmd_parameters(argc, argv, params_unmap_parameters,
				   ctx, cmd, help_context, 0);
	if (err < 0)
		return err;

	argc -= err; argv += err;

	if (argc > 0) {

		handle_unknown_param(*argv, params_default);
		return -EINVAL;
	}

	err = check_root(ctx);
	if (err < 0)
		return err;

	return client_devices_remap(ctx->name, ctx);
}

int cmd_client_recover_device(int argc, const char *argv[],
			       const struct param *cmd,
			       const char *help_context, struct rnbd_ctx *ctx)
{
	const struct rnbd_sess_dev *ds;
	int i, err, tmp_err;

	err = parse_name_help(argc--, argv++,
			      help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_cmd_parameters(argc, argv, params_default,
				   ctx, cmd, help_context, 0);
	if (err < 0)
		return err;

	argc -= err; argv += err; err = 0;

	if (argc > 0) {
		handle_unknown_param(*argv, params_default);
		return -EINVAL;
	}

	err = check_root(ctx);
	if (err < 0)
		return err;

	if (!strcmp(ctx->name, "all")) {
		for (i = 0; sds_clt[i]; i++) {
			if (!strcmp(sds_clt[i]->dev->state, "closed")) {
				tmp_err = client_device_remap(sds_clt[i]->dev, ctx);
				if (!err)
					err = tmp_err;
			} else {
				INF(ctx->debug_set,
				    "Device is still open, no need to recover.\n");
				err = 0;
			}
		}
	} else {
		ds = find_single_device(ctx->name, ctx, sds_clt, sds_clt_cnt, true/*print_err*/);
		if (!ds)
			return -EINVAL;

		if (!strcmp(ds->dev->state, "closed")) {
			err = client_device_remap(ds->dev, ctx);
		} else {
			INF(ctx->debug_set,
			    "Device is still open, no need to recover.\n");
			err = 0;
		}
	}

	if (err)
		ERR(trm, "Failed to recover device: %s (%d)\n",
		    strerror(-err), err);

	return err;
}

int cmd_session_remap(int argc, const char *argv[], const struct param *cmd,
		      const char *help_context, struct rnbd_ctx *ctx)
{
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_cmd_parameters(argc, argv,
				   params_unmap_parameters,
				   ctx, cmd, help_context, 0);
	if (err < 0)
		return err;

	argc -= err; argv += err;

	if (argc > 0) {

		handle_unknown_param(*argv, params_default);
		return -EINVAL;
	}
	err = check_root(ctx);
	if (err < 0)
		return err;

	return client_session_remap(ctx->name, ctx);
}

int cmd_path_add(int argc, const char *argv[], const struct param *cmd,
		 const char *help_context, struct rnbd_ctx *ctx)
{
	int accepted = 0;
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_map_parameters(argc, argv, &accepted,
				   params_add_path_parameters,
				   ctx, cmd, help_context,
				   0 /* allow_port_desc */);
	if (accepted == 0) {

		cmd_print_usage_short(cmd, help_context, ctx);
		ERR(trm,
		    ":Please specify the path to add to session %s\n",
		    ctx->name);
		print_opt(_params_path_param.param_str,
			  _params_path_param.descr);
		return -EINVAL;
	}
	argc -= accepted; argv += accepted;

	if (argc > 0 || (err < 0 && err != -EAGAIN)) {

		handle_unknown_param(*argv, params_add_path_help);
		if (err < 0) {
			printf("\n");
			print_param(params_add_path_help);
		}
		return -EINVAL;
	}
	if (ctx->path_cnt <= 0) {
		cmd_print_usage_short(cmd, help_context, ctx);
		ERR(trm,
		    "No valid path provided. Please provide a path to add to session '%s'.\n",
		    ctx->name);
	}
	if (ctx->path_cnt > 1) {
		cmd_print_usage_short(cmd, help_context, ctx);
		ERR(trm,
		    "You provided %d paths. Please provide exactly one path to add to session '%s'.\n",
		    ctx->path_cnt, ctx->name);
	}
	err = check_root(ctx);
	if (err < 0)
		return err;

	return client_session_add(ctx->name, ctx->paths, ctx);
}

int cmd_path_delete(int argc, const char *argv[], const struct param *cmd,
		    const char *help_context, struct rnbd_ctx *ctx)
{
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_cmd_parameters(argc, argv, params_default,
				   ctx, cmd, help_context, 0);
	if (err < 0)
		return err;

	argc -= err; argv += err;

	if (argc > 0) {

		handle_unknown_param(*argv, params_default);
		return -EINVAL;
	}
	err = check_root(ctx);
	if (err < 0)
		return err;

	return client_path_delete(NULL, ctx->name, ctx);
}

int cmd_client_session_reconnect(int argc, const char *argv[], const struct param *cmd,
				 const char *help_context, struct rnbd_ctx *ctx)
{
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_cmd_parameters(argc, argv,
				   params_default,
				   ctx, cmd, help_context, 0);
	if (err < 0)
		return err;

	argc -= err; argv += err;

	if (argc > 0) {

		handle_unknown_param(*argv, params_default);
		return -EINVAL;
	}
	err = check_root(ctx);
	if (err < 0)
		return err;

	/* We want the session to change it's state to */
	/* disconnected. So disconnect all paths first.*/
	err = session_do_all_paths(RNBD_CLIENT, ctx->name,
				   client_path_disconnect,
				   ctx);
	/* If the session does not exist at all we will   */
	/* get -EINVAL. In all other error cases we try   */
	/* to reconnect the path to reconnect the session.*/
	if (err != -EINVAL)
		err = session_do_all_paths(RNBD_CLIENT,
					   ctx->name,
					   client_path_reconnect,
					   ctx);
	return err;
}

static int client_session_add_missing_paths(const char *session_name,
					    struct rnbd_ctx *ctx)
{
	int err = 0;
	char hostname[NAME_MAX];
	struct path paths[MAX_PATHS_PER_SESSION]; /* lazy */
	struct rnbd_sess *sess;
	struct rnbd_path *path;
	int path_cnt = 0, i;

	memset(paths, 0, sizeof(paths));

	INF(ctx->debug_set, "Looking for missing paths of session %s\n", session_name);

	sess = find_single_session(session_name, ctx,
				   sess_clt, sess_clt_cnt,
				   false);
	if (sess && strlen(sess->hostname)) {

		strncpy(hostname, sess->hostname, sizeof(hostname));
		INF(ctx->debug_set,
		    "Using counterpart hostname %s from session %s.\n",
		    hostname, session_name);
	} else {

		INF(ctx->debug_set,
		    "No hostname for session, attempting to use existing path(s)\n");

		path = find_first_path_for_session(session_name, paths_clt,
						   paths_clt_cnt);
		if (!path) {
			INF(trm, "No paths in session %s, not possible to recover\n",
			    session_name);
			return 0;
		}
		
		err = hostname_from_path(hostname, sizeof(hostname),
					 path->hca_name, path->hca_port,
					 path->dst_addr);
		if (err < 0) {
			ERR(trm, "Could not look up hostname for path %s\n", path->pathname);
			return err;
		} else {
			INF(ctx->debug_set, "Hostname is %s\n", hostname);
		}
	}
	err = resolve_host(hostname, paths, ctx);
	if (err < 0) {
		ERR(trm,
		    "Failed to resolve host name for %s: %s (%d)\n",
		    hostname, strerror(-err), err);
	} else if (err == 0) {
		INF(ctx->debug_set,
		    "Found no paths to host %s.\n",
		    hostname);
	} else {
		path_cnt = err;
	}
	if (path_cnt) {
		for (i = 0; i < path_cnt; i++) {
			path = find_single_path(session_name, paths[i].dst, ctx,
						paths_clt, paths_clt_cnt, false);
			if (path) {
				INF(ctx->debug_set,
				    "Path %s of session %s already exists.\n",
				    paths[i].dst,
				    session_name);
			} else {
				INF(ctx->debug_set,
				    "Try to add path %s to session %s.\n",
				    paths[i].dst, session_name);
				err = client_session_add(session_name, paths+i, ctx);
			}
		}
	}
	return err;
}

int cmd_client_session_recover(int argc, const char *argv[],
			       const struct param *cmd,
			       const char *help_context, struct rnbd_ctx *ctx)
{
	struct rnbd_sess *sess;
	int i, err, tmp_err;

	err = parse_name_help(argc--, argv++,
			      help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_cmd_parameters(argc, argv,
				   params_recover_session_parameters,
				   ctx, cmd, help_context, 0);
	if (err < 0)
		return err;

	argc -= err; argv += err;

	if (argc > 0) {

		handle_unknown_param(*argv,
				     params_recover_session_parameters_help);
		return -EINVAL;
	}

	err = check_root(ctx);
	if (err < 0)
		return err;

	if (!strcmp(ctx->name, "all")) {

		err = 0;
		sess = find_single_session(ctx->name, ctx,
					   sess_clt, sess_clt_cnt,
					   false);
		/*
		 * If session with the name "all" doesn't exist
		 * recover all sessions
		 */
		if (!sess) {
			for (i = 0; sess_clt[i]; i++) {
				tmp_err = session_do_all_paths(RNBD_CLIENT,
							sess_clt[i]->sessname,
							client_path_recover,
							ctx);
				if (tmp_err < 0 && err >= 0)
					err = tmp_err;

				if (ctx->add_missing_set) {

					tmp_err = client_session_add_missing_paths(
							sess_clt[i]->sessname, ctx);
					if (tmp_err < 0 && err >= 0)
						err = tmp_err;
				}
			}
			return err;
		}
	}
	err = session_do_all_paths(RNBD_CLIENT,
				   ctx->name,
				   client_path_recover,
				   ctx);

	if (ctx->add_missing_set) {

		tmp_err = client_session_add_missing_paths(ctx->name, ctx);

		if (tmp_err < 0 && err >= 0)
			err = tmp_err;
	}
	return err;
}

int cmd_recover_device_session_or_path(int argc, const char *argv[],
				       const struct param *cmd,
				       const char *help_context, struct rnbd_ctx *ctx)
{
	const struct rnbd_sess_dev *ds = NULL;
	const struct rnbd_sess *sess = NULL;
	const struct rnbd_path *path = NULL;
	int i, err, tmp_err;
	int accepted = 0;

	err = parse_name_help(argc--, argv++,
			      help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_map_parameters(argc, argv, &accepted,
				   params_recover_session_parameters,
				   ctx, cmd, help_context,
				   0 /* allow_port_desc */);
	if (err < 0)
		return err;

	argc -= err; argv += err; err = 0;

	if (argc > 0) {
		handle_unknown_param(*argv, params_default);
		return -EINVAL;
	}

	if (ctx->path_cnt > 1) {
		ERR(trm, "Multiple paths specified\n");
		err = -EINVAL;
	}
	err = check_root(ctx);
	if (err < 0)
		return err;

	if (!strcmp(ctx->name, "all")) {
		for (i = 0; sess_clt[i]; i++) {
			tmp_err = session_do_all_paths(RNBD_CLIENT,
						       sess_clt[i]->sessname,
						       client_path_recover,
						       ctx);
			if (tmp_err < 0 && err >= 0)
				err = tmp_err;

			if (ctx->add_missing_set) {

				tmp_err = client_session_add_missing_paths(
					sess_clt[i]->sessname, ctx);
				if (tmp_err < 0 && err >= 0)
						err = tmp_err;
			}
		}
		for (i = 0; sds_clt[i]; i++) {
			if (!strcmp(sds_clt[i]->dev->state, "closed")) {
				tmp_err = client_device_remap(sds_clt[i]->dev, ctx);
				if (!err)
					err = tmp_err;
			} else {
				INF(ctx->debug_set,
				    "Device is still open, no need to recover.\n");
			}
		}
	} else {
		ds = find_single_device(ctx->name, ctx, sds_clt, sds_clt_cnt, false/*print_err*/);
		if (ds) {
			INF(ctx->verbose_set,
			    "Recovering device %s.\n", ctx->name);
			if (!strcmp(ds->dev->state, "closed")) {
				err = client_device_remap(ds->dev, ctx);
			} else {
				INF(ctx->debug_set,
				    "Device is still open, no need to recover.\n");
				err = 0;
			}
		} else {
			if (ctx->path_cnt == 0)
				sess = find_single_session(ctx->name, ctx,
							   sess_clt,
							   sess_clt_cnt,
							   false);
			if (sess) {
				INF(ctx->verbose_set,
				    "Recovering session %s.\n", ctx->name);

				err = session_do_all_paths(RNBD_CLIENT,
							   ctx->name,
							   client_path_recover,
							   ctx);

				if (ctx->add_missing_set) {

					tmp_err = client_session_add_missing_paths(
							ctx->name, ctx);

					if (tmp_err < 0 && err >= 0)
						err = tmp_err;
				}
			} else {
				if (ctx->path_cnt == 0)
					path = find_single_path(NULL, ctx->name,
								ctx, paths_clt,
								paths_clt_cnt,
								false);
				else
					path = find_single_path(ctx->name,
								ctx->paths[0].dst,
								ctx, paths_clt,
								paths_clt_cnt,
								false);
				if (path) {
					if (!strcmp(path->state, "connected")) {
						INF(ctx->debug_set,
						    "Path '%s' is connected, skipping.\n",
						    (ctx->path_cnt == 0) ? ctx->name :
						    ctx->paths[0].dst);
					} else {

						INF(ctx->verbose_set, "Path '%s' is '%s', recovering.\n",
						    ctx->name, path->state);

						err = client_path_do((ctx->path_cnt == 0) ? NULL : ctx->name,
								     (ctx->path_cnt == 0) ? ctx->name : ctx->paths[0].dst,
								     "reconnect",
								     "Successfully reconnected path '%s' of session '%s'.\n",
								     "Failed to reconnect path '%s' from session '%s': %s (%d)\n",
								     ctx);
					}
				} else {
					ERR(trm,
					    "%s is neither a device, session, nor path.\n",
					    ctx->name);
				}
			}
		}
	}
	if (err)
		ERR(trm, "Failed to recover %s: %s (%d)\n",
		    (ds ? "device" : (sess ? "session" : "path")),
		    strerror(-err), err);

	return err;
}

int cmd_server_session_disconnect(int argc, const char *argv[], const struct param *cmd,
				  const char *help_context, struct rnbd_ctx *ctx)
{
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_cmd_parameters(argc, argv,
				   params_default,
				   ctx, cmd, help_context, 0);
	if (err < 0)
		return err;

	argc -= err; argv += err;

	if (argc > 0)
		return -EINVAL;

	err = check_root(ctx);
	if (err < 0)
		return err;

	return session_do_all_paths(RNBD_SERVER, ctx->name,
				    server_path_disconnect,
				    ctx);
}

int cmd_server_devices_force_close(int argc, const char *argv[], const struct param *cmd,
				   const char *help_context, struct rnbd_ctx *ctx)
{
	const char *device_name = NULL;
	const char *session_name = NULL;
	struct rnbd_sess_dev **ds_exp = NULL;
	struct rnbd_sess_dev *ds_match = NULL;
	struct rnbd_sess **ss_srv = NULL;
	int devs_cnt, i;
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	device_name = ctx->name;
	ctx->name = NULL;

	err = parse_cmd_parameters(argc, argv,
				   params_unmap_parameters,
				   ctx, cmd, help_context, 1);
	if (err < 0)
		return err;

	argc -= err; argv += err;

	if (argc > 0)
		return -EINVAL;

	ds_exp = calloc(sds_srv_cnt, sizeof(*ds_exp));
	if (!ds_exp) {
		ERR(trm, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	devs_cnt = find_devices(device_name, sds_srv, ds_exp);

	if (ctx->name) {
		int sess_cnt = 0;

		ss_srv = calloc(sess_srv_cnt, sizeof(*ss_srv));

		if (sess_srv_cnt && !ss_srv) {
			ERR(trm, "Failed to alloc memory\n");
			err = -ENOMEM;
			goto cleanup_err;
		}
		session_name = ctx->name;
		ss_srv[0] = find_sess(session_name, sess_srv);
		if (ss_srv[0])
			sess_cnt = 1;
		else
			sess_cnt = find_sess_match(session_name, ctx->rnbdmode, sess_srv, ss_srv);

		if (sess_cnt > 1) {
			ERR(trm, "Multiple sessions match '%s'\n", session_name);
			err = -EINVAL;
			goto cleanup_err;
		} else if (sess_cnt <= 0) {
			ERR(trm, "There is no session matching '%s'\n", session_name);
			err = -ENOENT;
			goto cleanup_err;
		}
		INF(ctx->debug_set,
		    "Session name '%s' matches %s.\n",
		    session_name, ss_srv[0]->sessname);
		for (i = 0; i < devs_cnt; i++) {
			if (ds_exp[i]->sess == ss_srv[0]) {
				ds_match = ds_exp[i];
				break;
			}
		}
		free(ss_srv);
		ss_srv = NULL;
		if (!ds_match) {
			if (ctx->debug_set && devs_cnt) {
				INF(ctx->debug_set,
				    "Device name '%s' matching:\n",
				    device_name);
				for (i = 0; i < devs_cnt; i++) {
					INF(ctx->debug_set,
					    "Device %s (%s).\n",
					    ds_exp[i]->dev->devname,
					    ds_exp[i]->mapping_path);
				}
			}
			ERR(trm,
			    "There is no match for device '%s' and session '%s'\n",
			    device_name, session_name);
			err = -ENOENT;
			goto cleanup_err;
		}
	} else if (devs_cnt == 1) {
		ds_match = ds_exp[0];
	} else if (devs_cnt > 1) {
		ERR(trm,
		    "Device name %s is ambiguous. Please provide a session name to make it unique.\n",
		    device_name);
		if (ctx->debug_set) {
			INF(ctx->debug_set,
			    "Candidate session are:\n");
			for (i = 0; i < devs_cnt; i++) {
				INF(ctx->debug_set,
				    "Session %s.\n",
				    ds_exp[i]->sess->sessname);
			}
		}
		err = -EINVAL;
		goto cleanup_err;
	} else {
		ERR(trm,
		    "No matching device for %s found.\n", device_name);
		err = -ENOENT;
		goto cleanup_err;
	}
	free(ds_exp);

	INF(ctx->verbose_set,
	    "Device name '%s' matches %s (%s).\n",
	    device_name, ds_match->dev->devname, ds_match->mapping_path);
	INF(ctx->verbose_set,
	    "Will be closed for session %s.\n",
	    ds_match->sess->sessname);

	err = check_root(ctx);
	if (err < 0)
		goto cleanup_err;

	return server_devices_force_close(ds_match->dev->devname,
					  ds_match->sess->sessname, ctx);
cleanup_err:
	if (ss_srv)
		free(ss_srv);
	if (ds_exp)
		free(ds_exp);
	return err;
}

int cmd_path_operation(int (*operation)(const char *session_name,
					const char *path_name,
					struct rnbd_ctx *ctx),
		       int argc, const char *argv[], const struct param *cmd,
		       const char *help_context, struct rnbd_ctx *ctx)
{
	int accepted = 0;
	int err = parse_name_help(argc--, argv++,
				  help_context, cmd, ctx);
	if (err < 0)
		return err;

	err = parse_map_parameters(argc, argv, &accepted,
				   params_default,
				   ctx, cmd, help_context,
				   1 /* allow_port_desc */);
	if (err < 0)
		return err;

	argc -= accepted; argv += accepted;

	if (argc > 0) {

		handle_unknown_param(*argv, params_default);
		return -EINVAL;
	}
	err = check_root(ctx);
	if (err < 0)
		return err;

	if (ctx->path_cnt == 1 || ctx->port_desc_set) {
		err = (*operation)(ctx->name, ctx->paths[0].provided, ctx);
	} else if (ctx->path_cnt == 0) {
		err = (*operation)(NULL, ctx->name, ctx);
	} else {
		ERR(trm, "Multiple paths specified\n");
		err = -EINVAL;
	}
	return err;
}

int cmd_ambiguous(int argc, const char *argv[], const struct param *cmd,
		  const char *help_context)
{
	ERR(trm, "Ambiguous command!\n");

	printf("Please specify either ");
	clr_print(trm, CCYN, "client");
	printf(" or ");
	clr_print(trm, CCYN, "server");
	printf(" to reconnect/disconnect a session or a path.\n");

	return -EINVAL;
}

int cmd_both_sessions(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	const char *_help_context = "session";

	int err = 0;
	const struct param *cmd;

	cmd = find_param(*argv, cmds_both_sessions);
	if (!cmd) {
		print_usage(_help_context, cmds_both_sessions_help, ctx);
		if (ctx->complete_set)
			err = -EAGAIN;
		else
			err = -EINVAL;

		if (argc)
			handle_unknown_param(*argv, cmds_both_sessions);
		else if (!ctx->complete_set)
			ERR(trm, "Please specify a command\n");
	}
	if (err >= 0) {

		argc--; argv++;

		switch (cmd->tok) {
		case TOK_LIST:

			err = parse_list_parameters(argc, argv, ctx,
						    parse_both_sessions_clms,
						    cmd, _help_context, 0);
			if (err < 0)
				break;

			err = list_sessions(sess_clt, sess_clt_cnt - 1,
					    sess_srv, sess_srv_cnt - 1, false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      _help_context, cmd, ctx);
			if (err < 0)
				break;

			init_show(RNBD_CLIENT|RNBD_SERVER, LST_SESSIONS, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    parse_both_sessions_clms,
						    cmd, _help_context, 0);
			if (err < 0)
				break;

			err = show_both_sessions(ctx->name, ctx);
			break;
		case TOK_REMAP:
			err = cmd_session_remap(argc, argv, cmd,
						"client session", ctx);
			break;
		case TOK_RECONNECT:
		case TOK_DISCONNECT:
			err = cmd_ambiguous(argc, argv, cmd, "both session");
			break;
		case TOK_RECOVER:
			err = cmd_client_session_recover(argc, argv, cmd, _help_context, ctx);
			break;

		case TOK_HELP:
			parse_help(argc, argv, NULL, ctx);
			print_help(_help_context,
				   cmd, cmds_both_sessions_help, ctx);
			break;
		default:
			print_usage(_help_context,
				    cmds_both_sessions_help, ctx);
			handle_unknown_param(cmd->param_str,
					     cmds_both_sessions);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_both_paths(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	const char *_help_context_client = ctx->pname_with_mode
		? "path" : "client path";
	const char *_help_context_both = "path";

	int err = 0;
	const struct param *cmd;

	cmd = find_param(*argv, cmds_both_paths);
	if (!cmd) {
		print_usage(_help_context_both, cmds_both_paths_help, ctx);
		if (ctx->complete_set)
			err = -EAGAIN;
		else
			err = -EINVAL;

		if (argc)
			handle_unknown_param(*argv, cmds_both_paths);
		else if (!ctx->complete_set)
			ERR(trm, "Please specify a command\n");
	}
	if (err >= 0) {

		argc--; argv++;

		switch (cmd->tok) {
		case TOK_LIST:

			err = parse_list_parameters(argc, argv, ctx,
						    parse_both_paths_clms,
						    cmd, _help_context_both, 0);
			if (err < 0)
				break;

			err = list_paths(paths_clt, paths_clt_cnt - 1,
					 paths_srv, paths_srv_cnt - 1,
					 false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      _help_context_both, cmd, ctx);
			if (err < 0)
				break;

			init_show(RNBD_CLIENT|RNBD_SERVER, LST_PATHS, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    parse_both_paths_clms,
						    cmd, _help_context_both, 1);
			if (err < 0)
				break;

			err = show_paths(ctx->name, ctx);
			break;
		case TOK_ADD:
			err = cmd_path_add(argc, argv, cmd, _help_context_client, ctx);
			break;
		case TOK_DELETE:
			err = cmd_path_operation(client_path_delete,
						 argc, argv, cmd,
						 _help_context_client, ctx);
			break;
		case TOK_READD:
			err = cmd_path_operation(client_path_readd,
						 argc, argv, cmd,
						 _help_context_client, ctx);
			break;
		case TOK_RECONNECT:
		case TOK_DISCONNECT:
			err = cmd_ambiguous(argc, argv, cmd, "both path");
			break;
		case TOK_HELP:
			parse_help(argc, argv, NULL, ctx);
			print_help(_help_context_both, cmd,
				   cmds_both_paths_help, ctx);
			break;
		default:
			print_usage(_help_context_both, cmds_both_paths_help, ctx);
			handle_unknown_param(cmd->param_str,
					     cmds_both_paths);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_client_sessions(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	const char *_help_context = ctx->pname_with_mode
		? "session" : "client session";

	int err = 0;
	const struct param *cmd;

	cmd = find_param(*argv, cmds_client_sessions);
	if (!cmd) {
		print_usage(_help_context, cmds_client_sessions_help, ctx);
		if (ctx->complete_set)
			err = -EAGAIN;
		else
			err = -EINVAL;

		if (argc)
			handle_unknown_param(*argv, cmds_client_sessions);
		else if (!ctx->complete_set)
			ERR(trm, "Please specify a command\n");
	}
	if (err >= 0) {

		argc--; argv++;

		switch (cmd->tok) {
		case TOK_LIST:

			err = parse_list_parameters(argc, argv, ctx,
						    parse_clt_sessions_clms,
						    cmd, _help_context, 0);
			if (err < 0)
				break;

			err = list_sessions(sess_clt, sess_clt_cnt - 1,
					    NULL, 0, false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      _help_context, cmd, ctx);
			if (err < 0)
				break;

			init_show(RNBD_CLIENT, LST_SESSIONS, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    parse_clt_sessions_clms,
						    cmd, _help_context, 0);
			if (err < 0)
				break;

			err = show_client_sessions(ctx->name, ctx);
			break;
		case TOK_RECONNECT:
			err = cmd_client_session_reconnect(argc, argv, cmd, _help_context, ctx);
			break;
		case TOK_RECOVER:
			err = cmd_client_session_recover(argc, argv, cmd, _help_context, ctx);
			break;
		case TOK_REMAP:
			err = cmd_session_remap(argc, argv, cmd,
						_help_context, ctx);
			break;
		case TOK_HELP:
			parse_help(argc, argv, NULL, ctx);
			print_help(_help_context,
				   cmd, cmds_client_sessions_help, ctx);
			break;
		default:
			print_usage(_help_context,
				    cmds_client_sessions_help, ctx);
			handle_unknown_param(cmd->param_str,
					     cmds_client_sessions);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_client_devices(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	const char *_help_context_client = ctx->pname_with_mode
		? "device" : "client device";
	const char *_help_context_both = ctx->pname_with_mode ? "device" :
		(ctx->rnbdmode == RNBD_CLIENT ? "client device" : "device");

	int err = 0;
	const struct param *cmd;

	cmd = find_param(*argv, cmds_client_devices);
	if (!cmd) {
		print_usage(_help_context_both, cmds_client_devices, ctx);
		if (ctx->complete_set)
			err = -EAGAIN;
		else
			err = -EINVAL;

		if (argc)
			handle_unknown_param(*argv, cmds_client_devices);
		else if (!ctx->complete_set)
			ERR(trm, "Please specify a command\n");
	}
	if (err >= 0) {

		argc--; argv++;

		switch (cmd->tok) {
		case TOK_LIST:

			err = parse_list_parameters(argc, argv, ctx,
						    (ctx->rnbdmode == RNBD_CLIENT ?
						     parse_clt_devices_clms :
						     parse_both_devices_clms),
						    cmd, _help_context_both, 0);
			if (err < 0)
				break;

			err = list_devices(sds_clt, sds_clt_cnt - 1,
					   (ctx->rnbdmode == RNBD_CLIENT ?
					    NULL : sds_srv),
					   (ctx->rnbdmode == RNBD_CLIENT ?
					    0 : sds_srv_cnt - 1),
					   false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      _help_context_both, cmd, ctx);
			if (err < 0)
				break;

			init_show(ctx->rnbdmode, LST_DEVICES, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    (ctx->rnbdmode == RNBD_CLIENT ?
						     parse_clt_devices_clms : parse_both_devices_clms),
						    cmd, _help_context_both, 0);
			if (err < 0)
				break;

			err = show_devices(ctx->name, ctx);
			break;
		case TOK_MAP:
			err = cmd_map(argc, argv, cmd, _help_context_client, ctx);
			break;
		case TOK_RESIZE:
			err = cmd_resize(argc, argv, cmd, _help_context_client, ctx);
			break;
		case TOK_UNMAP:
			err = cmd_unmap(argc, argv, cmd, _help_context_client, ctx);
			break;
		case TOK_REMAP:
			err = cmd_remap(argc, argv, cmd, false, _help_context_client, ctx);
			break;
		case TOK_RECOVER:
			err = cmd_client_recover_device(argc, argv, cmd,
							_help_context_client, ctx);
			break;
		case TOK_HELP:
			parse_help(argc, argv, NULL, ctx);
			print_help(_help_context_client, cmd,
				   cmds_client_devices, ctx);
			break;
		default:
			print_usage(_help_context_both, cmds_client_devices, ctx);
			handle_unknown_param(cmd->param_str,
					     cmds_client_devices);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_client_paths(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	const char *_help_context = ctx->pname_with_mode
		? "path" : "client path";

	int err = 0;
	const struct param *cmd;

	cmd = find_param(*argv, cmds_client_paths);
	if (!cmd) {
		print_usage(_help_context, cmds_client_paths_help, ctx);
		if (ctx->complete_set)
			err = -EAGAIN;
		else
			err = -EINVAL;

		if (argc)
			handle_unknown_param(*argv, cmds_client_paths);
		else if (!ctx->complete_set)
			ERR(trm, "Please specify a command\n");
	}
	if (err >= 0) {

		argc--; argv++;

		switch (cmd->tok) {
		case TOK_LIST:

			err = parse_list_parameters(argc, argv, ctx,
						    parse_clt_paths_clms,
						    cmd, _help_context, 0);
			if (err < 0)
				break;

			err = list_paths(paths_clt, paths_clt_cnt - 1,
					 NULL, 0, false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      _help_context, cmd, ctx);
			if (err < 0)
				break;

			init_show(RNBD_CLIENT, LST_PATHS, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    parse_clt_paths_clms,
						    cmd, _help_context, 1);
			if (err < 0)
				break;

			err = show_paths(ctx->name, ctx);
			break;
		case TOK_DISCONNECT:
			err = cmd_path_operation(client_path_disconnect,
						 argc, argv, cmd, _help_context, ctx);
			break;
		case TOK_RECONNECT:
			err = cmd_path_operation(client_path_reconnect,
						 argc, argv, cmd, _help_context, ctx);
			break;
		case TOK_RECOVER:
			err = cmd_path_operation(client_path_recover,
						 argc, argv, cmd, _help_context, ctx);
			break;
		case TOK_ADD:
			err = cmd_path_add(argc, argv, cmd, _help_context, ctx);
			break;
		case TOK_DELETE:
			err = cmd_path_operation(client_path_delete,
						 argc, argv, cmd,
						 _help_context, ctx);
			break;
		case TOK_READD:
			err = cmd_path_operation(client_path_readd,
						 argc, argv, cmd,
						 _help_context, ctx);
			break;
		case TOK_HELP:
			parse_help(argc, argv, NULL, ctx);
			print_help(_help_context, cmd,
				   cmds_client_paths_help, ctx);
			break;
		default:
			print_usage(_help_context, cmds_client_paths_help, ctx);
			handle_unknown_param(cmd->param_str,
					     cmds_client_paths);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_server_sessions(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	const char *_help_context = ctx->pname_with_mode
		? "session" : "server session";

	int err = 0;
	const struct param *cmd;

	cmd = find_param(*argv, cmds_server_sessions);
	if (!cmd) {
		print_usage(_help_context, cmds_server_sessions_help, ctx);
		if (ctx->complete_set)
			err = -EAGAIN;
		else
			err = -EINVAL;

		if (argc)
			handle_unknown_param(*argv, cmds_server_sessions);
		else if (!ctx->complete_set)
			ERR(trm, "Please specify a command\n");
	}
	if (err >= 0) {

		argc--; argv++;

		switch (cmd->tok) {
		case TOK_LIST:

			err = parse_list_parameters(argc, argv, ctx,
						    parse_srv_sessions_clms,
						    cmd, _help_context, 0);
			if (err < 0)
				break;

			err = list_sessions(NULL, 0, sess_srv,
					    sess_srv_cnt - 1, false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      _help_context, cmd, ctx);
			if (err < 0)
				break;

			init_show(RNBD_SERVER, LST_SESSIONS, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    parse_srv_sessions_clms,
						    cmd, _help_context, 0);
			if (err < 0)
				break;

			err = show_server_sessions(ctx->name, ctx);
			break;
		case TOK_DISCONNECT:
			err = cmd_server_session_disconnect(argc, argv, cmd, _help_context, ctx);
			break;
		case TOK_HELP:
			parse_help(argc, argv, NULL, ctx);
			print_help(_help_context, cmd,
				   cmds_server_sessions_help, ctx);
			break;
		default:
			print_usage(_help_context,
				    cmds_server_sessions_help, ctx);
			handle_unknown_param(cmd->param_str,
					    cmds_server_sessions);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_server_devices(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	const char *_help_context = ctx->pname_with_mode
		? "device" : "server device";

	int err = 0;
	const struct param *cmd;

	cmd = find_param(*argv, cmds_server_devices);
	if (!cmd) {
		print_usage(_help_context, cmds_server_devices, ctx);
		if (ctx->complete_set)
			err = -EAGAIN;
		else
			err = -EINVAL;

		if (argc)
			handle_unknown_param(*argv, cmds_server_devices);
		else if (!ctx->complete_set)
			ERR(trm, "Please specify a command\n");
	}
	if (err >= 0) {

		argc--; argv++;

		switch (cmd->tok) {
		case TOK_LIST:

			err = parse_list_parameters(argc, argv, ctx,
						    parse_srv_devices_clms,
						    cmd, _help_context, 0);
			if (err < 0)
				break;

			err = list_devices(NULL, 0, sds_srv,
					   sds_srv_cnt - 1, false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      _help_context, cmd, ctx);
			if (err < 0)
				break;

			init_show(RNBD_SERVER, LST_DEVICES, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    parse_srv_devices_clms,
						    cmd, _help_context, 0);
			if (err < 0)
				break;

			err = show_devices(ctx->name, ctx);
			break;
		case TOK_CLOSE:
			err = cmd_server_devices_force_close(argc, argv, cmd,
					_help_context, ctx);
			break;
		case TOK_HELP:
			parse_help(argc, argv, NULL, ctx);
			print_help(_help_context, cmd,
				   cmds_server_devices, ctx);
			break;
		default:
			print_usage(_help_context, cmds_server_devices, ctx);
			handle_unknown_param(cmd->param_str,
					     cmds_client_sessions);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_server_paths(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	const char *_help_context = ctx->pname_with_mode
		? "path" : "server path";

	int err = 0;
	const struct param *cmd;

	cmd = find_param(*argv, cmds_server_paths);
	if (!cmd) {
		print_usage(_help_context, cmds_server_paths_help, ctx);
		if (ctx->complete_set)
			err = -EAGAIN;
		else
			err = -EINVAL;

		if (argc)
			handle_unknown_param(*argv, cmds_server_paths);
		else if (!ctx->complete_set)
			ERR(trm, "Please specify a command\n");
	}
	if (err >= 0) {

		argc--; argv++;

		switch (cmd->tok) {
		case TOK_LIST:
			err = parse_list_parameters(argc, argv, ctx,
						    parse_srv_paths_clms,
						    cmd, _help_context, 0);
			if (err < 0)
				break;

			err = list_paths(NULL, 0, paths_srv,
					 paths_srv_cnt - 1, false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      _help_context, cmd, ctx);
			if (err < 0)
				break;

			init_show(RNBD_SERVER, LST_PATHS, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    parse_srv_paths_clms,
						    cmd, _help_context, 1);
			if (err < 0)
				break;

			err = show_paths(ctx->name, ctx);
			break;
		case TOK_DISCONNECT:
			err = cmd_path_operation(server_path_disconnect,
						 argc, argv, cmd, _help_context, ctx);
			break;
		case TOK_HELP:
			parse_help(argc, argv, NULL, ctx);
			print_help(_help_context, cmd,
				   cmds_server_paths_help, ctx);
			break;
		default:
			print_usage(_help_context, cmds_server_paths_help, ctx);
			handle_unknown_param(cmd->param_str,
					    cmds_client_sessions);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_client(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	const char *_help_context = "client";
	int err = 0;
	const struct param *param;

	if (argc < 1) {
		usage_param("rnbd client",
			    params_object_type_help_client, ctx);
		if (ctx->complete_set) {
			err = -EAGAIN;
		} else {
			ERR(trm, "no object specified\n");
			err = -EINVAL;
		}
	}
	if (err >= 0) {
		param = find_param(*argv, params_object_type_client);
		if (!param) {
			usage_param("rnbd client",
				   params_object_type_help_client, ctx);
			handle_unknown_param(*argv, params_object_type_client);
			err = -EINVAL;
		} else if (param->parse) {
			(void) param->parse(argc, argv, param, ctx);
		}
	}

	argc--; argv++;

	if (err >= 0) {
		switch (param->tok) {
		case TOK_DEVICES:
			err = cmd_client_devices(argc, argv, ctx);
			break;
		case TOK_SESSIONS:
			err = cmd_client_sessions(argc, argv, ctx);
			break;
		case TOK_PATHS:
			err = cmd_client_paths(argc, argv, ctx);
			break;
		case TOK_DUMP:
			err = cmd_dump_all(argc, argv, param, "", ctx);
			break;
		case TOK_LIST:

			err = parse_list_parameters(argc, argv, ctx,
						    parse_clt_devices_clms,
						    param, _help_context, 0);
			if (err < 0)
				break;

			err = list_devices(sds_clt, sds_clt_cnt - 1,
					   NULL, 0, false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      "name", param, ctx);
			if (err < 0)
				break;

			init_show(RNBD_CLIENT, LST_ALL, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    parse_clt_clms,
						    param, _help_context, 1);
			if (err < 0)
				break;

			err = show_all(ctx->name, ctx);
			break;
		case TOK_MAP:
			err = cmd_map(argc, argv, param, _help_context, ctx);
			break;
		case TOK_RESIZE:
			err = cmd_resize(argc, argv, param, _help_context, ctx);
			break;
		case TOK_UNMAP:
			err = cmd_unmap(argc, argv, param, _help_context, ctx);
			break;
		case TOK_REMAP:
			err = cmd_remap(argc, argv, param, true,
					_help_context, ctx);
			break;
		case TOK_RECOVER:
			err = cmd_recover_device_session_or_path(argc, argv, param,
								 _help_context, ctx);
			break;
		case TOK_HELP:
			if (ctx->pname_with_mode && help_print_flags(ctx)) {
				help_param(ctx->pname, params_flags_help, ctx);
				printf("\n\n");
			}
			help_mode("client",
				  params_object_type_help_client, ctx);
			break;
		default:
			usage_param("rnbd client",
				   params_object_type_help_client, ctx);
			handle_unknown_param(*argv, params_object_type_client);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_server(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	const char *_help_context = "server";
	int err = 0;
	const struct param *param;

	if (argc < 1) {
		usage_param("rnbd server",
			    params_object_type_help_server, ctx);
		if (ctx->complete_set) {
			err = -EAGAIN;
		} else {
			ERR(trm, "no object specified\n");
			err = -EINVAL;
		}
	}
	if (err >= 0) {
		param = find_param(*argv, params_object_type_server);
		if (!param) {
			usage_param("rnbd server",
				   params_object_type_help_server, ctx);
			handle_unknown_param(*argv, params_object_type_server);
			err = -EINVAL;
		} else if (param->parse) {
			(void) param->parse(argc, argv, param, ctx);
		}
	}

	argc--; argv++;

	if (err >= 0) {
		switch (param->tok) {
		case TOK_DEVICES:
			err = cmd_server_devices(argc, argv, ctx);
			break;
		case TOK_SESSIONS:
			err = cmd_server_sessions(argc, argv, ctx);
			break;
		case TOK_PATHS:
			err = cmd_server_paths(argc, argv, ctx);
			break;
		case TOK_DUMP:
			err = cmd_dump_all(argc, argv, param, "", ctx);
			break;
		case TOK_CLOSE:
			err = cmd_server_devices_force_close(argc, argv, param, _help_context, ctx);
			break;
		case TOK_LIST:

			err = parse_list_parameters(argc, argv, ctx,
						    parse_srv_devices_clms,
						    param, _help_context, 0);
			if (err < 0)
				break;

			err = list_devices(NULL, 0, sds_srv,
					   sds_srv_cnt - 1, false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      "name", param, ctx);
			if (err < 0)
				break;

			init_show(RNBD_SERVER, LST_ALL, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    parse_srv_clms,
						    param, _help_context, 1);
			if (err < 0)
				break;

			err = show_all(ctx->name, ctx);
			break;
		case TOK_HELP:
			if (ctx->pname_with_mode && help_print_flags(ctx)) {
				help_param(ctx->pname, params_flags_help, ctx);
				printf("\n\n");
			}
			help_mode("server",
				  params_object_type_help_server,
				  ctx);
			break;
		default:
			usage_param("rnbd server",
				   params_object_type_help_server, ctx);
			handle_unknown_param(*argv, params_object_type_server);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_both(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	int err = 0;
	const struct param *param;

	if (argc < 1) {
		usage_param("rnbd both", params_both_help, ctx);
		if (ctx->complete_set) {
			err = -EAGAIN;
		} else {
			ERR(trm, "no object specified\n");
			err = -EINVAL;
		}
	}
	if (err >= 0) {
		param = find_param(*argv, params_both);
		if (!param) {
			handle_unknown_param(*argv, params_both);
			usage_param(ctx->pname, params_both_help, ctx);
			err = -EINVAL;
		} else if (param->parse) {
			(void) param->parse(argc, argv, param, ctx);
		}
	}

	argc--; argv++;

	if (err >= 0) {
		switch (param->tok) {
		case TOK_DEVICES:
			/* call client function here. Note that list and show */
			/* will behave differently because of ctx->rnbdmode. */
			err = cmd_client_devices(argc, argv, ctx);
			break;
		case TOK_SESSIONS:
			err = cmd_both_sessions(argc, argv, ctx);
			break;
		case TOK_PATHS:
			err = cmd_both_paths(argc, argv, ctx);
			break;
		case TOK_DUMP:
			err = cmd_dump_all(argc, argv, param, "", ctx);
			break;
		case TOK_LIST:
			err = parse_list_parameters(argc, argv, ctx,
						    parse_both_devices_clms,
						    param, "", 0);
			if (err < 0)
				break;

			err = list_devices(sds_clt, sds_clt_cnt - 1,
					   sds_srv, sds_srv_cnt - 1,
					   false, ctx);
			break;
		case TOK_SHOW:
			err = parse_name_help(argc--, argv++,
					      "name", param, ctx);
			if (err < 0)
				break;

			init_show(RNBD_CLIENT|RNBD_SERVER, LST_ALL, ctx);

			err = parse_list_parameters(argc, argv, ctx,
						    parse_both_clms,
						    param, "", 1);
			if (err < 0)
				break;

			err = show_all(ctx->name, ctx);
			break;
		case TOK_MAP:
			err = cmd_map(argc, argv, param, "device", ctx);
			break;
		case TOK_RESIZE:
			err = cmd_resize(argc, argv, param, "device", ctx);
			break;
		case TOK_UNMAP:
			err = cmd_unmap(argc, argv, param, "device", ctx);
			break;
		case TOK_REMAP:
			err = cmd_remap(argc, argv, param, false,
					"device", ctx);
			break;
		case TOK_RECOVER:
			err = cmd_recover_device_session_or_path(argc, argv, param,
								 "client", ctx);
			break;
		case TOK_HELP:
			help_param(ctx->pname, params_both_help, ctx);

			if (ctx->help_set) {

				help_mode("client",
					  params_object_type_help_client, ctx);
				help_mode("server",
					  params_object_type_help_server, ctx);
				help_mode("both", params_both_help, ctx);
			}
			break;
		default:
			handle_unknown_param(*argv, params_both);
			usage_param(ctx->pname, params_both_help, ctx);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

int cmd_start(int argc, const char *argv[], struct rnbd_ctx *ctx)
{
	int err = 0;
	const struct param *param = 0;

	if (argc >= 1)
		param = find_param(*argv, params_mode);
	if (!param) {
		ctx->rnbdmode = mode_for_host();

		INF(ctx->debug_set,
		    "RNBD mode deduced from sysfs: '%s'.\n",
		    mode_to_string(ctx->rnbdmode));

		switch (ctx->rnbdmode) {
		case RNBD_CLIENT:
			return cmd_client(argc, argv, ctx);
		case RNBD_SERVER:
			return cmd_server(argc, argv, ctx);
		case RNBD_BOTH:
			if (argc < 1) {
				usage_param(ctx->pname, params_mode_help, ctx);
				if (!ctx->complete_set)
					ERR(trm, "Please specify an argument\n");
				return -EINVAL;
			}
			return cmd_both(argc, argv, ctx);
		default:
			ERR(trm,
			    "RNBD mode not specified and could not be deduced.\n");
			print_usage(NULL, params_mode_help, ctx);

			handle_unknown_param(*argv, params_mode);
			usage_param(ctx->pname, params_mode_help, ctx);
			err = -EINVAL;
		}
	} else if (param->parse) {
		(void) param->parse(argc, argv, param, ctx);
	}

	if (err >= 0) {
		switch (param->tok) {
		case TOK_CLIENT:
			err = cmd_client(--argc, ++argv, ctx);
			break;
		case TOK_SERVER:
			err = cmd_server(--argc, ++argv, ctx);
			break;
		case TOK_BOTH:
			err = cmd_both(--argc, ++argv, ctx);
			break;
		case TOK_HELP:
			help_start(ctx);
			break;
		case TOK_VERSION:
			print_version(ctx);
			break;
		default:
			handle_unknown_param(*argv, params_mode);
			usage_param(ctx->pname, params_mode_help, ctx);
			err = -EINVAL;
			break;
		}
	}
	return err;
}

static int compar_sds_sess(const void *p1, const void *p2)
{
	const struct rnbd_sess_dev *const *sd1 = p1, *const *sd2 = p2;

	return strcmp((*sd1)->sess->sessname, (*sd2)->sess->sessname);
}

static int compar_sds_dev(const void *p1, const void *p2)
{
	const struct rnbd_sess_dev *const *sd1 = p1, *const *sd2 = p2;

	return strcmp((*sd1)->mapping_path, (*sd2)->mapping_path);
}

int main(int argc, const char *argv[])
{
	int ret = 0;

	struct rnbd_ctx ctx;

	init_rnbd_ctx(&ctx);
	parse_argv0(argv[0], &ctx);
	check_compat_sysfs(&ctx);

	ret = rnbd_sysfs_alloc_all(&sds_clt, &sds_srv,
				    &sess_clt, &sess_srv,
				    &paths_clt, &paths_srv,
				    &sds_clt_cnt, &sds_srv_cnt,
				    &sess_clt_cnt, &sess_srv_cnt,
				    &paths_clt_cnt, &paths_srv_cnt);
	if (ret) {
		ERR(trm, "Failed to alloc memory for sysfs entries: %d\n", ret);
		goto out;
	}

	ret = rnbd_sysfs_read_all(sds_clt, sds_srv, sess_clt, sess_srv,
				   paths_clt, paths_srv);
	if (ret) {
		ERR(trm, "Failed to read sysfs entries: %d\n", ret);
		goto free;
	}
	qsort(sds_clt, sds_clt_cnt - 1, sizeof(*sds_clt), compar_sds_dev);
	qsort(sds_srv, sds_srv_cnt - 1, sizeof(*sds_srv), compar_sds_dev);
	qsort(sds_clt, sds_clt_cnt - 1, sizeof(*sds_clt), compar_sds_sess);
	qsort(sds_srv, sds_srv_cnt - 1, sizeof(*sds_srv), compar_sds_sess);

	ret = read_port_descs(ctx.port_descs, MAX_PATHS_PER_SESSION);
	if (ret < 0) {

		ERR(trm, "Failed to read port descriptions entries: %d\n", ret);
		goto free;
	}
	ctx.port_cnt = ret; ret = 0;

	rnbd_ctx_default(&ctx);

	ret = parse_cmd_parameters(--argc, ++argv, params_flags,
				   &ctx, NULL, NULL, 0);
	if (ret < 0)
		goto free;

	argc -= ret; argv += ret;

	INF(ctx.debug_set, "%s using '%s' sysfs.\n",
	    ctx.pname, get_sysfs_info(&ctx)->path_dev_name);

	if (argc && *argv[0] == '-') {
		handle_unknown_param(*argv, params_flags);
		help_param(ctx.pname, params_flags_help, &ctx);
		ret = -EINVAL;
		goto free;
	} else if (ctx.help_set) {
		help_start(&ctx);
		ret = -EINVAL;
		goto free;
	}
	ret = cmd_start(argc, argv, &ctx);

free:
	rnbd_sysfs_free_all(sds_clt, sds_srv, sess_clt, sess_srv,
			     paths_clt, paths_srv);
out:
	deinit_rnbd_ctx(&ctx);

	if (ret == -EAGAIN)
		/* help message was printed */
		ret = 0;

	return ret;
}
