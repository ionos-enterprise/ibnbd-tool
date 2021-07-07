// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Configuration tool for RNBD driver and RTRS library.
 *
 * Copyright (c) 2019 1&1 IONOS SE. All rights reserved.
 * Authors: Danil Kipnis <danil.kipnis@cloud.ionos.com>
 *          Lutz Pogrell <lutz.pogrell@cloud.ionos.com>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <arpa/inet.h>  /* AF_INET */
#include <unistd.h>	/* for isatty() */
#include <stdbool.h>
#include <dirent.h>	/* for opendir() */

#include "table.h"
#include "misc.h"

#include "rnbd-sysfs.h"

#define HCA_DIR "/sys/class/infiniband/"
extern bool trm;

const struct bit_str bits[] = {
	{"B", 0, "Byte"}, {"K", 10, "KiB"}, {"M", 20, "MiB"}, {"G", 30, "GiB"},
	{"T", 40, "TiB"}, {"P", 50, "PiB"}, {"E", 60, "EiB"}
};

int get_unit_index(const char *unit, int *index)
{
	int i;

	for (i = 0; i < ARRSIZE(bits); i++)
		if (!strcasecmp(bits[i].str, unit)) {
			*index = i;
			return 0;
		}

	return -ENOENT;
}

int get_unit_shift(const char *unit, int *shift)
{
	int index;

	if (get_unit_index(unit, &index) >= 0) {
		*shift = bits[index].bits;
		return 0;
	}
	return -ENOENT;
}

int i_to_str_unit(uint64_t d, char *str, size_t len, int unit, int prec)
{
	if (unit >= ARRSIZE(bits))
		return -ENOENT;

	if (!unit)
		return snprintf(str, len, "%" PRIu64, d);

	return snprintf(str, len, "%.*f", prec,
			(double)d / (1L << bits[unit].bits));
}

void trim(char *s)
{
	char *new = s;

	while (*s) {
		if (!isspace(*s))
			*new++ = *s;
		s++;
	}
	*new = *s;
}

int str_to_size(const char *str, struct rnbd_ctx *ctx)
{
	char buff[NAME_MAX];
	char *unit = NULL;
	int index = 0;
	int shift;
	uint64_t num;
	int ret;

	strncpy(buff, str, NAME_MAX);
	trim(buff);
	ret = sscanf(buff, "%" SCNu64 "%ms", &num, &unit);
	if (ret < 1)
		return -EINVAL;

	if (ret == 2) {
		ret = get_unit_index(unit, &index);
		free(unit);
		if (ret)
			return ret;
		
		shift = bits[index].bits;
		if (shift > 9)
			ctx->size_sect = num << (shift-9);
		else
			ctx->size_sect = num >> (9-shift);

		ctx->size_state = size_unit;

	} else {
		ctx->size_sect = num;
		ctx->size_state = size_number;
	}
	return 0;
}

int i_to_str(uint64_t d, char *str, size_t len, int prec)
{
	int i;

	for (i = ARRSIZE(bits) - 1; i > 0; i--)
		if (d >> bits[i].bits)
			break;
	if (!i)
		return snprintf(str, len, "%" PRIu64 "%s", d, bits[i].str);

	return snprintf(str, len, "%.*f%s", prec,
			(double)d / (1L << bits[i].bits), bits[i].str);
}

int sd_state_to_str(char *str, size_t len, const struct rnbd_ctx *ctx,
		    enum color *clr, void *v, bool humanize)
{
	struct rnbd_sess_dev *sd = container_of(v, struct rnbd_sess_dev,
						 sess);

	if (!strcmp("open", sd->dev->state))
		*clr = CGRN;
	else
		*clr = CRED;

	return snprintf(str, len, "%s", sd->dev->state);
}

int byte_to_str(char *str, size_t len, const struct rnbd_ctx *ctx,
		enum color *clr, void *v, bool humanize)
{
	*clr = CNRM;
	return i_to_byte_unit(str, len, ctx, *(uint64_t *)v, humanize);
}

int sd_devname_to_str(char *str, size_t len, const struct rnbd_ctx *ctx,
		      enum color *clr, void *v, bool humanize)
{
	struct rnbd_sess_dev *sd = container_of(v, struct rnbd_sess_dev,
						 sess);

	*clr = CNRM;

	return snprintf(str, len, "%s", sd->dev->devname);
}

int sd_devpath_to_str(char *str, size_t len, const struct rnbd_ctx *ctx,
		      enum color *clr, void *v, bool humanize)
{
	struct rnbd_sess_dev *sd = container_of(v, struct rnbd_sess_dev,
						 sess);

	*clr = CNRM;

	return snprintf(str, len, "%s", sd->dev->devpath);
}

int sd_rx_to_str(char *str, size_t len, const struct rnbd_ctx *ctx,
		 enum color *clr, void *v, bool humanize)
{
	struct rnbd_sess_dev *sd = container_of(v, struct rnbd_sess_dev,
						 sess);

	*clr = CNRM;

	if (humanize)
		return i_to_byte_unit(str, len, ctx, sd->dev->rx_sect << 9, humanize);
	else
		return snprintf(str, len, "%" PRIu64, sd->dev->rx_sect);
}

int sd_tx_to_str(char *str, size_t len, const struct rnbd_ctx *ctx,
		 enum color *clr, void *v, bool humanize)
{
	struct rnbd_sess_dev *sd = container_of(v, struct rnbd_sess_dev,
						 sess);

	*clr = CNRM;

	if (humanize)
		return i_to_byte_unit(str, len, ctx, sd->dev->tx_sect << 9, humanize);
	else
		return snprintf(str, len, "%" PRIu64, sd->dev->tx_sect);
}

int dev_sessname_to_str(char *str, size_t len, const struct rnbd_ctx *ctx,
			enum color *clr, void *v, bool humanize)
{
	struct rnbd_sess_dev *sd = container_of(v, struct rnbd_sess_dev,
						 sess);
	*clr = CNRM;

	if (!sd->sess)
		return 0;

	return snprintf(str, len, "%s", sd->sess->sessname);
}

int rnbd_path_state_to_str(char *str, size_t len, const struct rnbd_ctx *ctx,
			    enum color *clr, void *v, bool humanize)
{
	struct rnbd_path *p = container_of(v, struct rnbd_path, state);

	if (!strcmp(p->state, "connected"))
		*clr = CGRN;
	else
		*clr = CRED;

	return snprintf(str, len, "%s", p->state);
}

static bool is_gid(const char *arg)
{
	if (strncmp("gid:", arg, 4) == 0)
		return true;
	else
		return false;
}

int rnbd_addr_to_norm(char *str, size_t len, char *v)
{
	char addr[16];
	int cnt, af;

	if (is_gid(v))
		cnt = sprintf(str, "%s", "gid:");
	else
		cnt = sprintf(str, "%s", "ip:");

	if (inet_pton(af = AF_INET6, v + cnt,  addr) < 1)
		if (inet_pton(af = AF_INET, v + cnt, addr) < 1)
			goto out;

	if (!inet_ntop(af, addr, str + cnt, len - cnt))
		goto out;

	return strlen(str);

out:
	return snprintf(str, len, "%s", v);
}

int rnbd_pathname_to_norm(char *str, size_t len, char *v)
{
	char *s, *at;
	int cnt;

	s = strdup(v);

	at = strchr(s, '@');
	if (!at)
		return snprintf(str, len, "%s", v);

	*at = 0;
	cnt = rnbd_addr_to_norm(str, len, s);
	cnt += snprintf(str + cnt, len - cnt, "%s", "@");
	cnt += rnbd_addr_to_norm(str + cnt, len - cnt, at + 1);

	free(s);

	return cnt;
}

int addr_to_norm(char *str, size_t len, const struct rnbd_ctx *ctx,
		 enum color *clr, void *v, bool humanize)
{
	*clr = CNRM;

	if (!humanize)
		return snprintf(str, len, "%s", (char *) v);

	return rnbd_addr_to_norm(str, len, v);
}

int path_to_norm(char *str, size_t len, const struct rnbd_ctx *ctx,
		 enum color *clr, void *v, bool humanize)
{
	*clr = CNRM;

	if (!humanize)
		return snprintf(str, len, "%s", (char *) v);

	return rnbd_pathname_to_norm(str, len, v);
}

int path_to_sessname(char *str, size_t len, const struct rnbd_ctx *ctx,
		     enum color *clr, void *v, bool humanize)
{
	struct rnbd_path *p = container_of(v, struct rnbd_path, sess);

	*clr = CNRM;

	if (p->sess)
		return snprintf(str, len, "%s", p->sess->sessname);
	else
		return snprintf(str, len, "%s", "");
}

int sd_sess_to_direction(char *str, size_t len, const struct rnbd_ctx *ctx,
			 enum color *clr, void *v, bool humanize)
{
	struct rnbd_sess_dev *p = container_of(v, struct rnbd_sess_dev, sess);

	*clr = CNRM;

	if (!p->sess)
		return 0;

	switch (p->sess->side) {
	case RNBD_CLIENT:
		return snprintf(str, len, "import");
	case RNBD_SERVER:
		return snprintf(str, len, "export");
	default:
		return snprintf(str, len, "?");
	}
}

int i_to_byte_unit(char *str, size_t len, const struct rnbd_ctx *ctx,
		   uint64_t v, bool humanize)
{
	if (humanize)
		if (ctx->unit_set)
			return i_to_str_unit(v, str, len, ctx->unit_id,
					     ctx->prec);
		else
			return i_to_str(v, str, len, ctx->prec);
	else
		return snprintf(str, len, "%" PRIu64, v);
}

int path_to_shortdesc(char *str, size_t len, const struct rnbd_ctx *ctx,
		      enum color *clr, void *v, bool humanize)
{
	struct rnbd_path *p = container_of(v, struct rnbd_path, sess);
	enum color c;
	char path_short[NAME_MAX];

	*clr = CDIM;

	/* return if sess not set (this is the case if called on a total) */
	if (!p->sess)
		return 0;

	if (!p->state[0] || !strcmp(p->state, "connected"))
		c = CDIM;
	else
		c = CRED;

	rnbd_pathname_to_norm(path_short, sizeof(path_short), p->pathname);

	return snprintf(str, len, "%s %d %s%s%s", p->hca_name, p->hca_port,
			CLR(trm, c, path_short));
}

int act_path_cnt_to_state(char *str, size_t len, const struct rnbd_ctx *ctx,
			  enum color *clr, void *v, bool humanize)
{
	int act_path_cnt = *(int *)v;

	if (act_path_cnt) {
		*clr = CGRN;
		return snprintf(str, len, "connected");
	}

	*clr = CRED;

	return snprintf(str, len, "disconnected");
}

int sessname_to_srvname(char *str, size_t len, const struct rnbd_ctx *ctx,
			enum color *clr, void *v, bool humanize)
{
	*clr = CNRM;

	return snprintf(str, len, "%s", strchrnul(v, '@') + 1);
}

int sess_side_to_direction(char *str, size_t len, const struct rnbd_ctx *ctx,
			   enum color *clr, void *v, bool humanize)
{
	struct rnbd_sess *s = container_of(v, struct rnbd_sess, side);

	*clr = CNRM;
	if (s->side == RNBD_CLIENT)
		return snprintf(str, len, "outgoing");
	else
		return snprintf(str, len, "incoming");
}


int path_sess_to_direction(char *str, size_t len, const struct rnbd_ctx *ctx,
			   enum color *clr, void *v, bool humanize)
{
	struct rnbd_path *p = container_of(v, struct rnbd_path, sess);

	*clr = CNRM;

	/* return if sess not set (this is the case if called on a total) */
	if (!p->sess)
		return 0;

	switch (p->sess->side) {
	case RNBD_CLIENT:
		return snprintf(str, len, "outgoing");
	case RNBD_SERVER:
		return snprintf(str, len, "incoming");
	default:
		return snprintf(str, len, "?");
	}
}

static bool is_ip(const char *arg)
{
	if (strncmp("ip:", arg, 3) == 0)
		return true;
	else
		return false;
}

static bool is_ipv4_addr(const char *arg)
{
	char addr[4];

	return inet_pton(AF_INET, arg, addr);
}

static bool is_ipv6_addr(const char *arg)
{
	char ipv6_str_base[INET6_ADDRSTRLEN];
	char addr[16];
	const char *str_addr = arg;
	const char *percent_sign = strchr(arg, '%');

	if (percent_sign) {
		memset(ipv6_str_base, 0, sizeof(ipv6_str_base));
		memcpy(ipv6_str_base, arg, percent_sign - arg < INET6_ADDRSTRLEN
		       		? percent_sign - arg : INET6_ADDRSTRLEN);
			str_addr = ipv6_str_base;
	}
	return inet_pton(AF_INET6, str_addr, addr);
}

bool is_path_addr(const char *arg)
{
	if (is_gid(arg) && is_ipv6_addr(arg+4))
		return true;

	if (is_ip(arg) && (is_ipv4_addr(arg+3) || is_ipv6_addr(arg+3)))
		return true;

	return false;
}

/**
 * Evaluate whether both left and right string represent
 * equivalent RNBD path addresses.
 *
 * Address strings start with either 'ip:' or 'gid:'
 * followed by an valid (IPv4 or IPv6) address
 * or a valid gid (which looks like a link local IPv6 address).
 *
 * If both are neither one or the other string comparison is used
 * as fallback.
 *
 * For both ip and gid two addresses match if they are representations
 * if the same 'normalized' ip address string.
 */
bool match_path_addr(const char *left, const char *right)
{
	bool left_is_gid;
	bool left_is_ip;
	bool right_is_ip;
	char left_addr[16];
	char right_addr[16];
	char left_normalized[INET6_ADDRSTRLEN];
	char right_normalized[INET6_ADDRSTRLEN];

	if (!left && !right)
		return true;

	if ((!left && right) || (left && !right))
		return false;

	if ((is_gid(left) && is_gid(right))
	    || (is_ip(left) && is_ip(right))) {

		left_is_gid = is_gid(left);
		left_is_ip = inet_pton(AF_INET6, left+(left_is_gid ? 4 : 3),
				       left_addr);
		right_is_ip = inet_pton(AF_INET6, right+(left_is_gid ? 4 : 3),
					right_addr);

		if ((!left_is_ip && right_is_ip)
		    || (left_is_ip && !right_is_ip))
			return false;

		if (left_is_ip) {

			inet_ntop(AF_INET6, left_addr, left_normalized,
				  INET6_ADDRSTRLEN);
			inet_ntop(AF_INET6, right_addr, right_normalized,
				  INET6_ADDRSTRLEN);

			return !strcmp(left_normalized, right_normalized);

		} else if (!left_is_gid) {
			left_is_ip = inet_aton(left+3,
					       (struct in_addr *)left_addr);
			right_is_ip = inet_aton(right+3,
						(struct in_addr *)right_addr);

			if ((!left_is_ip && right_is_ip)
			    || (left_is_ip && !right_is_ip))
				return false;

			if (left_is_ip) {

				inet_ntop(AF_INET, left_addr,
					  left_normalized, INET6_ADDRSTRLEN);
				inet_ntop(AF_INET, right_addr,
					  right_normalized, INET6_ADDRSTRLEN);

				return !strcmp(left_normalized,
					       right_normalized);
			}
		}
	}

	return !strcmp(left, right);
}

int sessname_from_host(const char *from_name, char *out_buf, size_t buf_len)
{
	char host_name[HOST_NAME_MAX];

	int res = gethostname(host_name, sizeof(host_name));
	if (res)
		return -errno;

	res = snprintf(out_buf, buf_len, "%s@%s", host_name, from_name);
	if (res < 0)
		return res;

	if (res >= buf_len)
		return -ENAMETOOLONG;

	return 0; /* not res, will be > 0 when snprintf succeeds */
}

int read_port_descs(struct port_desc *port_descs, int max_ports)
{
	int cnt = 0;
	char hca_subdir[PATH_MAX];
	char sysfs_path[PATH_MAX];

	struct dirent *hca_entry;
	struct dirent *port_entry;
	DIR *hca_dirp;
	DIR *port_dirp;

	hca_dirp = opendir(HCA_DIR);
	if (!hca_dirp)
		return 0;

	for (hca_entry = readdir(hca_dirp);
	     hca_entry;
	     hca_entry = readdir(hca_dirp)) {

		if (hca_entry->d_name[0] == '.')
			continue;

		snprintf(hca_subdir, sizeof(hca_subdir),
			 HCA_DIR "%s/ports/", hca_entry->d_name);

		port_dirp = opendir(hca_subdir);
		if (!hca_dirp)
			return -errno; /* TODO continue? */

		for (port_entry = readdir(port_dirp);
		     port_entry;
		     port_entry = readdir(port_dirp)) {

			if (port_entry->d_name[0] == '.')
				continue;

			if (cnt >= max_ports)
				return -ENAMETOOLONG;

			strncpy(port_descs[cnt].hca, hca_entry->d_name,
				sizeof(port_descs[cnt].hca));
			strncpy(port_descs[cnt].port, port_entry->d_name,
				sizeof(port_descs[cnt].port));

			snprintf(sysfs_path, sizeof(sysfs_path),
				 HCA_DIR "%s/ports/%s/gids/",
				 hca_entry->d_name, port_entry->d_name);
			scanf_sysfs(sysfs_path, "0", "%s", port_descs[cnt].gid);

			cnt++;
		}
		closedir(port_dirp);
	}
	closedir(hca_dirp);

	return cnt;
}

int start_shell_exec(FILE **pipe, const char *cmd)
{
	int err = 0;

	*pipe = popen(cmd, "re");
	if (!*pipe) {
		err = -errno;
		ERR(trm, "failed to execute '%s': %s (%d)\n",
		    cmd, strerror(-err), err);
	}

	return err;
}


int stop_shell_exec(FILE *pipe)
{
	int err;

	err = pclose(pipe);
	if (err == -1) {
		err = -errno;
		ERR(trm, "pclose failed: %s (%d)\n",
		    strerror(-err), err);
	} else if (err) {

		err = -(((unsigned int)err) << 8);
	}
	return err;
}

char *trimstr(char *str, char token)
{
	char *trimmed = str, *end;
	int len;

	if (str == NULL || (len = strlen(str)) == 0)
		return trimmed;

	/* Search from front for symbol != token */
	while (*str == token)
		str++;

	if (str != trimmed) {
		/* trim string from front */
		len -= (str - trimmed);
		memmove(trimmed, str, len + 1);
	}

	if (len) {
		end = trimmed + len;
		/* Search from back for symbol != token */
		while (end > trimmed && *--end == token);

		/* trim string from back */
		*(end + 1) = '\0';
	}

	return trimmed;
}

union gid_buffer {
	unsigned long long u64;
	struct { /* intel only! */
		unsigned short w0;
		unsigned short w1;
		unsigned short w2;
		unsigned short w3;
	};
};

int rnbd_resolve(const char *host, const char *hca, const char *port,
		  const char *client_gid,
		  struct path *path, int len,
		  const struct rnbd_ctx *ctx)
{
	int cnt = 0;
	int err = 0;
	FILE *pipe;
	char *read_success = NULL;
	char cmd[512];
	char buf[512];
	char *output;
	union gid_buffer val;

	snprintf(cmd, sizeof(cmd),
		 "saquery -C %s -P %s | grep -wB10 %s |grep port_guid | cut -d'x' -f2",
		 hca, port, host);

	err = start_shell_exec(&pipe, cmd);
	if (err)
		return err;

	do {
		read_success = fgets(buf, sizeof(buf), pipe);
		if (read_success) {
			output = trimstr(trimstr(buf, '\n'), ' ');
			sscanf(output, "%llx", &val.u64);
			snprintf(buf, sizeof(buf), "gid:%s", client_gid);
			path[cnt].src = strdup(buf);
			snprintf(buf, sizeof(buf),
				"gid:fe80:0000:0000:0000:%.04x:%.04x:%.04x:%.04x",
				val.w3, val.w2, val.w1, val.w0);
			path[cnt].dst = strdup(buf);
			cnt++;
		}
	} while (read_success && cnt < len);

	err = stop_shell_exec(pipe);
	if (err) {
		if (((err) < 0 && (err) >= -255))
			return err;
		else
			return -1;
	}

	return cnt;
}

int resolve_host(const char *from_name, struct path *path,
		 const struct rnbd_ctx *ctx)
{
	int err = 0, i, gid_cnt;

	gid_cnt = 0;

	for (i = 0; i < ctx->port_cnt && err >= 0; i++) {

		err = rnbd_resolve(from_name,
				   ctx->port_descs[i].hca,
				   ctx->port_descs[i].port,
				   ctx->port_descs[i].gid,
				   path+gid_cnt, MAX_PATHS_PER_SESSION-gid_cnt,
				   ctx);
		if (err >= 0)
			gid_cnt += err;
	}
	if (err >= 0)
		err = gid_cnt;

	return err;
}

int hostname_from_path(char *host, int host_len, const char *hca, int port,
		       const char *server_gid)
{
	int err = 0;
	union gid_buffer val;
	int w3_i, w2_i, w1_i, w0_i;

	char cmd[512];
	char buf[512];
	char *output;
	FILE *pipe;
	char *read_success = NULL;

	if (strncmp(server_gid, "gid:", 4)) {
		ERR(trm, "Destination address is not a GID '%s'\n", server_gid);
		return -EINVAL;
	}

	sscanf(server_gid, "gid:fe80:0000:0000:0000:%x:%x:%x:%x",
	       &w3_i, &w2_i, &w1_i, &w0_i);
	val.w3 = w3_i; val.w2 = w2_i; val.w1 = w1_i; val.w0 = w0_i;

	snprintf(cmd, sizeof(cmd),
		 "saquery -C %s -P %d -U 0x%llx | awk '{ print $1 }'",
		 hca, port, val.u64);

	err = start_shell_exec(&pipe, cmd);
	if (err)
		return err;

	do {
		read_success = fgets(buf, sizeof(buf), pipe);
		if (read_success) {
			output = trimstr(trimstr(buf, '\n'), ' ');
			strncpy(host, output, host_len);
			break;
		}
	} while (read_success);

	err = stop_shell_exec(pipe);
	if (!read_success) {
		return -EINVAL;
	} else if (err) {
		if (((err) < 0 && (err) >= -255))
			return err;
		else
			return -1;
	}

	return err;
}

