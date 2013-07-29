/* Extern. */

#include <libfutil/misc.h>
#include <libfutil/db/db.h>

#ifndef DB_PSQL_H
#error "Requires PostgreSQL to be selected in db.h"
#endif

#define ERRCODE_UNIQUE_VIOLATION "23505"

/* Forward. */

static void db_noticeprocessor(void UNUSED *arg, const char *message);
static bool db_tryconnect(dbconn_t *db);
static bool db_connect(dbconn_t *db);
static bool db_setupA(dbconn_t *db, dbres_t *res);
static void *db_result_getval(dbres_t *result,
			      unsigned int row,
			      unsigned int column);

/* Constants. */

#define DB_MAX_PARAMS 16	/* Random number, should be good enough. */

/* Public. */

/* Initialize a db structure. */
bool
db_init(dbconn_t *db, const char *dbname, const char *dbuser) {

	assert(db != NULL);
	memzero(db, sizeof *db);

	db->dbname = dbname;
	db->dbuser = dbuser;
	db->notices = true;
	db->keeptrying = false;

	if (dbname || dbuser) {
		char buf[128];
		memzero(buf, sizeof buf);

		snprintf(buf, sizeof buf,
			"%s%s%s%s%s",
			dbname ? "dbname = " : "", dbname ? dbname : "",
			dbname && dbuser ? " " : "",
			dbuser ? "user = " : "", dbuser ? dbuser : "");

		db->conninfo = mstrdup(buf, "conninfo");
	} else {
		db->conninfo = NULL;
	}

	/* Create our mutex */
	mutex_init(db->mutex);

	return (true);
}

/* Initialize a db result structure. */
void
db_initres(dbres_t *res) {
	assert(res != NULL);
	memzero(res, sizeof *res);
}

/* Clean it up, closing left-over connections, etc. */
void
db_cleanup(dbconn_t *db) {
	assert(db != NULL);

	if (db->conn) {
		PQfinish(db->conn);
		db->conn = NULL;
	}

	if (db->conninfo) {
		free(db->conninfo);
		db->conninfo = NULL;
	}

	/* Destroy our mutex */
	mutex_destroy(db->mutex);
}

/*
 * PostgreSQL OID's are defined in /usr/include/postgresql/catalog/pg_type.h
 * but kept locally in db_psql.h due to them not being for clients
 *
 * Format modifiers
 * Variables:
 *  %u = 32bit int
 *  %U = 64bit int
 *  %S = string
 *  %a = IP Address, either IPv4 or IPv6, prefixlength optional
 *  %A = IP Address, as an ipaddres_t (XXX: not implemented)
 *  %t = Type
 *
 * Direct copy:
 *	%s = string
 *
 *	Note that 16bit ints are promoted to gcc to 32bit ints when passed.
 *
 * Examples:
 * ----------------------------------------------------------
 * const char	sortfield = "netblocks";
 * bool		sort_up = false;
 *
 *  db_query(db, res, caller,
 *		"SELECT gw_id, nb_id, netblock state "
 *		"FROM netblocks "
 *		"ORDER BY %s %s",
 *		sortfield,
 *		sort_up ? "DESC" : "ASC");
 *
 * This results in the SQL Query:
 *
 *  SELECT gw_id, nb_id, netblock, state FROM netblocks ORDER BY netblock DESC
 *
 * ----------------------------------------------------------
 * const char	netblock = "192.0.2.0/24";
 * uint32_t	gw_id = 1;
 *
 *  db_query(db, res, __func__,
 *		"INSERT INTO netblocks "
 *		"(gw_id, netblock, state) "
 *		"VALUES(%u, %a, %S)",
 *              gw_id, netblock, "idle");
 *
 * This results in the SQL Query:
 *
 *   INSERT INTO netblocks (gw_id, netblock, state) VALUES($1, $2, $3)
 *
 * with values:
 *    1: integer, the gw_id
 *    2: IP address (inet), the netblock
 *    3: string, the state
 *
 */
dbreply_t
db_query(dbconn_t *db, dbres_t *result, const char *caller,
	 const char *txt, ...)
{
	va_list		ap;
	unsigned int	i, o, v = 0, t32s = 0, t64s = 0;
	int		n;
	dbreply_t	rep = DB_R_OK;
	Oid		typs[DB_MAX_PARAMS];
	const char	*vals[DB_MAX_PARAMS], *t;
	int		lens[DB_MAX_PARAMS];
	int		fmts[DB_MAX_PARAMS];
	uint32_t	t32[DB_MAX_PARAMS];
	uint64_t	t64[DB_MAX_PARAMS];

	memzero(typs, sizeof typs);
	memzero(vals, sizeof vals);
	memzero(lens, sizeof lens);
	memzero(fmts, sizeof fmts);
	memzero(t32, sizeof t32);
	memzero(t64, sizeof t64);

	/* We require a place to store our result */
	assert(result);

	mutex_lock(db->mutex);

	/*
	 * The caller must always provide a NULL result->res pointer to us.
	 * That makes sure we catch un-finished() results eg in a loop.
	 */
	if (result->res != NULL)
	{
		logline(log_CRIT_, "Query still open: %s\n", db->q);
		logline(log_CRIT_, "New Query: %s\n", txt);
		abort();
		return (DB_R_ERR);
	}

	/* Prepare query. */
	va_start(ap, txt);

	o = 0;
	for (i = 0; rep == DB_R_OK && txt[i] != '\0'; i++) {
		/* Straight copy */
		if (txt[i] != '%') {
			db->q[o++] = txt[i];
			continue;
		}

		/* Skip the '%'. */
		i++;

		/* Just in case our random amount is not good enough */
		if (v >= lengthof(vals)) {
			logline(log_CRIT, caller,
				"Too many variable arguments in SQL query: %s",
				txt);
			break;
		}

		switch (txt[i]) {
		case '\0':
			/* Need to break */
			rep = DB_R_ERR;
			logline(log_CRIT, caller,
				"Variable at the end of the line");
			break;

		case 'a':
			/* IP address
			 * XXX (Ticket #48)- should pass this in binary instead
			 *     of letting pSQL figure it out
			 */
			vals[v] = va_arg(ap, char *);
			typs[v] = INETOID;
			lens[v] = 0;
			fmts[v] = 0;
			v++;
			n = snprintf(&db->q[o], sizeof db->q - o, "$%u", v);
			if (!snprintfok(n, sizeof db->q - o)) {
				rep = DB_R_ERR;
				logline(log_CRIT, caller,
					"IP Address Variable did not "
					"fit anymore");
				break;
			}

			o += n;
			break;

		case 'b':
			/* IP block (aka CIDR block)
			 */
			vals[v] = va_arg(ap, char *);
			typs[v] = CIDROID;
			lens[v] = 0;
			fmts[v] = 0;
			v++;
			n = snprintf(&db->q[o], sizeof db->q - o, "$%u", v);
			if (!snprintfok(n, sizeof db->q - o)) {
				rep = DB_R_ERR;
				logline(log_CRIT, caller,
					"IP Address Variable did not "
					"fit anymore");
				break;
			}

			o += n;
			break;

		case 'u':
			/* 32bit integer. */
			t32[t32s] = va_arg(ap, uint32_t);
			t32[t32s] = htonl(t32[t32s]);
			typs[v] = INT4OID;
			vals[v] = (char *)&t32[t32s];
			lens[v] = sizeof t32[t32s];
			fmts[v] = 1;
			v++;
			t32s++;
			n = snprintf(&db->q[o], sizeof db->q - o, "$%u", v);
			if (!snprintfok(n, sizeof db->q - o)) {
				rep = DB_R_ERR;
				logline(log_CRIT, caller,
					"32bit Variable did not "
					"fit anymore");
				break;
			}

			o += n;
			break;

		case 'U':
			/* 64bit integer. */
			t64[t64s] = va_arg(ap, uint64_t);
			t64[t64s] = htonll(t64[t64s]);
			typs[v] = INT8OID;
			vals[v] = (char *)&t64[t64s];
			lens[v] = sizeof t64[t64s];
			fmts[v] = 1;
			v++;
			t64s++;
			n = snprintf(&db->q[o], sizeof db->q - o, "$%u", v);
			if (!snprintfok(n, sizeof db->q - o)) {
				rep = DB_R_ERR;
				logline(log_CRIT, caller,
					"64bit Variable did not "
					"fit anymore");
				break;
			}

			o += n;
			break;

		case 'S':
			/* String. */
			vals[v] = va_arg(ap, char *);
			typs[v] = TEXTOID;
			lens[v] = 0;
			fmts[v] = 0;
			v++;
			n = snprintf(&db->q[o], sizeof db->q - o, "$%u", v);
			if (!snprintfok(n, sizeof db->q - o)) {
				rep = DB_R_ERR;
				logline(log_CRIT, caller,
					"String Variable did not "
					"fit anymore");
				break;
			}

			o += n;
			break;

		case 's':
			/* String - inline copy. */
			t = va_arg(ap, char *);
			n = snprintf(&db->q[o], sizeof db->q - o, "%s", t);
			if (!snprintfok(n, sizeof db->q - o)) {
				rep = DB_R_ERR;
				logline(log_CRIT, caller,
					"Direct String did not "
					"fit anymore");
				break;
			}

			o += n;
			break;

		case 't':
			/* Type - semi inline copy
			 * XXX: Should dig deeper for this, but this
			 * works for now so that we can proceed
			 */
			t = va_arg(ap, char *);
			n = snprintf(&db->q[o], sizeof db->q - o, "'%s'", t);
			if (!snprintfok(n, sizeof db->q - o)) {
				rep = DB_R_ERR;
				logline(log_CRIT, caller,
					"Type did not fit anymore");
				break;
			}

			o += n;
			break;

		default:
			rep = DB_R_ERR;
			logline(log_CRIT, caller,
				"Unknown Variable Type %%%c",
				txt[i]);
			break;
		}
	}

	va_end(ap);

	/* Terminate the string */
	db->q[o] = '\0';

	/* Failed already? */
	if (rep != DB_R_OK) {
		logline(log_CRIT, caller, "String setup failed");
		mutex_unlock(db->mutex);
		return (DB_R_ERR);
	}

	for (i = 0; i < 2; i++) {
		ExecStatusType est;

		/* Not connected? Set it up */
		if (db->conn == NULL)
			db_connect(db);
		if (db->conn == NULL) {
			logline(log_ALERT, caller, "No connection");
			mutex_unlock(db->mutex);
			return (DB_R_ERR);
		}

		/* We ask for binary results */
		result->res = PQexecParams(db->conn, db->q, v, typs,
				   vals, lens, fmts, 1);

		/* If we got results, all is fine */
		if (result->res == NULL) {
			logline(log_CRIT_, "Query(%s) - no result", db->q);

			/* Something funky, disconnect it */
			PQfinish(db->conn);
			db->conn = NULL;

			/* Signal error */
			rep = DB_R_ERR;

			/* Try again if possible */
			continue;
		}

		/* Check the result status */
		est = PQresultStatus(result->res);
		if (est != PGRES_COMMAND_OK && est != PGRES_TUPLES_OK) {
			char *r = PQresultErrorField(result->res,
						     PG_DIAG_SQLSTATE);
			char dupcol[] = ERRCODE_UNIQUE_VIOLATION;

			/* We got an error, which is it */
			if (r != NULL && strcmp(r, dupcol) == 0) {
				rep = DB_R_DUPLICATE_KEY;
			} else {
				rep = DB_R_ERR;
			}

			/* Can we retry this? */
			if (est == PGRES_FATAL_ERROR && r == NULL) {
				logline(log_WARNING_,
					"Query(%s) failed, disconnecting",
					db->q);

				/* Close this connection */
				PQfinish(db->conn);
				db->conn = NULL;

				/* Try a bit more if possible */
				continue;
			}

			logline(rep == DB_R_ERR ? LOG_ERR : LOG_WARNING,
				__FILE__, __LINE__, caller,
				"Query(%s) failed: %u/%s :: %s",
				db->q,
				est,
				r ? r : "<no SQLState>",
				PQerrorMessage(db->conn));

			/* Give up on it */
			break;
		}

		/* Success */
		rep = DB_R_OK;
		break;
	}

	return (rep);
}

void
db_query_finish(dbconn_t *db, dbres_t *result) {
	assert(db != NULL);

	if (result != NULL && result->res != NULL) {
		PQclear(result->res);
		result->res = NULL;
		memzero(db->q, sizeof db->q);
	}

	mutex_unlock(db->mutex);
}

int
db_result_columnno(dbres_t *result, const char *field) {
	return (PQfnumber(result->res, field));
}

bool
db_result_get_string(dbres_t *result, unsigned int row,
		     unsigned int column, const char **str) {
	void	*d;
	Oid	t;

	/* Get the data */
	d = db_result_getval(result, row, column);
	if (d == NULL) {
		assert(false);
		return (false);
	}

	/* Figure out the type */
	t = PQftype(result->res, column);
	if (t == TEXTOID || t == VARCHAROID) {
		*str = (char *)d;
		return (true);
	}

	assert(false);
	*str = NULL;
	return (false);
}

bool
db_result_get_enum(dbres_t *result, unsigned int row,
		   unsigned int column, const char **str) {
	void	*d;

	/* Get the data */
	d = db_result_getval(result, row, column);
	if (d == NULL) {
		assert(false);
		return (false);
	}

	*str = (char *)d;
	return (true);
}

bool
db_result_get_uint32(dbres_t *result, unsigned int row,
		     unsigned int column, uint32_t *t32)
{
	void	*d;
	Oid	t;

	/* Get the data */
	d = db_result_getval(result, row, column);
	if (d == NULL) {
		assert(false);
		return (false);
	}

	/* Figure out the type */
	t = PQftype(result->res, column);

	switch (t) {
	case INT4OID:
		*t32 = ntohl(*(uint32_t *)d);
		return (true);
	case INT8OID:
		*t32 = ntohll(*(uint64_t *)d);
		return (true);
	default:
		assert(false);
		*t32 = 0;
		break;
	}

	/* Unknown type */
	return (false);
}

bool
db_result_get_uint64(dbres_t *result, unsigned int row,
		     unsigned int column, uint64_t *t64)
{
	void	*d;
	Oid	t;

	/* Get the data */
	d = db_result_getval(result, row, column);
	if (d == NULL) {
		assert(false);
		return (false);
	}

	/* Figure out the type */
	t = PQftype(result->res, column);
	switch (t) {
	case INT4OID:
		*t64 = ntohl(*(uint32_t *)d);
		return (true);
	case INT8OID:
		*t64 = ntohll(*(uint64_t *)d);
		return (true);
	default:
		assert(false);
		*t64 = 0;
		break;
	}

	/* Unknown type */
	return (false);
}

bool
db_result_get_bool(dbres_t *result, unsigned int row,
		   unsigned int column, bool *b)
{
	void	*d;
	Oid	t;

	/* Get the data */
	d = db_result_getval(result, row, column);
	if (d == NULL)
		return (false);

	/* Figure out the type */
	t = PQftype(result->res, column);
	if (t == BOOLOID) {
		*b = *(bool *)d;
		return (true);
	}

	*b = false;
	return (false);
}

unsigned int
db_result_getnumrows(dbres_t *result) {
	return (PQntuples(result->res));
}

bool
db_result_field_bool(dbres_t *result, const char *caller, unsigned int row,
		     const char *field, bool *b) {
	int column = PQfnumber(result->res, field);

	if (column == -1) {
		logline(log_CRIT, caller,
			"%s field missing, check the SQL",
			field);
		return (false);
	}

	if (!db_result_get_bool(result, row, column, b)) {
		logline(log_CRIT, caller,
			"%s field is not boolean",
			field);
		return (false);
	}

	return (true);
}

bool
db_result_field_string(dbres_t *result, const char *caller, unsigned int row,
		       const char *field, const char **string) {
	int column = PQfnumber(result->res, field);

	if (column == -1) {
		logline(log_CRIT, caller,
			"%s field missing, check the SQL",
			field);
		return (false);
	}

	if (!db_result_get_string(result, row, column, string)) {
		logline(log_CRIT, caller,
			"%s field is not string",
			field);
		return (false);
	}

	return (true);
}

bool
db_result_field_uint32(dbres_t *result, const char *caller, unsigned int row,
		       const char *field, uint32_t *t32) {
	int column = PQfnumber(result->res, field);

	if (column == -1) {
		logline(log_CRIT, caller,
			"%s field missing, check the SQL",
			field);
		return (false);
	}

	if (!db_result_get_uint32(result, row, column, t32)) {
		logline(log_CRIT, caller,
			"%s field is not number",
			field);
		return (false);
	}

	return (true);
}

bool
db_result_field_uint64(dbres_t *result, const char *caller, unsigned int row,
		       const char *field, uint64_t *t64) {
	int column = PQfnumber(result->res, field);

	if (column == -1) {
		logline(log_CRIT, caller,
			"%s field missing, check the SQL",
			field);
		return (false);
	}

	if (!db_result_get_uint64(result, row, column, t64)) {
		logline(log_CRIT, caller,
			"%s field is not number",
			field);
		return (false);
	}

	return (true);
}

bool
db_setup(const char *dbname, const char *dbuser) {
	dbconn_t	db;
	dbres_t		res;
	int		ret;

	/* Init */
	db_init(&db, dbname, dbuser);
	db_initres(&res);

	/* Be quiet */
	db_set_notices(&db, false);

	ret = db_setupA(&db, &res);

	db_cleanup(&db);

	return (ret);
}

bool
db_create(dbconn_t *db,
          unsigned int num_types, const char **types, const char **typeQs,
	  unsigned int num_tables, const char **tables, const char **tableQs)
{
	dbres_t		res;
	dbreply_t	rep;
	unsigned int	i;

	/* Init our struct. */
	db_initres(&res);

	/* Drop any existing tables. */
	for (i = 0; i < num_tables; i++) {
		rep = db_query(db, &res, __func__,
			       "DROP TABLE IF EXISTS %s CASCADE",
			       tables[i]);
		db_query_finish(db, &res);
		if (rep != DB_R_OK) {
			logline(log_CRIT_,
				"Failure in dropping table '%s'",
				tables[i]);
			return (false);
		}
	}

	/* Drop any existing types. */
	for (i = 0; i < num_types; i++) {
		rep = db_query(db, &res, __func__,
			       "DROP TYPE IF EXISTS %s CASCADE",
			       types[i]);
		db_query_finish(db, &res);
		if (rep != DB_R_OK) {
			logline(log_CRIT_,
				"Failure in dropping type '%s'",
				types[i]);
			return (false);
		}
	}

	/* Create the database types. */
	for (i = 0; i < num_types; i++) {
		rep = db_query(db, &res, __func__, typeQs[i]);
		db_query_finish(db, &res);
		if (rep != DB_R_OK) {
			logline(log_CRIT_,
				"Could not create type '%s'",
				types[i]);
			return (false);
		}
	}

	/* Create the database tables. */
	for (i = 0; i < num_tables; i++) {
		rep = db_query(db, &res, __func__, tableQs[i]);
		db_query_finish(db, &res);
		if (rep != DB_R_OK) {
			logline(log_CRIT_,
				"Could not create table '%s'",
				tables[i]);
			return (false);
		}
	}

	logline(log_INFO_,
		"Database tables are now ready for use");
	return (true);
}

/* Private. */

static void *
db_result_getval(dbres_t *result, unsigned int row, unsigned int column) {
	return (PQgetvalue(result->res, row, column));
}

/* For sending or hiding notices generated by the PostgreSQL. */
static void
db_noticeprocessor(void *arg, const char *message) {
	dbconn_t	*db = (dbconn_t *)arg;

	/* Ignore NOTICE: lines? */
	if (	!db->notices &&
		strncasecmp("NOTICE:", message, 7) == 0) {
		return;
	}

	logline(log_INFO, "PSQL", "%s", message);
}

/* Connect to the database. */
/* Mutex locked by caller */
static bool
db_tryconnect(dbconn_t *db) {
	/* Already connected? (Should not come here then) */
	assert(!db->conn);

	if (!db->conninfo) {
		logline(log_CRIT_,
			"No connection information available, "
			"thus can't connect");
		db->conn = NULL;
		return (false);
	}

	/* Make a connection to the database */
	logline(log_DEBUG_, "Connecting %p to: %s", (void *)db, db->conninfo);
	db->conn = PQconnectdb(db->conninfo);
	logline(log_DEBUG_, "Connecting %p to: %s - done", (void *)db, db->conninfo);

	/* Check to see that the backend connection was successfully made */
	if (PQstatus(db->conn) != CONNECTION_OK) {
		logline(log_CRIT_,
			"Connection to database (%s) failed: %s",
			db->conninfo,
			PQerrorMessage(db->conn));
		PQfinish(db->conn);
		db->conn = NULL;
		return (false);
	}

	/* Configure the notice processor so it uses ours */
	PQsetNoticeProcessor(db->conn, db_noticeprocessor, db);

	return (true);
}

/* Get a connection to the database. */
/* Mutex locked by caller */
static bool
db_connect(dbconn_t *db) {
	unsigned int	i, max = 3;

	if (db->keeptrying)
		logline(log_DEBUG_, "(keep trying)");
	else
		logline(log_DEBUG_, "(maxtries = %u)", max);

	/* Already connected? (Should not come here then) */
	assert(db->conn == NULL);

	for (i = 0; db->keeptrying || i < max; i++) {
		if (db_tryconnect(db))
			break;

		if (!thread_keep_running()) {
			logline(log_DEBUG_, "Stop running");
			break;
		}

		logline(log_WARNING_,
			"Connection attempt failed, "
			"trying again (attempt %u/%u%s)",
			i+1, max, db->keeptrying ? " [keeptrying]" : "");

		/* Sleep at least 2 seconds, max 30 seconds before retrying */
		thread_sleep((i % 28) + 2, 0);
	}

	return ((db->conn != NULL) ? true : false);
}

/* Do the actual work of db_setup(). */
static bool
db_setupA(dbconn_t *db, dbres_t *res) {
	dbreply_t	rep;
	bool		already = false;
	int		libver;
	char		fold[128], fnew[128], fstr[128], buf[1024];
	FILE		*fo, *fn;

	mutex_lock(db->mutex);

	/* Overrule the connection info */
	if (db->conninfo) free(db->conninfo);
	db->conninfo = strdup("dbname = postgres");

	/* Connect, but as postgresql user, thus just db name. */
	/* This should be run as a user with perms to do so */
	if (!db_tryconnect(db)) {
		logline(log_ALERT_,
			"Could not connect to database with postgres "
			"user rights");
		logline(log_ALERT_,
			"Database setup needs to be run as the 'postgres' "
			"user, as such: sudo postgres -c ddb setup_psql");
		mutex_unlock(db->mutex);
		return (false);
	}

	/* Grab the PostgreSQL version. */
	libver = PQserverVersion(db->conn);

	mutex_unlock(db->mutex);

	/* Destroy the database when it was already there. */
	rep = db_query(db, res, __func__,
		       "DROP DATABASE IF EXISTS %s",
		       db->dbname);
	db_query_finish(db, res);
	if (rep != DB_R_OK) {
		logline(log_ALERT_,
			"CREATE DATABASE failed: %s",
			PQerrorMessage(db->conn));
		return (false);
	}

	/* Remove the user when it was already there. */
	rep = db_query(db, res, __func__,
		       "DROP USER IF EXISTS %s",
		       db->dbuser);
	db_query_finish(db, res);
	if (rep != DB_R_OK) {
		logline(log_ALERT_, "DROP USER failed: %s",
			PQerrorMessage(db->conn));
		return (false);
	}

	/* Create a safdef user. */
	rep = db_query(db, res, __func__,
		       "CREATE USER %s NOCREATEDB NOCREATEROLE",
		       db->dbuser);
	db_query_finish(db, res);
	if (rep != DB_R_OK) {
		logline(log_ALERT_, "CREATE USER failed: %s",
			PQerrorMessage(db->conn));
		return (false);
	}

	/* Create the database. */
	rep = db_query(db, res, __func__,
		       "CREATE DATABASE %s OWNER = %s ENCODING = 'UTF-8' "
				"TEMPLATE template0",
		       db->dbname, db->dbuser);
	db_query_finish(db, res);
	if (rep != DB_R_OK) {
		logline(log_ALERT_, "CREATE DATABASE failed: %s",
			PQerrorMessage(db->conn));
		return (false);
	}

	/* Disconnect. */
	db_cleanup(db);

	/* Add our user to pg_hba.conf */

	/* Open the old one */
	snprintf(fold, sizeof fold,
		"/etc/postgresql/%u.%u/main/pg_hba.conf",
		libver / 10000, libver % 1000 / 100);
	fo = fopen(fold, "r+");
	if (!fo) {
		logline(log_ALERT_, "Could not open pg_hba (%s): %u",
			fold, errno);
		return (false);
	}

	/* Open a temporary new one */
	snprintf(fnew, sizeof fnew,
		"/etc/postgresql/%u.%u/main/pg_hba.conf.defiance",
		libver / 10000, libver % 1000 / 100);
	fn = fopen(fnew, "w+");
	if (!fn) {
		logline(log_ALERT_, "Could not open pg_hba (%s): %u",
			fnew, errno);
		return (false);
	}

	snprintf(fstr, sizeof fstr,
		"local   %s %s trust\n", db->dbname, db->dbuser);

	/* Prepend our new line */
	fputs(fstr, fn);

	/* Append the old file */
	while (fgets(buf, sizeof buf, fo) != NULL) {
		fputs(buf, fn);

		if (strcasecmp(buf, fstr) != 0) continue;

		already = true;
	}

	fclose(fn);
	fclose(fo);

	if (already) {
		logline(log_INFO_, "%s already ok", fold);

		/* Remove the new one as it has our line already */
		unlink(fnew);
	}
	else {
		logline(log_INFO_, "Updating %s for permissions", fold);

		/* Replace it */
		rename(fnew, fold);

		/* Request a restart of PostgreSQL*/
		system("/etc/init.d/postgresql restart");
	}

	return (rep == DB_R_OK ? true : false);
}

void
db_set_notices(dbconn_t *db, bool notices) {
	db->notices = notices;
}

bool
db_set_keeptrying(dbconn_t *db, bool keeptrying) {
	bool old = db->keeptrying;

	db->keeptrying = keeptrying;

	return (old);
}