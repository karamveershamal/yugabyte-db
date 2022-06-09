/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.cloud;

import com.yugabyte.yw.commissioner.Common;

public class PublicCloudConstants {

  public static final String PRODUCT_FAMILY_COMPUTE_INSTANCE = "Compute Instance";
  public static final String PRODUCT_FAMILY_STORAGE = "Storage";
  public static final String PRODUCT_FAMILY_SYSTEM_OPERATION = "System Operation";
  public static final String PRODUCT_FAMILY_PROVISIONED_THROUGHPUT = "Provisioned Throughput";

  public static final String VOLUME_TYPE_PROVISIONED_IOPS = "Provisioned IOPS";
  public static final String VOLUME_API_GENERAL_PURPOSE = "General Purpose";

  public static final String VOLUME_API_NAME_GP2 = "gp2";
  public static final String VOLUME_API_NAME_GP3 = "gp3";
  public static final String VOLUME_API_NAME_IO1 = "io1";

  public static final String GROUP_EBS_IOPS = "EBS IOPS";
  public static final String GROUP_EBS_THROUGHPUT = "EBS Throughput";

  public static final String IO1_SIZE = "io1.size";
  public static final String IO1_PIOPS = "io1.piops";
  public static final String GP2_SIZE = "gp2.size";
  public static final String GP3_SIZE = "gp3.size";
  public static final String GP3_PIOPS = "gp3.piops";
  public static final String GP3_THROUGHPUT = "gp3.throughput";

  public enum Tenancy {
    Shared,
    Dedicated,
    Host
  }

  public enum Architecture {
    x86_64("glob:**yugabyte*{centos,alma,linux,el}*x86_64.tar.gz"),
    arm64("glob:**yugabyte*{centos,alma,linux,el}*aarch64.tar.gz");

    private final String glob;

    Architecture(String glob) {
      this.glob = glob;
    }

    public String getGlob() {
      return glob;
    }
  }

  /**
   * Tracks the supported storage options for each cloud provider. Options in the UI will be ordered
   * alphabetically e.g. Persistent will be the default value for GCP, not Scratch
   */
  public enum StorageType {
    IO1(Common.CloudType.aws, true, false),
    GP2(Common.CloudType.aws, false, false),
    GP3(Common.CloudType.aws, true, true),
    Scratch(Common.CloudType.gcp, false, false),
    Persistent(Common.CloudType.gcp, false, false),
    StandardSSD_LRS(Common.CloudType.azu, false, false),
    Premium_LRS(Common.CloudType.azu, false, false),
    UltraSSD_LRS(Common.CloudType.azu, true, true);

    private final Common.CloudType cloudType;
    private final boolean iopsProvisioning;
    private final boolean throughputProvisioning;

    StorageType(
        Common.CloudType cloudType, boolean iopsProvisioning, boolean throughputProvisioning) {
      this.cloudType = cloudType;
      this.iopsProvisioning = iopsProvisioning;
      this.throughputProvisioning = throughputProvisioning;
    }

    public Common.CloudType getCloudType() {
      return cloudType;
    }

    public boolean isIopsProvisioning() {
      return iopsProvisioning;
    }

    public boolean isThroughputProvisioning() {
      return throughputProvisioning;
    }
  }
}
