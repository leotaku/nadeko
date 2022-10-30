/*
** This file implements an eponymous-only virtual table with a rowid and
** two columns named "line" and "data".  The table returns segments of
** the "data" column separated by UNIX or DOS style newlines as values
** of the "line" column. Usage example:
**
**     SELECT rowid, line FROM lines("aaa" || char(13) || char(10) || "bbb");
**     SELECT rowid, line FROM lines WHERE data == "aaa" || char(10) || "bbb";
*/
#include <assert.h>
#include <string.h>
#ifndef SQLITEINT_H
#include <sqlite3ext.h>
#endif
SQLITE_EXTENSION_INIT1

/* lines_vtab is a subclass of sqlite3_vtab which is
** the underlying representation of the virtual table
*/
typedef struct lines_vtab lines_vtab;
struct lines_vtab {
    sqlite3_vtab base; /* Base class - must be first */
                       /* Insert new fields here */
};

/* lines_cursor is a subclass of sqlite3_vtab_cursor which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct lines_cursor lines_cursor;
struct lines_cursor {
    sqlite3_vtab_cursor base; /* Base class - must be first */
                              /* Insert new fields here */
    sqlite3_int64 iRowid;     /* The rowid */
    sqlite3_value *pValue;
    sqlite3_int64 iOffset;
    sqlite3_int64 iLength;
    sqlite3_int64 iBytes;
    char *pBuffer;
};

/*
** The linesConnect() method is invoked to create a new
** template virtual table.
**
** Think of this routine as the constructor for lines_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the lines_vtab object and initialize all fields.
**
**    (2) Tell SQLite (via the sqlite3_declare_vtab() interface) what the
**        result set of queries against the virtual table will look like.
*/
static int linesConnect(sqlite3 *db,
    void *pAuxUnused,
    int argcUnused,
    const char *const *argvUnused,
    sqlite3_vtab **ppVtab,
    char **pzErrUnused) {
    (void)(pAuxUnused);
    (void)(argcUnused);
    (void)(argvUnused);
    (void)(pzErrUnused);

    lines_vtab *pNew;

    pNew = sqlite3_malloc(sizeof(*pNew));
    *ppVtab = (sqlite3_vtab *)pNew;
    if (pNew == 0) return SQLITE_NOMEM;
    memset(pNew, 0, sizeof(*pNew));

    /* For convenience, define symbolic names for the index to each column. */
#define LINES_LINE 0
#define LINES_DATA 1
    return sqlite3_declare_vtab(db, "CREATE TABLE x(line, data HIDDEN)");
}

/*
** This method is the destructor for lines_vtab objects.
*/
static int linesDisconnect(sqlite3_vtab *pVtab) {
    lines_vtab *pLns = (lines_vtab *)pVtab;
    sqlite3_free(pLns);
    return SQLITE_OK;
}

/*
** Constructor for a new lines_cursor object.
*/
static int linesOpen(sqlite3_vtab *pVTabUnused, sqlite3_vtab_cursor **ppVtabCur) {
    (void)(pVTabUnused);

    lines_cursor *pCur = sqlite3_malloc(sizeof(lines_cursor));
    if (pCur == 0) return SQLITE_NOMEM;
    memset(pCur, 0, sizeof(*pCur));
    *ppVtabCur = &pCur->base;
    return SQLITE_OK;
}

/*
** Destructor for a lines_cursor.
*/
static int linesClose(sqlite3_vtab_cursor *pVtabCur) {
    lines_cursor *pCur = (lines_cursor *)pVtabCur;
    sqlite3_free(pCur->pBuffer);
    sqlite3_free(pCur);
    return SQLITE_OK;
}

/*
** Advance a lines_cursor to its next row of output.
*/
static int linesNext(sqlite3_vtab_cursor *pVtabCur) {
    lines_cursor *pCur = (lines_cursor *)pVtabCur;
    pCur->iOffset += pCur->iLength;
    if (pCur->iOffset != 0) pCur->iOffset++;
    if (pCur->pBuffer[pCur->iOffset] == '\r') pCur->iOffset++;
    if (pCur->iOffset >= pCur->iBytes) return SQLITE_OK;

    char *pNewline;
    pNewline = memchr(pCur->pBuffer + pCur->iOffset, '\n', pCur->iBytes - pCur->iOffset);
    if (pNewline != 0) {
        pCur->iLength = pNewline - (pCur->pBuffer + pCur->iOffset);
    } else {
        pCur->iLength = pCur->iBytes - pCur->iOffset;
    };

    pCur->iRowid++;
    return SQLITE_OK;
}

/*
** Return values of columns for the row at which the lines_cursor
** is currently pointing.
*/
static int linesColumn(sqlite3_vtab_cursor *pVtabCur, /* The cursor */
    sqlite3_context *pCtx, /* First argument to sqlite3_result_...() */
    int iColumn            /* Which column to return */
) {
    lines_cursor *pCur = (lines_cursor *)pVtabCur;
    switch (iColumn) {
    case LINES_LINE:
        sqlite3_result_text(
            pCtx, pCur->pBuffer + pCur->iOffset, pCur->iLength, SQLITE_TRANSIENT);
        break;
    default:
        assert(iColumn == LINES_DATA);
        sqlite3_result_value(pCtx, pCur->pValue);
        break;
    }
    return SQLITE_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int linesRowid(sqlite3_vtab_cursor *pVtabCur, sqlite_int64 *pRowid) {
    lines_cursor *pCur = (lines_cursor *)pVtabCur;
    *pRowid = pCur->iRowid;
    return SQLITE_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int linesEof(sqlite3_vtab_cursor *pVtabCur) {
    lines_cursor *pCur = (lines_cursor *)pVtabCur;
    return pCur->iOffset >= pCur->iBytes;
}

/*
** This method is called to "rewind" the lines_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to linesColumn() or linesRowid() or
** linesEof().
*/
static int linesFilter(sqlite3_vtab_cursor *pVtabCur,
    int idxNumUnused,
    const char *idxStrUnused,
    int argcUnused,
    sqlite3_value **argv) {
    (void)(idxNumUnused);
    (void)(idxStrUnused);
    (void)(argcUnused);

    lines_cursor *pCur = (lines_cursor *)pVtabCur;
    int rc = SQLITE_OK;

    const sqlite_int64 iOldBytes = pCur->iBytes;
    pCur->pValue = argv[0];
    pCur->iBytes = sqlite3_value_bytes(argv[0]);
    pCur->iLength = 0;
    pCur->iOffset = 0;
    switch (sqlite3_value_type(argv[0])) {
    case SQLITE_TEXT:
        if (pCur->iBytes > iOldBytes) {
            sqlite3_free(pCur->pBuffer);
            pCur->pBuffer = sqlite3_malloc(pCur->iBytes * sizeof(char));
        }
        memcpy(pCur->pBuffer, sqlite3_value_text(argv[0]), pCur->iBytes);
        break;
    case SQLITE_BLOB:
        if (pCur->iBytes > iOldBytes) {
            sqlite3_free(pCur->pBuffer);
            pCur->pBuffer = sqlite3_malloc(pCur->iBytes * sizeof(char));
        }
        memcpy(pCur->pBuffer, sqlite3_value_blob(argv[0]), pCur->iBytes);
        break;
    default:
        pVtabCur->pVtab->zErrMsg =
            sqlite3_mprintf("first argument to lines() not a string or blob");
        return SQLITE_ERROR;
    }

    return rc || linesNext(pVtabCur);
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int linesBestIndex(sqlite3_vtab *pVTabUnused, sqlite3_index_info *pIdxInfo) {
    (void)(pVTabUnused);

    const struct sqlite3_index_constraint *pConstraint = pIdxInfo->aConstraint;
    for (int i = 0; i < pIdxInfo->nConstraint; i++, pConstraint++) {
        if (pConstraint->iColumn == LINES_DATA && pConstraint->usable) {
            if (pConstraint->op == SQLITE_INDEX_CONSTRAINT_EQ) {
                pIdxInfo->aConstraintUsage[i].argvIndex = 1;
                return SQLITE_OK;
            }
        }
    };

    return SQLITE_CONSTRAINT;
}

/*
** This following structure defines all the methods for the
** virtual table.
*/
static sqlite3_module linesModule = {
    /* iVersion    */ 0,
    /* xCreate     */ 0,
    /* xConnect    */ linesConnect,
    /* xBestIndex  */ linesBestIndex,
    /* xDisconnect */ linesDisconnect,
    /* xDestroy    */ 0,
    /* xOpen       */ linesOpen,
    /* xClose      */ linesClose,
    /* xFilter     */ linesFilter,
    /* xNext       */ linesNext,
    /* xEof        */ linesEof,
    /* xColumn     */ linesColumn,
    /* xRowid      */ linesRowid,
    /* xUpdate     */ 0,
    /* xBegin      */ 0,
    /* xSync       */ 0,
    /* xCommit     */ 0,
    /* xRollback   */ 0,
    /* xFindMethod */ 0,
    /* xRename     */ 0,
    /* xSavepoint  */ 0,
    /* xRelease    */ 0,
    /* xRollbackTo */ 0,
    /* xShadowName */ 0};

#ifdef _WIN32
__declspec(dllexport)
#endif
    int sqlite3_lines_init(
        sqlite3 *db, char **pzErrMsgUnused, const sqlite3_api_routines *pApi) {
    (void)(pzErrMsgUnused);

    int rc = SQLITE_OK;
    SQLITE_EXTENSION_INIT2(pApi);
    rc = sqlite3_create_module(db, "lines", &linesModule, 0);
    return rc;
}
