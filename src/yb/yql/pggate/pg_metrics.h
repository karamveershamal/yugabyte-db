//--------------------------------------------------------------------------------------------------
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
//--------------------------------------------------------------------------------------------------

#pragma once

#include "yb/yql/pggate/ybc_pg_typedefs.h"

namespace yb {

namespace pggate {

// For each collected stat/metric, this function subtracts the metric from 'from' by 'amount'
// and stores the result in 'result'.
void SubtractExecStats(YBCPgExecStats *from, YBCPgExecStats *amount, YBCPgExecStats *result);

// Updates the metrics snapshot with the collected stats/metrics, in accordance to the phase of
// query execution.
void RefreshExecStats(YBCPgExecStats *current, YBCPgExecStats *snapshot,
                      YBCQueryExecutionPhase phase);

} // namespace pggate

} // namespace yb
