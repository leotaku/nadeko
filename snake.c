#include <errno.h>
#include <lines.c>
#include <nadeko.c>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define FLAG_SQLITE_TRACE \
    SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE | SQLITE_TRACE_ROW | SQLITE_TRACE_CLOSE
#define FLAG_SQLITE_OPEN \
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX
#define READ_BUFFER_SIZE 1 << 16

void consumeSingleStatement(char **ppPoint, int *pLinum, int isOutside) {
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
    int rc = SQLITE_OK;
    char *err = sqlite3_malloc(0);

    FILE *fd = fopen(zFilename, "r");
    if (fd == NULL) {
        fprintf(stderr, "error: opening \"%s\": %s\n", zFilename, strerror(errno));
        rc = SQLITE_EMPTY;
        goto abort;
    }

    char buf[READ_BUFFER_SIZE];
    if (fread(buf, sizeof(*buf), READ_BUFFER_SIZE, fd) && ferror(fd)) {
        fprintf(stderr, "error: reading \"%s\": %s\n", zFilename, strerror(errno));
        rc = errno;
        goto abort;
    } else if (!feof(fd)) {
        fprintf(stderr, "error: reading \"%s\": %s\n", zFilename, "buffer too small");
        rc = errno;
        goto abort;
    }

    int endLine = 1;
    char *end = buf;
    for (;;) {
        // Find start of SQL statement
        char *start = end;
        int startLine = endLine;
        consumeSingleStatement(&start, &startLine, 1);
        if (start[0] == '\0') {
            break;
        };

        // Find end of SQL statement
        end = start;
        endLine = startLine;
        consumeSingleStatement(&end, &endLine, 0);
        if (end[0] == '\0') {
            rc = SQLITE_ERROR;
            fprintf(stderr, "error: %s:%i: unterminated SQL\n", zFilename, startLine);
            break;
        };

        // Execute current SQL statement
        end[-1] = '\0';
        if ((rc = sqlite3_exec(db, start, NULL, NULL, &err) != SQLITE_OK)) {
            fprintf(stderr, "error: %s:%i: %s\n", zFilename, startLine, err);
            break;
        }
    }

abort:
    sqlite3_free(err);
    if (fd) fclose(fd);
    return rc;
}

void debugLogCallback(void *, int, const char *zMsg) {
    fprintf(stderr, "debug: %s\n", zMsg);
}

int traceLogCallback(unsigned int uMask, void *, void *pData, void *pCtx) {
    char *zSql = NULL;
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

int commandOptionDebug = 0;
int commandOptionTrace = 0;

int parseCommandArgs(int argc, char *argv[]) {
    int keep = 0;
    for (int n = 1; n < argc; n++) {
        argv[1 + keep] = argv[n];
        if (!strcmp(argv[n], "--debug")) {
            commandOptionDebug = 1;
        } else if (!strcmp(argv[n], "--trace")) {
            commandOptionTrace = 1;
        } else if (!strncmp(argv[n], "--", 2)) {
            fprintf(stderr, "error: unknown switch \"%s\"\n", argv[n]);
            return SQLITE_ERROR;
        } else {
            keep++;
        }
    }

    if (keep < 1) {
        fprintf(stderr, "error: missing positional argument\n");
        return SQLITE_ERROR;
    }

    return SQLITE_OK;
}

int main(int argc, char *argv[]) {
    sqlite3 *db = NULL;
    char *err = NULL;
    int rc = SQLITE_OK;

    if ((rc = parseCommandArgs(argc, argv)) == SQLITE_DONE) {
        return SQLITE_OK;
    } else if (rc != SQLITE_OK) {
        return SQLITE_ERROR;
    }

    if (commandOptionDebug &&
        (rc = sqlite3_config(SQLITE_CONFIG_LOG, debugLogCallback, NULL))) {
        fprintf(stderr, "internal: setting debug: %s", sqlite3_errstr(rc));
    } else if ((rc = sqlite3_initialize())) {
        fprintf(stderr, "internal: initializing sqlite: %s", sqlite3_errstr(rc));
    } else if ((rc = sqlite3_open_v2(":memory:", &db, FLAG_SQLITE_OPEN, NULL))) {
        fprintf(stderr, "internal: opening in-memory database: %s", sqlite3_errstr(rc));
    } else if (commandOptionTrace &&
               (rc = sqlite3_trace_v2(db, FLAG_SQLITE_TRACE, traceLogCallback, NULL))) {
        fprintf(stderr, "internal: setting tracing: %s", sqlite3_errstr(rc));
    } else if ((rc = sqlite3_nadeko_init(db, &err, NULL))) {
        fprintf(stderr, "internal: initializing extension: %s", sqlite3_errmsg(db));
    } else if ((rc = sqlite3_lines_init(db, &err, NULL))) {
        fprintf(stderr, "internal: initializing extension: %s", sqlite3_errmsg(db));
    } else {
        rc = readAndLoadFile(db, argv[1]);
    }

    if (err) sqlite3_free(err);
    if (db) sqlite3_close(db);
    sqlite3_shutdown();

    return rc;
}
