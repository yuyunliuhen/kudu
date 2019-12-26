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

#include "kudu/sentry/sentry_action.h"

#include <cstdint>

#include <ostream>
#include <string>

#include <boost/algorithm/string/predicate.hpp>
#include <glog/logging.h>

#include "kudu/gutil/strings/substitute.h"

using std::string;
using strings::Substitute;

namespace kudu {
namespace sentry {

const char* ActionToString(SentryAction::Action action) {
  switch (action) {
    case SentryAction::Action::UNINITIALIZED: return "UNINITIALIZED";
    case SentryAction::Action::ALL: return kActionAll;
    case SentryAction::Action::METADATA: return kActionMetadata;
    case SentryAction::Action::SELECT: return kActionSelect;
    case SentryAction::Action::INSERT: return kActionInsert;
    case SentryAction::Action::UPDATE: return kActionUpdate;
    case SentryAction::Action::DELETE: return kActionDelete;
    case SentryAction::Action::ALTER: return kActionAlter;
    case SentryAction::Action::CREATE: return kActionCreate;
    case SentryAction::Action::DROP: return kActionDrop;
    case SentryAction::Action::OWNER: return kActionOwner;
  }
  LOG(FATAL) << static_cast<uint16_t>(action) << ": unknown action";
  return nullptr;
}

std::ostream& operator<<(std::ostream& o, SentryAction::Action action) {
  return o << ActionToString(action);
}

const char* const SentryAction::kWildCard = "*";

SentryAction::SentryAction()
  : action_(Action::UNINITIALIZED) {
}

SentryAction::SentryAction(Action action)
  : action_(action) {
}

Status SentryAction::FromString(const string& str, SentryAction* action) {
  // Consider action '*' equals to ALL to be compatible with the existing
  // Java Sentry client.
  //
  // See org.apache.sentry.api.service.thrift.SentryPolicyServiceClientDefaultImpl.
  if (boost::iequals(str, kActionAll) || str == kWildCard) {
    action->action_ = Action::ALL;
  } else if (boost::iequals(str, kActionMetadata)) {
    action->action_ = Action::METADATA;
  } else if (boost::iequals(str, kActionSelect)) {
    action->action_ = Action::SELECT;
  } else if (boost::iequals(str, kActionInsert)) {
    action->action_ = Action::INSERT;
  } else if (boost::iequals(str, kActionUpdate)) {
    action->action_ = Action::UPDATE;
  } else if (boost::iequals(str, kActionDelete)) {
    action->action_ = Action::DELETE;
  } else if (boost::iequals(str, kActionAlter)) {
    action->action_ = Action::ALTER;
  } else if (boost::iequals(str, kActionCreate)) {
    action->action_ = Action::CREATE;
  } else if (boost::iequals(str, kActionDrop)) {
    action->action_ = Action::DROP;
  } else if (boost::iequals(str, kActionOwner)) {
    action->action_ = Action::OWNER;
  } else {
    return Status::InvalidArgument(Substitute("unknown SentryAction: $0", str));
  }

  return Status::OK();
}

bool SentryAction::Implies(const SentryAction& other) const {
  // Every action must be initialized.
  CHECK_NE(action(), Action::UNINITIALIZED);
  CHECK_NE(other.action(), Action::UNINITIALIZED);

  // Action ALL and OWNER subsume every other action.
  if (action() == Action::ALL ||
      action() == Action::OWNER) {
    return true;
  }

  // Any action subsumes Action METADATA.
  if (other.action() == Action::METADATA) {
    return true;
  }

  return action() == other.action();
}

} // namespace sentry
} // namespace kudu
