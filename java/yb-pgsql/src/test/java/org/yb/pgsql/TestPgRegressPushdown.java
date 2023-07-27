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
package org.yb.pgsql;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.yb.YBTestRunner;

/**
 * Runs the pg_regress test suite on YB code.
 */
@RunWith(value=YBTestRunner.class)
public class TestPgRegressPushdown extends BasePgSQLTest {
  @Override
  public int getTestMethodTimeoutSec() {
    return 1800;
  }

  /**
   * Test meaning has been reversed as of GHI #13541
   * Expression pushdown is on now by default, so test the cases when expression pushdown is off
   * @throws Exception
   */
  @Test
  public void testPgRegressPushdown() throws Exception {
    runPgRegressTest("yb_pg_pushdown_serial_schedule");
  }
}