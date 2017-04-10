/*
 * (c) Copyright 2015 Hewlett Packard Enterprise Development LP
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 */

/************************************************************************//**
 * @ingroup ops-intfd
 *
 * @file
 * Source for Interface daemon (ops-intfd).
 *
 * Purpose: Main file for the implementation of the OpenSwitch Interface Daemon (ops-intfd).
 *
 *          Its purpose in life is:
 *
 *                1. During start up, read interface-related
 *                   configuration data and apply to hardware.
 *
 *                2. During operations, receive administrative
 *                   configuration changes and apply to the hardware.
 *
 *                3. Dynamically configure hardware based on
 *                   operational state changes as needed.
 *
 ***************************************************************************/

#include <stdio.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <config.h>
#include <command-line.h>
#include <compiler.h>
#include <daemon.h>
#include <dirs.h>
#include <dynamic-string.h>
#include <fatal-signal.h>
#include <ovsdb-idl.h>
#include <poll-loop.h>
#include <unixctl.h>
#include <util.h>
#include <openvswitch/vconn.h>
#include <openvswitch/vlog.h>
#include <vswitch-idl.h>
#include <openswitch-idl.h>
#include <hash.h>
#include <shash.h>

#include "intfd.h"
#include "eventlog.h"
#include <diag_dump.h>

VLOG_DEFINE_THIS_MODULE(ops_intfd);

#define DIAGNOSTIC_BUFFER_LEN 16000    /*One interface= 700 characters*/

/** @ingroup ops-intfd
 * @{ */

static void
usage(void)
{
    printf("%s: OpenSwitch interface daemon\n"
           "usage: %s [OPTIONS] [DATABASE]\n"
           "where DATABASE is a socket on which ovsdb-server is listening\n"
           "      (default: \"unix:%s/db.sock\").\n",
           program_name, program_name, ovs_rundir());
    daemon_usage();
    vlog_usage();
    printf("\nOther options:\n"
           "  --unixctl=SOCKET        override default control socket name\n"
           "  -h, --help              display this help message\n");
    exit(EXIT_SUCCESS);
} /* usage */

static void
intfd_unixctl_dump(struct unixctl_conn *conn, int argc,
                          const char *argv[], void *aux OVS_UNUSED)
{
    struct ds ds = DS_EMPTY_INITIALIZER;

    intfd_debug_dump(&ds, argc, argv);

    unixctl_command_reply(conn, ds_cstr(&ds));
    ds_destroy(&ds);
} /* intfd_unixctl_dump */

/*
 * Function         : intfd_diag_dump_basic_cb
 * Responsibility   : callback handler function for diagnostic dump basic
 *                    it allocates memory as per requirement and populates data.
 *                    INIT_DIAG_DUMP_BASIC will free allocated memory.
 * Parameters       : feature name string, buffer ptr
 * Returns          : void
 */

static void
intfd_diag_dump_basic_cb(const char *feature , char **buf)
{
    struct ds ds1 = DS_EMPTY_INITIALIZER;
    const char *argv[]= { "1", "1", NULL};
    if (!buf)
        return;
    *buf = xcalloc(1, DIAGNOSTIC_BUFFER_LEN);
    if(*buf) {
        /* populate basic diagnostic data to buffer  */
        intfd_debug_dump(&ds1, 3, argv);
        sprintf(*buf, "%s", ds_cstr(&ds1));
        VLOG_INFO("basic diag-dump data populated for feature %s", feature);
    } else{
        VLOG_ERR("Memory allocation failed for feature %s , %d bytes",
                feature, DIAGNOSTIC_BUFFER_LEN);
    }
    return;
} /*intfd_diag_dump_basic_cb */

static void
intfd_init(const char *db_path)
{
    int retval;

    /* Initialize IDL through a new connection to the DB. */
    intfd_ovsdb_init(db_path);

    /* Register diagnostic callback function */
    INIT_DIAG_DUMP_BASIC(intfd_diag_dump_basic_cb);

    retval = event_log_init("INTERFACE");
    if(retval < 0) {
        VLOG_ERR("Event log initialization failed for interface");
    }

    /* Initialize the interface arbiter */
    intfd_arbiter_init();

    /* Register ovs-appctl commands for this daemon. */
    unixctl_command_register("ops-intfd/dump", "", 0, 1, intfd_unixctl_dump, NULL);
} /* intfd_init */

static void
intfd_exit(void)
{
    intfd_ovsdb_exit();
} /* intfd_exit */

static char *
parse_options(int argc, char *argv[], char **unixctl_pathp)
{
    enum {
        OPT_UNIXCTL = UCHAR_MAX + 1,
        VLOG_OPTION_ENUMS,
        DAEMON_OPTION_ENUMS,
    };
    static const struct option long_options[] = {
        {"help",        no_argument, NULL, 'h'},
        {"unixctl",     required_argument, NULL, OPT_UNIXCTL},
        DAEMON_LONG_OPTIONS,
        VLOG_LONG_OPTIONS,
        {NULL, 0, NULL, 0},
    };
    char *short_options = long_options_to_short_options(long_options);

    for (;;) {
        int c;

        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'h':
            usage();

        case OPT_UNIXCTL:
            *unixctl_pathp = optarg;
            break;

        VLOG_OPTION_HANDLERS
        DAEMON_OPTION_HANDLERS

        case '?':
            exit(EXIT_FAILURE);

        default:
            abort();
        }
    }
    free(short_options);

    argc -= optind;
    argv += optind;

    switch (argc) {
    case 0:
        return xasprintf("unix:%s/db.sock", ovs_rundir());

    case 1:
        return xstrdup(argv[0]);

    default:
        VLOG_FATAL("at most one non-option argument accepted; "
                   "use --help for usage");
    }
} /* parse_options */

static void
ops_intfd_exit(struct unixctl_conn *conn, int argc OVS_UNUSED,
                  const char *argv[] OVS_UNUSED, void *exiting_)
{
    bool *exiting = exiting_;
    *exiting = true;
    unixctl_command_reply(conn, NULL);
} /* ops_intfd_exit */

int
main(int argc, char *argv[])
{
    char *appctl_path = NULL;
    struct unixctl_server *appctl;
    char *ovsdb_sock;
    bool exiting;
    int retval;

    set_program_name(argv[0]);
    proctitle_init(argc, argv);
    fatal_ignore_sigpipe();

    /* Parse commandline args and get the name of the OVSDB socket. */
    ovsdb_sock = parse_options(argc, argv, &appctl_path);

    /* Initialize the metadata for the IDL cache. */
    ovsrec_init();

    /* Fork and return in child process; but don't notify parent of
     * startup completion yet. */
    daemonize_start();

    /* Create UDS connection for ovs-appctl. */
    retval = unixctl_server_create(appctl_path, &appctl);
    if (retval) {
        exit(EXIT_FAILURE);
    }

    /* Register the ovs-appctl "exit" command for this daemon. */
    unixctl_command_register("exit", "", 0, 0, ops_intfd_exit, &exiting);

    /* Create the IDL cache of the dB at ovsdb_sock. */
    intfd_init(ovsdb_sock);
    free(ovsdb_sock);

    /* Notify parent of startup completion. */
    daemonize_complete();

    /* Enable asynch log writes to disk. */
    vlog_enable_async();

    VLOG_INFO_ONCE("%s (OpenSwitch Interface Daemon) started", program_name);

    exiting = false;
    while (!exiting) {
        intfd_run();
        unixctl_server_run(appctl);

        intfd_wait();
        unixctl_server_wait(appctl);
        if (exiting) {
            poll_immediate_wake();
        } else {
            poll_block();
        }
    }

    intfd_exit();
    unixctl_server_destroy(appctl);

    return 0;
} /* main */
/** @} end of group intfd */
