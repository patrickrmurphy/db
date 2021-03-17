/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/catalog_test_fixture.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/timeseries/bucket_catalog.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace {
class BucketCatalogTest : public CatalogTestFixture {
protected:
    class Task {
        stdx::packaged_task<void()> _task;
        stdx::future<void> _future;
        stdx::thread _taskThread;
        AtomicWord<bool> _running{false};

    public:
        Task(std::function<void()>&& fn);
        ~Task();

        const stdx::future<void>& future();
    };

    void setUp() override;
    virtual BSONObj _makeTimeseriesOptionsForCreate() const;

    void _commit(const std::shared_ptr<BucketCatalog::WriteBatch>& batch,
                 uint16_t numPreviouslyCommittedMeasurements);
    void _insertOneAndCommit(const NamespaceString& ns,
                             uint16_t numPreviouslyCommittedMeasurements);

    long long _getNumWaits(const NamespaceString& ns);

    OperationContext* _opCtx;
    BucketCatalog* _bucketCatalog;

    StringData _timeField = "time";
    StringData _metaField = "meta";

    NamespaceString _ns1{"bucket_catalog_test_1", "t_1"};
    NamespaceString _ns2{"bucket_catalog_test_1", "t_2"};
    NamespaceString _ns3{"bucket_catalog_test_2", "t_1"};

    BucketCatalog::CommitInfo _commitInfo{StatusWith<SingleWriteResult>(SingleWriteResult{})};
};

class BucketCatalogWithoutMetadataTest : public BucketCatalogTest {
protected:
    BSONObj _makeTimeseriesOptionsForCreate() const override;
};

BucketCatalogTest::Task::Task(std::function<void()>&& fn)
    : _task{[this, fn = std::move(fn)]() {
          _running.store(true);
          fn();
      }},
      _future{_task.get_future()},
      _taskThread{std::move(_task)} {
    while (!_running.load()) {
        stdx::this_thread::yield();
    }
}
BucketCatalogTest::Task::~Task() {
    _taskThread.join();
}

const stdx::future<void>& BucketCatalogTest::Task::future() {
    return _future;
}

void BucketCatalogTest::setUp() {
    CatalogTestFixture::setUp();

    _opCtx = operationContext();
    _bucketCatalog = &BucketCatalog::get(_opCtx);

    for (const auto& ns : {_ns1, _ns2, _ns3}) {
        ASSERT_OK(createCollection(
            _opCtx,
            ns.db().toString(),
            BSON("create" << ns.coll() << "timeseries" << _makeTimeseriesOptionsForCreate())));
    }
}

BSONObj BucketCatalogTest::_makeTimeseriesOptionsForCreate() const {
    return BSON("timeField" << _timeField << "metaField" << _metaField);
}

BSONObj BucketCatalogWithoutMetadataTest::_makeTimeseriesOptionsForCreate() const {
    return BSON("timeField" << _timeField);
}

void BucketCatalogTest::_commit(const std::shared_ptr<BucketCatalog::WriteBatch>& batch,
                                uint16_t numPreviouslyCommittedMeasurements) {
    ASSERT(batch->claimCommitRights());
    _bucketCatalog->prepareCommit(batch);
    ASSERT_EQ(batch->measurements().size(), 1);
    ASSERT_EQ(batch->numPreviouslyCommittedMeasurements(), numPreviouslyCommittedMeasurements);

    _bucketCatalog->finish(batch, _commitInfo);
}

void BucketCatalogTest::_insertOneAndCommit(const NamespaceString& ns,
                                            uint16_t numPreviouslyCommittedMeasurements) {
    auto result = _bucketCatalog->insert(_opCtx, ns, BSON(_timeField << Date_t::now()));
    auto& batch = result.getValue();
    _commit(batch, numPreviouslyCommittedMeasurements);
}

long long BucketCatalogTest::_getNumWaits(const NamespaceString& ns) {
    BSONObjBuilder builder;
    _bucketCatalog->appendExecutionStats(ns, &builder);
    return builder.obj().getIntField("numWaits");
}

TEST_F(BucketCatalogTest, InsertIntoSameBucket) {
    // The first insert should be able to take commit rights, but batch is still active
    auto result1 = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now()));
    auto batch1 = result1.getValue();
    ASSERT(batch1->claimCommitRights());
    ASSERT(batch1->active());

    // A subsequent insert into the same bucket should land in the same batch, but not be able to
    // claim commit rights
    auto result2 = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now()));
    auto batch2 = result2.getValue();
    ASSERT_EQ(batch1, batch2);
    ASSERT(!batch2->claimCommitRights());

    // The batch hasn't actually been committed yet.
    ASSERT(!batch1->finished());

    _bucketCatalog->prepareCommit(batch1);

    // Still not finished, but no longer active.
    ASSERT(!batch1->finished());
    ASSERT(!batch1->active());

    // The batch should contain both documents since they belong in the same bucket and happened
    // in the same commit epoch. Nothing else has been committed in this bucket yet.
    ASSERT_EQ(batch1->measurements().size(), 2);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements(), 0);

    // Once the commit has occurred, the waiter should be notified.
    _bucketCatalog->finish(batch1, _commitInfo);
    ASSERT(batch2->finished());
    auto result3 = batch2->getResult();
    ASSERT_OK(result3.getStatus());
}

TEST_F(BucketCatalogTest, GetMetadataReturnsEmptyDocOnMissingBucket) {
    auto batch = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now())).getValue();
    _bucketCatalog->clear(batch->bucket(), nullptr);
    ASSERT_BSONOBJ_EQ(BSONObj(), _bucketCatalog->getMetadata(batch->bucketId()));
}

TEST_F(BucketCatalogTest, InsertIntoDifferentBuckets) {
    auto result1 = _bucketCatalog->insert(
        _opCtx, _ns1, BSON(_timeField << Date_t::now() << _metaField << "123"));
    auto result2 = _bucketCatalog->insert(
        _opCtx, _ns1, BSON(_timeField << Date_t::now() << _metaField << BSONObj()));
    auto result3 = _bucketCatalog->insert(_opCtx, _ns2, BSON(_timeField << Date_t::now()));

    // Inserts should all be into three distinct buckets (and therefore batches).
    ASSERT_NE(result1.getValue(), result2.getValue());
    ASSERT_NE(result1.getValue(), result3.getValue());
    ASSERT_NE(result2.getValue(), result3.getValue());

    // Check metadata in buckets.
    ASSERT_BSONOBJ_EQ(BSON(_metaField << "123"),
                      _bucketCatalog->getMetadata(result1.getValue()->bucketId()));
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONObj()),
                      _bucketCatalog->getMetadata(result2.getValue()->bucketId()));
    ASSERT_BSONOBJ_EQ(BSON(_metaField << BSONNULL),
                      _bucketCatalog->getMetadata(result3.getValue()->bucketId()));

    // Committing one bucket should only return the one document in that bucket and shoukd not
    // affect the other bucket.
    for (const auto& batch : {result1.getValue(), result2.getValue(), result3.getValue()}) {
        _commit(batch, 0);
    }
}

TEST_F(BucketCatalogTest, NumCommittedMeasurementsAccumulates) {
    // The numCommittedMeasurements returned when committing should accumulate as more entries in
    // the bucket are committed.
    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns1, 1);
}

TEST_F(BucketCatalogTest, ClearNamespaceBuckets) {
    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 0);

    _bucketCatalog->clear(_ns1);

    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 1);
}

TEST_F(BucketCatalogTest, ClearDatabaseBuckets) {
    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 0);
    _insertOneAndCommit(_ns3, 0);

    _bucketCatalog->clear(_ns1.db());

    _insertOneAndCommit(_ns1, 0);
    _insertOneAndCommit(_ns2, 0);
    _insertOneAndCommit(_ns3, 1);
}

TEST_F(BucketCatalogTest, InsertBetweenPrepareAndFinish) {
    auto batch1 =
        _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now())).getValue();
    ASSERT(batch1->claimCommitRights());
    _bucketCatalog->prepareCommit(batch1);
    ASSERT_EQ(batch1->measurements().size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements(), 0);

    // Insert before finish so there's a second batch live at the same time.
    auto batch2 =
        _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now())).getValue();
    ASSERT_NE(batch1, batch2);

    _bucketCatalog->finish(batch1, _commitInfo);
    ASSERT(batch1->finished());

    // Verify the second batch still commits one doc, and that the first batch only commited one.
    _commit(batch2, 1);
}

DEATH_TEST_F(BucketCatalogTest, CannotCommitWithoutRights, "invariant") {
    auto result = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now()));
    auto& batch = result.getValue();
    _bucketCatalog->prepareCommit(batch);
}

DEATH_TEST_F(BucketCatalogTest, CannotFinishUnpreparedBatch, "invariant") {
    auto result = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now()));
    auto& batch = result.getValue();
    ASSERT(batch->claimCommitRights());
    _bucketCatalog->finish(batch, _commitInfo);
}

TEST_F(BucketCatalogWithoutMetadataTest, GetMetadataReturnsEmptyDoc) {
    auto batch = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now())).getValue();

    ASSERT_BSONOBJ_EQ(BSONObj(), _bucketCatalog->getMetadata(batch->bucketId()));

    _commit(batch, 0);
}

TEST_F(BucketCatalogWithoutMetadataTest, CommitReturnsNewFields) {
    // Creating a new bucket should return all fields from the initial measurement.
    auto result =
        _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now() << "a" << 0));
    ASSERT_OK(result);
    auto batch = result.getValue();
    _commit(batch, 0);
    ASSERT_EQ(2U, batch->newFieldNamesToBeInserted().size()) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted().count(_timeField)) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted().count("a")) << batch->toBSON();

    // Inserting a new measurement with the same fields should return an empty set of new fields.

    result = _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now() << "a" << 1));
    ASSERT_OK(result);
    batch = result.getValue();
    _commit(batch, 1);
    ASSERT_EQ(0U, batch->newFieldNamesToBeInserted().size()) << batch->toBSON();

    // Insert a new measurement with the a new field.
    result = _bucketCatalog->insert(
        _opCtx, _ns1, BSON(_timeField << Date_t::now() << "a" << 2 << "b" << 2));
    ASSERT_OK(result);
    batch = result.getValue();
    _commit(batch, 2);
    ASSERT_EQ(1U, batch->newFieldNamesToBeInserted().size()) << batch->toBSON();
    ASSERT(batch->newFieldNamesToBeInserted().count("b")) << batch->toBSON();

    // Fill up the bucket.
    for (auto i = 3; i < gTimeseriesBucketMaxCount; ++i) {
        result =
            _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now() << "a" << i));
        ASSERT_OK(result);
        batch = result.getValue();
        _commit(batch, i);
        ASSERT_EQ(0U, batch->newFieldNamesToBeInserted().size()) << i << ":" << batch->toBSON();
    }

    // When a bucket overflows, committing to the new overflow bucket should return the fields of
    // the first measurement as new fields.
    auto result2 = _bucketCatalog->insert(
        _opCtx, _ns1, BSON(_timeField << Date_t::now() << "a" << gTimeseriesBucketMaxCount));
    auto& batch2 = result2.getValue();
    ASSERT_NE(*batch->bucketId(), *batch2->bucketId());
    _commit(batch2, 0);
    ASSERT_EQ(2U, batch2->newFieldNamesToBeInserted().size()) << batch2->toBSON();
    ASSERT(batch2->newFieldNamesToBeInserted().count(_timeField)) << batch2->toBSON();
    ASSERT(batch2->newFieldNamesToBeInserted().count("a")) << batch2->toBSON();
}

TEST_F(BucketCatalogTest, ClearBucketWithOutstandingInserts) {
    auto batch1 =
        _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now())).getValue();
    ASSERT(batch1->claimCommitRights());
    _bucketCatalog->prepareCommit(batch1);
    ASSERT_EQ(batch1->measurements().size(), 1);
    ASSERT_EQ(batch1->numPreviouslyCommittedMeasurements(), 0);

    // Insert before finish so there's a second batch live at the same time.
    auto batch2 =
        _bucketCatalog->insert(_opCtx, _ns1, BSON(_timeField << Date_t::now())).getValue();
    ASSERT_NE(batch1, batch2);

    ASSERT_EQ(0, _getNumWaits(_ns1));

    // Clearing the bucket will have to wait for the commit of batch1 to finish, then will proceed
    // to abort batch2.
    auto task = Task{[&]() { _bucketCatalog->clear(batch1->bucket(), nullptr); }};
    // Add a little extra wait to make sure clear actually gets to the blocking point.
    stdx::this_thread::sleep_for(stdx::chrono::milliseconds(10));
    ASSERT(task.future().valid());
    ASSERT(stdx::future_status::timeout == task.future().wait_for(stdx::chrono::microseconds(1)))
        << "clear finished before expected";

    _bucketCatalog->finish(batch1, _commitInfo);
    ASSERT(batch1->finished());

    // Now the clear should be able to continue, and will eventually abort batch2.
    task.future().wait();
    ASSERT_EQ(1, _getNumWaits(_ns1));
    ASSERT(batch2->finished());
    ASSERT_EQ(batch2->getResult().getStatus(), ErrorCodes::TimeseriesBucketCleared);
    ASSERT_EQ(1, _getNumWaits(_ns1));
}

}  // namespace
}  // namespace mongo
