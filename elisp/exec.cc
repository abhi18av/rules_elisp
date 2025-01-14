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

#include "elisp/exec.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <regex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Woverflow"
#include "absl/algorithm/container.h"
#include "absl/base/casts.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/types/optional.h"
#include "absl/utility/utility.h"
#include "nlohmann/json.hpp"
#include "tools/cpp/runfiles/runfiles.h"
#pragma GCC diagnostic pop

#include "elisp/file.h"
#include "elisp/status.h"
#include "elisp/str.h"

#ifdef __APPLE__
#include <crt_externs.h>  // for _NSGetEnviron
#endif

namespace phst_rules_elisp {

using bazel::tools::cpp::runfiles::Runfiles;
using Environment = absl::flat_hash_map<std::string, std::string>;

namespace {

template <typename T>
std::vector<T> Sort(const absl::flat_hash_set<T>& set) {
  std::vector<T> result(set.begin(), set.end());
  absl::c_sort(result);
  return result;
}

}  // namespace

static std::vector<char*> Pointers(std::vector<std::string>& strings) {
  std::vector<char*> ptrs;
  for (auto& s : strings) {
    if (s.empty()) {
      std::clog << "empty string" << std::endl;
      std::abort();
    }
    if (s.find('\0') != s.npos) {
      std::clog << s << " contains null character" << std::endl;
      std::abort();
    }
    ptrs.push_back(&s.front());
  }
  ptrs.push_back(nullptr);
  return ptrs;
}

static Environment CopyEnv() {
  const char* const* const envp =
#ifdef __APPLE__
      // See environ(7) why this is necessary.
      *_NSGetEnviron()
#else
      environ
#endif
      ;
  Environment map;
  for (const char* const* pp = envp; *pp != nullptr; ++pp) {
    const char* p = *pp;
    const char* q = std::strchr(p, '=');
    if (q != nullptr) {
      map.emplace(std::string(p, q), std::string(q + 1));
    }
  }
  return map;
}

static absl::StatusOr<std::unique_ptr<Runfiles>> CreateRunfiles(
    const std::string& argv0) {
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::Create(argv0, &error));
  if (runfiles == nullptr) {
    return absl::FailedPreconditionError(
        absl::StrCat("couldn’t create runfiles: ", error));
  }
  return std::move(runfiles);
}

static absl::StatusOr<std::unique_ptr<Runfiles>> CreateRunfilesForTest() {
  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));
  if (runfiles == nullptr) {
    return absl::FailedPreconditionError("couldn’t create runfiles for test: " +
                                         error);
  }
  return std::move(runfiles);
}

static absl::StatusOr<std::string> GetSharedDir(const std::string& install) {
  const auto emacs = JoinPath(install, "share", "emacs");
  ASSIGN_OR_RETURN(auto dir, Directory::Open(emacs));
  absl::flat_hash_set<std::string> dirs;
  while (true) {
    ASSIGN_OR_RETURN(const auto entry, dir.Read());
    if (entry.empty()) break;
    if (std::regex_match(entry, std::regex("[0-9][.0-9]*"))) {
      dirs.insert(entry);
    }
  }
  RETURN_IF_ERROR(dir.Close());
  if (dirs.empty()) return absl::NotFoundError("no shared directory found");
  if (dirs.size() != 1) {
    return absl::FailedPreconditionError(
        absl::StrCat("expected exactly one shared directory, got [",
                     absl::StrJoin(dirs, ", "), "]"));
  }
  return JoinPath(emacs, *dirs.begin());
}

static absl::StatusOr<std::string> FindDumpFile(const std::string& libexec) {
  const auto emacs = JoinPath(libexec, "emacs");
  ASSIGN_OR_RETURN(auto dir, Directory::Open(emacs));
  absl::flat_hash_set<std::string> files;
  while (true) {
    ASSIGN_OR_RETURN(const auto entry, dir.Read());
    if (entry.empty()) break;
    if (entry.front() != '.') {
      const auto version = JoinPath(emacs, entry);
      ASSIGN_OR_RETURN(auto dir, Directory::Open(version));
      while (true) {
        ASSIGN_OR_RETURN(const auto entry, dir.Read());
        if (entry.empty()) break;
        if (entry.front() != '.') {
          const auto arch = JoinPath(version, entry);
          ASSIGN_OR_RETURN(auto dir, Directory::Open(arch));
          while (true) {
            ASSIGN_OR_RETURN(const auto entry, dir.Read());
            if (entry.empty()) break;
            if (entry == "emacs.pdmp") {
              files.insert(JoinPath(arch, entry));
            }
          }
          RETURN_IF_ERROR(dir.Close());
        }
      }
      RETURN_IF_ERROR(dir.Close());
    }
  }
  RETURN_IF_ERROR(dir.Close());
  if (files.empty()) return absl::NotFoundError("no portable dump file found");
  if (files.size() != 1) {
    return absl::FailedPreconditionError(
        absl::StrCat("expected exactly one dump file, got [",
                     absl::StrJoin(files, ", "), "]"));
  }
  return *files.begin();
}

static absl::StatusOr<absl::optional<TempFile>> AddManifest(
    const Mode mode, std::vector<std::string>& args, absl::BitGen& random) {
  using Type = absl::optional<TempFile>;
  if (mode == Mode::kDirect) return absl::implicit_cast<Type>(absl::nullopt);
  ASSIGN_OR_RETURN(auto stream,
                   TempFile::Create(TempDir(), "manifest-*.json", random));
  args.push_back(absl::StrCat("--manifest=", stream.path()));
  args.push_back("--");
  return absl::implicit_cast<Type>(std::move(stream));
}

static void CheckRelative(const std::vector<std::string>& files) {
  for (const std::string& file : files) {
    if (IsAbsolute(file)) {
      std::clog << "filename " << file << " is absolute" << std::endl;
      std::abort();
    }
  }
}

static absl::Status WriteManifest(const CommonOptions& opts,
                                  std::vector<std::string> input_files,
                                  const std::vector<std::string>& output_files,
                                  TempFile& file) {
  CheckRelative(opts.load_path);
  CheckRelative(opts.load_files);
  const auto data_files = Sort(opts.data_files);
  CheckRelative(data_files);
  input_files.insert(input_files.end(),
                     opts.load_files.begin(), opts.load_files.end());
  input_files.insert(input_files.end(), data_files.begin(), data_files.end());
  const nlohmann::json json = {
      {"root", "RUNFILES_ROOT"},
      {"loadPath", opts.load_path},
      {"inputFiles", input_files},
      {"outputFiles", output_files},
      {"tags", Sort(opts.rule_tags)},
  };
  return file.Write(json.dump());
}

namespace {

class Executor {
 public:
  static absl::StatusOr<Executor> Create(std::vector<std::string> argv);
  static absl::StatusOr<Executor> CreateForTest(std::vector<std::string> argv);

  Executor(const Executor&) = delete;
  Executor(Executor&&) = default;
  Executor& operator=(const Executor&) = delete;
  Executor& operator=(Executor&&) = default;

  absl::StatusOr<int> RunEmacs(const EmacsOptions& opts);
  absl::StatusOr<int> RunBinary(const BinaryOptions& opts);
  absl::StatusOr<int> RunTest(const TestOptions& opts);

 private:
  explicit Executor(std::vector<std::string> argv,
                    std::unique_ptr<Runfiles> runfiles);

  absl::StatusOr<std::string> Runfile(const std::string& rel) const;
  std::string RunfilesDir() const;
  std::string EnvVar(const std::string& name) const noexcept;

  absl::Status AddLoadPath(std::vector<std::string>& args,
                           const std::vector<std::string>& load_path) const;

  absl::StatusOr<int> Run(const std::string& binary,
                          const std::vector<std::string>& args,
                          const Environment& env);

  std::vector<std::string> BuildArgs(
      const std::vector<std::string>& prefix) const;

  std::vector<std::string> BuildEnv(const Environment& other) const;

  absl::StatusOr<std::vector<std::string>> ArgFiles(
      const std::string& root, const absl::flat_hash_set<int>& indices) const;

  std::vector<std::string> orig_args_;
  Environment orig_env_ = CopyEnv();
  std::unique_ptr<Runfiles> runfiles_;
  absl::BitGen random_;
};

absl::StatusOr<Executor> Executor::Create(std::vector<std::string> argv) {
  ASSIGN_OR_RETURN(auto runfiles, CreateRunfiles(argv.at(0)));
  return Executor(std::move(argv), std::move(runfiles));
}

absl::StatusOr<Executor> Executor::CreateForTest(
    std::vector<std::string> argv) {
  ASSIGN_OR_RETURN(auto runfiles, CreateRunfilesForTest());
  return Executor(std::move(argv), std::move(runfiles));
}

Executor::Executor(std::vector<std::string> argv,
                   std::unique_ptr<Runfiles> runfiles)
    : orig_args_(std::move(argv)), runfiles_(std::move(runfiles)) {}

absl::StatusOr<int> Executor::RunEmacs(const EmacsOptions& opts) {
  ASSIGN_OR_RETURN(const auto install, this->Runfile(opts.install_rel));
  const auto emacs = JoinPath(install, "bin", "emacs");
  ASSIGN_OR_RETURN(const auto shared, GetSharedDir(install));
  const auto etc = JoinPath(shared, "etc");
  const auto libexec = JoinPath(install, "libexec");
  std::vector<std::string> args;
  switch (opts.dump_mode) {
    case DumpMode::kPortable: {
      ASSIGN_OR_RETURN(const auto dump, FindDumpFile(libexec));
      args.push_back("--dump-file=" + dump);
      break;
    }
    case DumpMode::kUnexec:
      break;
  }
  Environment map;
  map.emplace("EMACSDATA", etc);
  map.emplace("EMACSDOC", etc);
  map.emplace("EMACSLOADPATH", JoinPath(shared, "lisp"));
  map.emplace("EMACSPATH", libexec);
  return this->Run(emacs, args, map);
}

absl::StatusOr<int> Executor::RunBinary(const BinaryOptions& opts) {
  ASSIGN_OR_RETURN(const auto emacs, this->Runfile(opts.wrapper));
  std::vector<std::string> args;
  ASSIGN_OR_RETURN(auto manifest, AddManifest(opts.mode, args, random_));
  args.push_back("--quick");
  args.push_back("--batch");
  RETURN_IF_ERROR(this->AddLoadPath(args, opts.load_path));
  for (const auto& file : opts.load_files) {
    ASSIGN_OR_RETURN(const auto abs, this->Runfile(file));
    args.push_back(absl::StrCat("--load=", abs));
  }
  if (manifest) {
    const auto runfiles = this->RunfilesDir();
    ASSIGN_OR_RETURN(auto input_files,
                     this->ArgFiles(runfiles, opts.input_args));
    ASSIGN_OR_RETURN(auto output_files,
                     this->ArgFiles(runfiles, opts.output_args));
    RETURN_IF_ERROR(WriteManifest(opts, std::move(input_files),
                                  std::move(output_files), manifest.value()));
  }
  ASSIGN_OR_RETURN(const auto code, this->Run(emacs, args, {}));
  if (manifest) RETURN_IF_ERROR(manifest->Close());
  return code;
}

absl::StatusOr<int> Executor::RunTest(const TestOptions& opts) {
  ASSIGN_OR_RETURN(const auto emacs, this->Runfile(opts.wrapper));
  std::vector<std::string> args;
  ASSIGN_OR_RETURN(auto manifest, AddManifest(opts.mode, args, random_));
  args.push_back("--quick");
  args.push_back("--batch");
  args.push_back("--module-assertions");
  RETURN_IF_ERROR(this->AddLoadPath(args, opts.load_path));
  ASSIGN_OR_RETURN(const auto runner,
                   this->Runfile("phst_rules_elisp/elisp/ert/runner.elc"));
  args.push_back(absl::StrCat("--load=", runner));
  // Note that using equals signs for "--test-source, --skip-test, and
  // --skip-tag doesn’t work.
  for (const auto& file : opts.load_files) {
    ASSIGN_OR_RETURN(const auto abs, this->Runfile(file));
    args.push_back("--test-source");
    args.push_back(absl::StrCat("/:", abs));
  }
  for (const auto& test : Sort(opts.skip_tests)) {
    args.push_back("--skip-test");
    args.push_back(test);
  }
  for (const auto& tag : Sort(opts.skip_tags)) {
    args.push_back("--skip-tag");
    args.push_back(tag);
  }
  args.push_back("--funcall=elisp/ert/run-batch-and-exit");
  if (manifest) {
    std::vector<std::string> inputs, outputs;
    const auto report_file = this->EnvVar("XML_OUTPUT_FILE");
    if (!report_file.empty()) {
      outputs.push_back(report_file);
    }
    if (this->EnvVar("COVERAGE") == "1") {
      std::string coverage_manifest = this->EnvVar("COVERAGE_MANIFEST");
      if (!coverage_manifest.empty()) {
        inputs.push_back(std::move(coverage_manifest));
      }
      const auto coverage_dir = this->EnvVar("COVERAGE_DIR");
      if (!coverage_dir.empty()) {
        outputs.push_back(JoinPath(coverage_dir, "emacs-lisp.dat"));
      }
    }
    RETURN_IF_ERROR(
        WriteManifest(opts, std::move(inputs), outputs, manifest.value()));
  }
  ASSIGN_OR_RETURN(const auto code, this->Run(emacs, args, {}));
  if (manifest) RETURN_IF_ERROR(manifest->Close());
  return code;
}

absl::StatusOr<std::string> Executor::Runfile(const std::string& rel) const {
  const std::string str = runfiles_->Rlocation(rel);
  if (str.empty()) {
    return absl::NotFoundError(absl::StrCat("runfile not found: ", rel));
  }
  // Note: Don’t canonicalize the filename here, because the Python stub looks
  // for the runfiles directory in the original filename.
  return MakeAbsolute(str);
}

std::string Executor::RunfilesDir() const {
  const std::string vars[] = {"RUNFILES_DIR", "TEST_SRCDIR"};
  for (const auto& var : vars) {
    auto value = this->EnvVar(var);
    if (!value.empty()) return value;
  }
  return std::string();
}

std::string Executor::EnvVar(const std::string& name) const noexcept {
  const auto it = orig_env_.find(name);
  return it == orig_env_.end() ? std::string() : it->second;
}

absl::Status Executor::AddLoadPath(
    std::vector<std::string>& args,
    const std::vector<std::string>& load_path) const {
  constexpr const char* const runfiles_elc =
      "phst_rules_elisp/elisp/runfiles/runfiles.elc";
  bool runfile_handler_installed = false;
  for (const auto& dir : load_path) {
    const auto status_or_dir = this->Runfile(dir);
    if (status_or_dir.ok()) {
      args.push_back(absl::StrCat("--directory=", status_or_dir.value()));
    } else if (absl::IsNotFound(status_or_dir.status())) {
      if (!absl::exchange(runfile_handler_installed, true)) {
        ASSIGN_OR_RETURN(const auto file, this->Runfile(runfiles_elc));
        args.push_back(absl::StrCat("--load=", file));
        args.push_back("--funcall=elisp/runfiles/install-handler");
      }
      args.push_back(absl::StrCat("--directory=/bazel-runfile:", dir));
    } else {
      return status_or_dir.status();
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<int> Executor::Run(const std::string& binary,
                                  const std::vector<std::string>& args,
                                  const Environment& env) {
  auto final_args = this->BuildArgs(args);
  const auto argv = Pointers(final_args);
  auto final_env = this->BuildEnv(env);
  const auto envp = Pointers(final_env);
  pid_t pid;
  const int error = posix_spawn(&pid, Pointer(binary), nullptr, nullptr,
                                argv.data(), envp.data());
  if (error != 0) {
    return ErrorStatus(std::error_code(error, std::system_category()),
                       "posix_spawn", binary);
  }
  int wstatus;
  const pid_t status = waitpid(pid, &wstatus, 0);
  if (status != pid) return ErrnoStatus("waitpid", pid);
  return WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 0xFF;
}

std::vector<std::string> Executor::BuildArgs(
    const std::vector<std::string>& prefix) const {
  std::vector<std::string> vec{orig_args_.at(0)};
  vec.insert(vec.end(), prefix.begin(), prefix.end());
  vec.insert(vec.end(), std::next(orig_args_.begin()), orig_args_.end());
  return vec;
}

std::vector<std::string> Executor::BuildEnv(const Environment& other) const {
  const auto& pairs = runfiles_->EnvVars();
  Environment map(pairs.begin(), pairs.end());
  map.insert(other.begin(), other.end());
  map.insert(orig_env_.begin(), orig_env_.end());
  std::vector<std::string> vec;
  for (const auto& p : map) {
    vec.push_back(absl::StrCat(p.first, "=", p.second));
  }
  // Sort entries for hermeticity.
  absl::c_sort(vec);
  return vec;
}

absl::StatusOr<std::vector<std::string>> Executor::ArgFiles(
    const std::string& root, const absl::flat_hash_set<int>& indices) const {
  // The assertion holds because orig_args_ was constructed from argc and argv,
  // so it necessarily has fewer than std::numeric_limits<int>::max() elements.
  assert(orig_args_.size() <
         static_cast<unsigned int>(std::numeric_limits<int>::max()));
  const int argc = static_cast<int>(orig_args_.size());
  std::vector<std::string> result;
  for (int i : Sort(indices)) {
    if (i < 0) i += argc;
    if (i >= 0 && i < argc) {
      absl::string_view arg = orig_args_[static_cast<unsigned int>(i)];
      // File arguments are often quoted so that Emacs doesn’t interpret them as
      // special filenames.  Unquote them first.
      absl::ConsumePrefix(&arg, "/:");
      ASSIGN_OR_RETURN(auto file, MakeAbsolute(arg));
      // Make filenames relative if possible.
      if (!root.empty()) {
        ASSIGN_OR_RETURN(file, MakeRelative(file, root));
      }
      result.push_back(std::move(file));
    }
  }
  return result;
}

}  // namespace

int RunEmacs(const EmacsOptions& opts) {
  auto status_or_executor = Executor::Create(opts.argv);
  if (!status_or_executor.ok()) {
    std::clog << status_or_executor.status() << std::endl;
    return EXIT_FAILURE;
  }
  auto& executor = status_or_executor.value();
  const auto status_or_code = executor.RunEmacs(opts);
  if (!status_or_code.ok()) {
    std::clog << status_or_code.status() << std::endl;
    return EXIT_FAILURE;
  }
  return status_or_code.value();
}

int RunBinary(const BinaryOptions& opts) {
  auto status_or_executor = Executor::Create(opts.argv);
  if (!status_or_executor.ok()) {
    std::clog << status_or_executor.status() << std::endl;
    return EXIT_FAILURE;
  }
  auto& executor = status_or_executor.value();
  const auto status_or_code = executor.RunBinary(opts);
  if (!status_or_code.ok()) {
    std::clog << status_or_code.status() << std::endl;
    return EXIT_FAILURE;
  }
  return status_or_code.value();
}

int RunTest(const TestOptions& opts) {
  auto status_or_executor = Executor::CreateForTest(opts.argv);
  if (!status_or_executor.ok()) {
    std::clog << status_or_executor.status() << std::endl;
    return EXIT_FAILURE;
  }
  auto& executor = status_or_executor.value();
  const auto status_or_code = executor.RunTest(opts);
  if (!status_or_code.ok()) {
    std::clog << status_or_code.status() << std::endl;
    return EXIT_FAILURE;
  }
  return status_or_code.value();
}

}  // namespace phst_rules_elisp
