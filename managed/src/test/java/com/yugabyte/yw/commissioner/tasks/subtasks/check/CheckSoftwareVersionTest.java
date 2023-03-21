package com.yugabyte.yw.commissioner.tasks.subtasks.check;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThrows;
import static org.junit.Assert.fail;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.anyInt;
import static org.mockito.ArgumentMatchers.anyString;
import static org.mockito.Mockito.lenient;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.when;
import static play.mvc.Http.Status.BAD_REQUEST;
import static play.mvc.Http.Status.INTERNAL_SERVER_ERROR;

import com.yugabyte.yw.commissioner.AbstractTaskBase;
import com.yugabyte.yw.commissioner.tasks.CommissionerBaseTest;
import com.yugabyte.yw.common.ModelFactory;
import com.yugabyte.yw.common.PlatformServiceException;
import com.yugabyte.yw.common.TestUtils;
import com.yugabyte.yw.forms.UniverseDefinitionTaskParams;
import com.yugabyte.yw.models.Universe;
import com.yugabyte.yw.models.helpers.CloudSpecificInfo;
import com.yugabyte.yw.models.helpers.NodeDetails;
import junitparams.JUnitParamsRunner;
import junitparams.Parameters;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.mockito.junit.MockitoJUnit;
import org.mockito.junit.MockitoRule;
import org.yb.client.YBClient;

@RunWith(JUnitParamsRunner.class)
public class CheckSoftwareVersionTest extends CommissionerBaseTest {

  @Rule public MockitoRule mockitoRule = MockitoJUnit.rule();

  private Universe defaultUniverse;
  private NodeDetails node;
  private YBClient mockClient;

  @Override
  @Before
  public void setUp() {
    super.setUp();
    defaultCustomer = ModelFactory.testCustomer();
    defaultUniverse = ModelFactory.createUniverse();
    UniverseDefinitionTaskParams details = defaultUniverse.getUniverseDetails();
    node = new NodeDetails();
    node.cloudInfo = new CloudSpecificInfo();
    node.cloudInfo.private_ip = "1.2.3.4";
    node.nodeName = "node-1";
    details.nodeDetailsSet.add(node);
    defaultUniverse.setUniverseDetails(details);
    defaultUniverse.save();
    mockClient = mock(YBClient.class);
    try {
      lenient().when(mockYBClient.getClient(any(), any())).thenReturn(mockClient);
    } catch (Exception ignored) {
      fail();
    }
  }

  @Test
  @Parameters({
    "2.17.0.1-b1, 2.17.0.1, 9",
    "2.17.0.0-b1, 2.17.0.2, 1",
    "2.18.0.0-b1, 2.17.0.0, 1",
    "2.18.0.0-b1, 2.17.0.0, 1"
  })
  public void testDifferentVersionFail(String version1, String nodeVersion, String nodeBuild) {
    updateUniverseVersion(defaultUniverse, version1);
    try {
      when(mockClient.getStatus(any(), anyInt()))
          .thenReturn(TestUtils.prepareGetStatusResponse(nodeVersion, nodeBuild));
    } catch (Exception ignored) {
      fail();
    }
    CheckSoftwareVersion task = AbstractTaskBase.createTask(CheckSoftwareVersion.class);
    CheckSoftwareVersion.Params params = new CheckSoftwareVersion.Params();
    params.universeUUID = defaultUniverse.universeUUID;
    params.nodeName = node.nodeName;
    params.requiredVersion = version1;
    task.initialize(params);
    PlatformServiceException pe = assertThrows(PlatformServiceException.class, () -> task.run());
    assertEquals(BAD_REQUEST, pe.getHttpStatus());
    String newYbSoftwareVersion = nodeVersion + "-b" + nodeBuild;
    assertEquals(
        "Expected version: "
            + version1
            + " but found: "
            + newYbSoftwareVersion
            + " on node: "
            + node.nodeName,
        pe.getMessage());
  }

  @Test
  @Parameters({
    "2.17.0.1-b1, 2.17.0.1, 1",
    "2.17.0.0-b1, 2.17.0.0, PRE_RELEASE",
    "2.17.0.0-PRE_RELEASE, 2.17.0.0, b1"
  })
  public void testCheckSoftwareVersionSuccess(
      String version, String nodeVersion, String nodeBuild) {
    updateUniverseVersion(defaultUniverse, version);
    try {
      when(mockClient.getStatus(any(), anyInt()))
          .thenReturn(TestUtils.prepareGetStatusResponse(nodeVersion, nodeBuild));
    } catch (Exception ignored) {
      fail();
    }
    CheckSoftwareVersion task = AbstractTaskBase.createTask(CheckSoftwareVersion.class);
    CheckSoftwareVersion.Params params = new CheckSoftwareVersion.Params();
    params.universeUUID = defaultUniverse.universeUUID;
    params.nodeName = node.nodeName;
    params.requiredVersion = version;
    task.initialize(params);
    task.run();
  }

  @Test
  public void testMissingVersionDetails() {
    try {
      CheckSoftwareVersion task = AbstractTaskBase.createTask(CheckSoftwareVersion.class);
      CheckSoftwareVersion.Params params = new CheckSoftwareVersion.Params();
      params.universeUUID = defaultUniverse.universeUUID;
      params.nodeName = node.nodeName;
      params.requiredVersion = "2.17.0.0-b1";
      task.initialize(params);
      when(mockClient.getStatus(anyString(), anyInt()))
          .thenThrow(new RuntimeException("Unable to get the status"));
      PlatformServiceException pe = assertThrows(PlatformServiceException.class, () -> task.run());
      assertEquals(INTERNAL_SERVER_ERROR, pe.getHttpStatus());
      assertEquals("Unable to get the status", pe.getMessage());
    } catch (Exception ignored) {
      fail();
    }
  }

  private void updateUniverseVersion(Universe universe, String version) {
    UniverseDefinitionTaskParams details = defaultUniverse.getUniverseDetails();
    UniverseDefinitionTaskParams.UserIntent userIntent = details.getPrimaryCluster().userIntent;
    userIntent.ybSoftwareVersion = version;
    details.upsertPrimaryCluster(userIntent, null);
    universe.setUniverseDetails(details);
    universe.save();
  }
}
