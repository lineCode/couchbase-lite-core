//
// SQLiteKeyStore.hh
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

#pragma once
#include "KeyStore.hh"
#include "QueryParser.hh"
#include "FleeceImpl.hh"
#include <mutex>
#include <atomic>

namespace SQLite {
    class Column;
    class Statement;
}


namespace litecore {   

    class SQLiteDataFile;
    

    /** SQLite implementation of KeyStore; corresponds to a SQL table. */
    class SQLiteKeyStore : public KeyStore, public QueryParser::delegate {
    public:
        using KeyStore::get; // GCC gets confused by the overloaded virtual functions in KeyStore

        uint64_t recordCount() const override;
        sequence_t lastSequence() const override;
        uint64_t purgeCount() const override;

        Record get(sequence_t) const override;
        bool read(Record &rec, ContentOption) const override;

        sequence_t set(slice key, slice meta, slice value, DocumentFlags,
                       Transaction&,
                       const sequence_t *replacingSequence =nullptr,
                       bool newSequence =true) override;

        bool del(slice key, Transaction&, sequence_t s) override;

        bool setDocumentFlag(slice key, sequence_t, DocumentFlags, Transaction&) override;

        void erase() override;

        virtual bool setExpiration(slice key, expiration_t) override;
        virtual expiration_t getExpiration(slice key) override;
        virtual expiration_t nextExpiration() override;
        virtual unsigned expireRecords(ExpirationCallback =nullptr) override;

        bool supportsIndexes(IndexSpec::Type t) const override               {return true;}
        bool createIndex(const IndexSpec&) override;

        void deleteIndex(slice name) override;
        std::vector<IndexSpec> getIndexes() const override;

        virtual std::vector<alloc_slice> withDocBodies(const std::vector<slice> &docIDs,
                                                       WithDocBodyCallback callback) override;

        void createSequenceIndex();
        void createConflictsIndex();
        void createBlobsIndex();

        // QueryParser::delegate:
        virtual std::string tableName() const override  {return std::string("kv_") + name();}
        virtual std::string FTSTableName(const std::string &property) const override;
        virtual std::string unnestedTableName(const std::string &property) const override;
#ifdef COUCHBASE_ENTERPRISE
        virtual std::string predictiveTableName(const std::string &property) const override;
#endif
        virtual bool tableExists(const std::string &tableName) const override;


    protected:
        RecordEnumerator::Impl* newEnumeratorImpl(bool bySequence,
                                                  sequence_t since,
                                                  RecordEnumerator::Options) override;
        Retained<Query> compileQuery(slice expression, QueryLanguage) override;

        SQLite::Statement* compile(const std::string &sql) const;
        SQLite::Statement& compile(const std::unique_ptr<SQLite::Statement>& ref,
                                   const char *sqlTemplate) const;

        void transactionWillEnd(bool commit);

        void close() override;
        void reopen() override;

        static slice columnAsSlice(const SQLite::Column &col);
        static void setRecordMetaAndBody(Record &rec,
                                         SQLite::Statement &stmt,
                                         ContentOption);
        virtual bool mayHaveExpiration() override;

    private:
        friend class SQLiteDataFile;
        friend class SQLiteEnumerator;
        friend class SQLiteQuery;
        
        SQLiteKeyStore(SQLiteDataFile&, const std::string &name, KeyStore::Capabilities options);
        void createTable();
        SQLiteDataFile& db() const                    {return (SQLiteDataFile&)dataFile();}
        std::string subst(const char *sqlTemplate) const;
        void setLastSequence(sequence_t seq);
        void incrementPurgeCount();
        void createTrigger(string_view triggerName,
                           string_view triggerSuffix,
                           string_view operation,
                           std::string when,
                           string_view statements);
        bool createValueIndex(const IndexSpec&);
        bool createIndex(const IndexSpec&,
                              const std::string &sourceTableName,
                              fleece::impl::Array::iterator &expressions);
        void _createFlagsIndex(const char *indexName NONNULL, DocumentFlags flag, bool &created);
        bool createFTSIndex(const IndexSpec&);
        bool createArrayIndex(const IndexSpec&);
        std::string createUnnestedTable(const fleece::impl::Value *arrayPath, const IndexSpec::Options*);
        void addExpiration();

#ifdef COUCHBASE_ENTERPRISE
        bool createPredictiveIndex(const IndexSpec&);
        std::string createPredictionTable(const fleece::impl::Value *arrayPath, const IndexSpec::Options*);
        void garbageCollectPredictiveIndexes();
#endif

        // All of these Statement pointers have to be reset in the close() method.
        std::unique_ptr<SQLite::Statement> _recCountStmt;
        std::unique_ptr<SQLite::Statement> _getByKeyStmt, _getCurByKeyStmt, _getMetaByKeyStmt;
        std::unique_ptr<SQLite::Statement> _getBySeqStmt, _getCurBySeqStmt, _getMetaBySeqStmt;
        std::unique_ptr<SQLite::Statement> _setStmt, _insertStmt, _replaceStmt, _updateBodyStmt;
        std::unique_ptr<SQLite::Statement> _delByKeyStmt, _delBySeqStmt, _delByBothStmt;
        std::unique_ptr<SQLite::Statement> _setFlagStmt, _withDocBodiesStmt;
        std::unique_ptr<SQLite::Statement> _setExpStmt, _getExpStmt, _nextExpStmt, _findExpStmt;

        enum Existence : uint8_t { kNonexistent, kUncommitted, kCommitted };

        bool _createdSeqIndex {false}, _createdConflictsIndex {false}, _createdBlobsIndex {false};
        bool _lastSequenceChanged {false};
        bool _purgeCountChanged {false};
        mutable bool _purgeCountValid {false};      // TODO: Use optional class from C++17
        mutable int64_t _lastSequence {-1};
        mutable std::atomic<uint64_t> _purgeCount {0};
        bool _hasExpirationColumn {false};
        bool _uncommittedExpirationColumn {false};
        mutable std::mutex _stmtMutex;
        Existence _existence;
    };

}
