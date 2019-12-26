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

#include "kudu/tools/tool_action_common.h"

#include <stdlib.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <stack>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <gflags/gflags.h>
#include <glog/logging.h>
// IWYU pragma: no_include <yaml-cpp/node/impl.h>
// IWYU pragma: no_include <yaml-cpp/node/node.h>

#include "kudu/client/client-internal.h"  // IWYU pragma: keep
#include "kudu/client/client.h"
#include "kudu/client/master_proxy_rpc.h"
#include "kudu/client/shared_ptr.h"
#include "kudu/common/common.pb.h"
#include "kudu/common/row_operations.h"
#include "kudu/common/schema.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/consensus/consensus.pb.h"
#include "kudu/consensus/consensus.proxy.h" // IWYU pragma: keep
#include "kudu/consensus/log.pb.h"
#include "kudu/consensus/log_util.h"
#include "kudu/consensus/opid.pb.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/ref_counted.h"
#include "kudu/gutil/strings/join.h"
#include "kudu/gutil/strings/numbers.h"
#include "kudu/gutil/strings/split.h"
#include "kudu/gutil/strings/stringpiece.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/master/master.proxy.h" // IWYU pragma: keep
#include "kudu/rpc/messenger.h"
#include "kudu/rpc/response_callback.h"
#include "kudu/rpc/rpc.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/rpc/rpc_header.pb.h"
#include "kudu/server/server_base.pb.h"
#include "kudu/server/server_base.proxy.h"
#include "kudu/tools/tool.pb.h" // IWYU pragma: keep
#include "kudu/tools/tool_action.h"
#include "kudu/tserver/tserver.pb.h"
#include "kudu/tserver/tserver_admin.proxy.h"   // IWYU pragma: keep
#include "kudu/tserver/tserver_service.proxy.h" // IWYU pragma: keep
#include "kudu/util/async_util.h"
#include "kudu/util/env.h"
#include "kudu/util/jsonwriter.h"
#include "kudu/util/mem_tracker.pb.h"
#include "kudu/util/memory/arena.h"
#include "kudu/util/monotime.h"
#include "kudu/util/net/net_util.h"
#include "kudu/util/net/sockaddr.h"
#include "kudu/util/path_util.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/status.h"
#include "kudu/util/yamlreader.h"

DEFINE_bool(force, false, "If true, allows the set_flag command to set a flag "
            "which is not explicitly marked as runtime-settable. Such flag "
            "changes may be simply ignored on the server, or may cause the "
            "server to crash.");
DEFINE_bool(print_meta, true, "Include metadata in output");
DEFINE_string(print_entries, "decoded",
              "How to print entries:\n"
              "  false|0|no = don't print\n"
              "  true|1|yes|decoded = print them decoded\n"
              "  pb = print the raw protobuf\n"
              "  id = print only their ids");
DEFINE_string(table_name, "",
              "Restrict output to a specific table by name");
DEFINE_string(tablets, "",
              "Tablets to check (comma-separated list of IDs) "
              "If not specified, checks all tablets.");
DEFINE_int64(timeout_ms, 1000 * 60, "RPC timeout in milliseconds");
DEFINE_int32(truncate_data, 100,
             "Truncate the data fields to the given number of bytes "
             "before printing. Set to 0 to disable");

DEFINE_string(columns, "", "Comma-separated list of column fields to include in output tables");
DEFINE_string(format, "pretty",
              "Format to use for printing list output tables.\n"
              "Possible values: pretty, space, tsv, csv, and json");

DEFINE_string(flag_tags, "", "Comma-separated list of tags used to restrict which "
                             "flags are returned. An empty value matches all tags");
DEFINE_bool(all_flags, false, "Whether to return all flags, or only flags that "
                              "were explicitly set.");
DEFINE_string(flags, "", "Comma-separated list of flags used to restrict which "
                         "flags are returned. An empty value means no restriction. "
                         "If non-empty, all_flags is ignored.");
DEFINE_string(tables, "", "Tables to include (comma-separated list of table names)"
                          "If not specified, includes all tables.");

DEFINE_string(memtracker_output, "table",
              "One of 'json', 'json_compact' or 'table'. Table output flattens "
              "the memtracker hierarchy.");

DEFINE_int32(num_threads, 2,
             "Number of threads to run. Each thread runs its own "
             "KuduSession.");
static bool ValidateNumThreads(const char* flag_name, int32_t flag_value) {
  if (flag_value <= 0) {
    LOG(ERROR) << strings::Substitute("'$0' flag should have a positive value",
                                      flag_name);
    return false;
  }
  return true;
}
DEFINE_validator(num_threads, &ValidateNumThreads);

using kudu::client::KuduClient;
using kudu::client::KuduClientBuilder;
using kudu::client::internal::AsyncLeaderMasterRpc;
using kudu::consensus::ConsensusServiceProxy; // NOLINT
using kudu::consensus::ReplicateMsg;
using kudu::log::LogEntryPB;
using kudu::log::LogEntryReader;
using kudu::log::ReadableLogSegment;
using kudu::master::MasterServiceProxy;
using kudu::pb_util::SecureDebugString;
using kudu::pb_util::SecureShortDebugString;
using kudu::rpc::BackoffType;
using kudu::rpc::Messenger;
using kudu::rpc::MessengerBuilder;
using kudu::rpc::RequestIdPB;
using kudu::rpc::ResponseCallback;
using kudu::rpc::RpcController;
using kudu::server::GenericServiceProxy;
using kudu::server::GetFlagsRequestPB;
using kudu::server::GetFlagsResponsePB;
using kudu::server::GetStatusRequestPB;
using kudu::server::GetStatusResponsePB;
using kudu::server::ServerClockRequestPB;
using kudu::server::ServerClockResponsePB;
using kudu::server::ServerStatusPB;
using kudu::server::SetFlagRequestPB;
using kudu::server::SetFlagResponsePB;
using kudu::tserver::TabletServerAdminServiceProxy; // NOLINT
using kudu::tserver::TabletServerServiceProxy; // NOLINT
using kudu::tserver::WriteRequestPB;
using std::cout;
using std::endl;
using std::ostream;
using std::setfill;
using std::setw;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;
using strings::Split;
using strings::Substitute;

namespace boost {
template <typename Signature>
class function;
} // namespace boost

namespace kudu {
namespace tools {

const char* const kMasterAddressesArg = "master_addresses";
const char* const kMasterAddressesArgDesc = "Either comma-separated list of Kudu "
    "master addresses where each address is of form 'hostname:port', or a cluster name if it has "
    "been configured in ${KUDU_CONFIG}/kudurc";
const char* const kDestMasterAddressesArg = "dest_master_addresses";
const char* const kDestMasterAddressesArgDesc = "Either comma-separated list of destination Kudu "
    "master addresses where each address is of form 'hostname:port', or a cluster name if it has "
    "been configured in ${KUDU_CONFIG}/kudurc";
const char* const kTableNameArg = "table_name";
const char* const kTabletIdArg = "tablet_id";
const char* const kTabletIdArgDesc = "Tablet Identifier";

namespace {

enum PrintEntryType {
  DONT_PRINT,
  PRINT_PB,
  PRINT_DECODED,
  PRINT_ID
};

PrintEntryType ParsePrintType() {
  if (ParseLeadingBoolValue(FLAGS_print_entries.c_str(), true) == false) {
    return DONT_PRINT;
  } else if (ParseLeadingBoolValue(FLAGS_print_entries.c_str(), false) == true ||
             FLAGS_print_entries == "decoded") {
    return PRINT_DECODED;
  } else if (FLAGS_print_entries == "pb") {
    return PRINT_PB;
  } else if (FLAGS_print_entries == "id") {
    return PRINT_ID;
  } else {
    LOG(FATAL) << "Unknown value for --print_entries: " << FLAGS_print_entries;
  }
}

void PrintIdOnly(const LogEntryPB& entry) {
  switch (entry.type()) {
    case log::REPLICATE:
    {
      cout << entry.replicate().id().term() << "." << entry.replicate().id().index()
           << "@" << entry.replicate().timestamp() << "\t";
      cout << "REPLICATE "
           << OperationType_Name(entry.replicate().op_type());
      break;
    }
    case log::COMMIT:
    {
      cout << "COMMIT " << entry.commit().commited_op_id().term()
           << "." << entry.commit().commited_op_id().index();
      break;
    }
    default:
      cout << "UNKNOWN: " << SecureShortDebugString(entry);
  }

  cout << endl;
}

Status PrintDecodedWriteRequestPB(const string& indent,
                                  const Schema& tablet_schema,
                                  const WriteRequestPB& write,
                                  const RequestIdPB* request_id) {
  Schema request_schema;
  RETURN_NOT_OK(SchemaFromPB(write.schema(), &request_schema));

  Arena arena(32 * 1024);
  RowOperationsPBDecoder dec(&write.row_operations(), &request_schema, &tablet_schema, &arena);
  vector<DecodedRowOperation> ops;
  RETURN_NOT_OK(dec.DecodeOperations<DecoderMode::WRITE_OPS>(&ops));

  cout << indent << "Tablet: " << write.tablet_id() << endl;
  cout << indent << "RequestId: "
      << (request_id ? SecureShortDebugString(*request_id) : "None") << endl;
  cout << indent << "Consistency: "
       << ExternalConsistencyMode_Name(write.external_consistency_mode()) << endl;
  if (write.has_propagated_timestamp()) {
    cout << indent << "Propagated TS: " << write.propagated_timestamp() << endl;
  }

  int i = 0;
  for (const DecodedRowOperation& op : ops) {
    // TODO (KUDU-515): Handle the case when a tablet's schema changes
    // mid-segment.
    cout << indent << "op " << (i++) << ": " << op.ToString(tablet_schema) << endl;
  }

  return Status::OK();
}

Status PrintDecoded(const LogEntryPB& entry, const Schema& tablet_schema) {
  PrintIdOnly(entry);

  const string indent = "\t";
  if (entry.has_replicate()) {
    // We can actually decode REPLICATE messages.

    const ReplicateMsg& replicate = entry.replicate();
    if (replicate.op_type() == consensus::WRITE_OP) {
      RETURN_NOT_OK(PrintDecodedWriteRequestPB(
          indent,
          tablet_schema,
          replicate.write_request(),
          replicate.has_request_id() ? &replicate.request_id() : nullptr));
    } else {
      cout << indent << SecureShortDebugString(replicate) << endl;
    }
  } else if (entry.has_commit()) {
    // For COMMIT we'll just dump the PB
    cout << indent << SecureShortDebugString(entry.commit()) << endl;
  }

  return Status::OK();
}

// A valid 'cluster name' is beginning with a special character '@'.
// '@' is a character which has no special significance in shells and
// it's an invalid character in hostname list, so we can use it to
// distinguish cluster name from master addresses.
bool GetClusterName(const string& master_addresses_str, string* cluster_name) {
  CHECK(cluster_name);
  if (HasPrefixString(master_addresses_str, "@")) {
    *cluster_name = master_addresses_str.substr(1);  // Trim the first '@'.
    return true;
  }
  return false;
}

} // anonymous namespace

template<class ProxyClass>
Status BuildProxy(const string& address,
                  uint16_t default_port,
                  unique_ptr<ProxyClass>* proxy) {
  HostPort hp;
  RETURN_NOT_OK(hp.ParseString(address, default_port));
  shared_ptr<Messenger> messenger;
  RETURN_NOT_OK(MessengerBuilder("tool").Build(&messenger));

  vector<Sockaddr> resolved;
  RETURN_NOT_OK(hp.ResolveAddresses(&resolved));

  proxy->reset(new ProxyClass(messenger, resolved[0], hp.host()));
  return Status::OK();
}

// Explicit specialization for callers outside this compilation unit.
template
Status BuildProxy(const string& address,
                  uint16_t default_port,
                  unique_ptr<ConsensusServiceProxy>* proxy);
template
Status BuildProxy(const string& address,
                  uint16_t default_port,
                  unique_ptr<TabletServerServiceProxy>* proxy);
template
Status BuildProxy(const string& address,
                  uint16_t default_port,
                  unique_ptr<TabletServerAdminServiceProxy>* proxy);
template
Status BuildProxy(const string& address,
                  uint16_t default_port,
                  unique_ptr<MasterServiceProxy>* proxy);

Status GetServerStatus(const string& address, uint16_t default_port,
                       ServerStatusPB* status) {
  unique_ptr<GenericServiceProxy> proxy;
  RETURN_NOT_OK(BuildProxy(address, default_port, &proxy));

  GetStatusRequestPB req;
  GetStatusResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_timeout_ms));

  RETURN_NOT_OK(proxy->GetStatus(req, &resp, &rpc));
  if (!resp.has_status()) {
    return Status::Incomplete("Server response did not contain status",
                              proxy->ToString());
  }
  *status = resp.status();
  return Status::OK();
}

Status PrintSegment(const scoped_refptr<ReadableLogSegment>& segment) {
  PrintEntryType print_type = ParsePrintType();
  if (FLAGS_print_meta) {
    cout << "Header:\n" << SecureDebugString(segment->header());
  }
  if (print_type != DONT_PRINT) {
    Schema tablet_schema;
    RETURN_NOT_OK(SchemaFromPB(segment->header().schema(), &tablet_schema));

    LogEntryReader reader(segment.get());
    while (true) {
      unique_ptr<LogEntryPB> entry;
      Status s = reader.ReadNextEntry(&entry);
      if (s.IsEndOfFile()) {
        break;
      }
      RETURN_NOT_OK(s);

      if (print_type == PRINT_PB) {
        if (FLAGS_truncate_data > 0) {
          pb_util::TruncateFields(entry.get(), FLAGS_truncate_data);
        }

        cout << "Entry:\n" << SecureDebugString(*entry);
      } else if (print_type == PRINT_DECODED) {
        RETURN_NOT_OK(PrintDecoded(*entry, tablet_schema));
      } else if (print_type == PRINT_ID) {
        PrintIdOnly(*entry);
      }
    }
  }
  if (FLAGS_print_meta && segment->HasFooter()) {
    cout << "Footer:\n" << SecureDebugString(segment->footer());
  }

  return Status::OK();
}

Status GetServerFlags(const string& address,
                      uint16_t default_port,
                      bool all_flags,
                      const string& flags_to_get,
                      const string& flag_tags,
                      vector<server::GetFlagsResponsePB_Flag>* flags) {
  unique_ptr<GenericServiceProxy> proxy;
  RETURN_NOT_OK(BuildProxy(address, default_port, &proxy));

  GetFlagsRequestPB req;
  GetFlagsResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_timeout_ms));

  req.set_all_flags(all_flags);
  for (StringPiece tag : strings::Split(flag_tags, ",", strings::SkipEmpty())) {
    req.add_tags(tag.as_string());
  }
  for (StringPiece flag: strings::Split(flags_to_get, ",", strings::SkipEmpty())) {
    req.add_flags(flag.as_string());
  }

  RETURN_NOT_OK(proxy->GetFlags(req, &resp, &rpc));

  flags->clear();
  std::move(resp.flags().begin(), resp.flags().end(), std::back_inserter(*flags));
  return Status::OK();
}

Status PrintServerFlags(const string& address, uint16_t default_port) {
  vector<server::GetFlagsResponsePB_Flag> flags;
  RETURN_NOT_OK(GetServerFlags(address, default_port, FLAGS_all_flags,
      FLAGS_flags, FLAGS_flag_tags, &flags));

  std::sort(flags.begin(), flags.end(),
      [](const GetFlagsResponsePB::Flag& left,
         const GetFlagsResponsePB::Flag& right) -> bool {
        return left.name() < right.name();
      });
  DataTable table({ "flag", "value", "default value?", "tags" });
  vector<string> tags;
  for (const auto& flag : flags) {
    tags.clear();
    std::copy(flag.tags().begin(), flag.tags().end(), std::back_inserter(tags));
    std::sort(tags.begin(), tags.end());
    table.AddRow({ flag.name(),
                   flag.value(),
                   flag.is_default_value() ? "true" : "false",
                   JoinStrings(tags, ",") });
  }
  return table.PrintTo(cout);
}

Status SetServerFlag(const string& address, uint16_t default_port,
                     const string& flag, const string& value) {
  unique_ptr<GenericServiceProxy> proxy;
  RETURN_NOT_OK(BuildProxy(address, default_port, &proxy));

  SetFlagRequestPB req;
  SetFlagResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_timeout_ms));

  req.set_flag(flag);
  req.set_value(value);
  req.set_force(FLAGS_force);

  RETURN_NOT_OK(proxy->SetFlag(req, &resp, &rpc));
  switch (resp.result()) {
    case server::SetFlagResponsePB::SUCCESS:
      return Status::OK();
    case server::SetFlagResponsePB::NOT_SAFE:
      return Status::RemoteError(resp.msg() +
                                 " (use --force flag to allow anyway)");
    default:
      return Status::RemoteError(SecureShortDebugString(resp));
  }
}

bool MatchesAnyPattern(const vector<string>& patterns, const string& str) {
  // Consider no filter a wildcard.
  if (patterns.empty()) return true;

  for (const auto& p : patterns) {
    if (MatchPattern(str, p)) return true;
  }
  return false;
}

Status CreateKuduClient(const RunnerContext& context,
                        const char* master_addresses_arg,
                        client::sp::shared_ptr<KuduClient>* client) {
  vector<string> master_addresses;
  RETURN_NOT_OK(ParseMasterAddresses(context, master_addresses_arg, &master_addresses));
  return KuduClientBuilder()
             .master_server_addrs(master_addresses)
             .Build(client);
}

Status CreateKuduClient(const RunnerContext& context,
                        client::sp::shared_ptr<KuduClient>* client) {
  return CreateKuduClient(context, kMasterAddressesArg, client);
}

Status ParseMasterAddressesStr(const RunnerContext& context,
                               const char* master_addresses_arg,
                               string* master_addresses_str) {
  CHECK(master_addresses_str);
  *master_addresses_str = FindOrDie(context.required_args, master_addresses_arg);
  string cluster_name;
  if (!GetClusterName(*master_addresses_str, &cluster_name)) {
    // Treat it as master addresses.
    return Status::OK();
  }

  // Try to resolve cluster name.
  char* kudu_config_path = getenv("KUDU_CONFIG");
  if (!kudu_config_path) {
    return Status::NotFound("${KUDU_CONFIG} is missing");
  }
  auto config_file = JoinPathSegments(kudu_config_path, "kudurc");
  if (!Env::Default()->FileExists(config_file)) {
    return Status::NotFound(Substitute("configuration file $0 was not found", config_file));
  }
  YamlReader reader(config_file);
  RETURN_NOT_OK(reader.Init());
  YAML::Node clusters_info;
  RETURN_NOT_OK(YamlReader::ExtractMap(reader.node(), "clusters_info", &clusters_info));
  YAML::Node cluster_info;
  RETURN_NOT_OK(YamlReader::ExtractMap(&clusters_info, cluster_name, &cluster_info));
  RETURN_NOT_OK(YamlReader::ExtractScalar(&cluster_info, "master_addresses",
                                          master_addresses_str));
  return Status::OK();
}

Status ParseMasterAddressesStr(
    const RunnerContext& context,
    string* master_addresses_str) {
  CHECK(master_addresses_str);
  return ParseMasterAddressesStr(context, kMasterAddressesArg, master_addresses_str);
}

Status ParseMasterAddresses(const RunnerContext& context,
                            const char* master_addresses_arg,
                            vector<string>* master_addresses) {
  CHECK(master_addresses);
  string master_addresses_str;
  RETURN_NOT_OK(ParseMasterAddressesStr(context, master_addresses_arg, &master_addresses_str));
  *master_addresses = strings::Split(master_addresses_str, ",");
  return Status::OK();
}

Status ParseMasterAddresses(
    const RunnerContext& context,
    vector<string>* master_addresses) {
  CHECK(master_addresses);
  return ParseMasterAddresses(context, kMasterAddressesArg, master_addresses);
}

Status PrintServerStatus(const string& address, uint16_t default_port) {
  ServerStatusPB status;
  RETURN_NOT_OK(GetServerStatus(address, default_port, &status));
  cout << SecureDebugString(status) << endl;
  return Status::OK();
}

Status PrintServerTimestamp(const string& address, uint16_t default_port) {
  unique_ptr<GenericServiceProxy> proxy;
  RETURN_NOT_OK(BuildProxy(address, default_port, &proxy));

  ServerClockRequestPB req;
  ServerClockResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_timeout_ms));
  RETURN_NOT_OK(proxy->ServerClock(req, &resp, &rpc));
  if (!resp.has_timestamp()) {
    return Status::Incomplete("Server response did not contain timestamp",
                              proxy->ToString());
  }
  cout << resp.timestamp() << endl;
  return Status::OK();
}

Status DumpMemTrackers(const string& address, uint16_t default_port) {
  unique_ptr<GenericServiceProxy> proxy;
  RETURN_NOT_OK(BuildProxy(address, default_port, &proxy));

  server::DumpMemTrackersRequestPB req;
  server::DumpMemTrackersResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromMilliseconds(FLAGS_timeout_ms));
  RETURN_NOT_OK(proxy->DumpMemTrackers(req, &resp, &rpc));

  if (boost::iequals(FLAGS_memtracker_output, "json")) {
    cout << JsonWriter::ToJson(resp.root_tracker(), JsonWriter::Mode::PRETTY)
         << endl;
  } else if (boost::iequals(FLAGS_memtracker_output, "json_compact")) {
    cout << JsonWriter::ToJson(resp.root_tracker(), JsonWriter::Mode::COMPACT)
         << endl;
  } else if (boost::iequals(FLAGS_memtracker_output, "table")) {
    DataTable table({ "id", "parent_id", "limit",
                      "current consumption", "peak_consumption" });
    const auto& root = resp.root_tracker();
    std::stack<const MemTrackerPB*> to_process;
    to_process.push(&root);
    while (!to_process.empty()) {
      const auto* tracker = to_process.top();
      to_process.pop();
      table.AddRow({ tracker->id(),
                     tracker->has_parent_id() ? tracker->parent_id() : "<none>",
                     std::to_string(tracker->limit()),
                     std::to_string(tracker->current_consumption()),
                     std::to_string(tracker->peak_consumption()) });
      for (const auto& child_tracker : tracker->child_trackers()) {
        to_process.push(&child_tracker);
      }
    }
    RETURN_NOT_OK(table.PrintTo(cout));
  } else {
    return Status::InvalidArgument("unknown output type (--memtracker_output)",
                                   FLAGS_memtracker_output);
  }
  return Status::OK();
}

namespace {

// Pretty print a table using the psql format. For example:
//
//                uuid               |         rpc-addresses          |      seqno
// ----------------------------------+--------------------------------+------------------
//  335d132897de4bdb9b87443f2c487a42 | 126.rack1.dc1.example.com:7050 | 1492596790237811
//  7425c65d80f54f2da0a85494a5eb3e68 | 122.rack1.dc1.example.com:7050 | 1492596755322350
//  dd23284d3a334f1a8306c19d89c1161f | 130.rack1.dc1.example.com:7050 | 1492596704536543
//  d8009e07d82b4e66a7ab50f85e60bc30 | 136.rack1.dc1.example.com:7050 | 1492596696557549
//  c108a85a68504c2bb9f49e4ee683d981 | 128.rack1.dc1.example.com:7050 | 1492596646623301
void PrettyPrintTable(const vector<string>& headers,
                      const vector<vector<string>>& columns,
                      ostream& out) {
  CHECK_EQ(headers.size(), columns.size());
  if (headers.empty()) return;
  size_t num_columns = headers.size();

  vector<size_t> widths;
  for (int col = 0; col < num_columns; col++) {
    size_t width = std::accumulate(columns[col].begin(), columns[col].end(), headers[col].size(),
                                   [](size_t acc, const string& cell) {
                                     return std::max(acc, cell.size());
                                   });
    widths.push_back(width);
  }

  // Print the header row.
  for (int col = 0; col < num_columns; col++) {
    int padding = widths[col] - headers[col].size();
    out << setw(padding / 2) << "" << " " << headers[col];
    if (col != num_columns - 1) out << setw((padding + 1) / 2) << "" << " |";
  }
  out << endl;

  // Print the separator row.
  out << setfill('-');
  for (int col = 0; col < num_columns; col++) {
    out << setw(widths[col] + 2) << "";
    if (col != num_columns - 1) out << "+";
  }
  out << endl;

  // Print the data rows.
  out << setfill(' ');
  int num_rows = columns.empty() ? 0 : columns[0].size();
  for (int row = 0; row < num_rows; row++) {
    for (int col = 0; col < num_columns; col++) {
      const auto& value = columns[col][row];
      out << " " << value;
      if (col != num_columns - 1) {
        size_t padding = widths[col] - value.size();
        out << setw(padding) << "" << " |";
      }
    }
    out << endl;
  }
}

// Print a table using JSON formatting.
//
// The table is formatted as an array of objects. Each object corresponds
// to a row whose fields are the column values.
void JsonPrintTable(const vector<string>& headers,
                    const vector<vector<string>>& columns,
                    ostream& out) {
  std::ostringstream stream;
  JsonWriter writer(&stream, JsonWriter::COMPACT);

  int num_columns = columns.size();
  int num_rows = columns.empty() ? 0 : columns[0].size();

  writer.StartArray();
  for (int row = 0; row < num_rows; row++) {
    writer.StartObject();
    for (int col = 0; col < num_columns; col++) {
      writer.String(headers[col]);
      writer.String(columns[col][row]);
    }
    writer.EndObject();
  }
  writer.EndArray();

  out << stream.str() << endl;
}

// Print the table using the provided separator. For example, with a comma
// separator:
//
// 335d132897de4bdb9b87443f2c487a42,126.rack1.dc1.example.com:7050,1492596790237811
// 7425c65d80f54f2da0a85494a5eb3e68,122.rack1.dc1.example.com:7050,1492596755322350
// dd23284d3a334f1a8306c19d89c1161f,130.rack1.dc1.example.com:7050,1492596704536543
// d8009e07d82b4e66a7ab50f85e60bc30,136.rack1.dc1.example.com:7050,1492596696557549
// c108a85a68504c2bb9f49e4ee683d981,128.rack1.dc1.example.com:7050,1492596646623301
void PrintTable(const vector<vector<string>>& columns, const string& separator, ostream& out) {
  // TODO(dan): proper escaping of string values.
  int num_columns = columns.size();
  int num_rows = columns.empty() ? 0 : columns[0].size();
  for (int row = 0; row < num_rows; row++) {
      for (int col = 0; col < num_columns; col++) {
        out << columns[col][row];
        if (col != num_columns - 1) out << separator;
      }
      out << endl;
  }
}

} // anonymous namespace

DataTable::DataTable(vector<string> col_names)
    : column_names_(std::move(col_names)),
      columns_(column_names_.size()) {
}

void DataTable::AddRow(vector<string> row) {
  CHECK_EQ(row.size(), columns_.size());
  int i = 0;
  for (auto& v : row) {
    columns_[i++].emplace_back(std::move(v));
  }
}

void DataTable::AddColumn(string name, vector<string> column) {
  if (!columns_.empty()) {
    CHECK_EQ(column.size(), columns_[0].size());
  }
  column_names_.emplace_back(std::move(name));
  columns_.emplace_back(std::move(column));
}

Status DataTable::PrintTo(ostream& out) const {
  if (boost::iequals(FLAGS_format, "pretty")) {
    PrettyPrintTable(column_names_, columns_, out);
  } else if (boost::iequals(FLAGS_format, "space")) {
    PrintTable(columns_, " ", out);
  } else if (boost::iequals(FLAGS_format, "tsv")) {
    PrintTable(columns_, "	", out);
  } else if (boost::iequals(FLAGS_format, "csv")) {
    PrintTable(columns_, ",", out);
  } else if (boost::iequals(FLAGS_format, "json")) {
    JsonPrintTable(column_names_, columns_, out);
  } else {
    return Status::InvalidArgument("unknown format (--format)", FLAGS_format);
  }
  return Status::OK();
}

LeaderMasterProxy::LeaderMasterProxy(client::sp::shared_ptr<KuduClient> client) :
  client_(std::move(client)) {
}

Status LeaderMasterProxy::Init(const vector<string>& master_addrs, const MonoDelta& timeout) {
  return KuduClientBuilder().master_server_addrs(master_addrs)
                            .default_rpc_timeout(timeout)
                            .default_admin_operation_timeout(timeout)
                            .Build(&client_);
}

Status LeaderMasterProxy::Init(const RunnerContext& context) {
  vector<string> master_addresses;
  RETURN_NOT_OK(ParseMasterAddresses(context, &master_addresses));
  return Init(master_addresses, MonoDelta::FromMilliseconds(FLAGS_timeout_ms));
}

template<typename Req, typename Resp>
Status LeaderMasterProxy::SyncRpc(const Req& req,
                                  Resp* resp,
                                  string func_name,
                                  const boost::function<void(master::MasterServiceProxy*,
                                                             const Req&, Resp*,
                                                             rpc::RpcController*,
                                                             const ResponseCallback&)>& func) {
  MonoTime deadline = MonoTime::Now() + MonoDelta::FromMilliseconds(FLAGS_timeout_ms);
  Synchronizer sync;
  AsyncLeaderMasterRpc<Req, Resp> rpc(deadline, client_.get(), BackoffType::EXPONENTIAL,
      req, resp, func, std::move(func_name), sync.AsStatusCallback(), {});
  rpc.SendRpc();
  return sync.Wait();
}

// Explicit specializations for callers outside this compilation unit.
template
Status LeaderMasterProxy::SyncRpc(
    const master::ChangeTServerStateRequestPB& req,
    master::ChangeTServerStateResponsePB* resp,
    string func_name,
    const boost::function<void(MasterServiceProxy*,
                               const master::ChangeTServerStateRequestPB&,
                               master::ChangeTServerStateResponsePB*,
                               RpcController*,
                               const ResponseCallback&)>& func);
template
Status LeaderMasterProxy::SyncRpc(
    const master::ListTabletServersRequestPB& req,
    master::ListTabletServersResponsePB* resp,
    string func_name,
    const boost::function<void(MasterServiceProxy*,
                               const master::ListTabletServersRequestPB&,
                               master::ListTabletServersResponsePB*,
                               RpcController*,
                               const ResponseCallback&)>& func);
template
Status LeaderMasterProxy::SyncRpc(
    const master::ListMastersRequestPB& req,
    master::ListMastersResponsePB* resp,
    string func_name,
    const boost::function<void(MasterServiceProxy*,
                               const master::ListMastersRequestPB&,
                               master::ListMastersResponsePB*,
                               RpcController*,
                               const ResponseCallback&)>& func);
template
Status LeaderMasterProxy::SyncRpc(
    const master::ReplaceTabletRequestPB& req,
    master::ReplaceTabletResponsePB* resp,
    string func_name,
    const boost::function<void(MasterServiceProxy*,
                               const master::ReplaceTabletRequestPB&,
                               master::ReplaceTabletResponsePB*,
                               RpcController*,
                               const ResponseCallback&)>& func);

} // namespace tools
} // namespace kudu
