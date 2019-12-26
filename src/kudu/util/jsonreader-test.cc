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

#include "kudu/util/jsonreader.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <glog/logging.h> // IWYU pragma: keep
#include <gtest/gtest.h>
#include <rapidjson/document.h>

#include "kudu/gutil/integral_types.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/status.h"
#include "kudu/util/test_macros.h"

using rapidjson::Value;
using std::string;
using std::vector;
using strings::Substitute;

namespace kudu {

TEST(JsonReaderTest, Corrupt) {
  JsonReader r("");
  Status s = r.Init();
  ASSERT_TRUE(s.IsCorruption());
  ASSERT_STR_CONTAINS(
      s.ToString(), "JSON text is corrupt: The document is empty.");
}

TEST(JsonReaderTest, Empty) {
  JsonReader r("{}");
  ASSERT_OK(r.Init());
  JsonReader r2("[]");
  ASSERT_OK(r2.Init());

  // Not found.
  ASSERT_TRUE(r.ExtractBool(r.root(), "foo", nullptr).IsNotFound());
  ASSERT_TRUE(r.ExtractInt32(r.root(), "foo", nullptr).IsNotFound());
  ASSERT_TRUE(r.ExtractInt64(r.root(), "foo", nullptr).IsNotFound());
  ASSERT_TRUE(r.ExtractUint32(r.root(), "foo", nullptr).IsNotFound());
  ASSERT_TRUE(r.ExtractUint64(r.root(), "foo", nullptr).IsNotFound());
  ASSERT_TRUE(r.ExtractDouble(r.root(), "foo", nullptr).IsNotFound());
  ASSERT_TRUE(r.ExtractFloat(r.root(), "foo", nullptr).IsNotFound());
  ASSERT_TRUE(r.ExtractString(r.root(), "foo", nullptr).IsNotFound());
  ASSERT_TRUE(r.ExtractObject(r.root(), "foo", nullptr).IsNotFound());
  ASSERT_TRUE(r.ExtractObjectArray(r.root(), "foo", nullptr).IsNotFound());
}

TEST(JsonReaderTest, Basic) {
  JsonReader r("{ \"foo\" : \"bar\" }");
  ASSERT_OK(r.Init());
  string foo;
  ASSERT_OK(r.ExtractString(r.root(), "foo", &foo));
  ASSERT_EQ("bar", foo);

  // Bad types.
  ASSERT_TRUE(r.ExtractBool(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt32(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt64(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint32(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractDouble(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractFloat(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObject(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObjectArray(r.root(), "foo", nullptr).IsInvalidArgument());
}

TEST(JsonReaderTest, LessBasic) {
  string doc = Substitute(
      "{ \"small\" : 1, \"big\" : $0, \"null\" : null, \"empty\" : \"\", \"bool\" : true }",
      kint64max);
  JsonReader r(doc);
  ASSERT_OK(r.Init());
  int32_t small;
  ASSERT_OK(r.ExtractInt32(r.root(), "small", &small));
  ASSERT_EQ(1, small);
  int64_t big;
  ASSERT_OK(r.ExtractInt64(r.root(), "big", &big));
  ASSERT_EQ(kint64max, big);
  string str;
  ASSERT_OK(r.ExtractString(r.root(), "null", &str));
  ASSERT_EQ("", str);
  ASSERT_OK(r.ExtractString(r.root(), "empty", &str));
  ASSERT_EQ("", str);
  bool b;
  ASSERT_OK(r.ExtractBool(r.root(), "bool", &b));
  ASSERT_TRUE(b);

  // Bad types.
  ASSERT_TRUE(r.ExtractBool(r.root(), "small", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractString(r.root(), "small", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObject(r.root(), "small", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObjectArray(r.root(), "small", nullptr).IsInvalidArgument());

  ASSERT_TRUE(r.ExtractBool(r.root(), "big", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt32(r.root(), "big", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractString(r.root(), "big", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObject(r.root(), "big", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObjectArray(r.root(), "big", nullptr).IsInvalidArgument());

  ASSERT_TRUE(r.ExtractBool(r.root(), "null", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt32(r.root(), "null", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt64(r.root(), "null", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint32(r.root(), "null", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), "null", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractDouble(r.root(), "null", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractFloat(r.root(), "null", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObject(r.root(), "null", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObjectArray(r.root(), "null", nullptr).IsInvalidArgument());

  ASSERT_TRUE(r.ExtractBool(r.root(), "empty", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt32(r.root(), "empty", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt64(r.root(), "empty", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint32(r.root(), "empty", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), "empty", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractDouble(r.root(), "empty", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractFloat(r.root(), "empty", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObject(r.root(), "empty", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObjectArray(r.root(), "empty", nullptr).IsInvalidArgument());

  ASSERT_TRUE(r.ExtractInt32(r.root(), "bool", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt64(r.root(), "bool", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint32(r.root(), "bool", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), "bool", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractDouble(r.root(), "bool", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractFloat(r.root(), "bool", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractString(r.root(), "bool", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObject(r.root(), "bool", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObjectArray(r.root(), "bool", nullptr).IsInvalidArgument());
}

TEST(JsonReaderTest, SignedAndUnsignedInts) {
  constexpr auto kMaxInt32 = std::numeric_limits<int32_t>::max();
  constexpr auto kMaxInt64 = std::numeric_limits<int64_t>::max();
  constexpr auto kMaxUint32 = std::numeric_limits<uint32_t>::max();
  constexpr auto kMaxUint64 = std::numeric_limits<uint64_t>::max();
  constexpr auto kMinInt32 = std::numeric_limits<int32_t>::min();
  constexpr auto kMinInt64 = std::numeric_limits<int64_t>::min();
  const string doc = Substitute(
      "{ \"negative\" : -1, \"signed_big32\" : $0, \"signed_big64\" : $1, "
      "\"unsigned_big32\" : $2, \"unsigned_big64\" : $3, "
      "\"signed_small32\" : $4, \"signed_small64\" : $5 }",
      kMaxInt32, kMaxInt64, kMaxUint32, kMaxUint64, kMinInt32, kMinInt64);
  JsonReader r(doc);
  ASSERT_OK(r.Init());

  // -1.
  const char* const negative = "negative";
  int32_t negative32;
  ASSERT_OK(r.ExtractInt32(r.root(), negative, &negative32));
  ASSERT_EQ(-1, negative32);
  int64_t negative64;
  ASSERT_OK(r.ExtractInt64(r.root(), negative, &negative64));
  ASSERT_EQ(-1, negative64);
  ASSERT_TRUE(r.ExtractUint32(r.root(), negative, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), negative, nullptr).IsInvalidArgument());
  double negative_double;
  ASSERT_OK(r.ExtractDouble(r.root(), negative, &negative_double));
  ASSERT_EQ(-1, negative_double);
  float negative_float;
  ASSERT_OK(r.ExtractFloat(r.root(), negative, &negative_float));
  ASSERT_EQ(-1, negative_float);

  // Max signed 32-bit integer.
  const char* const signed_big32 = "signed_big32";
  int32_t signed_big32_int32;
  ASSERT_OK(r.ExtractInt32(r.root(), signed_big32, &signed_big32_int32));
  ASSERT_EQ(kMaxInt32, signed_big32_int32);
  int64_t signed_big32_int64;
  ASSERT_OK(r.ExtractInt64(r.root(), signed_big32, &signed_big32_int64));
  ASSERT_EQ(kMaxInt32, signed_big32_int64);
  uint32_t signed_big32_uint32;
  ASSERT_OK(r.ExtractUint32(r.root(), signed_big32, &signed_big32_uint32));
  ASSERT_EQ(kMaxInt32, signed_big32_uint32);
  uint64_t signed_big32_uint64;
  ASSERT_OK(r.ExtractUint64(r.root(), signed_big32, &signed_big32_uint64));
  ASSERT_EQ(kMaxInt32, signed_big32_uint64);
  double signed_big32_double;
  ASSERT_OK(r.ExtractDouble(r.root(), signed_big32, &signed_big32_double));
  ASSERT_EQ(kMaxInt32, signed_big32_double);
  ASSERT_TRUE(r.ExtractFloat(r.root(), signed_big32, nullptr).IsInvalidArgument());

  // Max signed 64-bit integer.
  const char* const signed_big64 = "signed_big64";
  ASSERT_TRUE(r.ExtractInt32(r.root(), signed_big64, nullptr).IsInvalidArgument());
  int64_t signed_big64_int64;
  ASSERT_OK(r.ExtractInt64(r.root(), signed_big64, &signed_big64_int64));
  ASSERT_EQ(kMaxInt64, signed_big64_int64);
  ASSERT_TRUE(r.ExtractUint32(r.root(), signed_big64, nullptr).IsInvalidArgument());
  uint64_t signed_big64_uint64;
  ASSERT_OK(r.ExtractUint64(r.root(), signed_big64, &signed_big64_uint64));
  ASSERT_EQ(kMaxInt64, signed_big64_uint64);
  ASSERT_TRUE(r.ExtractDouble(r.root(), signed_big64, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractFloat(r.root(), signed_big64, nullptr).IsInvalidArgument());

  // Max unsigned 32-bit integer.
  const char* const unsigned_big32 = "unsigned_big32";
  ASSERT_TRUE(r.ExtractInt32(r.root(), unsigned_big32, nullptr).IsInvalidArgument());
  int64_t unsigned_big32_int64;
  ASSERT_OK(r.ExtractInt64(r.root(), unsigned_big32, &unsigned_big32_int64));
  ASSERT_EQ(kMaxUint32, unsigned_big32_int64);
  uint32_t unsigned_big32_uint32;
  ASSERT_OK(r.ExtractUint32(r.root(), unsigned_big32, &unsigned_big32_uint32));
  ASSERT_EQ(kMaxUint32, unsigned_big32_uint32);
  uint64_t unsigned_big32_uint64;
  ASSERT_OK(r.ExtractUint64(r.root(), unsigned_big32, &unsigned_big32_uint64));
  ASSERT_EQ(kMaxUint32, unsigned_big32_uint64);
  double unsigned_big32_double;
  ASSERT_OK(r.ExtractDouble(r.root(), unsigned_big32, &unsigned_big32_double));
  ASSERT_EQ(kMaxUint32, unsigned_big32_double);
  ASSERT_TRUE(r.ExtractFloat(r.root(), unsigned_big32, nullptr).IsInvalidArgument());

  // Max unsigned 64-bit integer.
  const char* const unsigned_big64 = "unsigned_big64";
  ASSERT_TRUE(r.ExtractInt32(r.root(), unsigned_big64, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt64(r.root(), unsigned_big64, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint32(r.root(), unsigned_big64, nullptr).IsInvalidArgument());
  uint64_t unsigned_big64_uint64;
  ASSERT_OK(r.ExtractUint64(r.root(), unsigned_big64, &unsigned_big64_uint64));
  ASSERT_EQ(kMaxUint64, unsigned_big64_uint64);
  ASSERT_TRUE(r.ExtractDouble(r.root(), unsigned_big64, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractFloat(r.root(), unsigned_big64, nullptr).IsInvalidArgument());

  // Min signed 32-bit integer.
  const char* const signed_small32 = "signed_small32";
  int32_t small32_int32;
  ASSERT_OK(r.ExtractInt32(r.root(), signed_small32, &small32_int32));
  ASSERT_EQ(kMinInt32, small32_int32);
  int64_t small32_int64;
  ASSERT_OK(r.ExtractInt64(r.root(), signed_small32, &small32_int64));
  ASSERT_EQ(kMinInt32, small32_int64);
  ASSERT_TRUE(r.ExtractUint32(r.root(), signed_small32, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), signed_small32, nullptr).IsInvalidArgument());
  double small32_double;
  ASSERT_OK(r.ExtractDouble(r.root(), signed_small32, &small32_double));
  ASSERT_EQ(kMinInt32, small32_double);
  float small32_float;
  ASSERT_OK(r.ExtractFloat(r.root(), signed_small32, &small32_float));
  ASSERT_EQ(kMinInt32, small32_float);

  // Min signed 64-bit integer.
  const char* const signed_small64 = "signed_small64";
  ASSERT_TRUE(r.ExtractInt32(r.root(), signed_small64, nullptr).IsInvalidArgument());
  int64_t small64_int64;
  ASSERT_OK(r.ExtractInt64(r.root(), signed_small64, &small64_int64));
  ASSERT_EQ(kMinInt64, small64_int64);
  ASSERT_TRUE(r.ExtractUint32(r.root(), signed_small64, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), signed_small64, nullptr).IsInvalidArgument());
  double small64_double;
  ASSERT_OK(r.ExtractDouble(r.root(), signed_small64, &small64_double));
  ASSERT_EQ(kMinInt64, small64_double);
  float small64_float;
  ASSERT_OK(r.ExtractFloat(r.root(), signed_small64, &small64_float));
  ASSERT_EQ(kMinInt64, small64_float);
}

TEST(JsonReaderTest, Doubles) {
  JsonReader r("{ \"foo\" : 5.125 }");
  ASSERT_OK(r.Init());

  double foo;
  ASSERT_OK(r.ExtractDouble(r.root(), "foo", &foo));
  ASSERT_EQ(5.125, foo);

  // Bad types.
  ASSERT_TRUE(r.ExtractBool(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt32(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt64(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint32(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractString(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObject(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObjectArray(r.root(), "foo", nullptr).IsInvalidArgument());
}

TEST(JsonReaderTest, Floats) {
  JsonReader r("{ \"foo\" : 5.125 }");
  ASSERT_OK(r.Init());

  float foo;
  ASSERT_OK(r.ExtractFloat(r.root(), "foo", &foo));
  ASSERT_EQ(5.125, foo);

  // Bad types.
  ASSERT_TRUE(r.ExtractBool(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt32(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt64(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint32(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractString(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObject(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObjectArray(r.root(), "foo", nullptr).IsInvalidArgument());
}

TEST(JsonReaderTest, Objects) {
  JsonReader r("{ \"foo\" : { \"1\" : 1 } }");
  ASSERT_OK(r.Init());

  const Value* foo = nullptr;
  ASSERT_OK(r.ExtractObject(r.root(), "foo", &foo));
  ASSERT_TRUE(foo);

  int32_t one;
  ASSERT_OK(r.ExtractInt32(foo, "1", &one));
  ASSERT_EQ(1, one);

  // Bad types.
  ASSERT_TRUE(r.ExtractBool(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt32(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt64(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint32(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractDouble(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractFloat(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractString(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObjectArray(r.root(), "foo", nullptr).IsInvalidArgument());
}

TEST(JsonReaderTest, TopLevelArray) {
  JsonReader r("[ { \"name\" : \"foo\" }, { \"name\" : \"bar\" } ]");
  ASSERT_OK(r.Init());

  vector<const Value*> objs;
  ASSERT_OK(r.ExtractObjectArray(r.root(), nullptr, &objs));
  ASSERT_EQ(2, objs.size());
  string name;
  ASSERT_OK(r.ExtractString(objs[0], "name", &name));
  ASSERT_EQ("foo", name);
  ASSERT_OK(r.ExtractString(objs[1], "name", &name));
  ASSERT_EQ("bar", name);

  // Bad types.
  ASSERT_TRUE(r.ExtractBool(r.root(), nullptr, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt32(r.root(), nullptr, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt64(r.root(), nullptr, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint32(r.root(), nullptr, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractUint64(r.root(), nullptr, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractDouble(r.root(), nullptr, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractFloat(r.root(), nullptr, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractString(r.root(), nullptr, nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObject(r.root(), nullptr, nullptr).IsInvalidArgument());
}

TEST(JsonReaderTest, NestedArray) {
  JsonReader r("{ \"foo\" : [ { \"val\" : 0 }, { \"val\" : 1 }, { \"val\" : 2 } ] }");
  ASSERT_OK(r.Init());

  vector<const Value*> foo;
  ASSERT_OK(r.ExtractObjectArray(r.root(), "foo", &foo));
  ASSERT_EQ(3, foo.size());
  int i = 0;
  for (const Value* v : foo) {
    int32_t number;
    ASSERT_OK(r.ExtractInt32(v, "val", &number));
    ASSERT_EQ(i, number);
    i++;
  }

  // Bad types.
  ASSERT_TRUE(r.ExtractBool(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt32(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractInt64(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractString(r.root(), "foo", nullptr).IsInvalidArgument());
  ASSERT_TRUE(r.ExtractObject(r.root(), "foo", nullptr).IsInvalidArgument());
}

} // namespace kudu
