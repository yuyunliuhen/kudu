// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "kudu/gutil/port.h"
#include "kudu/rpc/sasl_common.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/status.h"

namespace kudu {

class Subprocess;

namespace hms {

class MiniHms {
 public:

  MiniHms();

  ~MiniHms();

  // Configures the mini HMS to use Kerberos.
  void EnableKerberos(std::string krb5_conf,
                      std::string service_principal,
                      std::string keytab_file,
                      rpc::SaslProtection::Type protection);

  // Configures the mini HMS to enable the Sentry plugin, passing the
  // Sentry service's principal to be used in Kerberos environment.
  //
  // Parameters 'sentry_client_rpc_retry_num' and
  // 'sentry_client_rpc_retry_interval_ms' are used to override default settings
  // of the Sentry client used by HMS plugins. The default values for these two
  // parameters are set to allow for shorter HMS --> Sentry RPC timeout
  // (i.e. shorter than with the default Sentry v2.{0,1} client's settings).
  void EnableSentry(const HostPort& sentry_address,
                    std::string sentry_service_principal,
                    int sentry_client_rpc_retry_num = 3,
                    int sentry_client_rpc_retry_interval_ms = 500);

  // Configures the mini HMS to enable or disable the Kudu plugin.
  void EnableKuduPlugin(bool enable);

  // Configures the mini HMS to store its data in the provided path. If not set,
  // it uses a test-only temporary directory.
  void SetDataRoot(std::string data_root);

  // Starts the mini Hive metastore.
  //
  // If the MiniHms has already been started and stopped, it will be restarted
  // using the same listening port.
  Status Start() WARN_UNUSED_RESULT;

  // Stops the mini Hive metastore.
  Status Stop() WARN_UNUSED_RESULT;

  // Pause the Hive metastore process.
  Status Pause() WARN_UNUSED_RESULT;

  // Unpause the Hive metastore process.
  Status Resume() WARN_UNUSED_RESULT;

  // Returns the address of the Hive metastore. Should only be called after the
  // metastore is started.
  HostPort address() const {
    return HostPort("127.0.0.1", port_);
  }

  // Returns the Metastore URIs, in the format that the Hive
  // hive.metastore.uris configuration expects.
  std::string uris() const;

  // Returns true when Sentry as well as Kerberos is enabled.
  bool IsAuthorizationEnabled() const;

  // Returns true when Kerberos is enabled.
  bool IsKerberosEnabled() const {
    return !keytab_file_.empty();
  }

 private:

  // Creates a hive-site.xml for the mini HMS.
  Status CreateHiveSite() const WARN_UNUSED_RESULT;

  // Creates a core-site.xml for the mini HMS.
  Status CreateCoreSite() const WARN_UNUSED_RESULT;

  // Creates a log4j2 configuration properties file for the mini HMS.
  Status CreateLogConfig() const WARN_UNUSED_RESULT;

  std::unique_ptr<Subprocess> hms_process_;
  MonoDelta notification_log_ttl_ = MonoDelta::FromSeconds(86400);
  uint16_t port_ = 0;

  std::string data_root_;
  // Sub-directory under 'data_root_' where Derby keeps its database.
  std::string metadb_subdir_ = "metadb";

  // Kerberos configuration
  std::string krb5_conf_;
  std::string service_principal_;
  std::string keytab_file_;
  rpc::SaslProtection::Type protection_ = rpc::SaslProtection::kAuthentication;

  // Sentry configuration
  std::string sentry_address_;
  std::string sentry_service_principal_;
  int sentry_client_rpc_retry_num_;
  int sentry_client_rpc_retry_interval_ms_;

  // Whether to enable the Kudu listener plugin.
  bool enable_kudu_plugin_ = true;
};

} // namespace hms
} // namespace kudu
