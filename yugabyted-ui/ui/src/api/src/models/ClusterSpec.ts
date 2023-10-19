// tslint:disable
/**
 * Yugabyte Cloud
 * YugabyteDB as a Service
 *
 * The version of the OpenAPI document: v1
 * Contact: support@yugabyte.com
 *
 * NOTE: This class is auto generated by OpenAPI Generator (https://openapi-generator.tech).
 * https://openapi-generator.tech
 * Do not edit the class manually.
 */


// eslint-disable-next-line no-duplicate-imports
import type { CloudInfo } from './CloudInfo';
// eslint-disable-next-line no-duplicate-imports
import type { ClusterInfo } from './ClusterInfo';
// eslint-disable-next-line no-duplicate-imports
import type { ClusterRegionInfo } from './ClusterRegionInfo';
// eslint-disable-next-line no-duplicate-imports
import type { EncryptionInfo } from './EncryptionInfo';


/**
 * Cluster spec
 * @export
 * @interface ClusterSpec
 */
export interface ClusterSpec  {
  /**
   * The name of the cluster
   * @type {string}
   * @memberof ClusterSpec
   */
  name: string;
  /**
   * 
   * @type {CloudInfo}
   * @memberof ClusterSpec
   */
  cloud_info: CloudInfo;
  /**
   * 
   * @type {ClusterInfo}
   * @memberof ClusterSpec
   */
  cluster_info: ClusterInfo;
  /**
   * 
   * @type {ClusterRegionInfo[]}
   * @memberof ClusterSpec
   */
  cluster_region_info?: ClusterRegionInfo[];
  /**
   * 
   * @type {EncryptionInfo}
   * @memberof ClusterSpec
   */
  encryption_info: EncryptionInfo;
}


