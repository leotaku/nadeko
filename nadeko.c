/*
**
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
#include <stddef.h>
#include <string.h>
#ifndef SQLITEINT_H
#include <sqlite3ext.h>
#endif
SQLITE_EXTENSION_INIT1
#include <archive.h>
#include <archive_entry.h>

/*
** Test if the given filename points at a directory using
** only the public libarchive API for compatibility.
*/
static int nadekoIsDirectory(const char *filename) {
    struct archive *a = archive_read_disk_new();
    if (archive_read_disk_open(a, filename)) {
        return 0;
    }
    struct archive_entry *entry;
    archive_read_next_header(a, &entry);
    int result = archive_read_disk_can_descend(a);
    archive_read_free(a);

    return result;
}

/* nadeko_vtab is a subclass of sqlite3_vtab which is
** underlying representation of the virtual table
*/
typedef struct nadeko_vtab nadeko_vtab;
struct nadeko_vtab {
    sqlite3_vtab base; /* Base class - must be first */
                       /* Insert new fields here */
    char *filename;
    int isDisk;
};

/* nadeko_cursor is a subclass of sqlite3_vtab_cursor which will
** serve as the underlying representation of a cursor that scans
** over rows of the result
*/
typedef struct nadeko_cursor nadeko_cursor;
struct nadeko_cursor {
    sqlite3_vtab_cursor base; /* Base class - must be first */
                              /* Insert new fields here */
    sqlite3_int64 iRowid;     /* The rowid */
    struct archive *archive;
    struct archive_entry *entry;
    char *filename;
    int isEof;
    int isDisk;
};

/*
** The nadekoConnect() method is invoked to create a new
** template virtual table.
**
** Think of this routine as the constructor for nadeko_vtab objects.
**
** All this routine needs to do is:
**
**    (1) Allocate the nadeko_vtab object and initialize all fields.
**
**    (2) Tell SQLite (via the sqlite3_declare_vtab() interface) what the
**        result set of queries against the virtual table will look like.
*/
static int nadekoConnect(sqlite3 *db, void *pAux, int argc, const char *const *argv,
    sqlite3_vtab **ppVtab, char **pzErr) {
    /* For convenience, define symbolic names for the index to each column. */
#define NADEKO_FILENAME 0
#define NADEKO_CONTENTS 1
    return sqlite3_declare_vtab(db, "CREATE TABLE x(filename,contents)");
}

/*
** This method is the destructor for nadeko_vtab objects.
*/
static int nadekoDisconnect(sqlite3_vtab *pVtab) {
    nadeko_vtab *p = (nadeko_vtab *)pVtab;
    sqlite3_free(p);
    return SQLITE_OK;
}

/*
** The nadekoCreate() method is invoked to create a new template
** virtual table and its backing store.
*/
static int nadekoCreate(sqlite3 *db, void *pAux, int argc, const char *const *argv,
    sqlite3_vtab **ppVtab, char **pzErr) {
    int rc = nadekoConnect(db, pAux, argc, argv, ppVtab, pzErr);
    char *filename;
    int length;

    if (argc != 4) return SQLITE_ERROR;
    if ((length = strlen(argv[3])) < 2) return SQLITE_ERROR;
    filename = sqlite3_malloc(sizeof(char) * length);
    if (filename == 0) return SQLITE_NOMEM;
    memset(filename, 0, length);
    memcpy(filename, argv[3] + 1, length - 2);

    if (rc == SQLITE_OK) {
        nadeko_vtab *pNew = sqlite3_malloc(sizeof(*pNew));
        if (pNew == 0) return SQLITE_NOMEM;
        memset(pNew, 0, sizeof(*pNew));
        pNew->isDisk = nadekoIsDirectory(filename);
        pNew->filename = filename;
        *ppVtab = (sqlite3_vtab *)pNew;
    }
    return rc;
}

/*
** This method is the destructor for nadeko_vtab objects as well
** as their associated backing store.
*/
static int nadekoDestroy(sqlite3_vtab *pVtab) {
    nadeko_vtab *p = (nadeko_vtab *)(pVtab);
    sqlite3_free(p->filename);
    return nadekoDisconnect(pVtab);
}

/*
** Constructor for a new nadeko_cursor object.
*/
static int nadekoOpen(sqlite3_vtab *p, sqlite3_vtab_cursor **ppCursor) {
    nadeko_vtab *pVtab = (nadeko_vtab *)(p);
    nadeko_cursor *pCur;
    pCur = sqlite3_malloc(sizeof(*pCur));
    if (pCur == 0) return SQLITE_NOMEM;
    memset(pCur, 0, sizeof(*pCur));
    pCur->filename = pVtab->filename;
    pCur->isDisk = pVtab->isDisk;
    *ppCursor = &pCur->base;
    return SQLITE_OK;
}

/*
** Destructor for a nadeko_cursor.
*/
static int nadekoClose(sqlite3_vtab_cursor *cur) {
    nadeko_cursor *pCur = (nadeko_cursor *)cur;
    archive_read_free(pCur->archive);
    sqlite3_free(pCur);
    return SQLITE_OK;
}

/*
** Advance a nadeko_cursor to its next row of output.
*/
static int nadekoNext(sqlite3_vtab_cursor *cur) {
    nadeko_cursor *pCur = (nadeko_cursor *)cur;
    if (pCur->isDisk && archive_read_disk_can_descend(pCur->archive)) {
        archive_read_disk_descend(pCur->archive);
    }

    int rc = archive_read_next_header(pCur->archive, &pCur->entry);

    switch (rc) {
    case ARCHIVE_EOF:
        pCur->isEof = 1;
        break;
    case ARCHIVE_OK:
        break;
    default:
        puts(archive_error_string(pCur->archive));
        return SQLITE_ERROR;
    }

    pCur->iRowid++;
    return SQLITE_OK;
}

/*
** Return values of columns for the row at which the nadeko_cursor
** is currently pointing.
*/
static int nadekoColumn(sqlite3_vtab_cursor *cur, /* The cursor */
    sqlite3_context *ctx, /* First argument to sqlite3_result_...() */
    int i                 /* Which column to return */
) {
    nadeko_cursor *pCur = (nadeko_cursor *)cur;
    const void *buf;
    size_t size;
    long offset;
    switch (i) {
    case NADEKO_FILENAME:
        sqlite3_result_text(ctx, archive_entry_pathname(pCur->entry), -1, 0);
        break;
    default:
        assert(i == NADEKO_CONTENTS);
        archive_read_data_block(pCur->archive, &buf, &size, &offset);
        sqlite3_result_text(ctx, buf, size, 0);
        break;
    }
    return SQLITE_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int nadekoRowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid) {
    nadeko_cursor *pCur = (nadeko_cursor *)cur;
    *pRowid = pCur->iRowid;
    return SQLITE_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int nadekoEof(sqlite3_vtab_cursor *cur) {
    nadeko_cursor *pCur = (nadeko_cursor *)cur;
    return pCur->isEof;
}

/*
** This method is called to "rewind" the nadeko_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to nadekoColumn() or nadekoRowid() or
** nadekoEof().
*/
static int nadekoFilter(sqlite3_vtab_cursor *pVtabCursor, int idxNum, const char *idxStr,
    int argc, sqlite3_value **argv) {
    nadeko_cursor *pCur = (nadeko_cursor *)pVtabCursor;
    int rc;

    if (pCur->archive) archive_read_free(pCur->archive);
    if (pCur->isDisk) {
        pCur->archive = archive_read_disk_new();
        if ((rc = archive_read_disk_open(pCur->archive, pCur->filename))) {
            return SQLITE_ERROR;
        }
    } else {
        pCur->archive = archive_read_new();
        archive_read_support_format_all(pCur->archive);
        archive_read_support_filter_all(pCur->archive);
        if ((rc = archive_read_open_filename(pCur->archive, pCur->filename, 10240))) {
            return SQLITE_ERROR;
        }
    }

    archive_read_next_header(pCur->archive, &pCur->entry);
    pCur->iRowid = 1;
    return SQLITE_OK;
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int nadekoBestIndex(sqlite3_vtab *tab, sqlite3_index_info *pIdxInfo) {
    pIdxInfo->estimatedCost = (double)10;
    pIdxInfo->estimatedRows = 10;
    return SQLITE_OK;
}

/*
** This following structure defines all the methods for the
** virtual table.
*/
static sqlite3_module nadekoModule = {
    /* iVersion    */ 0,
    /* xCreate     */ nadekoCreate,
    /* xConnect    */ nadekoConnect,
    /* xBestIndex  */ nadekoBestIndex,
    /* xDisconnect */ nadekoDisconnect,
    /* xDestroy    */ nadekoDestroy,
    /* xOpen       */ nadekoOpen,
    /* xClose      */ nadekoClose,
    /* xFilter     */ nadekoFilter,
    /* xNext       */ nadekoNext,
    /* xEof        */ nadekoEof,
    /* xColumn     */ nadekoColumn,
    /* xRowid      */ nadekoRowid,
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
    int sqlite3_nadeko_init(
        sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    int rc = SQLITE_OK;
    SQLITE_EXTENSION_INIT2(pApi);
    rc = sqlite3_create_module(db, "nadeko", &nadekoModule, 0);
    return rc;
}
