// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#pragma once

#include <boost/function.hpp>

#include "yb/common/common_fwd.h"
#include "yb/common/entity_ids_types.h"

#include "yb/docdb/docdb_fwd.h"
#include "yb/docdb/doc_operation.h"
#include "yb/dockv/intent.h"
#include "yb/docdb/shared_lock_manager.h"
#include "yb/docdb/wait_queue.h"

namespace rocksdb {

class DB;
class Iterator;

}

namespace yb {

class Counter;

namespace docdb {

// Note -- we use boost::function here instead of std::function as it's implementation is better
// suited for small callback instances.
using ResolutionCallback = boost::function<void(const Result<HybridTime>&)>;

// There are 3 conflict management policies supported (assume that the current transaction which is
// attempting to detect conflicts is T0):
//
// 1. Wait-on-Conflict:
//    (a) If T0 tries to write an intent or data that conflicts with data modififed by committed
//        transactions, T0 fails  (i.e., tserver returns kConflict to YSQL)
//    (b) If T0 tries to write an intent or data that conflicts with intents of pending
//        transactions, it will wait for all pending transactions to finish (i.e., commit or abort)
//        and re-run conflict resolution.
//
// 2. Skip-on-Conflict:
//    If T0 tries to write an intent or data that conflicts with data modififed by committed
//    transactions or with intents of pending transactions, the tserver will return the kSkipLocking
//    error to YSQL.
//
// 3. Fail-on-Conflict:
//    (a) In this mode, transactions are assigned random priorities (with some exceptions based on
//        TxnPriorityRequirement).
//    (b) If T0 tries to write an intent or data that conflicts with data modififed by committed
//        transactions, T0 fails (i.e., tserver returns a kConflict error back to YSQL).
//    (c) If T0 tries to write an intent or data that conflicts with intents of pending
//        transactions, there are two possibilities:
//
//        (i) If T0 has a priority higher than all the other conflicting transactions,
//            T0 will abort them and proceed.
//        (ii) Otherwise, T0 fails (i.e., tserver returns kConflict to YSQL)
typedef enum {
  WAIT_ON_CONFLICT,
  SKIP_ON_CONFLICT,
  FAIL_ON_CONFLICT
} ConflictManagementPolicy;

// Resolves conflicts for write batch of transaction.
// Read all intents that could conflict with intents generated by provided write_batch.
// Forms set of conflicting transactions.
// Perform either of the above three conflict management policies as applicable.
//
// write_batch - values that would be written as part of transaction.
// intial_resolution_ht - current hybrid time. Used to request status of conflicting transactions.
// db - db that contains tablet data.
// status_manager - status manager that should be used during this conflict resolution.
// conflicts_metric - transaction_conflicts metric to update.
// lock_batch - a pointer to the lock_batch used by this operation, which will be temporarily
//              unlocked in the event that blocking conflicting transactions are found and
//              waited-on. Only used in conjunction with the wait_queue.
// wait_queue - a pointer to the tablet's wait queue. Required if the Wait-on-Conflict policy is to
//              be used. If Wait-on-Conflict policy is to be used but wait_queue is nullptr, an
//              error will be returned.
Status ResolveTransactionConflicts(const DocOperations& doc_ops,
                                   const ConflictManagementPolicy conflict_management_policy,
                                   const LWKeyValueWriteBatchPB& write_batch,
                                   HybridTime intial_resolution_ht,
                                   HybridTime read_time,
                                   const DocDB& doc_db,
                                   dockv::PartialRangeKeyIntents partial_range_key_intents,
                                   TransactionStatusManager* status_manager,
                                   Counter* conflicts_metric,
                                   LockBatch* lock_batch,
                                   WaitQueue* wait_queue,
                                   ResolutionCallback callback);

// Resolves conflicts for doc operations.
// Read all intents that could conflict with provided doc_ops.
// Forms set of conflicting transactions.
// Tries to abort conflicting transactions.
// If it conflicts with already committed transaction, then returns maximal commit time of such
// transaction. So we could update local clock and apply those operations later than conflicting
// transaction.
//
// doc_ops - doc operations that would be applied as part of operation.
// intial_resolution_ht - current hybrid time. Used to request status of conflicting transactions.
// db - db that contains tablet data.
// status_manager - status manager that should be used during this conflict resolution.
Status ResolveOperationConflicts(const DocOperations& doc_ops,
                                 const ConflictManagementPolicy conflict_management_policy,
                                 HybridTime intial_resolution_ht,
                                 const DocDB& doc_db,
                                 dockv::PartialRangeKeyIntents partial_range_key_intents,
                                 TransactionStatusManager* status_manager,
                                 Counter* conflicts_metric,
                                 LockBatch* lock_batch,
                                 WaitQueue* wait_queue,
                                 ResolutionCallback callback);

struct ParsedIntent {
  // Intent DocPath.
  Slice doc_path;
  dockv::IntentTypeSet types;
  // Intent doc hybrid time.
  Slice doc_ht;
};

// Parses the intent pointed to by intent_iter to a ParsedIntent.
// Intent is encoded as Prefix + DocPath + IntentType + DocHybridTime.
// `transaction_id_source` could be larger that 16 bytes, it is not problem here, because it is
// used for error reporting.
Result<ParsedIntent> ParseIntentKey(Slice intent_key, Slice transaction_id_source);

std::string DebugIntentKeyToString(Slice intent_key);

// Abstarct class to enable fetching table info from parsed intents while populating lock info in
// PopulateLockInfoFromParsedIntent.
class TableInfoProvider {
 public:
  virtual Result<tablet::TableInfoPtr> GetTableInfo(ColocationId colocation_id) const = 0;

  virtual ~TableInfoProvider() = default;
};

// Decodes the doc_path present in the parsed_intent, and adds the lock information to the given
// lock_info pointer. parsed_intent is expected to have a hybrid time by default. If not,
// intent_has_ht needs to be set to false for the function to not return an error status.
Status PopulateLockInfoFromParsedIntent(
    const ParsedIntent& parsed_intent, const dockv::DecodedIntentValue& decoded_value,
    const TableInfoProvider& table_info_provider, LockInfoPB* lock_info,
    bool intent_has_ht = true);

} // namespace docdb
} // namespace yb
