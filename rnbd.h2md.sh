#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later

############################################
# Run that and redirect to rnbd.8.md       #
# To generate man out of Markdown use:     #
# pandoc rnbd.8.md -s -t man -o man/rnbd.8 # 
############################################

PATH=$PATH:./
VER=$1

if [ -z "$VER" ]; then
	echo "Please provide the version to generate manpage for" 1>&2
	exit 1
else
	echo "Generating manpage version $VER" 1>&2
fi

DATE=$(LANG=en_us_8859_1 && date +"%B %Y")

modes="client server"
objects="device session path"
cmds="list show map resize unmap remap close disconnect reconnect add delete readd recover"

modes_formatted=$(echo "**$modes**" | sed 's/ /** | **/g')
objects_formatted=$(echo "**$objects**" | sed 's/ /** | **/g')
cmds_formatted=$(echo "**$cmds**" | sed 's/ /** | **/g')

echo "---
title: RNBD
section: 8
header: System Administration Utilities
footer: $VER
date: $DATE
---
# NAME
rnbd - configuration tool for RNBD driver and RTRS library

# SYNOPSIS
**rnbd** *[MODE]* *[TARGET]* *<COMMAND\>* *[OPTIONS]*

*MODE* := { $modes_formatted }

*TARGET* := { $objects_formatted }

*COMMAND* := { $cmds_formatted }

*OPTIONS* are command specific.

# DESCRIPTION
RNBD \(RDMA Network Block Device\) is a pair of kernel modules \(client and server\) that allow for remote access of a block device on the server over RTRS protocol using the RDMA \(InfiniBand, RoCE, iWARP\) transport. After being mapped, the remote block devices can be accessed on the client side as local block devices.

**rnbd** is a tool which allows user to control said kernel modules in a convenient way.

# OPTIONS
"
for m in $modes; do
	for o in $objects; do
		for c in $cmds; do
			if rnbd $m $o $c help 2>1 > /dev/null; then
				output=$(rnbd $m $o $c help all | \
				sed -n -e '1s/>/\\>/g' \
					-e '1s/Usage: /**/' \
					-e '1s/ \[OPTIONS\]/** *\[OPTIONS\]*/' \
					-e 's/[ \t]*$//' \
					-e '1s=>=\>=g' \
					-e 's/^Options:/Options:\n/' \
					-e 's/^Arguments:/Arguments:\n/' \
					-e '/^Example:/q;p')
				echo "$output"
			fi
		done
	done
done

echo "
If the context of a command is unambiguous, it can be also called directly. For example: rnbd map (instead of rnbd client device map), rnbd session list (instead of rnbd client session list), rnbd show client@server (instead of rnbd client session show client@server), etc.

# EXAMPLES
List server devices:

    rnbd server devices list

List client sessions:

    rnbd client sessions list

List paths of server, display sizes in KB, display all columns:

    rnbd server paths list K all

List devices imported on client, show only mapping_path and devpath, output in json:

    rnbd client devices list mapping_path,devpath json

# COPYRIGHT
Copyright © 2019 - 2021 IONOS Cloud GmbH. All Rights Reserved

# AUTHORS
Danil Kipnis <danil.kipnis@ionos.com>  
Lutz Pogrell <lutz.pogrell@ionos.com>"
