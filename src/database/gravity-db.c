/* Pi-hole: A black hole for Internet advertisements
*  (c) 2017 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  Gravity database routines
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "sqlite3.h"
#include "gravity-db.h"
#include "config.h"
#include "log.h"
// match_regex()
#include "regex_r.h"
// getstr()
#include "shmem.h"
// SQLite3 prepared statement vectors
#include "../vector.h"
// log_subnet_warning()
#include "database/message-table.h"
// getMACfromIP()
#include "database/network-table.h"

// Process-private prepared statements are used to support multiple forks (might
// be TCP workers) to use the database simultaneously without corrupting the
// gravity database
sqlite3_stmt_vec *whitelist_stmt = NULL;
sqlite3_stmt_vec *gravity_stmt = NULL;
sqlite3_stmt_vec *blacklist_stmt = NULL;

// Private variables
static sqlite3 *gravity_db = NULL;
static sqlite3_stmt* table_stmt = NULL;
static sqlite3_stmt* auditlist_stmt = NULL;
bool gravityDB_opened = false;
static pid_t main_process = 0, this_process = 0;

// Table names corresponding to the enum defined in gravity-db.h
static const char* tablename[] = { "vw_gravity", "vw_blacklist", "vw_whitelist", "vw_regex_blacklist", "vw_regex_whitelist" , ""};

// Prototypes from functions in dnsmasq's source
void rehash(int size);

// Initialize gravity subroutines
static void gravityDB_check_fork(void)
{
	// Memorize main process PID on first call of this funtion (guaranteed to be
	// the main dnsmasq thread)
	if(main_process == 0)
	{
		main_process = getpid();
		this_process = main_process;
	}

	if(this_process == getpid())
		return;

	// If we reach this point, FTL forked to handle TCP connections with
	// dedicated (forked) workers SQLite3's mentions that carrying an open
	// database connection across a fork() can lead to all kinds of locking
	// problems as SQLite3 was not intended to work under such circumstances.
	// Doing so may easily lead to ending up with a corrupted database.
	logg("Note: FTL forked to handle TCP requests");

	// Memorize PID of this thread to avoid re-opening the gravity database
	// connection multiple times for the same fork
	this_process = getpid();

	// Pretend that we did not open the database so far so it needs to be
	// re-opened, also pretend we have not yet prepared the list statements
	gravityDB_opened = false;
	gravity_db = NULL;
	whitelist_stmt = NULL;
	blacklist_stmt = NULL;
	gravity_stmt = NULL;
	gravityDB_open();
}

// Open gravity database
bool gravityDB_open(void)
{
	struct stat st;
	if(stat(FTLfiles.gravity_db, &st) != 0)
	{
		// File does not exist
		logg("gravityDB_open(): %s does not exist", FTLfiles.gravity_db);
		return false;
	}

	if(gravityDB_opened && gravity_db != NULL)
	{
		if(config.debug & DEBUG_DATABASE)
			logg("gravityDB_open(): Database already connected");
		return true;
	}

	if(config.debug & DEBUG_DATABASE)
		logg("gravityDB_open(): Trying to open %s in read-only mode", FTLfiles.gravity_db);
	int rc = sqlite3_open_v2(FTLfiles.gravity_db, &gravity_db, SQLITE_OPEN_READONLY, NULL);
	if( rc != SQLITE_OK )
	{
		logg("gravityDB_open() - SQL error: %s", sqlite3_errstr(rc));
		gravityDB_close();
		return false;
	}

	// Database connection is now open
	gravityDB_opened = true;

	// Tell SQLite3 to store temporary tables in memory. This speeds up read operations on
	// temporary tables, indices, and views.
	if(config.debug & DEBUG_DATABASE)
		logg("gravityDB_open(): Setting location for temporary object to MEMORY");
	char *zErrMsg = NULL;
	rc = sqlite3_exec(gravity_db, "PRAGMA temp_store = MEMORY", NULL, NULL, &zErrMsg);
	if( rc != SQLITE_OK )
	{
		logg("gravityDB_open(PRAGMA temp_store) - SQL error (%i): %s", rc, zErrMsg);
		sqlite3_free(zErrMsg);
		gravityDB_close();
		return false;
	}

	// Prepare audit statement
	if(config.debug & DEBUG_DATABASE)
		logg("gravityDB_open(): Preparing audit query");

	// We support adding audit domains with a wildcard character (*)
	// Example 1: google.de
	//            matches only google.de
	// Example 2: *.google.de
	//            matches all subdomains of google.de
	//            BUT NOT google.de itself
	// Example 3: *google.de
	//            matches 'google.de' and all of its subdomains but
	//            also other domains starting in google.de, like
	//            abcgoogle.de
	rc = sqlite3_prepare_v2(gravity_db,
	        "SELECT EXISTS("
	          "SELECT domain, "
	            "CASE WHEN substr(domain, 1, 1) = '*' " // Does the database string start in '*' ?
	              "THEN '*' || substr(:input, - length(domain) + 1) " // If so: Crop the input domain and prepend '*'
	              "ELSE :input " // If not: Use input domain directly for comparison
	            "END matcher "
	          "FROM domain_audit WHERE matcher = domain" // Match where (modified) domain equals the database domain
	        ");", -1, &auditlist_stmt, NULL);

	if( rc != SQLITE_OK )
	{
		logg("gravityDB_open(\"SELECT EXISTS(... domain_audit ...)\") - SQL error prepare: %s", sqlite3_errstr(rc));
		gravityDB_close();
		return false;
	}

	// Set SQLite3 busy timeout to a user-defined value (defaults to 1 second)
	// to avoid immediate failures when the gravity database is still busy
	// writing the changes to disk
	if(config.debug & DEBUG_DATABASE)
		logg("gravityDB_open(): Setting busy timeout to %d", DATABASE_BUSY_TIMEOUT);
	sqlite3_busy_timeout(gravity_db, DATABASE_BUSY_TIMEOUT);

	// Prepare private vector of statements for this process (might be a TCP fork!)
	if(whitelist_stmt == NULL)
		whitelist_stmt = new_sqlite3_stmt_vec(counters->clients);
	if(blacklist_stmt == NULL)
		blacklist_stmt = new_sqlite3_stmt_vec(counters->clients);
	if(gravity_stmt == NULL)
		gravity_stmt = new_sqlite3_stmt_vec(counters->clients);

	// Explicitly set busy handler to zero milliseconds
	if(config.debug & DEBUG_DATABASE)
		logg("gravityDB_open(): Setting busy timeout to zero");
	rc = sqlite3_busy_timeout(gravity_db, 0);
	if(rc != SQLITE_OK)
	{
		logg("gravityDB_open() - Cannot set busy handler: %s", sqlite3_errstr(rc));
	}

	if(config.debug & DEBUG_DATABASE)
		logg("gravityDB_open(): Successfully opened gravity.db");
	return true;
}

static char* get_client_querystr(const char* table, const char* groups)
{
	// Build query string with group filtering
	char *querystr = NULL;
	if(asprintf(&querystr, "SELECT EXISTS(SELECT domain from %s WHERE domain = ? AND group_id IN (%s));", table, groups) < 1)
	{
		logg("get_client_querystr(%s, %s) - asprintf() error", table, groups);
		return NULL;
	}

	if(config.debug & DEBUG_DATABASE)
		logg("get_client_querystr: %s", querystr);

	return querystr;
}

// Determine whether to show IP or hardware address
static inline const char *show_client_string(const char *hwaddr, const char *hostname, const char *ip)
{
	if(hostname != NULL && strlen(hostname) > 0)
	{
		// Valid hostname address, display it
		return hostname;
	}
	else if(hwaddr != NULL && strncasecmp(hwaddr, "ip-", 3) != 0)
	{
		// Valid hardware address and not a mock-device
		return hwaddr;
	}

	// Fallback: display IP address
	return ip;
}


// Get associated groups for this client (if defined)
static bool get_client_groupids(clientsData* client)
{
	const char *ip = getstr(client->ippos);
	client->groups = NULL;

	// Do not proceed when database is not available
	if(!gravityDB_opened && !gravityDB_open())
	{
		logg("get_client_groupids(): Gravity database not available");
		return false;
	}

	if(config.debug & DEBUG_DATABASE)
		logg("Querying gravity database for client with IP %s...", ip);

	// Check if client is configured through the client table
	// This will return nothing if the client is unknown/unconfigured
	const char *querystr = "SELECT count(id) matching_count, "
	                       "max(id) chosen_match_id, "
	                       "ip chosen_match_text, "
	                       "group_concat(id) matching_ids, "
	                       "subnet_match(ip,?) matching_bits FROM client "
	                       "WHERE matching_bits > 0 "
	                       "GROUP BY matching_bits "
	                       "ORDER BY matching_bits DESC LIMIT 1;";

	// Prepare query
	int rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &table_stmt, NULL);
	if(rc != SQLITE_OK)
	{
		logg("get_client_groupids(\"%s\") - SQL error prepare: %s",
		     ip, sqlite3_errstr(rc));
		return false;
	}

	// Bind ipaddr to prepared statement
	if((rc = sqlite3_bind_text(table_stmt, 1, ip, -1, SQLITE_STATIC)) != SQLITE_OK)
	{
		logg("get_client_groupids(\"%s\"): Failed to bind ip: %s",
		     ip, sqlite3_errstr(rc));
		sqlite3_reset(table_stmt);
		sqlite3_finalize(table_stmt);
		return NULL;
	}

	// Perform query
	rc = sqlite3_step(table_stmt);
	int matching_count = 0, chosen_match_id = -1, matching_bits = 0;
	char *matching_ids = NULL, *chosen_match_text = NULL;
	if(rc == SQLITE_ROW)
	{
		// There is a record for this client in the database,
		// extract the result (there can be at most one line)
		matching_count = sqlite3_column_int(table_stmt, 0);
		chosen_match_id = sqlite3_column_int(table_stmt, 1);
		chosen_match_text = strdup((const char*)sqlite3_column_text(table_stmt, 2));
		matching_ids = strdup((const char*)sqlite3_column_text(table_stmt, 3));
		matching_bits = sqlite3_column_int(table_stmt, 4);

		if(config.debug & DEBUG_DATABASE && matching_count == 1)
			// Case matching_count > 1 handled below using logg_subnet_warning()
			logg("--> Found record for %s in the client (ID %d)", ip, chosen_match_id);
	}
	else if(rc == SQLITE_DONE)
	{
		if(config.debug & DEBUG_DATABASE)
			logg("--> No record for %s in the client table", ip);
	}
	else
	{
		// Error
		logg("get_client_groupids(\"%s\") - SQL error step: %s",
		     ip, sqlite3_errstr(rc));
		gravityDB_finalizeTable();
		return false;
	}

	// Finalize statement nad free allocated memory
	gravityDB_finalizeTable();

	if(matching_count > 1)
	{
		// There is more than one configured subnet that matches to current device
		// with the same number of subnet mask bits. This is likely unintended by
		// the user so we issue a warning so they can address it.
		// Example:
		//   Device 10.8.0.22
		//   Client 1: 10.8.0.0/24
		//   Client 2: 10.8.1.0/24
		logg_subnet_warning(ip, matching_count, matching_ids, matching_bits, chosen_match_text, chosen_match_id);
	}

	// Free memory if applicable
	if(matching_ids != NULL)
	{
		free(matching_ids);
		matching_ids = NULL;
	}
	if(chosen_match_text != NULL)
	{
		free(chosen_match_text);
		chosen_match_text = NULL;
	}

	// If we didn't find an IP address match above, try with MAC address matches
	// 1. Look up MAC address of this client
	//   1.1. Look up IP address in network_addresses table
	//   1.2. Get MAC address from this network_id
	// 2. If found -> Get groups by looking up MAC address in client table
	char *hwaddr = NULL;
	if(chosen_match_id < 0)
	{
		if(config.debug & DEBUG_DATABASE)
			logg("Querying gravity database for MAC address of %s...", ip);

		// Do the lookup
		hwaddr = getMACfromIP(ip);

		if(hwaddr == NULL && config.debug & DEBUG_DATABASE)
			logg("--> No result.");

		if(hwaddr != NULL && strlen(hwaddr) > 3 && strncasecmp(hwaddr, "ip-", 3) == 0)
		{
			free(hwaddr);
			hwaddr = 0;
			logg("Skipping mock-device hardware address lookup");
		}
	}

	// Check if we received a valid MAC address
	// This ensures we skip mock hardware addresses such as "ip-127.0.0.1"
	if(hwaddr != NULL)
	{
		if(config.debug & DEBUG_DATABASE)
			logg("--> Querying client table for \"%s\"", hwaddr);

		// Check if client is configured through the client table
		// This will return nothing if the client is unknown/unconfigured
		// We use COLLATE NOCASE to ensure the comparison is done case-insensitive
		querystr = "SELECT id FROM client WHERE ip = ? COLLATE NOCASE;";

		// Prepare query
		rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &table_stmt, NULL);
		if(rc != SQLITE_OK)
		{
			logg("get_client_groupids(%s) - SQL error prepare: %s",
				querystr, sqlite3_errstr(rc));
			return false;
		}

		// Bind hwaddr to prepared statement
		if((rc = sqlite3_bind_text(table_stmt, 1, hwaddr, -1, SQLITE_STATIC)) != SQLITE_OK)
		{
			logg("get_client_groupids(\"%s\", \"%s\"): Failed to bind hwaddr: %s",
				ip, hwaddr, sqlite3_errstr(rc));
			sqlite3_reset(table_stmt);
			sqlite3_finalize(table_stmt);
			return false;
		}

		// Perform query
		rc = sqlite3_step(table_stmt);
		if(rc == SQLITE_ROW)
		{
			// There is a record for this client in the database,
			// extract the result (there can be at most one line)
			chosen_match_id = sqlite3_column_int(table_stmt, 0);

			if(config.debug & DEBUG_DATABASE)
				logg("--> Found record for \"%s\" in the client table (ID %d)", hwaddr, chosen_match_id);
		}
		else if(rc == SQLITE_DONE)
		{
			if(config.debug & DEBUG_DATABASE)
				logg("--> There is no record for \"%s\" in the client table", hwaddr);
		}
		else
		{
			// Error
			logg("get_client_groupids(\"%s\", \"%s\") - SQL error step: %s",
				ip, hwaddr, sqlite3_errstr(rc));
			gravityDB_finalizeTable();
			return false;
		}

		// Finalize statement and free allocated memory
		gravityDB_finalizeTable();
	}

	// If we did neither find an IP nor a MAC address match above, we try to look
	// up the client using its host name
	// 1. Look up host name address of this client
	// 2. If found -> Get groups by looking up host name in client table
	char *hostname = NULL;
	if(chosen_match_id < 0)
	{
		if(config.debug & DEBUG_DATABASE)
			logg("Querying gravity database for host name %s...", ip);

		// Do the lookup
		hostname = getNameFromIP(ip);

		if(hostname == NULL && config.debug & DEBUG_DATABASE)
			logg("--> No result.");

		if(hostname != NULL && strlen(hostname) == 0)
		{
			free(hostname);
			hostname = 0;
			logg("Skipping empty host name lookup");
		}
	}

	// Check if we received a valid MAC address
	// This ensures we skip mock hardware addresses such as "ip-127.0.0.1"
	if(hostname != NULL)
	{
		if(config.debug & DEBUG_DATABASE)
			logg("--> Querying client table for \"%s\"", hostname);

		// Check if client is configured through the client table
		// This will return nothing if the client is unknown/unconfigured
		// We use COLLATE NOCASE to ensure the comparison is done case-insensitive
		querystr = "SELECT id FROM client WHERE ip = ? COLLATE NOCASE;";

		// Prepare query
		rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &table_stmt, NULL);
		if(rc != SQLITE_OK)
		{
			logg("get_client_groupids(%s) - SQL error prepare: %s",
				querystr, sqlite3_errstr(rc));
			return false;
		}

		// Bind hostname to prepared statement
		if((rc = sqlite3_bind_text(table_stmt, 1, hostname, -1, SQLITE_STATIC)) != SQLITE_OK)
		{
			logg("get_client_groupids(\"%s\", \"%s\"): Failed to bind hostname: %s",
				ip, hostname, sqlite3_errstr(rc));
			sqlite3_reset(table_stmt);
			sqlite3_finalize(table_stmt);
			return false;
		}

		// Perform query
		rc = sqlite3_step(table_stmt);
		if(rc == SQLITE_ROW)
		{
			// There is a record for this client in the database,
			// extract the result (there can be at most one line)
			chosen_match_id = sqlite3_column_int(table_stmt, 0);

			if(config.debug & DEBUG_DATABASE)
				logg("--> Found record for \"%s\" in the client table (ID %d)", hostname, chosen_match_id);
		}
		else if(rc == SQLITE_DONE)
		{
			if(config.debug & DEBUG_DATABASE)
				logg("--> There is no record for \"%s\" in the client table", hostname);
		}
		else
		{
			// Error
			logg("get_client_groupids(\"%s\", \"%s\") - SQL error step: %s",
				ip, hostname, sqlite3_errstr(rc));
			gravityDB_finalizeTable();
			return false;
		}

		// Finalize statement and free allocated memory
		gravityDB_finalizeTable();
	}

	// We use the default group and return early here
	// if aboves lookups didn't return any results
	// (the client is not configured through the client table)
	if(chosen_match_id < 0)
	{
		if(config.debug & DEBUG_DATABASE)
			logg("Gravity database: Client %s not found. Using default group.\n",
			     show_client_string(hwaddr, hostname, ip));

		client->groups = strdup("0");

		if(hwaddr != NULL)
		{
			free(hwaddr);
			hwaddr = NULL;
		}

		return true;
	}

	// Build query string to get possible group associations for this particular client
	// The SQL GROUP_CONCAT() function returns a string which is the concatenation of all
	// non-NULL values of group_id separated by ','. The order of the concatenated elements
	// is arbitrary, however, is of no relevance for your use case.
	// We check using a possibly defined subnet and use the first result
	querystr = "SELECT GROUP_CONCAT(group_id) FROM client_by_group "
	           "WHERE client_id = ?;";

	if(config.debug & DEBUG_DATABASE)
		logg("Querying gravity database for client %s (getting groups)", ip);

	// Prepare query
	rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &table_stmt, NULL);
	if(rc != SQLITE_OK)
	{
		logg("get_client_groupids(\"%s\", \"%s\", %d) - SQL error prepare: %s",
		     ip, hwaddr, chosen_match_id, sqlite3_errstr(rc));
		sqlite3_finalize(table_stmt);
		return false;
	}

	// Bind hwaddr to prepared statement
	if((rc = sqlite3_bind_int(table_stmt, 1, chosen_match_id)) != SQLITE_OK)
	{
		logg("get_client_groupids(\"%s\", \"%s\", %d): Failed to bind chosen_match_id: %s",
			ip, hwaddr, chosen_match_id, sqlite3_errstr(rc));
		sqlite3_reset(table_stmt);
		sqlite3_finalize(table_stmt);
		return false;
	}

	// Perform query
	rc = sqlite3_step(table_stmt);
	if(rc == SQLITE_ROW)
	{
		// There is a record for this client in the database
		const char* result = (const char*)sqlite3_column_text(table_stmt, 0);
		if(result != NULL)
			client->groups = strdup(result);
		else
			client->groups = strdup("");
	}
	else if(rc == SQLITE_DONE)
	{
		// Found no record for this client in the database
		// -> No associated groups
		client->groups = strdup("");
	}
	else
	{
		logg("get_client_groupids(\"%s\", \"%s\", %d) - SQL error step: %s",
		     ip, hwaddr, chosen_match_id, sqlite3_errstr(rc));
		gravityDB_finalizeTable();
		return false;
	}
	// Finalize statement
	gravityDB_finalizeTable();

	if(config.debug & DEBUG_DATABASE)
		logg("Gravity database: Client %s found. Using groups [%s].\n",
		     show_client_string(hwaddr, hostname, ip), client->groups);

	// Free possibly allocated memory
	if(hwaddr != NULL)
	{
		free(hwaddr);
		hwaddr = NULL;
	}
	if(hostname != NULL)
	{
		free(hostname);
		hostname = NULL;
	}

	// Return success
	return true;
}

char* __attribute__ ((malloc)) get_client_names_from_ids(const char *group_ids)
{
	// Build query string to get concatenated groups
	char *querystr = NULL;
	if(asprintf(&querystr, "SELECT GROUP_CONCAT(ip) FROM client "
	                       "WHERE id IN (%s);", group_ids) < 1)
	{
		logg("group_names(%s) - asprintf() error", group_ids);
		return false;
	}

	if(config.debug & DEBUG_DATABASE)
		logg("Querying group names for IDs (%s)", group_ids);

	// Prepare query
	int rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &table_stmt, NULL);
	if(rc != SQLITE_OK){
		logg("get_client_groupids(%s) - SQL error prepare: %s",
		     querystr, sqlite3_errstr(rc));
		sqlite3_finalize(table_stmt);
		free(querystr);
		return strdup("N/A");
	}

	// Perform query
	char *result = NULL;
	rc = sqlite3_step(table_stmt);
	if(rc == SQLITE_ROW)
	{
		// There is a record for this client in the database
		result = strdup((const char*)sqlite3_column_text(table_stmt, 0));
		if(result == NULL)
			result = strdup("N/A");
	}
	else if(rc == SQLITE_DONE)
	{
		// Found no record for this client in the database
		// -> No associated groups
		result = strdup("N/A");
	}
	else
	{
		logg("group_names(%s) - SQL error step: %s",
		     querystr, sqlite3_errstr(rc));
		gravityDB_finalizeTable();
		free(querystr);
		return strdup("N/A");
	}
	// Finalize statement
	gravityDB_finalizeTable();
	free(querystr);
	return result;
}

// Prepare statements for scanning white- and blacklist as well as gravit for one client
bool gravityDB_prepare_client_statements(const int clientID, clientsData *client)
{
	// Return early if gravity database is not available
	if(!gravityDB_opened && !gravityDB_open())
		return false;

	const char *clientip = getstr(client->ippos);

	if(config.debug & DEBUG_DATABASE)
		logg("Initializing gravity statements for %s", clientip);

	// Get associated groups for this client (if defined)
	char *querystr = NULL;
	if(client->groups == NULL && !get_client_groupids(client))
		return false;

	// Prepare whitelist statement
	// We use SELECT EXISTS() as this is known to efficiently use the index
	// We are only interested in whether the domain exists or not in the
	// list but don't case about duplicates or similar. SELECT EXISTS(...)
	// returns true as soon as it sees the first row from the query inside
	// of EXISTS().
	if(config.debug & DEBUG_DATABASE)
		logg("gravityDB_open(): Preparing vw_whitelist statement for client %s", clientip);
	querystr = get_client_querystr("vw_whitelist", client->groups);
	sqlite3_stmt* stmt = NULL;
	int rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &stmt, NULL);
	if( rc != SQLITE_OK )
	{
		logg("gravityDB_open(\"SELECT EXISTS(... vw_whitelist ...)\") - SQL error prepare: %s", sqlite3_errstr(rc));
		gravityDB_close();
		return false;
	}
	whitelist_stmt->set(whitelist_stmt, clientID, stmt);
	free(querystr);

	// Prepare gravity statement
	if(config.debug & DEBUG_DATABASE)
		logg("gravityDB_open(): Preparing vw_gravity statement for client %s", clientip);
	querystr = get_client_querystr("vw_gravity", client->groups);
	rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &stmt, NULL);
	if( rc != SQLITE_OK )
	{
		logg("gravityDB_open(\"SELECT EXISTS(... vw_gravity ...)\") - SQL error prepare: %s", sqlite3_errstr(rc));
		gravityDB_close();
		return false;
	}
	gravity_stmt->set(gravity_stmt, clientID, stmt);
	free(querystr);

	// Prepare blacklist statement
	if(config.debug & DEBUG_DATABASE)
		logg("gravityDB_open(): Preparing vw_blacklist statement for client %s", clientip);
	querystr = get_client_querystr("vw_blacklist", client->groups);
	rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &stmt, NULL);
	if( rc != SQLITE_OK )
	{
		logg("gravityDB_open(\"SELECT EXISTS(... vw_blacklist ...)\") - SQL error prepare: %s", sqlite3_errstr(rc));
		gravityDB_close();
		return false;
	}
	blacklist_stmt->set(blacklist_stmt, clientID, stmt);
	free(querystr);

	return true;
}

// Finalize non-NULL prepared statements and set them to NULL for a given client
static inline void gravityDB_finalize_client_statements(const int clientID)
{
	if(whitelist_stmt != NULL &&
	   whitelist_stmt->get(whitelist_stmt, clientID) != NULL)
	{
		sqlite3_finalize(whitelist_stmt->get(whitelist_stmt, clientID));
		whitelist_stmt->set(whitelist_stmt, clientID, NULL);
	}
	if(blacklist_stmt != NULL &&
	   blacklist_stmt->get(blacklist_stmt, clientID) != NULL)
	{
		sqlite3_finalize(blacklist_stmt->get(blacklist_stmt, clientID));
		blacklist_stmt->set(blacklist_stmt, clientID, NULL);
	}
	if(gravity_stmt != NULL &&
	   gravity_stmt->get(gravity_stmt, clientID) != NULL)
	{
		sqlite3_finalize(gravity_stmt->get(gravity_stmt, clientID));
		gravity_stmt->set(gravity_stmt, clientID, NULL);
	}

	// Free group memory
	clientsData* client = getClient(clientID, true);
	if(client != NULL && client->groups != NULL)
	{
		free(client->groups);
		client->groups = NULL;
	}
}

// Close gravity database connection
void gravityDB_close(void)
{
	// Return early if gravity database is not available
	if(!gravityDB_opened)
		return;

	// Finalize prepared list statements for all clients
	for(int clientID = 0; clientID < counters->clients; clientID++)
	{
		gravityDB_finalize_client_statements(clientID);
	}

	// Free allocated memory for vectors of prepared client statements
	free_sqlite3_stmt_vec(whitelist_stmt);
	whitelist_stmt = NULL;
	free_sqlite3_stmt_vec(blacklist_stmt);
	blacklist_stmt = NULL;
	free_sqlite3_stmt_vec(gravity_stmt);
	gravity_stmt = NULL;

	// Finalize audit list statement
	sqlite3_finalize(auditlist_stmt);
	auditlist_stmt = NULL;

	// Close table
	sqlite3_close(gravity_db);
	gravity_db = NULL;
	gravityDB_opened = false;
}

// Prepare a SQLite3 statement which can be used by gravityDB_getDomain() to get
// blocking domains from a table which is specified when calling this function
bool gravityDB_getTable(const unsigned char list)
{
	// First check if FTL forked to handle TCP connections
	gravityDB_check_fork();

	if(!gravityDB_opened && !gravityDB_open())
	{
		logg("gravityDB_getTable(%u): Gravity database not available", list);
		return false;
	}

	// Checking for smaller than GRAVITY_LIST is omitted due to list being unsigned
	if(list >= UNKNOWN_TABLE)
	{
		logg("gravityDB_getTable(%u): Requested list is not known!", list);
		return false;
	}

	char *querystr = NULL;
	// Build correct query string to be used depending on list to be read
	// We GROUP BY id as the view also includes the group_id leading to possible duplicates
	// when domains are included in more than one group
	if(asprintf(&querystr, "SELECT domain, id FROM %s GROUP BY id", tablename[list]) < 18)
	{
		logg("readGravity(%u) - asprintf() error", list);
		return false;
	}

	// Prepare SQLite3 statement
	int rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &table_stmt, NULL);
	if(rc != SQLITE_OK)
	{
		logg("readGravity(%s) - SQL error prepare: %s", querystr, sqlite3_errstr(rc));
		gravityDB_close();
		free(querystr);
		return false;
	}

	// Free allocated memory and return success
	free(querystr);
	return true;
}

// Get a single domain from a running SELECT operation
// This function returns a pointer to a string as long
// as there are domains available. Once we reached the
// end of the table, it returns NULL. It also returns
// NULL when it encounters an error (e.g., on reading
// errors). Errors are logged to pihole-FTL.log
// This function is performance critical as it might
// be called millions of times for large blocking lists
inline const char* gravityDB_getDomain(int *rowid)
{
	// Perform step
	const int rc = sqlite3_step(table_stmt);

	// Valid row
	if(rc == SQLITE_ROW)
	{
		const char* domain = (char*)sqlite3_column_text(table_stmt, 0);
		*rowid = sqlite3_column_int(table_stmt, 1);
		return domain;
	}

	// Check for error. An error happened when the result is neither
	// SQLITE_ROW (we returned earlier in this case), nor
	// SQLITE_DONE (we are finished reading the table)
	if(rc != SQLITE_DONE)
	{
		logg("gravityDB_getDomain() - SQL error step: %s", sqlite3_errstr(rc));
		*rowid = -1;
		return NULL;
	}

	// Finished reading, nothing to get here
	*rowid = -1;
	return NULL;
}

// Finalize statement of a gravity database transaction
void gravityDB_finalizeTable(void)
{
	if(!gravityDB_opened)
		return;

	// Finalize statement
	sqlite3_finalize(table_stmt);
	table_stmt = NULL;
}

// Get number of domains in a specified table of the gravity database
// We return the constant DB_FAILED and log to pihole-FTL.log if we
// encounter any error
int gravityDB_count(const unsigned char list)
{
	if(!gravityDB_opened && !gravityDB_open())
	{
		logg("gravityDB_count(%d): Gravity database not available", list);
		return DB_FAILED;
	}

	// Checking for smaller than GRAVITY_LIST is omitted due to list being unsigned
	if(list >= UNKNOWN_TABLE)
	{
		logg("gravityDB_getTable(%u): Requested list is not known!", list);
		return false;
	}

	char *querystr = NULL;
	// Build correct query string to be used depending on list to be read
	if(list != GRAVITY_TABLE && asprintf(&querystr, "SELECT COUNT(DISTINCT domain) FROM %s", tablename[list]) < 18)
	{
		logg("readGravity(%u) - asprintf() error", list);
		return false;
	}
	// We get the number of unique gravity domains as counted and stored by gravity. Counting the number
	// of distinct domains in vw_gravity may take up to several minutes for very large blocking lists on
	// very low-end devices such as the Raspierry Pi Zero
	else if(list == GRAVITY_TABLE && asprintf(&querystr, "SELECT value FROM info WHERE property = 'gravity_count';") < 18)
	{
		logg("readGravity(%u) - asprintf() error", list);
		return false;
	}

	if(config.debug & DEBUG_DATABASE)
		logg("Querying count of distinct domains in gravity database table %s", tablename[list]);

	// Prepare query
	int rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &table_stmt, NULL);
	if(rc != SQLITE_OK){
		logg("gravityDB_count(%s) - SQL error prepare %s", querystr, sqlite3_errstr(rc));
		gravityDB_finalizeTable();
		gravityDB_close();
		free(querystr);
		return DB_FAILED;
	}

	// Perform query
	rc = sqlite3_step(table_stmt);
	if(rc != SQLITE_ROW){
		logg("gravityDB_count(%s) - SQL error step %s", querystr, sqlite3_errstr(rc));
		if(list == GRAVITY_TABLE)
		{
			logg("Count of gravity domains not available. Please run pihole -g");
		}
		gravityDB_finalizeTable();
		gravityDB_close();
		free(querystr);
		return DB_FAILED;
	}

	// Get result when there was no error
	const int result = sqlite3_column_int(table_stmt, 0);

	// Finalize statement
	gravityDB_finalizeTable();

	// Free allocated memory and return result
	free(querystr);
	return result;
}

static bool domain_in_list(const char *domain, sqlite3_stmt* stmt, const char* listname)
{
	// Do not try to bind text to statement when database is not available
	if(!gravityDB_opened && !gravityDB_open())
	{
		logg("domain_in_list(\"%s\", %p, %s): Gravity database not available",
		     domain, stmt, listname);
		return false;
	}

	int rc;
	// Bind domain to prepared statement
	// SQLITE_STATIC: Use the string without first duplicating it internally.
	// We can do this as domain has dynamic scope that exceeds that of the binding.
	// We need to bind the domain onl once even to the prepared audit statement as:
	//     When the same named SQL parameter is used more than once, second and
	//     subsequent occurrences have the same index as the first occurrence.
	//     (https://www.sqlite.org/c3ref/bind_blob.html)
	if((rc = sqlite3_bind_text(stmt, 1, domain, -1, SQLITE_STATIC)) != SQLITE_OK)
	{
		logg("domain_in_list(\"%s\", %p, %s): Failed to bind domain: %s",
		     domain, stmt, listname, sqlite3_errstr(rc));
		return false;
	}

	// Perform step
	rc = sqlite3_step(stmt);
	if(rc == SQLITE_BUSY)
	{
		// Database is busy
		logg("domain_in_list(\"%s\", %p, %s): Database is busy, assuming domain is NOT on list",
		     domain, stmt, listname);
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		return false;
	}
	else if(rc != SQLITE_ROW)
	{
		// Any return code that is neither SQLITE_BUSY not SQLITE_ROW
		// is a real error we should log
		logg("domain_in_list(\"%s\", %p, %s): Failed to perform step: %s",
		     domain, stmt, listname, sqlite3_errstr(rc));
		sqlite3_reset(stmt);
		sqlite3_clear_bindings(stmt);
		return false;
	}

	// Get result of query "SELECT EXISTS(...)"
	const int result = sqlite3_column_int(stmt, 0);

	if(config.debug & DEBUG_DATABASE)
		logg("domain_in_list(\"%s\", %p, %s): %d", domain, stmt, listname, result);

	// The sqlite3_reset() function is called to reset a prepared statement
	// object back to its initial state, ready to be re-executed. Note: Any SQL
	// statement variables that had values bound to them using the
	// sqlite3_bind_*() API retain their values.
	sqlite3_reset(stmt);

	// Contrary to the intuition of many, sqlite3_reset() does not reset the
	// bindings on a prepared statement. Use this routine to reset all host
	// parameters to NULL.
	sqlite3_clear_bindings(stmt);

	// Return if domain was found in current table
	// SELECT EXISTS(...) either returns 0 (false) or 1 (true).
	return (result == 1);
}

bool in_whitelist(const char *domain, const int clientID, clientsData* client)
{
	// First check if FTL forked to handle TCP connections
	gravityDB_check_fork();

	// If list statement is not ready and cannot be initialized (e.g. no
	// access to the database), we return false to prevent an FTL crash
	if(whitelist_stmt == NULL)
		return false;

	// Get whitelist statement from vector of prepared statements if available
	sqlite3_stmt *stmt = whitelist_stmt->get(whitelist_stmt, clientID);

	// If client statement is not ready and cannot be initialized (e.g. no access to
	// the database), we return false (not in whitelist) to prevent an FTL crash
	if(stmt == NULL && !gravityDB_prepare_client_statements(clientID, client))
	{
		logg("ERROR: Gravity database not available, assuming domain is not whitelisted");
		return false;
	}

	// Update statement if has just been initialized
	if(stmt == NULL)
	{
		stmt = whitelist_stmt->get(whitelist_stmt, clientID);
	}

	// We have to check both the exact whitelist (using a prepared database statement)
	// as well the compiled regex whitelist filters to check if the current domain is
	// whitelisted. Due to short-circuit-evaluation in C, the regex evaluations is executed
	// only if the exact whitelist lookup does not deliver a positive match. This is an
	// optimization as the database lookup will most likely hit (a) more domains and (b)
	// will be faster (given a sufficiently large number of regex whitelisting filters).
	return domain_in_list(domain, stmt, "whitelist") ||
	       match_regex(domain, clientID, REGEX_WHITELIST) != -1;
}

bool in_gravity(const char *domain, const int clientID, clientsData* client)
{
	// First check if FTL forked to handle TCP connections
	gravityDB_check_fork();

	// If list statement is not ready and cannot be initialized (e.g. no
	// access to the database), we return false to prevent an FTL crash
	if(gravity_stmt == NULL)
		return false;

	// Get whitelist statement from vector of prepared statements
	sqlite3_stmt *stmt = gravity_stmt->get(gravity_stmt, clientID);

	// If client statement is not ready and cannot be initialized (e.g. no access to
	// the database), we return false (not in gravity list) to prevent an FTL crash
	if(stmt == NULL && !gravityDB_prepare_client_statements(clientID, client))
	{
		logg("ERROR: Gravity database not available, assuming domain is not gravity blocked");
		return false;
	}

	// Update statement if has just been initialized
	if(stmt == NULL)
	{
		stmt = gravity_stmt->get(gravity_stmt, clientID);
	}

	return domain_in_list(domain, stmt, "gravity");
}

inline bool in_blacklist(const char *domain, const int clientID, clientsData* client)
{
	// First check if FTL forked to handle TCP connections
	gravityDB_check_fork();

	// If list statement is not ready and cannot be initialized (e.g. no
	// access to the database), we return false to prevent an FTL crash
	if(blacklist_stmt == NULL)
		return false;

	// Get whitelist statement from vector of prepared statements
	sqlite3_stmt *stmt = blacklist_stmt->get(blacklist_stmt, clientID);

	// If client statement is not ready and cannot be initialized (e.g. no access to
	// the database), we return false (not in blacklist) to prevent an FTL crash
	if(stmt == NULL && !gravityDB_prepare_client_statements(clientID, client))
	{
		logg("ERROR: Gravity database not available, assuming domain is not blacklisted");
		return false;
	}

	// Update statement if has just been initialized
	if(stmt == NULL)
	{
		stmt = blacklist_stmt->get(blacklist_stmt, clientID);
	}

	return domain_in_list(domain, stmt, "blacklist");
}

bool in_auditlist(const char *domain)
{
	// First check if FTL forked to handle TCP connections
	gravityDB_check_fork();

	// If audit list statement is not ready and cannot be initialized (e.g. no access
	// to the database), we return false (not in audit list) to prevent an FTL crash
	if(auditlist_stmt == NULL)
		return false;

	// We check the domain_audit table for the given domain
	return domain_in_list(domain, auditlist_stmt, "auditlist");
}

bool gravityDB_get_regex_client_groups(clientsData* client, const int numregex, const int *regexid,
                                       const unsigned char type, const char* table, const int clientID)
{
	// First check if FTL forked to handle TCP connections
	gravityDB_check_fork();

	char *querystr = NULL;
	if(client->groups == NULL && !get_client_groupids(client))
		return false;

	// Group filtering
	if(asprintf(&querystr, "SELECT id from %s WHERE group_id IN (%s);", table, client->groups) < 1)
	{
		logg("gravityDB_get_regex_client_groups(%s, %s) - asprintf() error", table, client->groups);
		return false;
	}

	// Prepare query
	sqlite3_stmt *query_stmt;
	int rc = sqlite3_prepare_v2(gravity_db, querystr, -1, &query_stmt, NULL);
	if(rc != SQLITE_OK){
		logg("gravityDB_get_regex_client_groups(): %s - SQL error prepare: %s", querystr, sqlite3_errstr(rc));
		gravityDB_close();
		free(querystr);
		return false;
	}

	// Perform query
	if(config.debug & DEBUG_REGEX)
		logg("Regex %s: Querying groups for client %s: \"%s\"", regextype[type], getstr(client->ippos), querystr);
	while((rc = sqlite3_step(query_stmt)) == SQLITE_ROW)
	{
		const int result = sqlite3_column_int(query_stmt, 0);
		for(int regexID = 0; regexID < numregex; regexID++)
		{
			if(regexid[regexID] == result)
			{
				if(type == REGEX_WHITELIST)
					regexID += counters->num_regex[REGEX_BLACKLIST];

				set_per_client_regex(clientID, regexID, true);

				if(config.debug & DEBUG_REGEX)
					logg("Regex %s: Enabling regex with DB ID %i for client %s", regextype[type], regexid[regexID], getstr(client->ippos));

				break;
			}
		}
	}

	// Finalize statement
	sqlite3_finalize(query_stmt);

	// Free allocated memory and return result
	free(querystr);

	return true;
}
