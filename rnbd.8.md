---
title: RNBD
section: 8
header: System Administration Utilities
footer: 1.2.0
date: July 2021
---
# NAME
rnbd - configuration tool for RNBD driver and RTRS library

# SYNOPSIS
**rnbd** *[MODE]* *[TARGET]* *<COMMAND\>* *[OPTIONS]*

*MODE* := { **client** | **server** }

*TARGET* := { **device** | **session** | **path** }

*COMMAND* := { **list** | **show** | **map** | **resize** | **unmap** | **remap** | **close** | **disconnect** | **reconnect** | **add** | **delete** | **readd** | **recover** }

*OPTIONS* are command specific.

# DESCRIPTION
RNBD \(RDMA Network Block Device\) is a pair of kernel modules \(client and server\) that allow for remote access of a block device on the server over RTRS protocol using the RDMA \(InfiniBand, RoCE, iWARP\) transport. After being mapped, the remote block devices can be accessed on the client side as local block devices.

**rnbd** is a tool which allows user to control said kernel modules in a convenient way.

# OPTIONS

**rnbd client device list** *[OPTIONS]*

List information on devices.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Session        Name of the RTRS session of the device
                    mapping_path    Mapping Path   Mapping name of the remote device
                    devname         Device         Device name under /dev/. I.e. rnbd0
                    devpath         Device path    Device path under /dev/. I.e. /dev/rnbd0
                    state           State          State of the RNBD device. (client only)
                    access_mode     Access Mode    RW mode of the device: ro, rw or migration
                    rx_sect         RX             Amount of data read from the device
                    tx_sect         TX             Amount of data written to the device
                    direction       Direction      Direction of data transfer: imported or exported

                    Default: sessname,mapping_path,devname,state,access_mode

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    notree          Don't display paths for each sessions
    noheaders       Don't print headers
    nototals        Don't print totals
    help            Display help and exit. [fields|all]
**rnbd client device show <device\>** *[OPTIONS]*

Show information about an rnbd block device.

Arguments:

    <device>        Name of a local or a remote block device.
                    I.e. rnbd0, /dev/rnbd0, d12aef94-4110-4321-9373-3be8494a557b.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Session        Name of the RTRS session of the device
                    mapping_path    Mapping Path   Mapping name of the remote device
                    devname         Device         Device name under /dev/. I.e. rnbd0
                    devpath         Device path    Device path under /dev/. I.e. /dev/rnbd0
                    state           State          State of the RNBD device. (client only)
                    access_mode     Access Mode    RW mode of the device: ro, rw or migration
                    rx_sect         RX             Amount of data read from the device
                    tx_sect         TX             Amount of data written to the device
                    direction       Direction      Direction of data transfer: imported or exported

                    Default: sessname,mapping_path,devname,state,access_mode

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    help            Display help and exit. [fields|all]
**rnbd client device map <device\> from <server\>** *[OPTIONS]*

Map a device from a given server

Arguments:

    <device>        Path to the device to be mapped on server side
    from <server>   Address, hostname or session name of the server

Options:

    <path>          Path(s) to establish: [src_addr@]dst_addr
                    Address is [ip:]<ipv4>, [ip:]<ipv6> or gid:<gid>
    {rw}            Access permission on server side: ro|rw|migration. Default: rw
    verbose         Verbose output
    help            Display help and exit
**rnbd client device resize <device\> <size\>** *[OPTIONS]*

Change size of a mapped device

Arguments:

    <device>        Name of the device to be resized
    <size>          New size of the device in bytes
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E

Options:

    verbose         Verbose output
    help            Display help and exit
**rnbd client device unmap <device\>** *[OPTIONS]*

Unmap a given imported device

Arguments:

    <device>        Name of the device to be unmapped

Options:

    force           Force operation
    verbose         Verbose output
    help            Display help and exit
**rnbd client device remap <device\>** *[OPTIONS]*

Remap an imported device

Arguments:

    <identifier>    Identifier of a device to be remapped.

Options:

    force           Force operation
    verbose         Verbose output
    help            Display help and exit
**rnbd client device recover <device\>|all** *[OPTIONS]*

Recover a device: recover a device when it is not open.

Arguments:

    <device>        Name or identifier of a device.

Options:

    all             Recover all
    verbose         Verbose output
    help            Display help and exit
**rnbd client session list** *[OPTIONS]*

List information on sessions.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Session name   Name of the session
                    path_cnt        Path cnt       Number of paths
                    act_path_cnt    Act path cnt   Number of active paths
                    state           State          State of the session.
                    path_uu         PS             Up (U) or down (_) state of every path
                    mp              MP Policy      Multipath policy
                    mp_short        MP             Multipath policy (short)
                    rx_bytes        RX             Bytes received
                    tx_bytes        TX             Bytes send
                    inflights       Inflights      Inflights
                    reconnects      Reconnects     Reconnects
                    direction       Direction      Direction of the session: incoming or outgoing
                    srvname         Server Name    Server name
                    hostname        Hostname       Hostname of the counterpart

                    Default: sessname,state,path_uu,mp_short,tx_bytes,rx_bytes,reconnects

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    notree          Don't display paths for each sessions
    noheaders       Don't print headers
    nototals        Don't print totals
    help            Display help and exit. [fields|all]
**rnbd client session show <session\>** *[OPTIONS]*

Show information about an rnbd session.

Arguments:

    <session>       Session name or remote hostname.
                    I.e. ps401a-1@st401b-2, st401b-2, <ip1>@<ip2>, etc.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Session name   Name of the session
                    path_cnt        Path cnt       Number of paths
                    act_path_cnt    Act path cnt   Number of active paths
                    state           State          State of the session.
                    path_uu         PS             Up (U) or down (_) state of every path
                    mp              MP Policy      Multipath policy
                    mp_short        MP             Multipath policy (short)
                    rx_bytes        RX             Bytes received
                    tx_bytes        TX             Bytes send
                    inflights       Inflights      Inflights
                    reconnects      Reconnects     Reconnects
                    direction       Direction      Direction of the session: incoming or outgoing
                    srvname         Server Name    Server name
                    hostname        Hostname       Hostname of the counterpart

                    Default: sessname,state,path_uu,mp_short,tx_bytes,rx_bytes,reconnects

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    help            Display help and exit. [fields|all]
**rnbd client session reconnect <session\>** *[OPTIONS]*

Disconnect and connect again a whole session

Arguments:

    <session>       Name or identifier of a session.

Options:

    verbose         Verbose output
    help            Display help and exit
**rnbd client session recover <session\>|all [add-missing]** *[OPTIONS]*

Recover a session: reconnect disconnected paths.

Arguments:

    <session>|all   Name or identifier of a session.
                    All recovers all sessions.

Options:

    add-missing     Add missing paths
    verbose         Verbose output
    help            Display help and exit
**rnbd client session remap <session\>** *[OPTIONS]*

Remap all devices of a given session

Arguments:

    <session>       Identifier of a session to remap all devices on.

Options:

    force           Force operation
                    When provided, all devices will be unmapped and mapped again.

    verbose         Verbose output
    help            Display help and exit
**rnbd client path list** *[OPTIONS]*

List information on paths.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Sessname       Name of the session.
                    pathname        Path name      Path name
                    src_addr        Client Addr    Client address of the path
                    dst_addr        Server Addr    Server address of the path
                    hca_name        HCA            HCA name
                    hca_port        Port           HCA port
                    state           State          Name of the path
                    rx_bytes        RX             Bytes received
                    tx_bytes        TX             Bytes send
                    inflights       Inflights      Inflights
                    reconnects      Reconnects     Reconnects
                    direction       Direction      Direction of the path: incoming or outgoing

                    Default: sessname,hca_name,hca_port,dst_addr,state,tx_bytes,rx_bytes

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    notree          Don't display paths for each sessions
    noheaders       Don't print headers
    nototals        Don't print totals
    help            Display help and exit. [fields|all]
**rnbd client path show [session] <path\>** *[OPTIONS]*

Show information about an rnbd transport path.

Arguments:

    [session]       Optional session name to select a path in the case paths
                    with same addresses are used in multiple sessions.
    <path>          Name or identifier of a path:
                    [pathname], [sessname:port]

    <hca_name>:<port>
    <hca_name>
    <port>          alternative to path a hca/port specification
                    might be provided.
                    This requires that session name has been provided.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Sessname       Name of the session.
                    pathname        Path name      Path name
                    src_addr        Client Addr    Client address of the path
                    dst_addr        Server Addr    Server address of the path
                    hca_name        HCA            HCA name
                    hca_port        Port           HCA port
                    state           State          Name of the path
                    rx_bytes        RX             Bytes received
                    tx_bytes        TX             Bytes send
                    inflights       Inflights      Inflights
                    reconnects      Reconnects     Reconnects
                    direction       Direction      Direction of the path: incoming or outgoing

                    Default: sessname,hca_name,hca_port,dst_addr,state,tx_bytes,rx_bytes

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    help            Display help and exit. [fields|all]
**rnbd client path disconnect [session] <path\>** *[OPTIONS]*

Disconnect a path of a given session

Arguments:

    [session]       Optional session name to select a path in the case paths
                    with same addresses are used in multiple sessions.
    <path>          Name or identifier of a path:
                    [pathname], [sessname:port]

    <hca_name>:<port>
    <hca_name>
    <port>          alternative to path a hca/port specification
                    might be provided.
                    This requires that session name has been provided.

Options:

    verbose         Verbose output
    help            Display help and exit
**rnbd client path reconnect [session] <path\>** *[OPTIONS]*

Disconnect and connect again a single path of a session

Arguments:

    [session]       Optional session name to select a path in the case paths
                    with same addresses are used in multiple sessions.
    <path>          Name or identifier of a path:
                    [pathname], [sessname:port]

    <hca_name>:<port>
    <hca_name>
    <port>          alternative to path a hca/port specification
                    might be provided.
                    This requires that session name has been provided.

Options:

    verbose         Verbose output
    help            Display help and exit
**rnbd client path recover [session] <path\>|all** *[OPTIONS]*

Recover a path: reconnect if not in connected state.

Arguments:

    [session]       Optional session name to select a path in the case paths
                    with same addresses are used in multiple sessions.
    <path>          Name or identifier of a path:
                    [pathname], [sessname:port]

    <hca_name>:<port>
    <hca_name>
    <port>          alternative to path a hca/port specification
                    might be provided.
                    This requires that session name has been provided.

Options:

    verbose         Verbose output
    help            Display help and exit
**rnbd client path add <session\> <path\>** *[OPTIONS]*

Add a new path to an existing session

Arguments:

    <session>       Name of the session to add the new path to
    <path>          Path to be added: [src_addr@]dst_addr
                    Address is of the form ip:<ipv4>, ip:<ipv6> or gid:<gid>

Options:

    verbose         Verbose output
    help            Display help and exit
**rnbd client path delete [session] <path\>** *[OPTIONS]*

Delete a given path from the corresponding session

Arguments:

    [session]       Optional session name to select a path in the case paths
                    with same addresses are used in multiple sessions.
    <path>          Name or identifier of a path:
                    [pathname], [sessname:port]

    <hca_name>:<port>
    <hca_name>
    <port>          alternative to path a hca/port specification
                    might be provided.
                    This requires that session name has been provided.

Options:

    verbose         Verbose output
    help            Display help and exit
**rnbd client path readd [session] <path\>** *[OPTIONS]*

Delete and add again a given path to the corresponding session

Arguments:

    [session]       Optional session name to select a path in the case paths
                    with same addresses are used in multiple sessions.
    <path>          Name or identifier of a path:
                    [pathname], [sessname:port]

    <hca_name>:<port>
    <hca_name>
    <port>          alternative to path a hca/port specification
                    might be provided.
                    This requires that session name has been provided.

Options:

    verbose         Verbose output
    help            Display help and exit
**rnbd server device list** *[OPTIONS]*

List information on devices.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Session        Name of the RTRS session of the device
                    mapping_path    Mapping Path   Mapping name of the remote device
                    devname         Device         Device name under /dev/. I.e. rnbd0
                    devpath         Device path    Device path under /dev/. I.e. /dev/rnbd0
                    access_mode     Access Mode    RW mode of the device: ro, rw or migration
                    rx_sect         RX             Amount of data read from the device
                    tx_sect         TX             Amount of data written to the device
                    direction       Direction      Direction of data transfer: imported or exported
                    Default: sessname,mapping_path,devname,access_mode

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    notree          Don't display paths for each sessions
    noheaders       Don't print headers
    nototals        Don't print totals
    help            Display help and exit. [fields|all]
**rnbd server device show <device\>** *[OPTIONS]*

Show information about an rnbd block device.

Arguments:

    <device>        Name of a local or a remote block device.
                    I.e. rnbd0, /dev/rnbd0, d12aef94-4110-4321-9373-3be8494a557b.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Session        Name of the RTRS session of the device
                    mapping_path    Mapping Path   Mapping name of the remote device
                    devname         Device         Device name under /dev/. I.e. rnbd0
                    devpath         Device path    Device path under /dev/. I.e. /dev/rnbd0
                    access_mode     Access Mode    RW mode of the device: ro, rw or migration
                    rx_sect         RX             Amount of data read from the device
                    tx_sect         TX             Amount of data written to the device
                    direction       Direction      Direction of data transfer: imported or exported
                    Default: sessname,mapping_path,devname,access_mode

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    help            Display help and exit. [fields|all]
**rnbd server device close <device\>** *[OPTIONS]*

Close a particular device for a given session

Arguments:

    <device>        Identifier of a device to be closed.

Options:

    <session>       Identifier of a session for which the device is to be closed.
    force           Force operation
    verbose         Verbose output
    help            Display help and exit
**rnbd server session list** *[OPTIONS]*

List information on sessions.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Session name   Name of the session
                    path_cnt        Path cnt       Number of paths
                    rx_bytes        RX             Bytes received
                    tx_bytes        TX             Bytes send
                    inflights       Inflights      Inflights
                    direction       Direction      Direction of the session: incoming or outgoing
                    hostname        Hostname       Hostname of the counterpart
                    Default: sessname,path_cnt,tx_bytes,rx_bytes,inflights

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    notree          Don't display paths for each sessions
    noheaders       Don't print headers
    nototals        Don't print totals
    help            Display help and exit. [fields|all]
**rnbd server session show <session\>** *[OPTIONS]*

Show information about an rnbd session.

Arguments:

    <session>       Session name or remote hostname.
                    I.e. ps401a-1@st401b-2, st401b-2, <ip1>@<ip2>, etc.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Session name   Name of the session
                    path_cnt        Path cnt       Number of paths
                    rx_bytes        RX             Bytes received
                    tx_bytes        TX             Bytes send
                    inflights       Inflights      Inflights
                    direction       Direction      Direction of the session: incoming or outgoing
                    hostname        Hostname       Hostname of the counterpart
                    Default: sessname,path_cnt,tx_bytes,rx_bytes,inflights

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    help            Display help and exit. [fields|all]
**rnbd server session disconnect <session\>** *[OPTIONS]*

Disconnect all paths on a given session

Arguments:

    <session>       Name or identifier of a session.

Options:

    verbose         Verbose output
    help            Display help and exit
**rnbd server path list** *[OPTIONS]*

List information on paths.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Sessname       Name of the session.
                    pathname        Path name      Path name
                    src_addr        Client Addr    Client address of the path
                    dst_addr        Server Addr    Server address of the path
                    hca_name        HCA            HCA name
                    hca_port        Port           HCA port
                    rx_bytes        RX             Bytes received
                    tx_bytes        TX             Bytes send
                    inflights       Inflights      Inflights
                    direction       Direction      Direction of the path: incoming or outgoing
                    Default: sessname,hca_name,hca_port,src_addr,tx_bytes,rx_bytes

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    notree          Don't display paths for each sessions
    noheaders       Don't print headers
    nototals        Don't print totals
    help            Display help and exit. [fields|all]
**rnbd server path show [session] <path\>** *[OPTIONS]*

Show information about an rnbd transport path.

Arguments:

    [session]       Optional session name to select a path in the case paths
                    with same addresses are used in multiple sessions.
    <path>          Name or identifier of a path:
                    [pathname], [sessname:port]

    <hca_name>:<port>
    <hca_name>
    <port>          alternative to path a hca/port specification
                    might be provided.
                    This requires that session name has been provided.

Options:

    {fields}        Comma separated list of fields to be printed.
                    The list can be prefixed with '+' or '-' to add or remove
                    fields from the default selection.

                    Field           Header         Description
                    sessname        Sessname       Name of the session.
                    pathname        Path name      Path name
                    src_addr        Client Addr    Client address of the path
                    dst_addr        Server Addr    Server address of the path
                    hca_name        HCA            HCA name
                    hca_port        Port           HCA port
                    rx_bytes        RX             Bytes received
                    tx_bytes        TX             Bytes send
                    inflights       Inflights      Inflights
                    direction       Direction      Direction of the path: incoming or outgoing
                    Default: sessname,hca_name,hca_port,src_addr,tx_bytes,rx_bytes

    {format}        Output format: csv|json|xml
    {unit}          Units to use for size (in binary): B|K|M|G|T|P|E
    help            Display help and exit. [fields|all]
**rnbd server path disconnect [session] <path\>** *[OPTIONS]*

Disconnect a path of a given session

Arguments:

    [session]       Optional session name to select a path in the case paths
                    with same addresses are used in multiple sessions.
    <path>          Name or identifier of a path:
                    [pathname], [sessname:port]

    <hca_name>:<port>
    <hca_name>
    <port>          alternative to path a hca/port specification
                    might be provided.
                    This requires that session name has been provided.

Options:

    verbose         Verbose output
    help            Display help and exit

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
Lutz Pogrell <lutz.pogrell@ionos.com>
