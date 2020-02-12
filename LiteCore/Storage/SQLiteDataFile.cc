//
// SQLiteDataFile.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

/*
 * DataFile version history
 * 201: Initial Version
 * 301: Add index table for use with FTS
 * 302: Add purgeCnt entry to kvmeta
 */

#include "SQLiteDataFile.hh"
#include "SQLiteKeyStore.hh"
#include "SQLite_Internal.hh"
#include "BothKeyStore.hh"
#include "Record.hh"
#include "UnicodeCollator.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "SharedKeys.hh"
#include "Stopwatch.hh"
#include "StringUtil.hh"
#include "SQLiteCpp/SQLiteCpp.h"
#include "SecureRandomize.hh"
#include "PlatformCompat.hh"
#include "fleece/Fleece.hh"
#include <mutex>
#include <sqlite3.h>
#include <sstream>
#include <mutex>
#include <thread>

extern "C" {
    #include "sqlite3_unicodesn_tokenizer.h"
    
#ifdef COUCHBASE_ENTERPRISE
    // These definitions were removed from the public SQLite header
    // in 3.32.0, and Xcode doesn't like to alter header search paths
    // between configurations, so as a compromise just lay them out here:
    SQLITE_API int sqlite3_key_v2(
      sqlite3 *db,                   /* Database to be rekeyed */
      const char *zDbName,           /* Name of the database */
      const void *pKey, int nKey     /* The key */
    );

    SQLITE_API int sqlite3_rekey_v2(
      sqlite3 *db,                   /* Database to be rekeyed */
      const char *zDbName,           /* Name of the database */
      const void *pKey, int nKey     /* The new key */
    );
#endif
}

#if __APPLE__
#include <TargetConditionals.h>
#else
#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#include "SQLiteTempDirectory.h"
#endif
#endif

using namespace std;

namespace litecore {

    static const int64_t MB = 1024 * 1024;

    // SQLite page size
    static const int64_t kPageSize = 4096;

    // SQLite cache size (per connection)
    static const size_t kCacheSize = 10 * MB;

    // Maximum size WAL journal will be left at after a commit
    static const int64_t kJournalSize = 5 * MB;

    // Amount of file to memory-map
#if TARGET_OS_OSX || TARGET_OS_SIMULATOR
    static const int kMMapSize =  -1;    // Avoid possible file corruption hazard on macOS
#else
    static const int kMMapSize = 50 * MB;
#endif

    // If this fraction of the database is composed of free pages, vacuum it on close
    static const float kVacuumFractionThreshold = 0.25;
    // If the database has many bytes of free space, vacuum it on close
    static const int64_t kVacuumSizeThreshold = 10 * MB;

    // Database busy timeout; generally not needed since we have other arbitration that keeps
    // multiple threads from trying to start transactions at once, but another process might
    // open the database and grab the write lock.
    static const unsigned kBusyTimeoutSecs = 10;

    // Name of the KeyStore for deleted documents
    static const string kDeletedKeyStoreName = "deleted";

    LogDomain SQL("SQL", LogLevel::Warning);

    void LogStatement(const SQLite::Statement &st) {
        LogTo(SQL, "... %s", st.getQuery().c_str());
    }

    static void sqlite3_log_callback(void *pArg, int errCode, const char *msg) {
        if (errCode == SQLITE_NOTICE_RECOVER_WAL)
            return;     // harmless "recovered __ frames from WAL file" message
        int baseCode = errCode & 0xFF;
        if (baseCode == SQLITE_SCHEMA)
            return;     // ignore harmless "statement aborts ... database schema has changed" warning
        if (errCode == SQLITE_WARNING && strncmp(msg, "file unlinked while open:", 25) == 0)
            return;     // ignore warning closing zombie db that's been deleted (#381)

        if (baseCode == SQLITE_NOTICE || baseCode == SQLITE_READONLY) {
            LogTo(DBLog, "SQLite message: %s", msg);
        } else {
            LogToAt(DBLog, Error, "SQLite error (code %d): %s", errCode, msg);
        }
    }


    UsingStatement::UsingStatement(SQLite::Statement &stmt) noexcept
    :_stmt(stmt)
    {
        LogStatement(stmt);
    }


    UsingStatement::~UsingStatement() {
        try {
            _stmt.reset();
        } catch (...) { }
    }


    SQLiteDataFile::Factory& SQLiteDataFile::sqliteFactory() {
        static SQLiteDataFile::Factory s;
        return s;
    }


    SQLiteDataFile::Factory::Factory() {
        // One-time initialization at startup:
        SQLite::Exception::logger = [](const SQLite::Exception &x) {
            LogToAt(SQL, Error, "%s (%d/%d)", x.what(), x.getErrorCode(), x.getExtendedErrorCode());
        };
        Assert(sqlite3_libversion_number() >= 300900, "LiteCore requires SQLite 3.9+");
        sqlite3_config(SQLITE_CONFIG_LOG, sqlite3_log_callback, NULL);
#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
        setSqliteTempDirectory();
#endif
    }


    bool SQLiteDataFile::Factory::encryptionEnabled(EncryptionAlgorithm alg) {
#ifdef COUCHBASE_ENTERPRISE
        return (alg == kNoEncryption || alg == kAES256);
#else
        return (alg == kNoEncryption);
#endif
    }


    SQLiteDataFile* SQLiteDataFile::Factory::openFile(const FilePath &path, Delegate *delegate, const Options *options) {
        return new SQLiteDataFile(path, delegate, options);
    }


    bool SQLiteDataFile::Factory::_deleteFile(const FilePath &path, const Options*) {
        LogTo(DBLog, "Deleting database file %s (with -wal and -shm)", path.path().c_str());
        bool ok =  path.del() | path.appendingToName("-shm").del() | path.appendingToName("-wal").del();
        // Note the non-short-circuiting 'or'! All 3 paths will be deleted.
        LogDebug(DBLog, "...finished deleting database file %s (with -wal and -shm)",
                 path.path().c_str());
        return ok;
    }


    SQLiteDataFile::SQLiteDataFile(const FilePath &path, Delegate *delegate, const Options *options)
    :DataFile(path, delegate, options)
    {
        reopen();
    }


    SQLiteDataFile::~SQLiteDataFile() {
        close();
    }


    void SQLiteDataFile::reopen() {
        DataFile::reopen();
        reopenSQLiteHandle();
        decrypt();

        withFileLock([this]{
            // http://www.sqlite.org/pragma.html
            _schemaVersion = SchemaVersion((int)_sqlDb->execAndGet("PRAGMA user_version"));
            bool isNew = false;
            if (_schemaVersion == SchemaVersion::None) {
                isNew = true;
                // Configure persistent db settings, and create the schema.
                // `auto_vacuum` has to be enabled ASAP, before anything's written to the db!
                // (even setting `auto_vacuum` writes to the db, it turns out! See CBSE-7971.)
                _exec(format("PRAGMA auto_vacuum=incremental; "
                             "PRAGMA journal_mode=WAL; "
                             "BEGIN; "
                             "CREATE TABLE IF NOT EXISTS "      // Table of metadata about KeyStores
                             "  kvmeta (name TEXT PRIMARY KEY, lastSeq INTEGER DEFAULT 0, purgeCnt INTEGER DEFAULT 0) WITHOUT ROWID; "
                             "PRAGMA user_version=%d; "
                             "END;",
                             SchemaVersion::Current));
                _schemaVersion = SchemaVersion::Current;
                Assert(intQuery("PRAGMA auto_vacuum") == 2, "Incremental vacuum was not enabled!");
                // Create the default KeyStore's table:
                (void)defaultKeyStore();
            } else if (_schemaVersion < SchemaVersion::MinReadable) {
                error::_throw(error::DatabaseTooOld);
            } else if (_schemaVersion > SchemaVersion::MaxReadable) {
                error::_throw(error::DatabaseTooNew);
            }

            _exec(format("PRAGMA cache_size=%d; "            // Memory cache
                         "PRAGMA mmap_size=%d; "             // Memory-mapped reads
                         "PRAGMA synchronous=normal; "       // Speeds up commits
                         "PRAGMA journal_size_limit=%lld; "  // Limit WAL disk usage
                         "PRAGMA case_sensitive_like=true",  // Case sensitive LIKE, for N1QL compat
                         -(int)kCacheSize/1024, kMMapSize, (long long)kJournalSize));

            bool ok = upgradeSchema(SchemaVersion::WithPurgeCount,
                                    "Adding purgeCnt column", [&] {
                // Schema upgrade: Add the `purgeCnt` column to the kvmeta table.
                // We can postpone this schema change if the db is read-only, since the purge count
                // is only used to mark changes to the database, and we won't be changing it.
                _exec("ALTER TABLE kvmeta ADD COLUMN purgeCnt INTEGER DEFAULT 0");
            });
            if (ok) {
                upgradeSchema(SchemaVersion::WithDeletedTable,
                              "Migrating deleted docs to 'deleted' KeyStore", [&] {
                    // Schema upgrade: Migrate deleted docs to separate table.
                    {
                        // First create the 'kv_deleted' table by instantitating its KeyStore:
                        SQLiteKeyStore ks(*this, kDeletedKeyStoreName, options().keyStores);
                    }
                    // Now move all the deleted docs to the new table:
                    _exec(format("INSERT INTO kv_%s SELECT * FROM kv_%s WHERE (flags&1)!=0;"
                                 "DELETE FROM kv_%s WHERE (flags&1)!=0;",
                                 kDeletedKeyStoreName.c_str(), kDefaultKeyStoreName.c_str(),
                                 kDefaultKeyStoreName.c_str()));
                });
            }
        });

        // Configure number of extra threads to be used by SQLite:
        auto sqlite = _sqlDb->getHandle();
        if (thread::hardware_concurrency() > 2)
            sqlite3_limit(sqlite, SQLITE_LIMIT_WORKER_THREADS, 2);

        // Register collators, custom functions, and the FTS tokenizer:
        RegisterSQLiteUnicodeCollations(sqlite, _collationContexts);
        RegisterSQLiteFunctions(sqlite, {delegate(), documentKeys()});
        int rc = register_unicodesn_tokenizer(sqlite);
        if (rc != SQLITE_OK)
            warn("Unable to register FTS tokenizer: SQLite err %d", rc);
    }


    bool SQLiteDataFile::upgradeSchema(SchemaVersion minVersion,
                                       const char *what,
                                       function_ref<void()> upgrade)
    {
        auto logUpgrade = [&](const char *msg) {
            logInfo("SCHEMA UPGRADE (%d-%d) %-s", _schemaVersion, minVersion, msg);
        };

        if (_schemaVersion < minVersion) {
            if (!options().writeable) {
                logUpgrade("skipped; cannot upgrade read-only connection");
                return false;
            }
            if (!options().upgradeable) {
                logUpgrade("blocked: opening with 'NoUpgrade' flag");
                error::_throw(error::CantUpgradeDatabase);
            }
            
            logUpgrade(what);
            bool inTransaction = false;
            try {
                _exec("BEGIN");
                inTransaction = true;
                upgrade();
                _exec(format("PRAGMA user_version=%d; END", minVersion));
            } catch (const SQLite::Exception &x) {
                // Recover if the db file itself is read-only (but not opened with writeable=false)
                if (x.getErrorCode() == SQLITE_READONLY) {
                    logUpgrade("skipped; cannot upgrade read-only file");
                    if (inTransaction)
                        _exec("ABORT");
                    auto opts = options();
                    opts.writeable = false;
                    setOptions(opts);
                    return false;
                } else {
                    throw;
                }
            }
            _schemaVersion = minVersion;
        }
        return true;
    }


    void SQLiteDataFile::reopenSQLiteHandle() {
        // We are about to replace the sqlite3 handle, so the compiled statements
        // need to be cleared
        _getLastSeqStmt.reset();
        _setLastSeqStmt.reset();
        _getPurgeCntStmt.reset();
        _setPurgeCntStmt.reset();
        
        int sqlFlags = options().writeable ? SQLite::OPEN_READWRITE : SQLite::OPEN_READONLY;
        if (options().create)
            sqlFlags |= SQLite::OPEN_CREATE;
        _sqlDb = make_unique<SQLite::Database>(filePath().path().c_str(),
                                               sqlFlags,
                                               kBusyTimeoutSecs * 1000);
    }


    void SQLiteDataFile::ensureSchemaVersionAtLeast(SchemaVersion version) {
        if (_schemaVersion < version) {
            const auto versionSql = "PRAGMA user_version=" + to_string(int(version));
            _exec(versionSql);
            _schemaVersion = version;
        }
    }


    bool SQLiteDataFile::isOpen() const noexcept {
        return _sqlDb != nullptr;
    }


    // Called by DataFile::close (the public method)
    void SQLiteDataFile::_close(bool forDelete) {
        _getLastSeqStmt.reset();
        _setLastSeqStmt.reset();
        _getPurgeCntStmt.reset();
        _setPurgeCntStmt.reset();
        if (_sqlDb) {
            if (options().writeable) {
                optimize();
                vacuum(false);
            }
            // Close the SQLite database:
            if (!_sqlDb->closeUnlessStatementsOpen()) {
                // There are still SQLite statements (queries) open, probably in QueryEnumerators
                // that haven't been deleted yet -- this can happen if the client code has garbage-
                // collected objects owning those enumerators, which won't release them until their
                // finalizers run. (Couchbase Lite Java has this issue.)
                // We'll log info about the statements so this situation can be detected from logs.
                _sqlDb->withOpenStatements([=](const char *sql, bool busy) {
                    _log((forDelete ? LogLevel::Warning : LogLevel::Info),
                         "SQLite::Database %p close deferred due to %s sqlite_stmt: %s",
                         _sqlDb.get(), (busy ? "busy" : "open"), sql);
                });
                if (forDelete)
                    error::_throw(error::Busy, "SQLite db has active statements, can't be deleted");
                // Also, tell SQLite not to checkpoint the WAL when it eventually closes the db
                // (after the last statement is freed), as that can have disastrous effects if the
                // db has since been deleted and re-created: see issue #381 for gory details.
                int noCheckpointResult = sqlite3_db_config(_sqlDb->getHandle(), SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE, 1, nullptr);
                Assert(noCheckpointResult == SQLITE_OK, "Failed to set SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE");
            }
            // Finally, delete the SQLite::Database instance:
            _sqlDb.reset();
            logVerbose("Closed SQLite database");
        }
        _collationContexts.clear();
    }


    void SQLiteDataFile::decrypt() {
        auto alg = options().encryptionAlgorithm;
        if (!factory().encryptionEnabled(alg))
            error::_throw(error::UnsupportedEncryption);
#ifdef COUCHBASE_ENTERPRISE
        // Set the encryption key in SQLite:
        slice key;
        if (alg != kNoEncryption) {
            key = options().encryptionKey;
            if (key.buf == nullptr || key.size != kEncryptionKeySize[alg])
                error::_throw(error::InvalidParameter);
        }
        bool success = _decrypt(alg, key);
#if __APPLE__
        if (!success && alg == kAES256) {
            // If using AES256, retry with AES128 for backward compatibility with earlier versions:
            logInfo("Retrying decryption with AES128...");
            reopenSQLiteHandle();
            success = _decrypt(kAES128, slice(key.buf, kEncryptionKeySize[kAES128]));
            if (success) {
                logInfo("Success! Database is decrypted.");
                if (options().writeable && options().upgradeable) {
                    // Now rekey with the full AES256 key:
                    logInfo("Rekeying db to full AES256 encryption; this may take time...");
                    int rc = sqlite3_rekey_v2(_sqlDb->getHandle(), nullptr, key.buf, (int)key.size);
                    if (rc != SQLITE_OK) {
                        logError("Rekeying to AES256 failed (err %d); continuing with existing db", rc);
                    }
                }
            }
        }
#endif
        if (!success)
            error::_throw(error::NotADatabaseFile);
#endif
    }


#ifdef COUCHBASE_ENTERPRISE
    // Returns true on success, false if key is not valid; other errors thrown as exceptions.
    bool SQLiteDataFile::_decrypt(EncryptionAlgorithm alg, slice key) {
        static const char* kAlgorithmName[3] = {"no encryption", "AES256", "AES128"};
        // Calling sqlite3_key_v2 even with a null key (no encryption) reserves space in the db
        // header for a nonce, which will enable secure rekeying in the future.
        int rc = sqlite3_key_v2(_sqlDb->getHandle(), nullptr, key.buf, (int)key.size);
        if (rc != SQLITE_OK) {
            error::_throw(error::UnsupportedEncryption,
                          "Unable to set encryption key (SQLite error %d)", rc);
        }

        // Since sqlite3_key_v2() does NOT attempt to read the database, we must do our own
        // verification that the encryption key is correct (or db is unencrypted, if no key given):
        rc = sqlite3_exec(_sqlDb->getHandle(), "SELECT count(*) FROM sqlite_master",
                          NULL, NULL, NULL);
        switch (rc) {
            case SQLITE_OK:
                return true;
            case SQLITE_NOTADB:
                logError("Could not decrypt database with %s", kAlgorithmName[alg]);
                return false;
            default:
                logError("Could not read database (err %d) using %s", rc, kAlgorithmName[alg]);
                error::_throw(error::SQLite, rc);
        }
    }
#endif


    void SQLiteDataFile::rekey(EncryptionAlgorithm alg, slice newKey) {
#ifdef COUCHBASE_ENTERPRISE
        if (!factory().encryptionEnabled(alg))
            error::_throw(error::UnsupportedEncryption);

        bool currentlyEncrypted = (options().encryptionAlgorithm != kNoEncryption);
        if (alg == kNoEncryption) {
            if (!currentlyEncrypted)
                return;
            logInfo("Decrypting DataFile");
        } else {
            if (currentlyEncrypted) {
                logInfo("Changing DataFile encryption key");
            }
            else {
                logInfo("Encrypting DataFile");
            }
        }
        
        if (newKey.size != kEncryptionKeySize[alg])
            error::_throw(error::InvalidParameter);
        int rekeyResult = 0;
        if(alg == kNoEncryption) {
            rekeyResult = sqlite3_rekey_v2(_sqlDb->getHandle(), nullptr, nullptr, 0);
        } else {
            rekeyResult = sqlite3_rekey_v2(_sqlDb->getHandle(), nullptr, newKey.buf, (int)newKey.size);
        }
        
        if(rekeyResult != SQLITE_OK) {
            error::_throw(litecore::error::SQLite, rekeyResult);
        }

        // Update encryption key:
        auto opts = options();
        opts.encryptionAlgorithm = alg;
        opts.encryptionKey = newKey;
        setOptions(opts);

        // Finally reopen:
        reopen();
#else
        error::_throw(litecore::error::UnsupportedEncryption);
#endif
    }


    KeyStore* SQLiteDataFile::newKeyStore(const string &name, KeyStore::Capabilities options) {
        Assert(name != kDeletedKeyStoreName); // can't access this store directly
        auto keyStore = new SQLiteKeyStore(*this, name, options);
        if (name == kDefaultKeyStoreName && _schemaVersion >= SchemaVersion::WithDeletedTable) {
            // Wrap the default store in a BothKeyStore that manages it and the deleted store
            return new BothKeyStore(keyStore, new SQLiteKeyStore(*this, kDeletedKeyStoreName, options));
        } else {
            return keyStore;
        }
    }

#if ENABLE_DELETE_KEY_STORES
    void SQLiteDataFile::deleteKeyStore(const string &name) {
        execWithLock(string("DROP TABLE IF EXISTS kv_") + name);
    }
#endif

    void SQLiteDataFile::_beginTransaction(Transaction*) {
        checkOpen();
        _exec("BEGIN");
    }


    void SQLiteDataFile::_endTransaction(Transaction *t, bool commit) {
        exec(commit ? "COMMIT" : "ROLLBACK");
    }


    void SQLiteDataFile::beginReadOnlyTransaction() {
        checkOpen();
        _exec("SAVEPOINT roTransaction");
    }

    void SQLiteDataFile::endReadOnlyTransaction() {
        _exec("RELEASE SAVEPOINT roTransaction");
    }


    int SQLiteDataFile::_exec(const string &sql) {
        LogTo(SQL, "%s", sql.c_str());
        return _sqlDb->exec(sql);
    }

    int SQLiteDataFile::exec(const string &sql) {
        Assert(inTransaction());
        return _exec(sql);
    }

    int SQLiteDataFile::execWithLock(const string &sql) {
        checkOpen();
        int result;
        withFileLock([&]{
            result = _exec(sql);
        });
        return result;
    }


    int64_t SQLiteDataFile::intQuery(const char *query) {
        SQLite::Statement st(*_sqlDb, query);
        LogStatement(st);
        return st.executeStep() ? st.getColumn(0) : 0;
    }


    SQLite::Statement& SQLiteDataFile::compile(const unique_ptr<SQLite::Statement>& ref,
                                               const char *sql) const
    {
        checkOpen();
        if (ref == nullptr) {
            try {
                const_cast<unique_ptr<SQLite::Statement>&>(ref)
                                            = make_unique<SQLite::Statement>(*_sqlDb, sql, true);
            } catch (const SQLite::Exception &x) {
                warn("SQLite error compiling statement \"%s\": %s", sql, x.what());
                throw;
            }
        }
        return *ref.get();
    }


    bool SQLiteDataFile::getSchema(const string &name,
                                   const string &type,
                                   const string &tableName,
                                   string &outSQL) const
    {
        SQLite::Statement check(*_sqlDb, "SELECT sql FROM sqlite_master "
                                         "WHERE name = ? AND type = ? AND tbl_name = ?");
        check.bind(1, name);
        check.bind(2, type);
        check.bind(3, tableName);
        LogStatement(check);
        if (!check.executeStep())
            return false;
        outSQL = check.getColumn(0).getString();
        return true;
    }


    bool SQLiteDataFile::tableExists(const string &name) const {
        string sql;
        return getSchema(name, "table", name, sql);
    }


    // Returns true if an index/table exists in the database with the given type and SQL schema OR
    // Returns true if the given sql is empty and the schema doesn't exist.
    bool SQLiteDataFile::schemaExistsWithSQL(const string &name, const string &type,
                                             const string &tableName, const string &sql) {
        string existingSQL;
        bool existed = getSchema(name, type, tableName, existingSQL);
        if (sql != "")
            return existed && existingSQL == sql;
        else
            return !existed;
    }

    
    sequence_t SQLiteDataFile::lastSequence(const string& keyStoreName) const {
        sequence_t seq = 0;
        compile(_getLastSeqStmt, "SELECT lastSeq FROM kvmeta WHERE name=?");
        UsingStatement u(_getLastSeqStmt);
        _getLastSeqStmt->bindNoCopy(1, keyStoreName);
        if (_getLastSeqStmt->executeStep())
            seq = (int64_t)_getLastSeqStmt->getColumn(0);
        return seq;
    }

    void SQLiteDataFile::setLastSequence(SQLiteKeyStore &store, sequence_t seq) {
        compile(_setLastSeqStmt,
                "INSERT INTO kvmeta (name, lastSeq) VALUES (?, ?) "
                "ON CONFLICT (name) "
                "DO UPDATE SET lastSeq = excluded.lastSeq");
        UsingStatement u(_setLastSeqStmt);
        _setLastSeqStmt->bindNoCopy(1, store.name());
        _setLastSeqStmt->bind(2, (long long)seq);
        _setLastSeqStmt->exec();
    }


    uint64_t SQLiteDataFile::purgeCount(const std::string& keyStoreName) const {
        uint64_t purgeCnt = 0;
        if (_schemaVersion >= SchemaVersion::WithPurgeCount) {
            compile(_getPurgeCntStmt, "SELECT purgeCnt FROM kvmeta WHERE name=?");
            UsingStatement u(_getPurgeCntStmt);
            _getPurgeCntStmt->bindNoCopy(1, keyStoreName);
            if(_getPurgeCntStmt->executeStep()) {
                purgeCnt = (int64_t)_getPurgeCntStmt->getColumn(0);
            }
        }
        return purgeCnt;
    }

    void SQLiteDataFile::setPurgeCount(SQLiteKeyStore& store, uint64_t count) {
        Assert(_schemaVersion >= SchemaVersion::WithPurgeCount);
        compile(_setPurgeCntStmt,
            "INSERT INTO kvmeta (name, purgeCnt) VALUES (?, ?) "
            "ON CONFLICT (name) "
            "DO UPDATE SET purgeCnt = excluded.purgeCnt");
        UsingStatement u(_setPurgeCntStmt);
        _setPurgeCntStmt->bindNoCopy(1, store.name());
        _setPurgeCntStmt->bind(2, (long long)count);
        _setPurgeCntStmt->exec();
    }


    uint64_t SQLiteDataFile::fileSize() {
        // Move all WAL changes into the main database file, so its size is accurate:
        _exec("PRAGMA wal_checkpoint(FULL)");
        return DataFile::fileSize();
    }


    void SQLiteDataFile::optimize() {
        // <https://sqlite.org/pragma.html#pragma_optimize>
        try {
            bool logged = false;
            if (SQL.willLog(LogLevel::Verbose)) {
                // Log the details of what the optimize will do, before actually doing it:
                SQLite::Statement stmt(*_sqlDb, "PRAGMA optimize(3)");
                while (stmt.executeStep()) {
                    LogVerbose(SQL, "PRAGMA optimize ... %s", stmt.getColumn(0).getString().c_str());
                    logged = true;
                }
            }
            if (!logged)
                LogVerbose(SQL, "PRAGMA optimize");
            _sqlDb->exec("PRAGMA optimize");
        } catch (const SQLite::Exception &x) {
            warn("Caught SQLite exception while optimizing: %s", x.what());
        }
    }


    void SQLiteDataFile::vacuum(bool always) {
        // <https://blogs.gnome.org/jnelson/2015/01/06/sqlite-vacuum-and-auto_vacuum/>
        try {
            int64_t pageCount = intQuery("PRAGMA page_count");
            int64_t freePages = intQuery("PRAGMA freelist_count");
            logVerbose("Housekeeping: %lld of %lld pages free (%.0f%%)",
                       (long long)freePages, (long long)pageCount,
                       100.0 * freePages / pageCount);

            if (!always && (pageCount == 0 || (float)freePages / pageCount < kVacuumFractionThreshold)
                        && (freePages * kPageSize < kVacuumSizeThreshold))
                return;

            string sql;
            bool fixAutoVacuum = (always || (pageCount * kPageSize) < 10*MB)
                                    && (intQuery("PRAGMA auto_vacuum") == 0);
            if (fixAutoVacuum) {
                // Due to issue CBL-707, auto-vacuum did not take effect when creating databases.
                // To enable auto-vacuum on an already-created db, you have to first invoke the
                // pragma and then run a full VACUUM.
                logInfo("Running one-time full VACUUM ... this may take a while [CBL-707]");
                sql = "PRAGMA auto_vacuum=incremental; VACUUM";
            } else {
                logInfo("Incremental-vacuuming database...");
                sql = "PRAGMA incremental_vacuum";
            }

            // On explicit compact, truncate the WAL file to save disk space:
            if (always)
                sql += "; PRAGMA wal_checkpoint(TRUNCATE)";

            fleece::Stopwatch st;
            _exec(sql);
            auto elapsed = st.elapsed();

            int64_t shrunk = pageCount - intQuery("PRAGMA page_count");
            logInfo("    ...removed %lld pages (%lldKB) in %.3f sec",
                    shrunk, shrunk * kPageSize / 1024, elapsed);

            if (fixAutoVacuum && intQuery("PRAGMA auto_vacuum") == 0)
                warn("auto_vacuum mode did not take effect after running full VACUUM!");
        } catch (const SQLite::Exception &x) {
            warn("Caught SQLite exception while vacuuming: %s", x.what());
        }
    }


    void SQLiteDataFile::integrityCheck() {
        fleece::Stopwatch st;
        _exec("PRAGMA integrity_check");
        SQLite::Statement stmt(*_sqlDb, "PRAGMA integrity_check");
        stringstream errors;
        while (stmt.executeStep()) {
            if (string row = stmt.getColumn(0).getString(); row != "ok") {
                errors << "\n" << row;
                warn("Integrity check: %s", row.c_str());
            }
        }
        auto elapsed = st.elapsed();
        logInfo("Integrity check took %.3f sec", elapsed);

        if (string error = errors.str(); !error.empty())
            error::_throw(error::CorruptData, "Database integrity check failed (details below)%s",
                          error.c_str());
    }


    void SQLiteDataFile::maintenance(MaintenanceType what) {
        switch (what) {
            case kCompact:
                checkOpen();
                optimize();
                vacuum(true);
                break;
            case kReindex:
                execWithLock("REINDEX");
                break;
            case kIntegrityCheck:
                integrityCheck();
                break;
            default:
                error::_throw(error::UnsupportedOperation);
        }
    }


    alloc_slice SQLiteDataFile::rawQuery(const string &query) {
        SQLite::Statement stmt(*_sqlDb, query);
        int nCols = stmt.getColumnCount();
        fleece::impl::Encoder enc;
        enc.beginArray();
        while (stmt.executeStep()) {
            enc.beginArray();
            for (int i = 0; i < nCols; ++i) {
                SQLite::Column col = stmt.getColumn(i);
                switch (col.getType()) {
                    case SQLITE_NULL:   enc.writeNull(); break;
                    case SQLITE_INTEGER:enc.writeInt(col.getInt64()); break;
                    case SQLITE_FLOAT:  enc.writeDouble(col.getDouble()); break;
                    case SQLITE_TEXT:   enc.writeString(col.getString()); break;
                    case SQLITE_BLOB:   enc.writeData(slice(col.getBlob(), col.getBytes())); break;
                }
            }
            enc.endArray();
        }
        enc.endArray();
        return enc.finish();
    }

}
