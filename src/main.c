/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Core routine
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "daemon.h"
#include "log.h"
#include "api/socket.h"
#include "setupVars.h"
#include "args.h"
#include "config.h"
#include "database/common.h"
#include "database/query-table.h"
#include "main.h"
#include "signals.h"
#include "regex_r.h"
#include "shmem.h"
#include "capabilities.h"
#include "database/gravity-db.h"
#include "timers.h"

char * username;
bool needGC = false;
bool needDBGC = false;
bool startup = true;

int main (int argc, char* argv[])
{
	// Get user pihole-FTL is running as
	// We store this in a global variable
	// such that the log routine can access
	// it if needed
	username = getUserName();

	// Parse arguments
	// We run this also for no direct arguments
	// to have arg{c,v}_dnsmasq initialized
	parse_args(argc, argv);

	// Try to open FTL log
	open_FTL_log(true);
	timer_start(EXIT_TIMER);
	logg("########## FTL started! ##########");
	log_FTL_version(false);

	// Catch signals like SIGTERM and SIGINT
	// Other signals like SIGHUP, SIGUSR1 are handled by the resolver part
	handle_signals();

	// Process pihole-FTL.conf
	read_FTLconf();

	// Initialize shared memory
	if(!init_shmem())
	{
		logg("Initialization of shared memory failed.");
		return EXIT_FAILURE;
	}

	// pihole-FTL should really be run as user "pihole" to not mess up with file permissions
	// print warning otherwise
	if(strcmp(username, "pihole") != 0)
		logg("WARNING: Starting pihole-FTL as user %s is not recommended", username);

	// Initialize query database (pihole-FTL.db)
	db_init();

	// Try to import queries from long-term database if available
	if(database && config.DBimport)
		DB_read_queries();

	log_counter_info();
	check_setupVarsconf();

	// Check for availability of advanced capabilities
	// immediately before starting the resolver.
	check_capabilities();

	// Start the resolver
	startup = false;
	main_dnsmasq(argc_dnsmasq, argv_dnsmasq);

	logg("Shutting down...");

	// Cancel active threads as we don't need them any more
	if(ipv4telnet) pthread_cancel(telnet_listenthreadv4);
	if(ipv6telnet) pthread_cancel(telnet_listenthreadv6);
	pthread_cancel(socket_listenthread);

	// Save new queries to database
	if(database)
	{
		DB_save_queries();
		logg("Finished final database update");
	}

	// Close sockets
	close_telnet_socket();
	close_unix_socket();

	// Close gravity database connection
	gravityDB_close();

	// Remove shared memory objects
	// Important: This invalidated all objects such as
	//            counters-> ... Do this last when
	//            terminating in main.c !
	destroy_shmem();

	//Remove PID file
	removepid();
	logg("########## FTL terminated after %.1f ms! ##########", timer_elapsed_msec(EXIT_TIMER));
	return EXIT_SUCCESS;
}
