/*
** This file implements a template virtual-table.
** Developers can make a copy of this file as a baseline for writing
** new virtual tables and/or table-valued functions.
**
** Steps for writing a new virtual table implementation:
**
**     (1)  Make a copy of this file.  Perhaps call it "mynewvtab.c"
**
**     (2)  Replace this header comment with something appropriate for
**          the new virtual table
**
**     (3)  Change every occurrence of "templatevtab" to some other string
**          appropriate for the new virtual table.  Ideally, the new string
**          should be the basename of the source file: "mynewvtab".  Also
**          globally change "TEMPLATEVTAB" to "MYNEWVTAB".
**
**     (4)  Run a test compilation to make sure the unmodified virtual
**          table works.
**
**     (5)  Begin making incremental changes, testing as you go, to evolve
**          the new virtual table to do what you want it to do.
**
** This template is minimal, in the sense that it uses only the required
** methods on the sqlite3_module object.  As a result, templatevtab is
** a read-only and eponymous-only table.  Those limitation can be removed
** by adding new methods.
**
** This template implements an eponymous-only virtual table with a rowid and
** two columns named "a" and "b".  The table as 10 rows with fixed integer
** values. Usage example:
**
**     SELECT rowid, a, b FROM templatevtab;
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
    sqlite3_int64 iBytes;
    sqlite3_int64 iLength;
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
static int linesConnect(
    sqlite3 *db, void *, int, const char *const *, sqlite3_vtab **ppVtab, char **) {
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
    lines_vtab *p = (lines_vtab *)pVtab;
    sqlite3_free(p);
    return SQLITE_OK;
}

/*
** Constructor for a new lines_cursor object.
*/
static int linesOpen(sqlite3_vtab *, sqlite3_vtab_cursor **ppVtabCur) {
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
    memmove(pCur->pBuffer, pCur->pBuffer + pCur->iLength, pCur->iBytes - pCur->iLength);
    memset(pCur->pBuffer + pCur->iBytes - pCur->iLength, 0, 1);
    char *ptr = strchr(pCur->pBuffer, '\n');
    if (!ptr) ptr = strchr(pCur->pBuffer, '\0');
    pCur->iLength = ptr - pCur->pBuffer;

    if (pCur->iLength <= 0) {
        sqlite3_free(pCur->pBuffer);
        pCur->pBuffer = 0;
    } else {
        pCur->iLength++;
    };

    pCur->iRowid++;
    return SQLITE_OK;
}

/*
** Return values of columns for the row at which the lines_cursor
** is currently pointing.
*/
static int linesColumn(sqlite3_vtab_cursor *pVtabCur, /* The cursor */
    sqlite3_context *ctx, /* First argument to sqlite3_result_...() */
    int i                 /* Which column to return */
) {
    lines_cursor *pCur = (lines_cursor *)pVtabCur;
    switch (i) {
    case LINES_LINE:
        if (pCur->pBuffer[pCur->iLength - 2] == '\r') {
            sqlite3_result_text(ctx, pCur->pBuffer, pCur->iLength - 2, SQLITE_TRANSIENT);
        } else {
            sqlite3_result_text(ctx, pCur->pBuffer, pCur->iLength - 1, SQLITE_TRANSIENT);
        }
        break;
    default:
        assert(i == LINES_DATA);
        sqlite3_result_value(ctx, pCur->pValue);
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
    return pCur->pBuffer == 0;
}

/*
** This method is called to "rewind" the lines_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to linesColumn() or linesRowid() or
** linesEof().
*/
static int linesFilter(
    sqlite3_vtab_cursor *pVtabCur, int, const char *, int, sqlite3_value **argv) {
    lines_cursor *pCur = (lines_cursor *)pVtabCur;
    int rc = SQLITE_OK;

    switch (sqlite3_value_type(argv[0])) {
    case SQLITE_TEXT:
        pCur->pValue = argv[0];
        pCur->iLength = 0;
        int iBytes = sqlite3_value_bytes(argv[0]) + 1;
        if (pCur->iBytes != iBytes) {
            sqlite3_free(pCur->pBuffer);
            pCur->pBuffer = sqlite3_malloc(iBytes * sizeof(char));
            pCur->iBytes = iBytes;
        }
        memset(pCur->pBuffer, 0, pCur->iBytes);
        memcpy(pCur->pBuffer, sqlite3_value_text(argv[0]), pCur->iBytes - 1);
        break;
    case SQLITE_BLOB:
        pVtabCur->pVtab->zErrMsg =
            sqlite3_mprintf("blob support for lines() not yet implemented");
        return SQLITE_ERROR;
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
static int linesBestIndex(sqlite3_vtab *, sqlite3_index_info *pIdxInfo) {
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
    int sqlite3_lines_init(sqlite3 *db, char **, const sqlite3_api_routines *pApi) {
    int rc = SQLITE_OK;
    SQLITE_EXTENSION_INIT2(pApi);
    rc = sqlite3_create_module(db, "lines", &linesModule, 0);
    return rc;
}
