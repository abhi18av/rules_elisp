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

#ifndef PHST_RULES_ELISP_ELISP_EXEC_H
#define PHST_RULES_ELISP_ELISP_EXEC_H

#include <initializer_list>

#include "absl/base/attributes.h"

namespace phst_rules_elisp {

enum class Mode { kDirect, kWrap };

ABSL_MUST_USE_RESULT int RunEmacs(const char* install_rel, int argc,
                                  const char* const* argv);

ABSL_MUST_USE_RESULT int RunBinary(
    const char* wrapper, Mode mode,
    std::initializer_list<const char*> load_path,
    std::initializer_list<const char*> load_files,
    std::initializer_list<const char*> data_files, int argc,
    const char* const* argv);

ABSL_MUST_USE_RESULT int RunTest(const char* wrapper, Mode mode,
                                 std::initializer_list<const char*> load_path,
                                 std::initializer_list<const char*> srcs,
                                 std::initializer_list<const char*> data_files,
                                 int argc, const char* const* argv);

}  // namespace phst_rules_elisp

#endif