#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define BUFFER_SIZE 1 << 16
#define FLAG_SQLITE_TRACE \
    SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE | SQLITE_TRACE_ROW | SQLITE_TRACE_CLOSE
#define FLAG_SQLITE_OPEN \
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX

void consume_whitespace_or_statement(char **point, int *line, int statement) {
    int comment = 0;
    for (;;) {
        if (*point[0] == '\0') {
            break;
        } else if (*point[0] == '\n') {
            *line += 1;
            *point += 1;
        } else if (*point[0] == ' ') {
            *point += 1;
        } else if (comment) {
            if (!strncmp(*point, "*/", 2)) {
                comment = 0;
                *point += 2;
            } else {
                *point += 1;
            }
        } else if (!strncmp(*point, "/*", 2)) {
            comment = 1;
            *point += 2;
        } else if (!strncmp(*point, "--", 2)) {
            *point = strchr(*point, '\n');
            if (!*point) *point = strchr(*point, '\0');
        } else if (*point[0] == ';') {
            *point += 1;
            if (statement) break;
        } else {
            if (!statement) break;
            *point += 1;
        }
    }
}

int read_and_load_file(sqlite3 *db, const char *filename) {
    int rc = SQLITE_OK;
    char *err = sqlite3_malloc(0);

    FILE *fd = fopen(filename, "r");
    if (fd == NULL) {
        fprintf(stderr, "error: opening \"%s\": %s\n", filename, strerror(errno));
        rc = SQLITE_EMPTY;
        goto abort;
    }

    char buf[BUFFER_SIZE];
    if (fread(buf, sizeof(*buf), BUFFER_SIZE, fd) && ferror(fd)) {
        fprintf(stderr, "error: reading \"%s\": %s\n", filename, strerror(errno));
        rc = errno;
        goto abort;
    } else if (!feof(fd)) {
        fprintf(stderr, "error: reading \"%s\": %s\n", filename, "buffer too small");
        rc = errno;
        goto abort;
    }

    int end_line = 1;
    char *end = buf;
    for (;;) {
        // Find start of SQL statement
        char *start = end;
        int start_line = end_line;
        consume_whitespace_or_statement(&start, &start_line, 0);
        if (start[0] == '\0') {
            break;
        };

        // Find end of SQL statement
        end = start;
        end_line = start_line;
        consume_whitespace_or_statement(&end, &end_line, 1);
        if (end[0] == '\0') {
            rc = SQLITE_ERROR;
            fprintf(stderr, "error: %s:%i: unterminated SQL\n", filename, start_line);
            break;
        };

        // Execute current SQL statement
        end[-1] = '\0';
        if ((rc = sqlite3_exec(db, start, NULL, NULL, &err) != SQLITE_OK)) {
            fprintf(stderr, "error: %s:%i: %s\n", filename, start_line, err);
            break;
        }
    }

abort:
    sqlite3_free(err);
    if (fd) fclose(fd);
    return rc;
}

void debug_log_callback(void *, int, const char *zMsg) {
    fprintf(stderr, "debug: %s\n", zMsg);
}

int trace_log_callback(unsigned int type, void *, void *object, void *context) {
    char *string = NULL;
    switch (type) {
    case SQLITE_TRACE_STMT:
        string = sqlite3_expanded_sql(object);
        fprintf(stderr, "trace: prepare: \"%s\"\n", string);
        break;
    case SQLITE_TRACE_ROW:
        string = sqlite3_expanded_sql(object);
        if (!string) break;
        fprintf(stderr, "trace: row: \"%s\" resulted in ", string);
        for (int n = 0; n < sqlite3_column_count(object); n++) {
            fprintf(stderr, "%s%s", n == 0 ? "" : ",", sqlite3_column_text(object, n));
        }
        fputc('\n', stderr);
        break;
    case SQLITE_TRACE_PROFILE:
        string = sqlite3_expanded_sql(object);
        fprintf(stderr, "trace: profile: \"%s\" took %ins\n", string, *(int *)(context));
        break;
    case SQLITE_TRACE_CLOSE:
        fprintf(stderr, "trace: close database connection\n");
        break;
    }
    if (string) sqlite3_free(string);

    return SQLITE_OK;
}

int option_debug = 0;
int option_trace = 0;

int parse_command_args(int argc, char *argv[]) {
    int keep = 0;
    for (int n = 1; n < argc; n++) {
        argv[1 + keep] = argv[n];
        if (!strcmp(argv[n], "--debug")) {
            option_debug = 1;
        } else if (!strcmp(argv[n], "--trace")) {
            option_trace = 1;
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

int sqlite3_nadeko_init(sqlite3 *, char **, const sqlite3_api_routines *);

int main(int argc, char *argv[]) {
    sqlite3 *db = NULL;
    char *err = NULL;
    int rc = SQLITE_OK;

    if ((rc = parse_command_args(argc, argv)) == SQLITE_DONE) {
        return SQLITE_OK;
    } else if (rc != SQLITE_OK) {
        return SQLITE_ERROR;
    }

    if (option_debug &&
        (rc = sqlite3_config(SQLITE_CONFIG_LOG, debug_log_callback, NULL))) {
        fprintf(stderr, "internal: setting debug: %s", sqlite3_errstr(rc));
    } else if ((rc = sqlite3_initialize())) {
        fprintf(stderr, "internal: initializing sqlite: %s", sqlite3_errstr(rc));
    } else if ((rc = sqlite3_open_v2(":memory:", &db, FLAG_SQLITE_OPEN, NULL))) {
        fprintf(stderr, "internal: opening in-memory database: %s", sqlite3_errstr(rc));
    } else if (option_trace &&
               (rc = sqlite3_trace_v2(db, FLAG_SQLITE_TRACE, trace_log_callback, NULL))) {
        fprintf(stderr, "internal: setting tracing: %s", sqlite3_errstr(rc));
    } else if ((rc = sqlite3_nadeko_init(db, &err, NULL))) {
        fprintf(stderr, "internal: initializing extension: %s", sqlite3_errmsg(db));
    } else {
        rc = read_and_load_file(db, argv[1]);
    }

    if (err) sqlite3_free(err);
    if (db) sqlite3_close(db);
    sqlite3_shutdown();

    return rc;
}
