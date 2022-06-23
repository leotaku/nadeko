#include "lines.c"
#include "nadeko.c"
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define FLAG_SQLITE_TRACE \
    SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE | SQLITE_TRACE_ROW | SQLITE_TRACE_CLOSE
#define FLAG_SQLITE_OPEN \
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX
#define READ_BUFFER_SIZE 1 << 16

void consumeSingleStatement(char **ppPoint, sqlite3_int64 *pLinum, int isOutside) {
    int isInLargeComment = 0;
    for (;;) {
        if (*ppPoint[0] == '\0') {
            break;
        } else if (*ppPoint[0] == '\n') {
            *pLinum += 1;
            *ppPoint += 1;
        } else if (*ppPoint[0] == ' ') {
            *ppPoint += 1;
        } else if (isInLargeComment) {
            if (!strncmp(*ppPoint, "*/", 2)) {
                isInLargeComment = 0;
                *ppPoint += 2;
            } else {
                *ppPoint += 1;
            }
        } else if (!strncmp(*ppPoint, "/*", 2)) {
            isInLargeComment = 1;
            *ppPoint += 2;
        } else if (!strncmp(*ppPoint, "--", 2)) {
            *ppPoint = strchr(*ppPoint, '\n');
            if (!*ppPoint) *ppPoint = strchr(*ppPoint, '\0');
        } else if (*ppPoint[0] == ';') {
            *ppPoint += 1;
            if (!isOutside) break;
        } else {
            if (isOutside) break;
            *ppPoint += 1;
        }
    }
}

int readAndLoadFile(sqlite3 *db, const char *zFilename) {
    FILE *fd = fopen(zFilename, "r");
    if (fd == 0) {
        fprintf(stderr, "error: opening \"%s\": %s\n", zFilename, strerror(errno));
        return errno;
    }

    char buf[READ_BUFFER_SIZE];
    if (fread(buf, sizeof(*buf), READ_BUFFER_SIZE, fd) && ferror(fd)) {
        fprintf(stderr, "error: reading \"%s\": %s\n", zFilename, strerror(errno));
        fclose(fd);
        return errno;
    } else if (!feof(fd)) {
        fprintf(stderr, "error: reading \"%s\": %s\n", zFilename, "buffer too small");
        fclose(fd);
        return errno;
    }

    sqlite3_int64 iEndLinum = 1;
    char *pEnd = buf;
    for (;;) {
        // Find start of SQL statement
        char *pStart = pEnd;
        sqlite3_int64 iStartLinum = iEndLinum;
        consumeSingleStatement(&pStart, &iStartLinum, 1);
        if (pStart[0] == '\0') {
            fclose(fd);
            return SQLITE_OK;
        };

        // Find end of SQL statement
        pEnd = pStart;
        iEndLinum = iStartLinum;
        consumeSingleStatement(&pEnd, &iEndLinum, 0);
        if (pEnd[0] == '\0') {
            fprintf(stderr, "error: %s:%llu: unterminated SQL\n", zFilename, iStartLinum);
            fclose(fd);
            return SQLITE_ERROR;
        };

        // Execute current SQL statement
        char *zErr = 0;
        pEnd[-1] = '\0';
        if (sqlite3_exec(db, pStart, 0, 0, &zErr) != SQLITE_OK) {
            fprintf(stderr, "error: %s:%llu: %s\n", zFilename, iStartLinum, zErr);
            sqlite3_free(zErr);
            fclose(fd);
            return SQLITE_ERROR;
        }
    }
}

void debugLogCallback(void *, int, const char *zMsg) {
    fprintf(stderr, "debug: %s\n", zMsg);
}

int traceLogCallback(unsigned int uMask, void *, void *pData, void *pCtx) {
    char *zSql = 0;
    char *pNewline;
    switch (uMask) {
    case SQLITE_TRACE_STMT:
        zSql = sqlite3_expanded_sql(pData);
        if ((pNewline = strchr(zSql, '\n'))) {
            fprintf(stderr, "trace: prepare: %.*s...\n", (int)(pNewline - zSql), zSql);
        } else {
            fprintf(stderr, "trace: prepare: %s\n", zSql);
        }
        break;
    case SQLITE_TRACE_ROW:
        zSql = sqlite3_expanded_sql(pData);
        if (!zSql) break;
        fprintf(stderr, "trace: row: statement resulted in ");
        for (int column = 0; column < sqlite3_column_count(pData); column++) {
            fprintf(stderr,
                "%s%s",
                column == 0 ? "" : ",",
                sqlite3_column_type(pData, column) == SQLITE_BLOB
                    ? "BLOB"
                    : (char *)(sqlite3_column_text(pData, column)));
        }
        fputc('\n', stderr);
        break;
    case SQLITE_TRACE_PROFILE:
        zSql = sqlite3_expanded_sql(pData);
        fprintf(
            stderr, "trace: profile: statement took %fms\n", *(int *)(pCtx) / 1000000.0);
        break;
    case SQLITE_TRACE_CLOSE:
        fprintf(stderr, "trace: close database connection\n");
        break;
    }
    if (zSql) sqlite3_free(zSql);

    return SQLITE_OK;
}

char *stringOptionDatabase = ":memory:";
int isOptionDebug = 0;
int isOptionTrace = 0;
int isOptionWipe = 0;

int parseCommandArgs(int argc, char *argv[]) {
    int iPositional = 0;
    for (int n = 1; n < argc; n++) {
        argv[1 + iPositional] = argv[n];
        if (!strcmp(argv[n], "--debug")) {
            isOptionDebug = 1;
        } else if (!strcmp(argv[n], "--trace")) {
            isOptionTrace = 1;
        } else if (!strcmp(argv[n], "--wipe")) {
            isOptionWipe = 1;
        } else if (!strcmp(argv[n], "--output")) {
            if (n + 1 < argc && strncmp(argv[n + 1], "--", 2)) {
                stringOptionDatabase = argv[++n];
            } else {
                fprintf(stderr, "error: missing argument to switch \"%s\"\n", argv[n]);
                return SQLITE_ERROR;
            }
        } else if (!strncmp(argv[n], "--", 2)) {
            fprintf(stderr, "error: unknown switch \"%s\"\n", argv[n]);
            return SQLITE_ERROR;
        } else {
            iPositional++;
        }
    }

    if (iPositional < 1) {
        fprintf(stderr, "error: missing positional argument\n");
        return SQLITE_ERROR;
    } else if (iPositional > 1) {
        fprintf(stderr, "error: too many positional arguments\n");
        return SQLITE_ERROR;
    }

    return SQLITE_OK;
}

int main(int argc, char *argv[]) {
    sqlite3 *db = 0;
    char *zErr = 0;
    int rc = SQLITE_OK;

    if ((rc = parseCommandArgs(argc, argv)) == SQLITE_DONE) {
        return SQLITE_OK;
    } else if (rc != SQLITE_OK) {
        return SQLITE_ERROR;
    }

    if (isOptionDebug && (rc = sqlite3_config(SQLITE_CONFIG_LOG, debugLogCallback, 0))) {
        fprintf(stderr, "internal: setting debug: %s", sqlite3_errstr(rc));
    } else if ((rc = sqlite3_initialize())) {
        fprintf(stderr, "internal: initializing sqlite: %s", sqlite3_errstr(rc));
    } else if ((rc = sqlite3_open_v2(stringOptionDatabase, &db, FLAG_SQLITE_OPEN, 0))) {
        fprintf(stderr, "internal: opening in-memory database: %s", sqlite3_errstr(rc));
    } else if (isOptionWipe &&
               (rc = sqlite3_db_config(db, SQLITE_DBCONFIG_RESET_DATABASE, 1, 0) ||
                     sqlite3_exec(db, "VACUUM", 0, 0, 0) ||
                     sqlite3_db_config(db, SQLITE_DBCONFIG_RESET_DATABASE, 0, 0))) {
        fprintf(stderr, "internal: wiping database: %s", sqlite3_errstr(rc));
    } else if (isOptionTrace &&
               (rc = sqlite3_trace_v2(db, FLAG_SQLITE_TRACE, traceLogCallback, 0))) {
        fprintf(stderr, "internal: setting tracing: %s", sqlite3_errstr(rc));
    } else if ((rc = sqlite3_nadeko_init(db, &zErr, 0))) {
        fprintf(stderr, "internal: initializing extension: %s", sqlite3_errmsg(db));
    } else if ((rc = sqlite3_lines_init(db, &zErr, 0))) {
        fprintf(stderr, "internal: initializing extension: %s", sqlite3_errmsg(db));
    } else {
        rc = readAndLoadFile(db, argv[1]);
    }

    if (zErr) sqlite3_free(zErr);
    if (db) sqlite3_close(db);
    sqlite3_shutdown();

    return rc;
}
