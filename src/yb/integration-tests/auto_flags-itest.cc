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

#include "yb/common/wire_protocol.h"

#include "yb/consensus/log.h"
#include "yb/consensus/raft_consensus.h"

#include "yb/integration-tests/external_mini_cluster-itest-base.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/ts_itest-base.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"

#include "yb/master/catalog_manager.h"
#include "yb/master/mini_master.h"
#include "yb/master/master.h"

#include "yb/server/server_base.proxy.h"

#include "yb/tablet/tablet_peer.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/tablet_server.h"

#include "yb/util/flags.h"
#include "yb/util/backoff_waiter.h"

DECLARE_bool(TEST_auto_flags_initialized);
DECLARE_bool(TEST_auto_flags_new_install);
DECLARE_bool(disable_auto_flags_management);
DECLARE_int32(limit_auto_flag_promote_for_new_universe);
DECLARE_int32(heartbeat_interval_ms);
DECLARE_bool(TEST_disable_versioned_auto_flags);
DECLARE_bool(enable_tablet_split_of_xcluster_replicated_tables);
DECLARE_bool(ysql_yb_pushdown_strict_inequality);

// Required for tests with AutoFlags management disabled
DISABLE_PROMOTE_ALL_AUTO_FLAGS_FOR_TEST;

using std::string;
using namespace std::chrono_literals;

namespace yb {

using OK = Status::OK;

const string kDisableAutoFlagsManagementFlagName = "disable_auto_flags_management";
const string kTESTAutoFlagsInitializedFlagName = "TEST_auto_flags_initialized";
const string kTESTAutoFlagsNewInstallFlagName = "TEST_auto_flags_new_install";
const string kTrue = "true";
const string kFalse = "false";
const MonoDelta kTimeout = 20s * kTimeMultiplier;
const int kNumMasterServers = 3;
const int kNumTServers = 3;

namespace {

// Count the number of times a given flag shows up in the config. Some flags are available in both
// yb-master and yb-tserver causing them to show up twice. This scans all flags to catch bugs that
// can cause duplicate entries as well.
size_t CountFlagsInConfig(
    const std::string& flag_name, const AutoFlagsConfigPB& config,
    std::optional<uint32> promoted_version = std::nullopt) {
  size_t count = 0;
  for (const auto& per_process_flags : config.promoted_flags()) {
    for (int i = 0; i < per_process_flags.flags_size(); i++) {
      if (per_process_flags.flags(i) == flag_name) {
        if (!promoted_version) {
          count++;
        } else if (
            !per_process_flags.flag_infos().empty() &&
            per_process_flags.flag_infos(i).promoted_version() == *promoted_version) {
          count++;
        }
        // No break or continue here since we want to catch duplicates if any.
      }
    }
  }
  return count;
}

uint32 CountPromotedFlags(const AutoFlagsConfigPB& config) {
  uint32 count = 0;
  for (const auto& per_process_flags : config.promoted_flags()) {
    count += per_process_flags.flags_size();
  }
  return count;
}

Status TestPromote(
    std::function<Result<AutoFlagsConfigPB>()> get_current_config,
    std::function<Result<master::PromoteAutoFlagsResponsePB>(master::PromoteAutoFlagsRequestPB)>
        promote_auto_flags,
    std::function<Status(uint32)> validate_config_on_all_nodes) {
  // Initial empty config.
  auto previous_config = VERIFY_RESULT(get_current_config());
  SCHECK_EQ(0, previous_config.config_version(), IllegalState, "Invalid config version");
  SCHECK_EQ(0, previous_config.promoted_flags_size(), IllegalState, "Invalid promoted flags");

  master::PromoteAutoFlagsRequestPB req;
  req.set_max_flag_class(ToString(AutoFlagClass::kExternal));
  req.set_promote_non_runtime_flags(true);
  req.set_force(false);

  // Promote all AutoFlags.
  auto resp = VERIFY_RESULT(promote_auto_flags(req));
  SCHECK(resp.has_new_config_version(), IllegalState, "Invalid new config version");
  SCHECK_EQ(
      resp.new_config_version(), previous_config.config_version() + 1, IllegalState,
      "Invalid new config version");
  SCHECK(resp.flags_promoted(), IllegalState, "Invalid flags promoted");

  RETURN_NOT_OK(validate_config_on_all_nodes(resp.new_config_version()));
  previous_config = VERIFY_RESULT(get_current_config());
  SCHECK_EQ(
      resp.new_config_version(), previous_config.config_version(), IllegalState,
      "Invalid config version");
  SCHECK_GE(previous_config.promoted_flags_size(), 1, IllegalState, "Invalid promoted flags");
  SCHECK_EQ(
      0, CountFlagsInConfig(kTESTAutoFlagsNewInstallFlagName, previous_config), IllegalState,
      "Invalid promoted flags");
  SCHECK_EQ(
      previous_config.promoted_flags_size(),
      CountFlagsInConfig(kTESTAutoFlagsInitializedFlagName, previous_config), IllegalState,
      "Invalid promoted flags");
  auto count_promoted_flags = CountPromotedFlags(previous_config);

  // Running again should be no op.
  resp = VERIFY_RESULT(promote_auto_flags(req));
  SCHECK(!resp.flags_promoted(), IllegalState, "Invalid flags promoted");

  // Force to bump up version alone, make sure flags still have expected values.
  req.set_force(true);
  resp.Clear();
  resp = VERIFY_RESULT(promote_auto_flags(req));
  SCHECK(resp.flags_promoted(), IllegalState, "Invalid flags promoted");
  SCHECK(!resp.non_runtime_flags_promoted(), IllegalState, "Invalid non runtime flags promoted");
  RETURN_NOT_OK(validate_config_on_all_nodes(resp.new_config_version()));
  previous_config = VERIFY_RESULT(get_current_config());
  SCHECK_EQ(
      resp.new_config_version(), previous_config.config_version(), IllegalState,
      "Invalid config version");
  SCHECK_EQ(
      0, CountFlagsInConfig(kTESTAutoFlagsNewInstallFlagName, previous_config), IllegalState,
      "Invalid promoted flags");
  SCHECK_EQ(
      previous_config.promoted_flags_size(),
      CountFlagsInConfig(kTESTAutoFlagsInitializedFlagName, previous_config), IllegalState,
      "Invalid promoted flags");
  SCHECK_EQ(
      count_promoted_flags, CountPromotedFlags(previous_config), IllegalState,
      "No new flags should be promoted");

  // Verify it is not possible to promote with AutoFlagClass::kNewInstallsOnly.
  req.set_max_flag_class(ToString(AutoFlagClass::kNewInstallsOnly));
  auto result = promote_auto_flags(req);
  SCHECK(!result.ok(), IllegalState, "Invalid result");
  SCHECK(
      result.status().IsInvalidArgument(), IllegalState, "Invalid outcome ",
      result.status().ToString());

  return Status::OK();
}

}  // namespace

class AutoFlagsMiniClusterTest : public YBMiniClusterTestBase<MiniCluster> {
 protected:
  void SetUp() override {}
  void TestBody() override {}

 public:
  Status RunSetUp() {
    YBMiniClusterTestBase::SetUp();
    MiniClusterOptions opts;
    opts.num_tablet_servers = kNumTServers;
    opts.num_masters = kNumMasterServers;
    cluster_.reset(new MiniCluster(opts));
    return cluster_->Start();
  }

  Status ValidateConfig() {
    auto leader_master = VERIFY_RESULT(cluster_->GetLeaderMiniMaster());
    const AutoFlagsConfigPB leader_config = leader_master->master()->GetAutoFlagsConfig();
    const auto leader_config_process_count =
        static_cast<size_t>(leader_config.promoted_flags().size());

    std::optional<uint32> promoted_version = leader_config.config_version();
    if (FLAGS_TEST_disable_versioned_auto_flags) {
      promoted_version = std::nullopt;
    }

    SCHECK_EQ(
        CountFlagsInConfig(kTESTAutoFlagsInitializedFlagName, leader_config, promoted_version),
        leader_config_process_count, IllegalState, Format("Unable to find $0", promoted_version));
    SCHECK_EQ(
        CountFlagsInConfig(kTESTAutoFlagsNewInstallFlagName, leader_config, promoted_version),
        leader_config_process_count, IllegalState, "Unable to find");

    if (FLAGS_disable_auto_flags_management) {
      SCHECK(
          !FLAGS_TEST_auto_flags_initialized, IllegalState,
          "TEST_auto_flags_initialized should not be set");
      SCHECK(
          !FLAGS_TEST_auto_flags_new_install, IllegalState,
          "TEST_auto_flags_new_install should not be set");
    } else {
      SCHECK(
          FLAGS_TEST_auto_flags_initialized, IllegalState,
          "TEST_auto_flags_initialized should be set");
      SCHECK(
          FLAGS_TEST_auto_flags_new_install, IllegalState,
          "TEST_auto_flags_new_install should be set");
    }

    for (size_t i = 0; i < cluster_->num_masters(); i++) {
      auto master = cluster_->mini_master(i);
      const AutoFlagsConfigPB follower_config = master->master()->GetAutoFlagsConfig();
      SCHECK_EQ(
          follower_config.DebugString(), leader_config.DebugString(), IllegalState,
          Format(
              "Config of master follower $0 does not match leader $1", *master->master(),
              *leader_master->master()));
    }

    RETURN_NOT_OK(cluster_->AddTabletServer());
    RETURN_NOT_OK(cluster_->WaitForTabletServerCount(kNumTServers + 1));

    for (size_t i = 0; i < cluster_->num_tablet_servers(); i++) {
      auto tserver = cluster_->mini_tablet_server(i);
      const AutoFlagsConfigPB tserver_config = tserver->server()->TEST_GetAutoFlagConfig();
      SCHECK_EQ(
          tserver_config.DebugString(), leader_config.DebugString(), IllegalState,
          Format(
              "Config of tserver $0 does not match leader $1", *tserver->server(),
              *leader_master->master()));
    }

    return OK();
  }

  Status WaitForAllLeaderOpsToApply() {
    auto leader_master = VERIFY_RESULT(cluster_->GetLeaderMiniMaster())->master();
    auto leader_peer = leader_master->catalog_manager()->tablet_peer();
    const auto last_op_id = leader_peer->log()->GetLatestEntryOpId();

    return WaitFor(
        [&]() -> Result<bool> {
          auto applied_op_id = VERIFY_RESULT(leader_peer->GetRaftConsensus())->GetAllAppliedOpId();
          return applied_op_id >= last_op_id;
        },
        kTimeout, Format("Wait OpId apply within $0 seconds failed", kTimeout.ToSeconds()));
  }

  Status ValidateConfigOnMasters(uint32_t expected_config_version) {
    RETURN_NOT_OK(WaitForAllLeaderOpsToApply());

    auto* leader_master = VERIFY_RESULT(cluster_->GetLeaderMiniMaster())->master();
    const auto leader_config = leader_master->GetAutoFlagsConfig();
    SCHECK_EQ(
        expected_config_version, leader_config.config_version(), IllegalState,
        Format("Invalid Config version leader master $0", *leader_master));

    for (size_t i = 0; i < cluster_->num_masters(); i++) {
      auto master = cluster_->mini_master(i);
      const auto follower_config = master->master()->GetAutoFlagsConfig();
      SCHECK_EQ(
          follower_config.DebugString(), leader_config.DebugString(), IllegalState,
          Format(
              "Config of master follower $0 does not match leader $1", *master->master(),
              *leader_master));
    }

    return OK();
  }

  Status ValidateConfigOnTservers(uint32_t expected_config_version) {
    auto leader_master = VERIFY_RESULT(cluster_->GetLeaderMiniMaster())->master();
    const auto master_config = leader_master->GetAutoFlagsConfig();
    SCHECK_EQ(
        expected_config_version, master_config.config_version(), IllegalState,
        Format("Invalid Config version leader master $0", *leader_master));
    LOG(INFO) << leader_master->ToString() << " AutoFlag config: " << master_config.DebugString();

    for (size_t i = 0; i < cluster_->num_tablet_servers(); i++) {
      auto tserver = cluster_->mini_tablet_server(i);
      RETURN_NOT_OK(WaitFor(
          [&]() {
            const auto config = tserver->server()->TEST_GetAutoFlagConfig();
            LOG(INFO) << tserver->server()->ToString()
                      << " AutoFlag config: " << config.DebugString();
            return config.DebugString() == master_config.DebugString();
          },
          FLAGS_heartbeat_interval_ms * 3ms * kTimeMultiplier,
          "AutoFlags not propagated to all TServers"));
    }
    return OK();
  }

  Status ValidateConfigOnAllProcesses(uint32_t expected_config_version) {
    RETURN_NOT_OK(ValidateConfigOnMasters(expected_config_version));
    return ValidateConfigOnTservers(expected_config_version);
  }

  auto PromoteFlagsAndValidate(
      uint32 expected_config_version, master::Master* leader_master, AutoFlagClass flag_class) {
    master::PromoteAutoFlagsRequestPB promote_req;
    promote_req.set_max_flag_class(ToString(flag_class));
    promote_req.set_promote_non_runtime_flags(true);
    promote_req.set_force(false);

    master::PromoteAutoFlagsResponsePB promote_resp;
    CHECK_OK(leader_master->catalog_manager_impl()->PromoteAutoFlags(&promote_req, &promote_resp));
    CHECK(!promote_resp.has_error());
    CHECK(promote_resp.has_new_config_version());
    CHECK_EQ(promote_resp.new_config_version(), expected_config_version);
    CHECK(promote_resp.flags_promoted());

    CHECK_OK(ValidateConfigOnAllProcesses(expected_config_version));

    auto config = leader_master->GetAutoFlagsConfig();
    CHECK_EQ(expected_config_version, config.config_version());
    CHECK_GE(CountPromotedFlags(config), 1);
    return config;
  }

  auto RollbackFlagsAndValidate(
      uint32_t rollback_config_version, uint32_t expected_config_version,
      master::Master* leader_master, bool expect_success = true) {
    master::RollbackAutoFlagsRequestPB rollback_req;
    rollback_req.set_rollback_version(rollback_config_version);

    master::RollbackAutoFlagsResponsePB rollback_resp;
    auto s =
        leader_master->catalog_manager_impl()->RollbackAutoFlags(&rollback_req, &rollback_resp);
    auto config = leader_master->GetAutoFlagsConfig();
    if (!expect_success) {
      CHECK(!s.ok());
      return config;
    }

    CHECK_OK(s);
    CHECK(!rollback_resp.has_error());
    CHECK_EQ(rollback_resp.new_config_version(), expected_config_version);
    CHECK(rollback_resp.flags_rolledback());
    CHECK_EQ(expected_config_version, config.config_version());

    CHECK_OK(ValidateConfigOnAllProcesses(expected_config_version));

    return config;
  }

  auto PromoteSingleFlagAndValidate(
      const std::string& process_name, const std::string& flag_name, master::Master* leader_master,
      uint32_t& expected_config_version, bool flag_promoted = true) {
    master::PromoteSingleAutoFlagRequestPB req;
    req.set_process_name(process_name);
    req.set_auto_flag_name(flag_name);
    master::PromoteSingleAutoFlagResponsePB resp;
    CHECK_OK(leader_master->catalog_manager_impl()->PromoteSingleAutoFlag(&req, &resp));
    CHECK(!resp.has_error());
    CHECK(resp.has_new_config_version());
    CHECK_EQ(resp.new_config_version(), expected_config_version);
    CHECK_EQ(resp.flag_promoted(), flag_promoted);

    CHECK_OK(ValidateConfigOnAllProcesses(expected_config_version));

    auto config = leader_master->GetAutoFlagsConfig();
    CHECK_EQ(expected_config_version, config.config_version());
    return config;
  }

  auto DemoteSingleFlagAndValidate(
      const std::string& process_name, const std::string& flag_name, master::Master* leader_master,
      uint32_t& expected_config_version, bool flag_demoted = true, bool expect_success = true) {
    master::DemoteSingleAutoFlagRequestPB req;
    req.set_process_name(process_name);
    req.set_auto_flag_name(flag_name);
    master::DemoteSingleAutoFlagResponsePB resp;
    auto s = leader_master->catalog_manager_impl()->DemoteSingleAutoFlag(&req, &resp);
    auto config = leader_master->GetAutoFlagsConfig();

    if (!expect_success) {
      CHECK(!s.ok());
      return config;
    }

    CHECK_OK(s);
    CHECK(!resp.has_error());
    CHECK(resp.has_new_config_version());
    CHECK_EQ(resp.new_config_version(), expected_config_version);
    CHECK_EQ(resp.flag_demoted(), flag_demoted);

    CHECK_OK(ValidateConfigOnAllProcesses(expected_config_version));

    CHECK_EQ(expected_config_version, config.config_version());
    return config;
  }
};

TEST_F(AutoFlagsMiniClusterTest, NewCluster) {
  ASSERT_OK(RunSetUp());
  ASSERT_OK(ValidateConfig());
}

// Validate AutoFlags in old mode with no AutoFlag management. This is a proxy to test old
// code behavior.
TEST_F(AutoFlagsMiniClusterTest, DisableAutoFlagManagement) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_disable_auto_flags_management) = true;

  ASSERT_OK(RunSetUp());
  ASSERT_OK(ValidateConfig());
}

// Validate AutoFlags in old mode with no per flag versioning works.
// And then enable backfill and make sure it works correctly.
TEST_F(AutoFlagsMiniClusterTest, BackfillFlagInfos) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_disable_versioned_auto_flags) = true;
  ASSERT_OK(RunSetUp());
  ASSERT_OK(ValidateConfig());

  auto leader_master = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->master();
  auto config = leader_master->GetAutoFlagsConfig();

  // yb-master and yb-tserver processes
  ASSERT_EQ(config.promoted_flags_size(), 2);
  // flag_infos should not be set
  ASSERT_EQ(config.promoted_flags(0).flag_infos_size(), 0);
  ASSERT_EQ(config.promoted_flags(1).flag_infos_size(), 0);

  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_disable_versioned_auto_flags) = false;
  uint32 expected_config_version = 2;
  ASSERT_NO_FATALS(
      config = PromoteFlagsAndValidate(
          expected_config_version, leader_master, AutoFlagClass::kLocalVolatile));

  // flag_infos should now be set
  ASSERT_EQ(config.promoted_flags(0).flag_infos_size(), config.promoted_flags(0).flags_size());
  for (const auto& flag_info : config.promoted_flags(0).flag_infos()) {
    ASSERT_EQ(flag_info.promoted_version(), 0);
  }
  ASSERT_EQ(config.promoted_flags(1).flag_infos_size(), config.promoted_flags(1).flags_size());
  for (const auto& flag_info : config.promoted_flags(1).flag_infos()) {
    ASSERT_EQ(flag_info.promoted_version(), 0);
  }
}

TEST_F(AutoFlagsMiniClusterTest, Promote) {
  // Start with an empty config
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_limit_auto_flag_promote_for_new_universe) = 0;
  ASSERT_OK(RunSetUp());
  auto leader_master = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->master();

  ASSERT_OK(TestPromote(
      /* get_current_config */
      [&]() { return leader_master->GetAutoFlagsConfig(); },
      /* promote_auto_flags */
      [&](const auto& req) -> Result<master::PromoteAutoFlagsResponsePB> {
        master::PromoteAutoFlagsResponsePB resp;
        RETURN_NOT_OK(leader_master->catalog_manager_impl()->PromoteAutoFlags(&req, &resp));
        return resp;
      },
      /* validate_config_on_all_nodes */
      [&](uint32_t config_version) { return ValidateConfigOnAllProcesses(config_version); }));
}

TEST_F(AutoFlagsMiniClusterTest, Rollback) {
  // Start with an empty config.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_limit_auto_flag_promote_for_new_universe) = 0;
  ASSERT_OK(RunSetUp());
  auto leader_master = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->master();
  uint32 expected_config_version = 0;
  auto config = leader_master->GetAutoFlagsConfig();
  ASSERT_EQ(expected_config_version, config.config_version());
  ASSERT_EQ(0, CountPromotedFlags(config));

  // Promote kLocalVolatile flags.
  ASSERT_NO_FATALS(PromoteFlagsAndValidate(
      ++expected_config_version, leader_master, AutoFlagClass::kLocalVolatile));

  // Rollback the promoted flags.
  auto rollback_version = expected_config_version - 1;
  ASSERT_NO_FATALS(
      RollbackFlagsAndValidate(rollback_version, ++expected_config_version, leader_master));
  CHECK_EQ(0, CountPromotedFlags(config));

  // Promote kLocalVolatile flags again.
  ASSERT_NO_FATALS(PromoteFlagsAndValidate(
      ++expected_config_version, leader_master, AutoFlagClass::kLocalVolatile));

  // Rollback promoted flags back to initial version 0.
  ASSERT_NO_FATALS(RollbackFlagsAndValidate(0, ++expected_config_version, leader_master));
  CHECK_EQ(0, CountPromotedFlags(config));

  // Promote kExternal AutoFlags.
  ASSERT_NO_FATALS(
      PromoteFlagsAndValidate(++expected_config_version, leader_master, AutoFlagClass::kExternal));

  // Rollback AutoFlags should now fail.
  rollback_version = expected_config_version - 1;
  ASSERT_NO_FATALS(RollbackFlagsAndValidate(
      rollback_version, ++expected_config_version, leader_master, /* expect_success= */ false));
  ASSERT_NO_FATALS(RollbackFlagsAndValidate(
      0, ++expected_config_version, leader_master, /* expect_success= */ false));
}

TEST_F(AutoFlagsMiniClusterTest, Demote) {
  ASSERT_OK(RunSetUp());
  auto leader_master = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->master();
  uint32 expected_config_version = 1;
  const auto kExternalAutoFlagName = "enable_tablet_split_of_xcluster_replicated_tables";
  const auto& kExternalAutoFlag = FLAGS_enable_tablet_split_of_xcluster_replicated_tables;
  const auto kYbMasterProcess = "yb-master";

  // Initial config.
  auto config = leader_master->GetAutoFlagsConfig();
  ASSERT_EQ(expected_config_version, config.config_version());
  // Flag exists with latest promoted_version.
  ASSERT_EQ(CountFlagsInConfig(kExternalAutoFlagName, config, config.config_version()), 1);
  const auto initial_promoted_flags_count = CountPromotedFlags(config);

  // Demote external flag.
  ASSERT_NO_FATALS(
      config = DemoteSingleFlagAndValidate(
          kYbMasterProcess, kExternalAutoFlagName, leader_master, ++expected_config_version));
  // Flag does not exist in any vers
  ASSERT_EQ(CountFlagsInConfig(kExternalAutoFlagName, config), 0);
  ASSERT_EQ(CountPromotedFlags(config), initial_promoted_flags_count - 1);
  ASSERT_FALSE(kExternalAutoFlag);

  // Demote of an un-promoted external flag should be a no-op.
  ASSERT_NO_FATALS(
      config = DemoteSingleFlagAndValidate(
          kYbMasterProcess, kExternalAutoFlagName, leader_master, expected_config_version,
          /* flag_demoted = */ false));
  ASSERT_EQ(CountFlagsInConfig(kExternalAutoFlagName, config), 0);
  ASSERT_EQ(CountPromotedFlags(config), initial_promoted_flags_count - 1);
  ASSERT_FALSE(kExternalAutoFlag);

  // Promote all AutoFlags.
  ASSERT_NO_FATALS(
      config = PromoteFlagsAndValidate(
          ++expected_config_version, leader_master, AutoFlagClass::kExternal));
  ASSERT_EQ(CountFlagsInConfig(kExternalAutoFlagName, config, config.config_version()), 1);
  ASSERT_EQ(CountPromotedFlags(config), initial_promoted_flags_count);
  ASSERT_TRUE(kExternalAutoFlag);
}

TEST_F(AutoFlagsMiniClusterTest, PromoteOneFlag) {
  // Start with an empty config.
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_limit_auto_flag_promote_for_new_universe) = 0;
  ASSERT_OK(RunSetUp());
  const auto kVolatileAutoFlagName = "ysql_yb_pushdown_strict_inequality";
  const auto& kVolatileAutoFlag = FLAGS_ysql_yb_pushdown_strict_inequality;
  const auto kYbTServerProcess = "yb-tserver";

  auto leader_master = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->master();
  uint32 expected_config_version = 0;

  auto config = leader_master->GetAutoFlagsConfig();
  ASSERT_EQ(expected_config_version, config.config_version());
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config, expected_config_version), 0);
  ASSERT_FALSE(kVolatileAutoFlag);

  // Demote and promote a flag so that it has a higher version than the rest.
  ASSERT_NO_FATALS(
      config = PromoteSingleFlagAndValidate(
          kYbTServerProcess, kVolatileAutoFlagName, leader_master, ++expected_config_version));
  // Flag exists in master but not tserver.
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config), 1);
  ASSERT_TRUE(kVolatileAutoFlag);
}

// Demote before the backfill is not allowed. We cannot Demote or Rollback flags before 2.20 upgrade
// has completed (including Promotion of all AutoFlags).
TEST_F(AutoFlagsMiniClusterTest, DemoteFlagBeforeBackfillFlagInfos) {
  ANNOTATE_UNPROTECTED_WRITE(FLAGS_TEST_disable_versioned_auto_flags) = true;
  ASSERT_OK(RunSetUp());
  auto leader_master = ASSERT_RESULT(cluster_->GetLeaderMiniMaster())->master();
  auto config = leader_master->GetAutoFlagsConfig();

  // yb-master and yb-tserver processes
  ASSERT_EQ(config.promoted_flags_size(), 2);
  // flag_infos should not be set
  ASSERT_EQ(config.promoted_flags(0).flag_infos_size(), 0);
  ASSERT_EQ(config.promoted_flags(1).flag_infos_size(), 0);

  const auto kVolatileAutoFlagName = "ysql_yb_pushdown_strict_inequality";
  const auto kYbTServerProcess = "yb-tserver";
  uint32_t expected_config_version = 2;

  // Demote and promote a flag so that it has a higher version than the rest.
  ASSERT_NO_FATALS(
      config = DemoteSingleFlagAndValidate(
          kYbTServerProcess, kVolatileAutoFlagName, leader_master, expected_config_version,
          /* flag_demoted = */ false, /* expect_success = */ false));

  // flag_infos should still not be set
  ASSERT_EQ(config.promoted_flags(0).flag_infos_size(), 0);
  ASSERT_EQ(config.promoted_flags(1).flag_infos_size(), 0);
}

class AutoFlagsExternalMiniClusterTest : public ExternalMiniClusterITestBase {
 public:
  void BuildAndStart(
      const std::vector<string>& extra_ts_flags = std::vector<string>(),
      const std::vector<string>& extra_master_flags = std::vector<string>()) {
    ASSERT_NO_FATALS(
        StartCluster(extra_ts_flags, extra_master_flags, kNumTServers, kNumMasterServers));
  }

  void SetUpCluster(ExternalMiniClusterOptions* opts) override {
    ASSERT_NO_FATALS(ExternalMiniClusterITestBase::SetUpCluster(opts));
    opts_ = *opts;
  }

  Status CheckFlagOnNode(
      const string& flag_name, const string& expected_val, ExternalDaemon* daemon) {
    auto value = VERIFY_RESULT(daemon->GetFlag(flag_name));
    SCHECK_EQ(
        value, expected_val, IllegalState,
        Format("Invalid value for flag $0 in $1", flag_name, daemon->id()));
    return OK();
  }

  Status CheckFlagOnAllNodes(string flag_name, string expected_val) {
    for (auto* daemon : cluster_->daemons()) {
      RETURN_NOT_OK(CheckFlagOnNode(flag_name, expected_val, daemon));
    }
    return OK();
  }

  Result<AutoFlagsConfigPB> GetAutoFlagsConfig() {
    master::GetAutoFlagsConfigRequestPB req;
    master::GetAutoFlagsConfigResponsePB resp;
    rpc::RpcController rpc;
    rpc.set_timeout(kTimeout);
    RETURN_NOT_OK(cluster_->GetLeaderMasterProxy<master::MasterClusterProxy>().GetAutoFlagsConfig(
        req, &resp, &rpc));
    if (resp.has_error()) {
      return StatusFromPB(resp.error().status())
          .CloneAndPrepend(Format("Code $0", resp.error().code()));
    }

    return resp.config();
  }

  uint32_t GetAutoFlagConfigVersion(ExternalDaemon* daemon) {
    server::GetAutoFlagsConfigVersionRequestPB req;
    server::GetAutoFlagsConfigVersionResponsePB resp;
    rpc::RpcController rpc;
    rpc.set_timeout(kTimeout);
    EXPECT_OK(cluster_->GetProxy<server::GenericServiceProxy>(daemon).GetAutoFlagsConfigVersion(
        req, &resp, &rpc));

    return resp.config_version();
  }

  Status ValidateConfigOnAllProcesses(uint32_t config_version) {
    for (auto* daemon : cluster_->daemons()) {
      uint32_t version = 0;
      RETURN_NOT_OK(WaitFor(
          [&]() {
            version = GetAutoFlagConfigVersion(daemon);
            return version == config_version;
          },
          kTimeout, "Config version to be updated"));

      SCHECK_EQ(
          version, config_version, IllegalState,
          Format(
              "Expected config version $0, got $1 on $2 $3", config_version, version, daemon->exe(),
              daemon->id()));
    }

    return OK();
  }

  Result<master::PromoteAutoFlagsResponsePB> PromoteFlags(
      const master::PromoteAutoFlagsRequestPB& req) {
    rpc::RpcController rpc;
    rpc.set_timeout(kTimeout);

    master::PromoteAutoFlagsResponsePB resp;
    RETURN_NOT_OK(cluster_->GetLeaderMasterProxy<master::MasterClusterProxy>().PromoteAutoFlags(
        req, &resp, &rpc));
    if (resp.has_error()) {
      return StatusFromPB(resp.error().status())
          .CloneAndPrepend(Format("Code $0", resp.error().code()));
    }
    return resp;
  }

  auto PromoteFlagsAndValidate(uint32 expected_config_version, AutoFlagClass flag_class) {
    master::PromoteAutoFlagsRequestPB promote_req;
    promote_req.set_max_flag_class(ToString(flag_class));
    promote_req.set_promote_non_runtime_flags(true);
    promote_req.set_force(false);

    auto promote_resp = CHECK_RESULT(PromoteFlags(promote_req));
    CHECK(promote_resp.has_new_config_version());
    CHECK_EQ(promote_resp.new_config_version(), expected_config_version);
    CHECK(promote_resp.flags_promoted());

    CHECK_OK(ValidateConfigOnAllProcesses(expected_config_version));

    auto config = CHECK_RESULT(GetAutoFlagsConfig());
    CHECK_EQ(expected_config_version, config.config_version());
    CHECK_GE(CountPromotedFlags(config), 1);
    return config;
  }

  auto RollbackFlagsAndValidate(
      uint32_t rollback_config_version, uint32_t expected_config_version,
      bool expect_success = true) {
    master::RollbackAutoFlagsRequestPB rollback_req;
    rollback_req.set_rollback_version(rollback_config_version);
    rpc::RpcController rpc;
    rpc.set_timeout(kTimeout);

    master::RollbackAutoFlagsResponsePB rollback_resp;
    auto s = cluster_->GetLeaderMasterProxy<master::MasterClusterProxy>().RollbackAutoFlags(
        rollback_req, &rollback_resp, &rpc);
    auto config = CHECK_RESULT(GetAutoFlagsConfig());
    if (!expect_success) {
      CHECK(!s.ok());
      return config;
    }

    CHECK_OK(s);
    CHECK(!rollback_resp.has_error());
    CHECK_EQ(rollback_resp.new_config_version(), expected_config_version);
    CHECK(rollback_resp.flags_rolledback());
    CHECK_EQ(expected_config_version, config.config_version());

    CHECK_OK(ValidateConfigOnAllProcesses(expected_config_version));

    return config;
  }

  auto DemoteSingleFlagAndValidate(
      const std::string& process_name, const std::string& flag_name,
      uint32_t& expected_config_version, bool flag_demoted = true, bool expect_success = true) {
    master::DemoteSingleAutoFlagRequestPB req;
    req.set_process_name(process_name);
    req.set_auto_flag_name(flag_name);
    master::DemoteSingleAutoFlagResponsePB resp;
    rpc::RpcController rpc;
    rpc.set_timeout(kTimeout);

    auto s = cluster_->GetLeaderMasterProxy<master::MasterClusterProxy>().DemoteSingleAutoFlag(
        req, &resp, &rpc);
    auto config = CHECK_RESULT(GetAutoFlagsConfig());

    if (!expect_success) {
      CHECK(!s.ok());
      return config;
    }

    CHECK_OK(s);
    CHECK(!resp.has_error());
    CHECK(resp.has_new_config_version());
    CHECK_EQ(resp.new_config_version(), expected_config_version);
    CHECK_EQ(resp.flag_demoted(), flag_demoted);

    CHECK_OK(ValidateConfigOnAllProcesses(expected_config_version));

    CHECK_EQ(expected_config_version, config.config_version());
    return config;
  }

 protected:
  ExternalMiniClusterOptions opts_;
};

// Validate AutoFlags in new cluster and make sure it handles process restarts, and addition of
// new nodes.
TEST_F(AutoFlagsExternalMiniClusterTest, NewCluster) {
  ASSERT_NO_FATALS(BuildAndStart());

  ASSERT_OK(CheckFlagOnAllNodes(kTESTAutoFlagsInitializedFlagName, kTrue));
  ASSERT_OK(CheckFlagOnAllNodes(kTESTAutoFlagsNewInstallFlagName, kTrue));

  ExternalMaster* new_master = nullptr;
  cluster_->StartShellMaster(&new_master);

  OpIdPB op_id;
  ASSERT_OK(cluster_->GetLastOpIdForLeader(&op_id));
  ASSERT_OK(cluster_->ChangeConfig(new_master, consensus::ADD_SERVER));
  ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(op_id.index()));

  ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kTrue, new_master));
  ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsNewInstallFlagName, kTrue, new_master));

  ASSERT_OK(cluster_->AddTabletServer());
  ASSERT_OK(cluster_->WaitForTabletServerCount(opts_.num_tablet_servers + 1, kTimeout));

  ASSERT_OK(CheckFlagOnAllNodes(kTESTAutoFlagsInitializedFlagName, kTrue));
  ASSERT_OK(CheckFlagOnAllNodes(kTESTAutoFlagsNewInstallFlagName, kTrue));

  for (auto* master : cluster_->master_daemons()) {
    master->Shutdown();
    ASSERT_OK(master->Restart());
    ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kTrue, master));
    ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsNewInstallFlagName, kTrue, master));
    ASSERT_EQ(GetAutoFlagConfigVersion(master), 1);
  }

  for (auto* tserver : cluster_->tserver_daemons()) {
    tserver->Shutdown();
    ASSERT_OK(tserver->Restart());
    ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kTrue, tserver));
    ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsNewInstallFlagName, kTrue, tserver));
    ASSERT_EQ(GetAutoFlagConfigVersion(tserver), 1);
  }
}

// Create a Cluster with AutoFlags management turned off to simulate a cluster running old code.
// Restart the cluster with AutoFlags management enabled to simulate an upgrade. Make sure nodes
// added to this cluster works as expected. And finally make sure Promotion after the upgrade
// works.
TEST_F(AutoFlagsExternalMiniClusterTest, UpgradeCluster) {
  string disable_auto_flag_management = "--" + kDisableAutoFlagsManagementFlagName;
  ASSERT_NO_FATALS(BuildAndStart(
      {disable_auto_flag_management} /* ts_flags */,
      {disable_auto_flag_management} /* master_flags */));

  ASSERT_OK(CheckFlagOnAllNodes(kDisableAutoFlagsManagementFlagName, kTrue));
  ASSERT_OK(CheckFlagOnAllNodes(kTESTAutoFlagsInitializedFlagName, kFalse));
  ASSERT_OK(CheckFlagOnAllNodes(kTESTAutoFlagsNewInstallFlagName, kFalse));

  // Remove the disable_auto_flag_management flag from cluster config
  ASSERT_TRUE(Erase(disable_auto_flag_management, cluster_->mutable_extra_master_flags()));
  ASSERT_TRUE(Erase(disable_auto_flag_management, cluster_->mutable_extra_tserver_flags()));

  ASSERT_OK(cluster_->AddTabletServer());
  ASSERT_OK(cluster_->WaitForTabletServerCount(opts_.num_tablet_servers + 1, kTimeout));

  // Add a new tserver
  auto* new_tserver = cluster_->tablet_server(cluster_->num_tablet_servers() - 1);
  ASSERT_OK(CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, new_tserver));
  ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kFalse, new_tserver));
  ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsNewInstallFlagName, kFalse, new_tserver));
  ASSERT_EQ(GetAutoFlagConfigVersion(new_tserver), 0);

  // Restart the new tserver
  new_tserver->Shutdown();
  ASSERT_OK(cluster_->WaitForTabletServerCount(opts_.num_tablet_servers, kTimeout));
  ASSERT_OK(new_tserver->Restart());
  ASSERT_OK(cluster_->WaitForTabletServerCount(opts_.num_tablet_servers + 1, kTimeout));
  ASSERT_OK(CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, new_tserver));
  ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kFalse, new_tserver));
  ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsNewInstallFlagName, kFalse, new_tserver));
  ASSERT_EQ(GetAutoFlagConfigVersion(new_tserver), 0);

  // Add a new master
  ExternalMaster* new_master = nullptr;
  cluster_->StartShellMaster(&new_master);

  OpIdPB op_id;
  ASSERT_OK(cluster_->GetLastOpIdForLeader(&op_id));
  ASSERT_OK(cluster_->ChangeConfig(new_master, consensus::ADD_SERVER));
  ASSERT_OK(cluster_->WaitForMastersToCommitUpTo(op_id.index()));
  ASSERT_OK(CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, new_master));
  ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kFalse, new_master));
  ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsNewInstallFlagName, kFalse, new_master));
  ASSERT_EQ(GetAutoFlagConfigVersion(new_master), 0);

  // Restart the master
  new_master->Shutdown();
  ASSERT_OK(new_master->Restart());
  ASSERT_OK(CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, new_master));
  ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsInitializedFlagName, kFalse, new_master));
  ASSERT_OK(CheckFlagOnNode(kTESTAutoFlagsNewInstallFlagName, kFalse, new_master));
  ASSERT_EQ(GetAutoFlagConfigVersion(new_master), 0);

  // Remove disable_auto_flag_management from each process config and restart
  for (auto* master : cluster_->master_daemons()) {
    Erase(disable_auto_flag_management, master->mutable_flags());

    master->Shutdown();
    ASSERT_OK(master->Restart());
    ASSERT_OK(CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, master));
    const auto config_version = GetAutoFlagConfigVersion(master);
    ASSERT_EQ(config_version, 0);
  }

  for (auto* tserver : cluster_->tserver_daemons()) {
    Erase(disable_auto_flag_management, tserver->mutable_flags());

    tserver->Shutdown();
    ASSERT_OK(tserver->Restart());
    ASSERT_OK(CheckFlagOnNode(kDisableAutoFlagsManagementFlagName, kFalse, tserver));
    const auto config_version = GetAutoFlagConfigVersion(tserver);
    ASSERT_EQ(config_version, 0);
  }

  ASSERT_OK(CheckFlagOnAllNodes(kTESTAutoFlagsInitializedFlagName, kFalse));
  ASSERT_OK(CheckFlagOnAllNodes(kTESTAutoFlagsNewInstallFlagName, kFalse));

  ASSERT_OK(TestPromote(
      [&]() -> Result<AutoFlagsConfigPB> { return GetAutoFlagsConfig(); },
      [&](const auto& req) -> Result<master::PromoteAutoFlagsResponsePB> {
        return PromoteFlags(req);
      },
      [&](uint32_t config_version) -> Status {
        return ValidateConfigOnAllProcesses(config_version);
      }));
}

// Promote one flag and make sure Rollback only affects that one flag.
TEST_F(AutoFlagsExternalMiniClusterTest, RollbackOneFlag) {
  ASSERT_NO_FATALS(BuildAndStart());
  const auto kVolatileAutoFlagName = "ysql_yb_pushdown_strict_inequality";
  const auto kYbTServerProcess = "yb-tserver";

  auto master_proxy = cluster_->GetLeaderMasterProxy<master::MasterClusterProxy>();
  uint32 expected_config_version = 1;

  // Initial config.
  auto config = ASSERT_RESULT(GetAutoFlagsConfig());
  ASSERT_EQ(expected_config_version, config.config_version());
  const auto initial_promoted_flags_count = CountPromotedFlags(config);
  ASSERT_GT(initial_promoted_flags_count, 0);
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config, expected_config_version), 2);
  for (auto& daemons : cluster_->daemons()) {
    ASSERT_EQ(ASSERT_RESULT(daemons->GetFlag(kVolatileAutoFlagName)), "true");
  }
  auto initial_config = config;

  // Demote and promote a flag so that it has a higher version than the rest.
  ASSERT_NO_FATALS(
      config = DemoteSingleFlagAndValidate(
          kYbTServerProcess, kVolatileAutoFlagName, ++expected_config_version));
  // Flag exists in master but not tserver.
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config), 1);
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config, initial_config.config_version()), 1);
  // MiniCluster tests with single process so the last `server` to the flag value depends on the
  // last process that promoted or demoted a flag. In this case master servers are no-op so the
  // demote from tserver should take effect.
  for (auto& master : cluster_->master_daemons()) {
    ASSERT_EQ(ASSERT_RESULT(master->GetFlag(kVolatileAutoFlagName)), "true");
  }
  for (auto& tserver : cluster_->tserver_daemons()) {
    ASSERT_EQ(ASSERT_RESULT(tserver->GetFlag(kVolatileAutoFlagName)), "false");
  }

  ASSERT_NO_FATALS(
      config = PromoteFlagsAndValidate(++expected_config_version, AutoFlagClass::kLocalVolatile));
  ASSERT_EQ(CountPromotedFlags(config), initial_promoted_flags_count);
  // Tserver has new promoted version and master on older promoted version.
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config, expected_config_version), 1);
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config, initial_config.config_version()), 1);
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config), 2);
  for (auto& daemons : cluster_->daemons()) {
    ASSERT_EQ(ASSERT_RESULT(daemons->GetFlag(kVolatileAutoFlagName)), "true");
  }

  // Rollback promoted flags.
  auto rollback_config_version = expected_config_version - 1;
  ASSERT_NO_FATALS(
      config = RollbackFlagsAndValidate(rollback_config_version, ++expected_config_version));
  ASSERT_EQ(CountPromotedFlags(config), initial_promoted_flags_count - 1);
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config), 1);
  for (auto& master : cluster_->master_daemons()) {
    ASSERT_EQ(ASSERT_RESULT(master->GetFlag(kVolatileAutoFlagName)), "true");
  }
  for (auto& tserver : cluster_->tserver_daemons()) {
    ASSERT_EQ(ASSERT_RESULT(tserver->GetFlag(kVolatileAutoFlagName)), "false");
  }

  // Promote volatile flags.
  ASSERT_NO_FATALS(
      config = PromoteFlagsAndValidate(++expected_config_version, AutoFlagClass::kLocalVolatile));
  ASSERT_EQ(CountPromotedFlags(config), initial_promoted_flags_count);
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config, expected_config_version), 1);
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config, initial_config.config_version()), 1);
  ASSERT_EQ(CountFlagsInConfig(kVolatileAutoFlagName, config), 2);
  for (auto& daemons : cluster_->daemons()) {
    ASSERT_EQ(ASSERT_RESULT(daemons->GetFlag(kVolatileAutoFlagName)), "true");
  }
}
}  // namespace yb
