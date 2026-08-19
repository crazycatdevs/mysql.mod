/*
 * mysql.c -- part of mysql.mod
 */
/*
 * Copyright (C) 2003 - 2006 BarkerJr <http://barkerjr.net>
 * Copyright (C) 2024 Michael Ortmann
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#define MODULE_NAME "mysql"
#define MAKING_MYSQL

#include "mysql_mod.h"

#undef global

static Function *global = NULL;

/* Connection structure to support multiple simultaneous connections */
typedef struct mysql_conn {
  char *handle;               /* e.g. "mysql0" */
  MYSQL *dbc;
  char *database;
  char *host;
  char *user;
  char *portsock;
  struct mysql_conn *next;
} mysql_conn;

/* Head of linked list of connections and helpers */
static mysql_conn *conn_head = NULL;
static unsigned int conn_counter = 0;
static mysql_conn *default_conn = NULL;

static unsigned short mem = 0;

/* Forward declarations of helper functions */
static mysql_conn *find_conn(const char *handle);
static char *make_handle(void);
static void add_conn(mysql_conn *c);
static void remove_conn_by_handle(const char *handle);
static void close_all_conns(void);
static void closedb(void); /* close default connection only */

static Function mysql_table[] =
{
/* 0 - 5 */
  (Function) mysql_start,
  (Function) mysql_stop,
  (Function) mysql_expmem,
  (Function) mysql_report,
  (Function) closedb
};

static tcl_cmds mysql_cmds[] =
{
  {"mysql_connect",		tcl_mysql_connect},
  {"mysql_close",		tcl_mysql_close},
  {"mysql_query",		tcl_mysql_query},
  {"mysql_escape",		tcl_mysql_escape},
  {"mysql_errno",		tcl_mysql_errno},
  {"mysql_ping", 		tcl_mysql_ping},
  {"mysql_connectioninfo",	tcl_mysql_connectioninfo},
  {"mysql_insert_id", 		tcl_mysql_insert_id},
  {"mysql_connected", 		tcl_mysql_connected},
  {"mysql_affected_rows",	 tcl_mysql_affected_rows},
  {NULL,				NULL}
};

char *mysql_start(Function *global_funcs)
{
  global = global_funcs;
  module_register(MODULE_NAME, mysql_table, 0, 8);
  add_tcl_commands(mysql_cmds);
  return NULL;
}

static char *mysql_stop()
{
  /* Close all connections on module stop */
  close_all_conns();
  rem_tcl_commands(mysql_cmds);
  module_undepend(MODULE_NAME);
  return NULL;
}

static int mysql_expmem() { return mem; }

static void mysql_report(int idx, int *details)
{
  if (details)
  {
    if (default_conn && default_conn->dbc)
      dprintf(idx, "    Connected to %s on %s as %s.\n",
              default_conn->database ? default_conn->database : "(null)",
              default_conn->host ? default_conn->host : "(null)",
              default_conn->user ? default_conn->user : "(null)");
    else dprintf(idx, "    Disconnected from a database.\n");
  }
}

/*
 * Helper: find a connection by handle. If handle is NULL, return default_conn.
 */
static mysql_conn *find_conn(const char *handle)
{
  mysql_conn *p;
  if (!handle) return default_conn;
  for (p = conn_head; p; p = p->next)
    if (p->handle && !strcmp(p->handle, handle)) return p;
  return NULL;
}

/* Helper: produce a unique handle like "mysql0", "mysql1" */
static char *make_handle(void)
{
  char namebuf[32];
  snprintf(namebuf, sizeof(namebuf), "mysql%u", conn_counter++);
  return allocit(namebuf);
}

/* Add connection to head of list and set default if none */
static void add_conn(mysql_conn *c)
{
  c->next = conn_head;
  conn_head = c;
  if (!default_conn) default_conn = c;
}

/* Remove a connection by handle and free its resources */
static void remove_conn_by_handle(const char *handle)
{
  mysql_conn **pp = &conn_head;
  while (*pp)
  {
    if ((*pp)->handle && (!handle || !strcmp((*pp)->handle, handle)))
    {
      mysql_conn *t = *pp;
      *pp = t->next;
      if (default_conn == t) default_conn = conn_head;
      if (t->dbc) mysql_close(t->dbc);
      freeit(&t->handle);
      freeit(&t->database);
      freeit(&t->host);
      freeit(&t->user);
      freeit(&t->portsock);
      nfree(t);
      return;
    }
    pp = &(*pp)->next;
  }
}

/* Close and free all connections */
static void close_all_conns(void)
{
  while (conn_head)
  {
    remove_conn_by_handle(conn_head->handle);
  }
  default_conn = NULL;
  conn_counter = 0;
}

/* Close default connection only (keeps compatibility with old closedb()) */
static void closedb(void)
{
  if (!default_conn) return;
  if (default_conn->handle) remove_conn_by_handle(default_conn->handle);
}

/*
 * Connects to the database.
 * mysql_connect <database> <hostname> [user] [password] [socket|port]
 *
 * Returns: a handle string for the new connection (e.g. "mysql0") on success.
 */
static int tcl_mysql_connect STDVAR
{
  char *sock = NULL, *pass = NULL;
  unsigned short port = 0;
  mysql_conn *c = NULL;

  BADARGS(3, 6, " database hostname ?user? ?password? ?socket|port?");

  /* Create new connection structure */
  c = (mysql_conn *)nmalloc(sizeof(mysql_conn));
  if (!c)
  {
    Tcl_SetObjResult(irp, Tcl_NewStringObj("Unable to allocate memory to connect", -1));
    return TCL_ERROR;
  }
  memset(c, 0, sizeof(*c));

  c->database = allocit(argv[1]);
  c->host     = allocit(argv[2]);
  if (!c->host || !c->database)
  {
    Tcl_SetObjResult(irp,
        Tcl_NewStringObj("Unable to allocate memory to connect", -1));
    freeit(&c->host);
    freeit(&c->database);
    nfree(c);
    return TCL_ERROR;
  }

  if ((argc > 3) && (*argv[3]))
  {
    if (!(c->user = allocit(argv[3])))
    {
      Tcl_SetObjResult(irp, Tcl_NewStringObj("Unable to allocate memory to connect", -1));
      freeit(&c->host);
      freeit(&c->database);
      nfree(c);
      return TCL_ERROR;
    }

    if ((argc > 4) && (*argv[4]))
      pass = argv[4];

    if ((argc > 5) && *argv[5])
    {
      if (!(c->portsock = allocit(argv[5])))
      {
        Tcl_SetObjResult(irp, Tcl_NewStringObj("Unable to allocate memory to connect", -1));
        freeit(&c->host);
        freeit(&c->database);
        freeit(&c->user);
        nfree(c);
        return TCL_ERROR;
      }
      if ((strlen(c->host) == 9) && !strcmp(c->host, "localhost")) sock = c->portsock;
      else port = atoi(c->portsock);
    }
  }

  /* Initialize MySQL handle */
  if (!(c->dbc = mysql_init(NULL)))
  {
    Tcl_SetObjResult(irp, Tcl_NewStringObj("Failed to connect to database: could not allocate memory", -1));
    freeit(&c->host);
    freeit(&c->database);
    freeit(&c->user);
    freeit(&c->portsock);
    nfree(c);
    return TCL_ERROR;
  }

  /* Connect */
  if (mysql_real_connect(c->dbc, c->host, c->user, pass, c->database, port, sock, 0))
  {
    c->handle = make_handle();
    add_conn(c);
    Tcl_SetObjResult(irp, Tcl_NewStringObj(c->handle, -1));
    return TCL_OK;
  }
  else
  {
    Tcl_Obj *obj = Tcl_NewStringObj("Failed to connect to database: ", -1);
    Tcl_AppendToObj(obj, mysql_error(c->dbc), -1);
    mysql_close(c->dbc);
    freeit(&c->host);
    freeit(&c->database);
    freeit(&c->user);
    freeit(&c->portsock);
    nfree(c);
    Tcl_SetObjResult(irp, obj);
    return TCL_ERROR;
  }
}

/*
 * Kills the connection to the database.
 * mysql_close ?handle?
 */
static int tcl_mysql_close STDVAR
{
  /* usage: mysql_close ?handle? */
  if (argc == 1)
  {
    if (!default_conn)
    {
      Tcl_SetObjResult(irp, Tcl_NewStringObj("No connection to close", -1));
      return TCL_ERROR;
    }
    remove_conn_by_handle(default_conn->handle);
    return TCL_OK;
  }
  else if (argc == 2)
  {
    mysql_conn *c = find_conn(argv[1]);
    if (!c)
    {
      Tcl_SetObjResult(irp, Tcl_NewStringObj("Invalid connection handle", -1));
      return TCL_ERROR;
    }
    remove_conn_by_handle(c->handle);
    return TCL_OK;
  }
  else
  {
    BADARGS(1, 2, " ?handle?");
    return TCL_ERROR;
  }
}

/*
 * Sends a query to the server and returns the results, if any, to the script.
 * mysql_query <query>
 * mysql_query <handle> <query>
 */
static int tcl_mysql_query STDVAR
{
  MYSQL_RES *result;
  unsigned short fields, x;
  MYSQL_ROW row;
  mysql_conn *c = NULL;
  const char *query = NULL;

  if (argc == 2)
  {
    query = argv[1];
    c = default_conn;
  }
  else if (argc == 3)
  {
    c = find_conn(argv[1]);
    query = argv[2];
  }
  else
  {
    BADARGS(2, 3, " ?handle? query");
    return TCL_ERROR;
  }

  if (!c || !c->dbc)
  {
    Tcl_SetObjResult(irp, Tcl_NewStringObj("No active connection", -1));
    return TCL_ERROR;
  }

  /* Attempt the query.  If we get an error, return a descriptive error. */
  if (mysql_query(c->dbc, query))
  {
    Tcl_Obj *obj = Tcl_NewStringObj("Query Failed: ", -1);
    Tcl_AppendToObj(obj, mysql_error(c->dbc), -1);
    Tcl_SetObjResult(irp, obj);
    return TCL_ERROR;
  }

  /* If there's no result, there's nothing to return.  We're done. */
  if (!(result = mysql_store_result(c->dbc))) return TCL_OK;

  fields = mysql_num_fields(result);

  /* Loop through each row of the result, appending the data into a series of
     nested lists, which we'll return to the script. */
  while ((row = mysql_fetch_row(result)))
  {
    Tcl_AppendResult(irp, "{", NULL);
    for (x = 0; x < fields; x++)
      Tcl_AppendElement(irp, (row[x]? row[x]: "NULL"));
    Tcl_AppendResult(irp, "} ", NULL);
  }

  /* Don't forget to free the memory used by the result. */
  mysql_free_result(result);

  return TCL_OK;
}

/*
 * Makes a string safe for sending to the MySQL server by escaping special
 * characters, such as backslashes and single quotes.
 * mysql_escape [handle] [byte] <string>
 *
 * Semantics:
 * - mysql_escape string
 * - mysql_escape bytes string
 * - mysql_escape handle string
 * - mysql_escape handle bytes string
 */
static int tcl_mysql_escape STDVAR
{
  unsigned long length;
  char *in = NULL;
  mysql_conn *c = NULL;
  int argi = 1;

  if (argc < 2 || argc > 4)
  {
    BADARGS(2, 4, " ?handle? ?bytes? string");
    return TCL_ERROR;
  }

  /* Determine if first arg is a handle */
  if (argc >= 3)
  {
    mysql_conn *maybe = find_conn(argv[1]);
    if (maybe)
    {
      c = maybe;
      argi = 2;
    }
  }

  if ((argc - argi) == 1)
  {
    /* only string provided */
    in = argv[argi];
    length = strlen(in);
  }
  else if ((argc - argi) == 2)
  {
    length = atoi(argv[argi]);
    in = argv[argi + 1];
  }
  else
  {
    BADARGS(2, 4, " ?handle? ?bytes? string");
    return TCL_ERROR;
  }

  {
    /* Worst case, all characters will need to be escaped, doubling the size */
    char result[length * 2 + 1];

    /* If we're connected, use real escape which accounts for charset. */
    if (c && c->dbc) mysql_real_escape_string(c->dbc, result, in, length);
    else if (default_conn && default_conn->dbc)
      mysql_real_escape_string(default_conn->dbc, result, in, length);
    else
      mysql_escape_string(result, in, length);

    Tcl_SetObjResult(irp, Tcl_NewStringObj(result, -1));
  }

  return TCL_OK;
}

/*
 * Fetches and return the error number.
 * mysql_errno ?handle?
 */
static int tcl_mysql_errno STDVAR
{
  mysql_conn *c = NULL;
  if (argc == 1) c = default_conn;
  else if (argc == 2) c = find_conn(argv[1]);
  else { BADARGS(1, 2, " ?handle?"); return TCL_ERROR; }

  if (!c || !c->dbc)
  {
    Tcl_SetObjResult(irp, Tcl_NewStringObj("No active connection", -1));
    return TCL_ERROR;
  }

  Tcl_SetObjResult(irp, Tcl_NewIntObj(mysql_errno(c->dbc)));
  return TCL_OK;
}

/*
 * Checks whether the connection to the server is working. If it has gone down,
 * an automatic reconnection is attempted.
 * mysql_ping ?handle?
 */
static int tcl_mysql_ping STDVAR
{
  mysql_conn *c = NULL;
  if (argc == 1) c = default_conn;
  else if (argc == 2) c = find_conn(argv[1]);
  else { BADARGS(1, 2, " ?handle?"); return TCL_ERROR; }

  if (!c || !c->dbc)
  {
    Tcl_SetObjResult(irp, Tcl_NewStringObj("No active connection", -1));
    return TCL_ERROR;
  }

  if (mysql_ping(c->dbc)) Tcl_SetObjResult(irp, Tcl_NewIntObj(mysql_errno(c->dbc)));
  return TCL_OK;
}

/* 
 * If connected, returns a list of database name, hostname, user, and port/sock.
 * mysql_connectioninfo ?handle?
 */
static int tcl_mysql_connectioninfo STDVAR
{
  mysql_conn *c = NULL;
  Tcl_Obj *objs[4];
  unsigned char count = 0;

  if (argc == 1) c = default_conn;
  else if (argc == 2) c = find_conn(argv[1]);
  else { BADARGS(1, 2, " ?handle?"); return TCL_ERROR; }

  if (!c || !c->dbc)
  {
    Tcl_SetObjResult(irp, Tcl_NewStringObj("No active connection", -1));
    return TCL_ERROR;
  }

  objs[0] = Tcl_NewStringObj(c->database ? c->database : "", -1);
  objs[1] = Tcl_NewStringObj(c->host ? c->host : "", -1);
  if (c->user)
  {
    objs[2] = Tcl_NewStringObj(c->user, -1);
    if (c->portsock)
    {
      objs[3] = Tcl_NewStringObj(c->portsock, -1);
      count = 4;
    }
    else count = 3;
  }
  else count = 2;
  Tcl_SetObjResult(irp, Tcl_NewListObj(count, objs));
  return TCL_OK;
}

/*
 * Fetches and returns the ID from the last insertion.
 * mysql_insert_id ?handle?
 */
static int tcl_mysql_insert_id STDVAR
{
  mysql_conn *c = NULL;
  if (argc == 1) c = default_conn;
  else if (argc == 2) c = find_conn(argv[1]);
  else { BADARGS(1, 2, " ?handle?"); return TCL_ERROR; }

  if (!c || !c->dbc)
  {
    Tcl_SetObjResult(irp, Tcl_NewStringObj("No active connection", -1));
    return TCL_ERROR;
  }

  Tcl_SetObjResult(irp, Tcl_NewLongObj((unsigned long)mysql_insert_id(c->dbc)));
  return TCL_OK;
}

/*
 * Return whether the database is linked (true) or not (false).
 * mysql_connected ?handle?
 */
static int tcl_mysql_connected STDVAR
{
  mysql_conn *c = NULL;
  if (argc == 1) c = default_conn;
  else if (argc == 2) c = find_conn(argv[1]);
  else { BADARGS(1, 2, " ?handle?"); return TCL_ERROR; }

  if (c && c->dbc) Tcl_SetObjResult(irp, Tcl_NewBooleanObj(1));
  else Tcl_SetObjResult(irp, Tcl_NewBooleanObj(0));
  return TCL_OK;
}

/*
 * Fetches and returns the number of rows that were changed by the last query.
 * mysql_affected_rows ?handle?
 */
static int tcl_mysql_affected_rows STDVAR
{
  mysql_conn *c = NULL;
  if (argc == 1) c = default_conn;
  else if (argc == 2) c = find_conn(argv[1]);
  else { BADARGS(1, 2, " ?handle?"); return TCL_ERROR; }

  if (!c || !c->dbc)
  {
    Tcl_SetObjResult(irp, Tcl_NewStringObj("No active connection", -1));
    return TCL_ERROR;
  }

  Tcl_SetObjResult(irp, Tcl_NewLongObj((unsigned long)mysql_affected_rows(c->dbc)));
  return TCL_OK;
}

/* Frees the memory and accounts for it. */
static void freeit(char **p)
{
  if (!*p) return; /* It's pointing to null */
  mem -= strlen(*p) + 1;
  nfree(*p);
  *p = NULL;
}

/*
 * Allocates memory for the string to be copied into, copies it, and returns a
 * pointer to the new string.
 */
static char *allocit(const char *p)
{
  char *newp;
// If the memory isn't allocated, we have to stop here.
  if (!(newp = (char *)nmalloc(strlen(p) + 1))) return NULL;
  mem += strlen(p) + 1;
  strcpy(newp, p);
  return newp;
}
