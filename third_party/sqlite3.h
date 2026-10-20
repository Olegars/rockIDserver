#ifndef ROCKID_SQLITE3_MIN_H
#define ROCKID_SQLITE3_MIN_H

/* Minimal declarations for the system libsqlite3.so.0.
   Used when the libsqlite3-dev headers are not installed. */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long sqlite3_int64;
typedef void (*sqlite3_destructor_type)(void *);

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_CONSTRAINT 19
#define SQLITE_NULL 5

#define SQLITE_OPEN_READONLY 0x00000001
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE 0x00000004
#define SQLITE_OPEN_FULLMUTEX 0x00010000

#define SQLITE_TRANSIENT ((sqlite3_destructor_type) - 1)

int sqlite3_open_v2(const char *filename, sqlite3 **ppDb, int flags, const char *zVfs);
int sqlite3_close(sqlite3 *);
int sqlite3_exec(sqlite3 *, const char *sql,
                 int (*callback)(void *, int, char **, char **), void *, char **errmsg);
int sqlite3_prepare_v2(sqlite3 *db, const char *zSql, int nByte, sqlite3_stmt **ppStmt,
                       const char **pzTail);
int sqlite3_bind_int64(sqlite3_stmt *, int, sqlite3_int64);
int sqlite3_bind_text(sqlite3_stmt *, int, const char *, int, void (*)(void *));
int sqlite3_bind_blob(sqlite3_stmt *, int, const void *, int n, void (*)(void *));
int sqlite3_step(sqlite3_stmt *);
int sqlite3_reset(sqlite3_stmt *);
int sqlite3_clear_bindings(sqlite3_stmt *);
int sqlite3_finalize(sqlite3_stmt *);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *, int iCol);
const unsigned char *sqlite3_column_text(sqlite3_stmt *, int iCol);
const void *sqlite3_column_blob(sqlite3_stmt *, int iCol);
int sqlite3_column_bytes(sqlite3_stmt *, int iCol);
int sqlite3_column_type(sqlite3_stmt *, int iCol);
sqlite3_int64 sqlite3_last_insert_rowid(sqlite3 *);
int sqlite3_changes(sqlite3 *);
const char *sqlite3_errmsg(sqlite3 *);
int sqlite3_errcode(sqlite3 *);
void sqlite3_free(void *);
int sqlite3_busy_timeout(sqlite3 *, int ms);

#ifdef __cplusplus
}
#endif

#endif
