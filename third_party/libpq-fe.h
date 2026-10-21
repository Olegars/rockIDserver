#ifndef ROCKID_LIBPQ_FE_H
#define ROCKID_LIBPQ_FE_H

/* Minimal libpq declarations for linking libpq.so.5 when libpq-dev is absent. */

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int Oid;
typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;

typedef enum {
  CONNECTION_OK,
  CONNECTION_BAD
} ConnStatusType;

typedef enum {
  PGRES_EMPTY_QUERY = 0,
  PGRES_COMMAND_OK,
  PGRES_TUPLES_OK,
  PGRES_COPY_OUT,
  PGRES_COPY_IN,
  PGRES_BAD_RESPONSE,
  PGRES_NONFATAL_ERROR,
  PGRES_FATAL_ERROR,
  PGRES_COPY_BOTH,
  PGRES_SINGLE_TUPLE
} ExecStatusType;

#define PG_DIAG_SQLSTATE 'C'

extern PGconn *PQconnectdb(const char *conninfo);
extern ConnStatusType PQstatus(const PGconn *conn);
extern char *PQerrorMessage(const PGconn *conn);
extern void PQfinish(PGconn *conn);

extern PGresult *PQexec(PGconn *conn, const char *query);
extern PGresult *PQexecParams(PGconn *conn, const char *command, int nParams, const Oid *paramTypes,
                              const char *const *paramValues, const int *paramLengths,
                              const int *paramFormats, int resultFormat);
extern ExecStatusType PQresultStatus(const PGresult *res);
extern char *PQresultErrorMessage(const PGresult *res);
extern char *PQresultErrorField(const PGresult *res, int fieldcode);
extern void PQclear(PGresult *res);
extern int PQntuples(const PGresult *res);
extern char *PQgetvalue(const PGresult *res, int tup_num, int field_num);
extern int PQgetisnull(const PGresult *res, int tup_num, int field_num);
extern char *PQcmdTuples(PGresult *res);

#ifdef __cplusplus
}
#endif

#endif
