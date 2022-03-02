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
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#ifndef SQLITEINT_H
#include <sqlite3ext.h>
#endif
SQLITE_EXTENSION_INIT1
#include <archive.h>
#include <archive_entry.h>

#define NADEKO_BUFFER_SIZE (1 << 16)

/*
** Test if the given filename points at a directory using
** only the public libarchive API for compatibility.
*/
static int nadekoIsDirectory(const char *zFilename) {
    struct archive *a = archive_read_disk_new();
    if (archive_read_disk_open(a, zFilename)) {
        return 0;
    }
    struct archive_entry *entry;
    archive_read_next_header(a, &entry);
    int result = archive_read_disk_can_descend(a);
    archive_read_free(a);

    return result;
}

/*
** Return archive handle to the disk archive or directory
** pointed at by the given filename.
*/
static int nadekoOpenThing(struct archive **pa, const char *zFilename, char **pzErr) {
    int rc = 0;
    if (nadekoIsDirectory(zFilename)) {
        *pa = archive_read_disk_new();
        if ((rc = archive_read_disk_open(*pa, zFilename))) goto abort;
    } else {
        *pa = archive_read_new();
        archive_read_support_format_all(*pa);
        archive_read_support_filter_all(*pa);
        if ((rc = archive_read_open_filename(*pa, zFilename, 10240))) goto abort;
    }
    return 0;

abort:
    *pzErr = sqlite3_mprintf("%s", archive_error_string(*pa));
    archive_read_close(*pa);
    return rc;
}

/*
** Return string with first and last character removed.
*/
static char *nadekoUnquote(const char *zString) {
    int length = strlen(zString);
    if (length < 2) return 0;
    if (zString[0] != '"' && zString[0] != '\'') return 0;
    if (zString[0] != zString[length - 1]) return 0;
    char *result = sqlite3_malloc(sizeof(char) * (length - 1));
    if (result == 0) return 0;
    memset(result, 0, length - 1);
    memcpy(result, zString + 1, length - 2);

    return result;
}

/*
** Copied verbatim from fts5ExecPrintf() in fts5_storage.c.
*/
static int nadekoExecPrintf(sqlite3 *db, char **pzErr, const char *zFormat, ...) {
    int rc;
    va_list ap; /* ... printf arguments */
    char *zSql;

    va_start(ap, zFormat);
    zSql = sqlite3_vmprintf(zFormat, ap);

    if (zSql == 0) {
        rc = SQLITE_NOMEM;
    } else {
        rc = sqlite3_exec(db, zSql, 0, 0, pzErr);
        sqlite3_free(zSql);
    }

    va_end(ap);
    return rc;
}

/*
** Fill the BLOB pointed at by the given arguments with
** data from the given archive handle.
*/
static int nadekoFillBlobFromArchive(struct archive *a, sqlite3 *db, const char *zDb,
    const char *zTable, const char *zColumn, int iRowid) {
    sqlite3_blob *blob;
    int offset = 0;
    int rc;

    if ((rc = sqlite3_blob_open(db, zDb, zTable, zColumn, iRowid, 1, &blob))) return rc;
    for (;;) {
        char buf[NADEKO_BUFFER_SIZE];
        int read = archive_read_data(a, buf, NADEKO_BUFFER_SIZE);
        if (read == -1) {
            sqlite3_blob_close(blob);
            return SQLITE_ERROR;
        } else if (read == 0) {
            sqlite3_blob_close(blob);
            return SQLITE_OK;
        } else {
            if ((rc = sqlite3_blob_write(blob, buf, read, offset))) {
                sqlite3_blob_close(blob);
                return rc;
            }
            offset += read;
        }
    }
}

static int nadekoFillArchiveFromBlob(struct archive *a, sqlite3_blob *pBlob) {
    char buf[NADEKO_BUFFER_SIZE];
    int bytes = sqlite3_blob_bytes(pBlob);
    for (int offset = 0; offset < bytes; offset += NADEKO_BUFFER_SIZE) {
        int write =
            offset + NADEKO_BUFFER_SIZE < bytes ? NADEKO_BUFFER_SIZE : bytes - offset;
        if (sqlite3_blob_read(pBlob, buf, write, offset)) {
            sqlite3_blob_close(pBlob);
            return SQLITE_ERROR;
        }
        if (archive_write_data(a, buf, write) == -1) {
            sqlite3_blob_close(pBlob);
            return SQLITE_ERROR;
        }
    }

    sqlite3_blob_close(pBlob);
    return SQLITE_OK;
}

/* nadeko_vtab is a subclass of sqlite3_vtab which is
** the underlying representation of the virtual table
*/
typedef struct nadeko_vtab nadeko_vtab;
struct nadeko_vtab {
    sqlite3_vtab base; /* Base class - must be first */
                       /* Insert new fields here */
    sqlite3 *db;
    struct archive *pArchive;
    sqlite3_int64 iKnown;
    sqlite3_int64 iBegun;
    char *zFilename;
    char *zTempname;
    char *zDb;
    char *zTable;
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
    nadeko_vtab *pParent;     /* Parent vtab */
    struct archive_entry *pEntry;
    sqlite3_stmt *pSelect;
    sqlite3_stmt *pInsert;
    sqlite3_stmt *pDelete;
};

/*
** This method is the destructor for nadeko_vtab objects.
*/
static int nadekoDisconnect(sqlite3_vtab *pVtab) {
    nadeko_vtab *pNdk = (nadeko_vtab *)(pVtab);
    archive_read_free(pNdk->pArchive);
    sqlite3_free(pNdk->zFilename);
    sqlite3_free(pNdk->zTempname);
    sqlite3_free(pNdk->zDb);
    sqlite3_free(pNdk->zTable);
    sqlite3_free(pVtab);
    return SQLITE_OK;
}

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
static int nadekoConnect(sqlite3 *db, void *, int argc, const char *const *argv,
    sqlite3_vtab **ppVtab, char **pzErr) {
    int rc;

    nadeko_vtab *pNew = sqlite3_malloc(sizeof(*pNew));
    if (pNew == 0) return SQLITE_NOMEM;
    memset(pNew, 0, sizeof(*pNew));
    pNew->db = db;
    pNew->zDb = sqlite3_mprintf("%s", argv[1]);
    pNew->zTable = sqlite3_mprintf("%s_store", argv[2]);
    *ppVtab = (sqlite3_vtab *)pNew;

    if (argc != 4) {
        nadekoDisconnect(&pNew->base);
        *pzErr = sqlite3_mprintf("wrong number of arguments to nadeko()");
        return SQLITE_ERROR;
    }
    if ((pNew->zFilename = nadekoUnquote(argv[3])) == 0) {
        nadekoDisconnect(&pNew->base);
        *pzErr = sqlite3_mprintf("first argument to nadeko() not a string");
        return SQLITE_ERROR;
    }
    if (!(pNew->zTempname = tmpnam(sqlite3_malloc(L_tmpnam * sizeof(char))))) {
        return SQLITE_NOMEM;
    }
    if ((rc = nadekoOpenThing(&pNew->pArchive, pNew->zFilename, pzErr))) {
        nadekoDisconnect(&pNew->base);
        return SQLITE_ERROR;
    }

    /* For convenience, define symbolic names for the index to each column. */
#define NADEKO_FILENAME 0
#define NADEKO_CONTENTS 1
    return rc || sqlite3_declare_vtab(db, "CREATE TABLE x(filename TEXT, contents BLOB)");
}

/*
** The nadekoCreate() method is invoked to create a new template
** virtual table and its backing store.
*/
static int nadekoCreate(sqlite3 *db, void *pAux, int argc, const char *const *argv,
    sqlite3_vtab **ppVtab, char **pzErr) {
    int rc = nadekoExecPrintf(db,
        pzErr,
        "CREATE TABLE IF NOT EXISTS %s.%s_store ("
        "  filename TEXT PRIMARY KEY,"
        "  contents BLOB,"
        ")",
        argv[1],
        argv[2]);

    return rc || nadekoConnect(db, pAux, argc, argv, ppVtab, pzErr);
}

/*
** This method is the destructor for nadeko_vtab objects as well
** as their associated backing store.
*/
static int nadekoDestroy(sqlite3_vtab *pVtab) {
    nadeko_vtab *pNdk = (nadeko_vtab *)(pVtab);
    int rc = nadekoExecPrintf(pNdk->db, 0, "DROP TABLE %s.%s", pNdk->zDb, pNdk->zTable);
    return rc || nadekoDisconnect(pVtab);
}

/*
** Constructor for a new nadeko_cursor object.
*/
static int nadekoOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppVtabCur) {
    nadeko_cursor *pCur;
    pCur = sqlite3_malloc(sizeof(*pCur));
    if (pCur == 0) return SQLITE_NOMEM;
    memset(pCur, 0, sizeof(*pCur));
    pCur->iRowid = 0;
    pCur->pParent = (nadeko_vtab *)(pVtab);
    pCur->pEntry = 0;

    char *zSel = sqlite3_mprintf("SELECT filename, contents FROM %s.%s WHERE rowid == ?",
        pCur->pParent->zDb,
        pCur->pParent->zTable);
    sqlite3_prepare(pCur->pParent->db, zSel, -1, &pCur->pSelect, 0);
    sqlite3_free(zSel);

    char *zIns =
        sqlite3_mprintf("INSERT OR REPLACE INTO %s.%s (rowid, filename, contents)"
                        "VALUES (?, ?, ?)",
            pCur->pParent->zDb,
            pCur->pParent->zTable);
    sqlite3_prepare(pCur->pParent->db, zIns, -1, &pCur->pInsert, 0);
    sqlite3_free(zIns);

    char *zDel = sqlite3_mprintf(
        "DELETE FROM %s.%s WHERE rowid = ?", pCur->pParent->zDb, pCur->pParent->zTable);
    sqlite3_prepare(pCur->pParent->db, zDel, -1, &pCur->pDelete, 0);
    sqlite3_free(zDel);

    *ppVtabCur = &pCur->base;
    return SQLITE_OK;
}

/*
** Destructor for a nadeko_cursor.
*/
static int nadekoClose(sqlite3_vtab_cursor *pVtabCur) {
    nadeko_cursor *pCur = (nadeko_cursor *)pVtabCur;
    sqlite3_finalize(pCur->pSelect);
    sqlite3_finalize(pCur->pInsert);
    sqlite3_finalize(pCur->pDelete);
    sqlite3_free(pCur);
    return SQLITE_OK;
}

/*
** Advance a nadeko_cursor to its next row of output.
*/
static int nadekoNext(sqlite3_vtab_cursor *pVtabCur) {
    nadeko_cursor *pCur = (nadeko_cursor *)pVtabCur;
    int rc = SQLITE_OK;
    pCur->iRowid++;

    if (pCur->pParent->pArchive != 0 && pCur->iRowid > pCur->pParent->iKnown) {
        switch (archive_read_next_header(pCur->pParent->pArchive, &pCur->pEntry)) {
        case ARCHIVE_OK:
            sqlite3_bind_int(pCur->pInsert, 1, pCur->pParent->iKnown + 1);
            sqlite3_bind_text(pCur->pInsert,
                2,
                archive_entry_pathname(pCur->pEntry),
                -1,
                SQLITE_TRANSIENT);
            sqlite3_bind_zeroblob(pCur->pInsert, 3, archive_entry_size(pCur->pEntry));
            sqlite3_step(pCur->pInsert);
            sqlite3_reset(pCur->pInsert);
            nadekoFillBlobFromArchive(pCur->pParent->pArchive,
                pCur->pParent->db,
                pCur->pParent->zDb,
                pCur->pParent->zTable,
                "contents",
                pCur->iRowid);
            pCur->pParent->iKnown++;
            break;
        case ARCHIVE_EOF:
            archive_read_free(pCur->pParent->pArchive);
            pCur->pParent->pArchive = 0;
            pCur->pEntry = 0;
            break;
        default:
            pVtabCur->pVtab->zErrMsg =
                sqlite3_mprintf("%s", archive_error_string(pCur->pParent->pArchive));
            rc = SQLITE_ERROR;
            break;
        };
    }

    sqlite3_reset(pCur->pSelect);
    sqlite3_bind_int(pCur->pSelect, 1, pCur->iRowid);
    sqlite3_step(pCur->pSelect);

    return rc;
}

/*
** Return values of columns for the row at which the nadeko_cursor
** is currently pointing.
*/
static int nadekoColumn(sqlite3_vtab_cursor *pVtabCur, /* The cursor */
    sqlite3_context *pCtx, /* First argument to sqlite3_result_...() */
    int iColumn            /* Which column to return */
) {
    nadeko_cursor *pCur = (nadeko_cursor *)pVtabCur;
    switch (iColumn) {
    case NADEKO_FILENAME:
        sqlite3_result_text(pCtx,
            (const char *)(sqlite3_column_text(pCur->pSelect, 0)),
            sqlite3_column_bytes(pCur->pSelect, 0),
            SQLITE_TRANSIENT);
        break;
    default:
        assert(iColumn == NADEKO_CONTENTS);
        sqlite3_result_blob(pCtx,
            sqlite3_column_blob(pCur->pSelect, 1),
            sqlite3_column_bytes(pCur->pSelect, 1),
            SQLITE_TRANSIENT);
        break;
    }

    return SQLITE_OK;
}

/*
** Return the rowid for the current row.  In this implementation, the
** rowid is the same as the output value.
*/
static int nadekoRowid(sqlite3_vtab_cursor *pVtabCur, sqlite_int64 *pRowid) {
    nadeko_cursor *pCur = (nadeko_cursor *)pVtabCur;
    *pRowid = pCur->iRowid;
    return SQLITE_OK;
}

/*
** Return TRUE if the cursor has been moved off of the last
** row of output.
*/
static int nadekoEof(sqlite3_vtab_cursor *pVtabCur) {
    nadeko_cursor *pCur = (nadeko_cursor *)pVtabCur;
    return pCur->iRowid > pCur->pParent->iKnown && pCur->pParent->pArchive == 0;
}

/*
** This method is called to "rewind" the nadeko_cursor object back
** to the first row of output.  This method is always called at least
** once prior to any call to nadekoColumn() or nadekoRowid() or
** nadekoEof().
*/
static int nadekoFilter(
    sqlite3_vtab_cursor *pVtabCur, int, const char *, int, sqlite3_value **) {
    int rc = SQLITE_OK;
    nadeko_cursor *pCur = (nadeko_cursor *)pVtabCur;
    pCur->iRowid = 0;

    return rc || nadekoNext(pVtabCur);
}

/*
** SQLite will invoke this method one or more times while planning a query
** that uses the virtual table.  This routine needs to create
** a query plan for each invocation and compute an estimated cost for that
** plan.
*/
static int nadekoBestIndex(sqlite3_vtab *, sqlite3_index_info *) { return SQLITE_OK; }

/*
** SQLite will invoke this method to determine whether a certain real table
** is in fact a shadow table for a virtual table.  This routine needs
** to return true when its input is the part of a shadow table name past
** the last "_" character.
*/
static int nadekoShadowName(const char *pName) { return sqlite3_stricmp(pName, "store"); }

static int nadekoUpdate(
    sqlite3_vtab *pVtab, int argc, sqlite3_value **argv, sqlite_int64 *pRowid) {
    puts("::Update::");
    nadeko_vtab *pNdk = (nadeko_vtab *)(pVtab);
    int rc;

    if (argc == 1) {
        // DELETE
        sqlite3_stmt *pDelete;
        char *zDel =
            sqlite3_mprintf("DELETE FROM %s.%s WHERE rowid = ?", pNdk->zDb, pNdk->zTable);
        sqlite3_prepare(pNdk->db, zDel, -1, &pDelete, 0);
        sqlite3_free(zDel);
        sqlite3_bind_value(pDelete, 1, argv[0]);
        sqlite3_step(pDelete);
        sqlite3_finalize(pDelete);
    } else {
        // INSERT OR UPDATE
        // TODO: Handle "INSERT OR REPLACE" specially
        sqlite3_stmt *pInsert;
        char *zIns =
            sqlite3_mprintf("INSERT OR REPLACE INTO %s.%s (rowid, filename, contents)"
                            "VALUES (?, ?, ?)",
                pNdk->zDb,
                pNdk->zTable);
        sqlite3_prepare(pNdk->db, zIns, -1, &pInsert, 0);
        sqlite3_free(zIns);
        sqlite3_bind_value(pInsert, 1, argv[1]);
        sqlite3_bind_value(pInsert, 2, argv[2]);
        sqlite3_bind_value(pInsert, 3, argv[3]);
        if ((rc = sqlite3_step(pInsert))) {
            sqlite3_finalize(pInsert);
            return sqlite3_extended_errcode(pNdk->db);
        }
        *pRowid = sqlite3_last_insert_rowid(pNdk->db);
        sqlite3_finalize(pInsert);
    }

    return SQLITE_OK;
}

static int nadekoBegin(sqlite3_vtab *pVtab) {
    puts("::Begin::");
    nadeko_vtab *pNdk = (nadeko_vtab *)(pVtab);
    pNdk->iBegun = 1;

    return SQLITE_OK;
}

static int nadekoSync(sqlite3_vtab *pVtab) {
    puts("::Sync::");
    nadeko_vtab *pNdk = (nadeko_vtab *)(pVtab);
    if (pNdk->iBegun == 0) {
        return SQLITE_OK;
    }

    puts("::RealSync::");
    struct archive *a = archive_write_new();
    int rc = SQLITE_OK;
    archive_write_set_format_filter_by_ext(a, pNdk->zFilename);
    archive_write_open_filename(a, pNdk->zTempname);

    sqlite3_stmt *pSelect;
    char *zSel =
        sqlite3_mprintf("SELECT filename, rowid FROM %s.%s", pNdk->zDb, pNdk->zTable);
    sqlite3_prepare(pNdk->db, zSel, -1, &pSelect, 0);
    sqlite3_free(zSel);

    sqlite3_blob *pBlob;
    sqlite3_blob_open(pNdk->db, pNdk->zDb, pNdk->zTable, "contents", 0, 0, &pBlob);
    struct archive_entry *entry = archive_entry_new();
    for (;;) {
        switch (sqlite3_step(pSelect)) {
        case SQLITE_ROW:
            sqlite3_blob_reopen(pBlob, sqlite3_column_int64(pSelect, 1));
            archive_entry_set_pathname(entry, (char *)(sqlite3_column_text(pSelect, 0)));
            archive_entry_set_size(entry, sqlite3_blob_bytes(pBlob));
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_write_header(a, entry);
            nadekoFillArchiveFromBlob(a, pBlob);
            archive_entry_clear(entry);
            break;
        case SQLITE_DONE:
            rc = SQLITE_OK;
            goto done;
        default:
            goto done;
        }
    }

done:
    sqlite3_finalize(pSelect);
    archive_entry_free(entry);
    archive_write_close(a);
    archive_write_free(a);

    return rc;
}

static int nadekoCommit(sqlite3_vtab *pVtab) {
    puts("::Commit::");
    nadeko_vtab *pNdk = (nadeko_vtab *)(pVtab);
    if (pNdk->iBegun == 0) {
        return SQLITE_OK;
    } else {
        pNdk->iBegun = 0;
    }

    puts("::RealCommit::");
    if (rename(pNdk->zTempname, pNdk->zFilename)) {
        remove(pNdk->zTempname);
        return SQLITE_ERROR;
    } else {
        return SQLITE_OK;
    }
}

static int nadekoRollback(sqlite3_vtab *pVtab) {
    puts("::Rollback::");
    nadeko_vtab *pNdk = (nadeko_vtab *)(pVtab);
    if (pNdk->iBegun == 0) {
        return SQLITE_OK;
    } else {
        pNdk->iBegun = 0;
    }

    puts("::RealRollback::");
    remove(pNdk->zTempname);
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
    /* xUpdate     */ nadekoUpdate,
    /* xBegin      */ nadekoBegin,
    /* xSync       */ nadekoSync,
    /* xCommit     */ nadekoCommit,
    /* xRollback   */ nadekoRollback,
    /* xFindMethod */ 0,
    /* xRename     */ 0,
    /* xSavepoint  */ 0,
    /* xRelease    */ 0,
    /* xRollbackTo */ 0,
    /* xShadowName */ nadekoShadowName};

#ifdef _WIN32
__declspec(dllexport)
#endif
    int sqlite3_nadeko_init(sqlite3 *db, char **, const sqlite3_api_routines *pApi) {
    int rc = SQLITE_OK;
    SQLITE_EXTENSION_INIT2(pApi);
    rc = sqlite3_create_module(db, "nadeko", &nadekoModule, 0);
    return rc;
}
