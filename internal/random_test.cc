// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "internal/random.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace phst_rules_elisp {
namespace {

using ::testing::StrNe;
using ::testing::StartsWith;
using ::testing::EndsWith;

TEST(random, temp_name) {
  random rnd;
  const auto a = rnd.temp_name("temp-*.json");
  const auto b = rnd.temp_name("temp-*.json");
  EXPECT_THAT(a, StartsWith("temp-"));
  EXPECT_THAT(a, EndsWith(".json"));
  EXPECT_THAT(b, StartsWith("temp-"));
  EXPECT_THAT(b, EndsWith(".json"));
  EXPECT_THAT(b, StrNe(a));
}

}  // namespace
}  // namespace phst_rules_elisp