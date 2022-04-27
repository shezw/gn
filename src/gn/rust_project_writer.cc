// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gn/rust_project_writer.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <tuple>
#include <variant>

#include "base/json/string_escape.h"
#include "base/strings/string_split.h"
#include "gn/builder.h"
#include "gn/deps_iterator.h"
#include "gn/ninja_target_command_util.h"
#include "gn/rust_project_writer_helpers.h"
#include "gn/rust_tool.h"
#include "gn/source_file.h"
#include "gn/string_output_buffer.h"
#include "gn/tool.h"
#include "loader.h"

#if defined(OS_WINDOWS)
#define NEWLINE "\r\n"
#else
#define NEWLINE "\n"
#endif

// Current structure of rust-project.json output file
//
// {
//    "crates": [
//        {
//            "deps": [
//                {
//                    "crate": 1, // index into crate array
//                    "name": "alloc" // extern name of dependency
//                },
//            ],
//            "source": [
//                "include_dirs": [
//                     "some/source/root",
//                     "some/gen/dir",
//                ],
//                "exclude_dirs": []
//            },
//            "edition": "2018", // edition of crate
//            "cfg": [
//              "unix", // "atomic" value config options
//              "rust_panic=\"abort\""", // key="value" config options
//            ]
//            "root_module": "absolute path to crate",
//            "label": "//path/target:value", // GN target for the crate
//            "target": "x86_64-unknown-linux" // optional rustc target
//        },
// }
//

bool RustProjectWriter::RunAndWriteFiles(const BuildSettings* build_settings,
                                         const Builder& builder,
                                         const std::string& file_name,
                                         bool quiet,
                                         Err* err) {
  SourceFile output_file = build_settings->build_dir().ResolveRelativeFile(
      Value(nullptr, file_name), err);
  if (output_file.is_null())
    return false;

  base::FilePath output_path = build_settings->GetFullPath(output_file);

  std::vector<const Target*> all_targets = builder.GetAllResolvedTargets();

  StringOutputBuffer out_buffer;
  std::ostream out(&out_buffer);

  RenderJSON(build_settings, builder.loader()->GetDefaultToolchain(),
             all_targets, out);
  return out_buffer.WriteToFileIfChanged(output_path, err);
}

// Set of dependency crates, represented by their root module path.
using DependencySet = UniqueVector<SourceFile>;

// Get the Rust deps for a target, recursively expanding OutputType::GROUPS
// that are present in the GN structure.  This will return a flattened list of
// deps from the groups, but will not expand a Rust lib dependency to find any
// transitive Rust dependencies.
void GetRustDeps(const Target* target, DependencySet* rust_deps) {
  for (const auto& pair : target->GetDeps(Target::DEPS_LINKED)) {
    const Target* dep = pair.ptr;

    if (dep->source_types_used().RustSourceUsed()) {
      // Include any Rust dep.
      rust_deps->push_back(dep->rust_values().crate_root());
    } else if (dep->output_type() == Target::OutputType::GROUP) {
      // Inspect (recursively) any group to see if it contains Rust deps.
      GetRustDeps(dep, rust_deps);
    }
  }
}

// Per-crate bookkeeping needed while constructing the crate list.
struct CrateInfo {
  // First, we record all targets that build the crate.
  TargetsVector targets;
  // Then, we do a depth-first traversal to process crates in dependency order,
  // and use the seen flag to avoid processing a crate twice.
  bool seen = false;
  // Finally, we append a new Crate to the crate list and save its index.
  std::optional<CrateIndex> index;
};

// Map from each crate (root module path) to info.
using CrateInfoMap = std::unordered_map<SourceFile, CrateInfo>;

std::vector<std::string> ExtractCompilerArgs(const Target* target) {
  std::vector<std::string> args;
  for (ConfigValuesIterator iter(target); !iter.done(); iter.Next()) {
    auto rustflags = iter.cur().rustflags();
    for (auto flag_iter = rustflags.begin(); flag_iter != rustflags.end();
         flag_iter++) {
      args.push_back(*flag_iter);
    }
  }
  return args;
}

std::optional<std::string> FindArgValue(const char* arg,
                                        const std::vector<std::string>& args) {
  for (auto current = args.begin(); current != args.end();) {
    // capture the current value
    auto previous = *current;
    // and increment
    current++;

    // if the previous argument matches `arg`, and after the above increment the
    // end hasn't been reached, this current argument is the desired value.
    if (previous == arg && current != args.end()) {
      return std::make_optional(*current);
    }
  }
  return std::nullopt;
}

std::optional<std::string> FindArgValueAfterPrefix(
    const std::string& prefix,
    const std::vector<std::string>& args) {
  for (auto arg : args) {
    if (!arg.compare(0, prefix.size(), prefix)) {
      auto value = arg.substr(prefix.size());
      return std::make_optional(value);
    }
  }
  return std::nullopt;
}

std::vector<std::string> FindAllArgValuesAfterPrefix(
    const std::string& prefix,
    const std::vector<std::string>& args) {
  std::vector<std::string> values;
  for (auto arg : args) {
    if (!arg.compare(0, prefix.size(), prefix)) {
      auto value = arg.substr(prefix.size());
      values.push_back(value);
    }
  }
  return values;
}

// TODO(bwb) Parse sysroot structure from toml files. This is fragile and
// might break if upstream changes the dependency structure.
const std::string_view sysroot_crates[] = {
    "std",        "core", "alloc",       "panic_unwind",
    "proc_macro", "test", "panic_abort", "unwind"};

// Multiple sysroot crates have dependencies on each other.  This provides a
// mechanism for specifying that in an extendible manner.
const std::unordered_map<std::string_view, std::vector<std::string_view>>
    sysroot_deps_map = {{"alloc", {"core"}},
                        {"std", {"alloc", "core", "panic_abort", "unwind"}}};

// Add each of the crates a sysroot has, including their dependencies.
void AddSysrootCrate(const BuildSettings* build_settings,
                     std::string_view crate,
                     std::string_view current_sysroot,
                     SysrootCrateIndexMap& sysroot_crate_lookup,
                     CrateList& crate_list) {
  if (sysroot_crate_lookup.find(crate) != sysroot_crate_lookup.end()) {
    // If this sysroot crate is already in the lookup, we don't add it again.
    return;
  }

  // Add any crates that this sysroot crate depends on.
  auto deps_lookup = sysroot_deps_map.find(crate);
  if (deps_lookup != sysroot_deps_map.end()) {
    auto deps = (*deps_lookup).second;
    for (auto dep : deps) {
      AddSysrootCrate(build_settings, dep, current_sysroot,
                      sysroot_crate_lookup, crate_list);
    }
  }

  size_t crate_index = crate_list.size();
  sysroot_crate_lookup.emplace(crate, crate_index);

  base::FilePath rebased_out_dir =
      build_settings->GetFullPath(build_settings->build_dir());
  auto crate_path =
      FilePathToUTF8(rebased_out_dir) + std::string(current_sysroot) +
      "/lib/rustlib/src/rust/library/" + std::string(crate) + "/src/lib.rs";

  crate_list.emplace_back(SourceFile(crate_path), TargetsVector(), std::nullopt,
                          crate_index, std::string(crate), std::string(crate),
                          "2018");
  auto& sysroot_crate = crate_list.back();

  sysroot_crate.AddConfigItem("debug_assertions");

  if (deps_lookup != sysroot_deps_map.end()) {
    auto deps = (*deps_lookup).second;
    for (auto dep : deps) {
      auto idx = sysroot_crate_lookup[dep];
      sysroot_crate.AddDependency(idx, std::string(dep));
    }
  }
}

// Add the given sysroot to the project, if it hasn't already been added.
void AddSysroot(const BuildSettings* build_settings,
                std::string_view sysroot,
                SysrootIndexMap& sysroot_lookup,
                CrateList& crate_list) {
  // If this sysroot is already in the lookup, we don't add it again.
  if (sysroot_lookup.find(sysroot) != sysroot_lookup.end()) {
    return;
  }

  // Otherwise, add all of its crates
  for (auto crate : sysroot_crates) {
    AddSysrootCrate(build_settings, crate, sysroot, sysroot_lookup[sysroot],
                    crate_list);
  }
}

void AddSysrootDependencyToCrate(Crate* crate,
                                 const SysrootCrateIndexMap& sysroot,
                                 std::string_view crate_name) {
  if (const auto crate_idx = sysroot.find(crate_name);
      crate_idx != sysroot.end()) {
    crate->AddDependency(crate_idx->second, std::string(crate_name));
  }
}

// Given the list of targets for a crate, returns the preferred one to use for
// editor support, favoring (1) the default toolchain and (2) non-testonly.
const Target* PreferredTarget(const Label& default_toolchain,
                              const TargetsVector& targets) {
  assert(!targets.empty());
  auto score = [&](const Target* target) {
    int n = 0;
    if (target->toolchain()->label() == default_toolchain)
      n += 2;
    if (!target->testonly())
      n += 1;
    return n;
  };
  auto r = *std::max_element(
      targets.begin(), targets.end(),
      [&](auto lhs, auto rhs) { return score(lhs) < score(rhs); });
  if (targets.front()->label().GetUserVisibleName(false).find(
          "//src/lib/fidl/rust/fidl") != std::string::npos) {
    printf("mkember: %zu targets, and they are:\n", targets.size());
    for (auto& t : targets) {
      printf("mkember: %s ---> %d\n",
             t->label().GetUserVisibleName(true).c_str(), score(t));
    }
    printf("mkember: ..... chose %s ---> %d\n",
           r->label().GetUserVisibleName(true).c_str(), score(r));
  }
  return r;
}

void AddCrate(const BuildSettings* build_settings,
              const Label& default_toolchain,
              SourceFile crate_root,
              CrateInfo& crate_info,
              CrateInfoMap& lookup,
              SysrootIndexMap& sysroot_lookup,
              CrateList& crate_list) {
  if (crate_info.seen) {
    // If the crate was already seen, we don't add it again.
    return;
  }
  crate_info.seen = true;

  auto& all_targets = crate_info.targets;
  const Target* main_target = PreferredTarget(default_toolchain, all_targets);
  if (crate_root.value().find("//src/lib/fidl/rust/fidl/src") !=
      std::string::npos) {
    printf("mkember: main target: %s\n",
           main_target->label().GetUserVisibleName(true).c_str());
  }

  auto compiler_args = ExtractCompilerArgs(main_target);
  auto compiler_target = FindArgValue("--target", compiler_args);

  // Check what sysroot this target needs.  Add it to the crate list if it
  // hasn't already been added.
  auto rust_tool =
      main_target->toolchain()->GetToolForTargetFinalOutputAsRust(main_target);
  auto current_sysroot = rust_tool->GetSysroot();
  if (current_sysroot != "" && sysroot_lookup.count(current_sysroot) == 0) {
    AddSysroot(build_settings, current_sysroot, sysroot_lookup, crate_list);
  }

  // Gather dependencies from targets in the same toolchain as the main target.
  // Typically this is the main target plus a test target, which ensures that we
  // record test-only dependencies (e.g. crates like assert_matches).
  DependencySet crate_deps;
  for (const auto* target : all_targets) {
    if (crate_root.value().find("//src/lib/fidl/rust/fidl/src") !=
        std::string::npos) {
      if (target->toolchain()->label() == main_target->toolchain()->label()) {
        printf("mkember: same toolchain: %s: %s\n", crate_root.value().c_str(),
               main_target->toolchain()
                   ->label()
                   .GetUserVisibleName(false)
                   .c_str());
      } else {
        printf(
            "mkember: diff toolchain: %s: %s --- vs --- %s\n",
            crate_root.value().c_str(),
            main_target->toolchain()->label().GetUserVisibleName(false).c_str(),
            target->toolchain()->label().GetUserVisibleName(false).c_str());
      }
    }
    if (target->toolchain()->label() != main_target->toolchain()->label())
      continue;
    GetRustDeps(target, &crate_deps);
  }

  // Recursively add dependency crates so that they get assigned IDs first.
  for (const auto& dep : crate_deps) {
    if (dep == crate_root)
      continue;
    AddCrate(build_settings, default_toolchain, dep, lookup.at(dep), lookup,
             sysroot_lookup, crate_list);
  }

  auto edition =
      FindArgValueAfterPrefix(std::string("--edition="), compiler_args);
  if (!edition.has_value()) {
    edition = FindArgValue("--edition", compiler_args);
  }

  auto gen_dir =
      GetBuildDirForTargetAsOutputFile(main_target, BuildDirType::GEN);

  // Assign the next index in the crate list to this crate.
  CrateIndex crate_index = crate_list.size();
  crate_info.index = crate_index;
  crate_list.emplace_back(crate_root, std::move(all_targets), gen_dir,
                          crate_index, main_target->rust_values().crate_name(),
                          main_target->label().GetUserVisibleName(false),
                          edition.value_or("2015"));
  Crate& crate = crate_list.back();

  crate.SetCompilerArgs(compiler_args);
  if (compiler_target.has_value())
    crate.SetCompilerTarget(compiler_target.value());

  crate.AddConfigItem("test");
  crate.AddConfigItem("debug_assertions");

  // Add configs from the main target.
  for (auto& cfg :
       FindAllArgValuesAfterPrefix(std::string("--cfg="), compiler_args)) {
    crate.AddConfigItem(cfg);
  }
  // Also add configs from other targets in the same toolchain.
  for (const auto* target : all_targets) {
    if (target->toolchain()->label() != main_target->toolchain()->label())
      continue;
    for (auto& cfg : FindAllArgValuesAfterPrefix(std::string("--cfg="),
                                                 ExtractCompilerArgs(target))) {
      crate.AddConfigItem(cfg);
    }
  }

  // Add the sysroot dependencies, if there is one.
  if (current_sysroot != "") {
    const auto& sysroot = sysroot_lookup[current_sysroot];
    AddSysrootDependencyToCrate(&crate, sysroot, "core");
    AddSysrootDependencyToCrate(&crate, sysroot, "alloc");
    AddSysrootDependencyToCrate(&crate, sysroot, "std");

    // Proc macros have the proc_macro crate as a direct dependency
    if (std::string_view(rust_tool->name()) ==
        std::string_view(RustTool::kRsToolMacro)) {
      AddSysrootDependencyToCrate(&crate, sysroot, "proc_macro");
    }
  }

  // If it's a proc macro, record its output location so IDEs can invoke it.
  if (std::string_view(rust_tool->name()) ==
      std::string_view(RustTool::kRsToolMacro)) {
    auto outputs = main_target->computed_outputs();
    if (outputs.size() > 0) {
      crate.SetIsProcMacro(outputs[0]);
    }
  }

  // Note any environment variables. These may be used by proc macros
  // invoked by the current crate (so we want to record these for all crates,
  // not just proc macro crates)
  for (const auto& env_var : main_target->config_values().rustenv()) {
    std::vector<std::string> parts = base::SplitString(
        env_var, "=", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
    if (parts.size() >= 2) {
      crate.AddRustenv(parts[0], parts[1]);
    }
  }

  // Add the rest of the crate dependencies.
  for (const auto& dep : crate_deps) {
    if (dep == crate_root)
      continue;
    auto& info = lookup[dep];
    assert(info.seen);
    if (!info.index.has_value()) {
      printf("mkember: %s -> %s\n", crate_root.value().c_str(),
             dep.value().c_str());
    } else {
      crate.AddDependency(info.index.value(),
                          crate_list[info.index.value()].name());
    }
  }
}

void WriteCrates(const BuildSettings* build_settings,
                 CrateList& crate_list,
                 std::ostream& rust_project) {
  rust_project << "{" NEWLINE;
  rust_project << "  \"crates\": [";
  bool first_crate = true;
  for (auto& crate : crate_list) {
    if (!first_crate)
      rust_project << ",";
    first_crate = false;

    auto crate_module =
        FilePathToUTF8(build_settings->GetFullPath(crate.root()));

    rust_project << NEWLINE << "    {" NEWLINE
                 << "      \"crate_id\": " << crate.index() << "," NEWLINE
                 << "      \"root_module\": \"" << crate_module << "\"," NEWLINE
                 << "      \"label\": \"" << crate.label() << "\"," NEWLINE
                 << "      \"source\": {" NEWLINE
                 << "          \"include_dirs\": [" NEWLINE
                 << "               \""
                 << FilePathToUTF8(
                        build_settings->GetFullPath(crate.root().GetDir()))
                 << "\"";
    auto gen_dir = crate.gen_dir();
    if (gen_dir.has_value()) {
      auto gen_dir_path = FilePathToUTF8(
          build_settings->GetFullPath(gen_dir->AsSourceDir(build_settings)));
      rust_project << "," NEWLINE << "               \"" << gen_dir_path
                   << "\"" NEWLINE;
    } else {
      rust_project << NEWLINE;
    }
    rust_project << "          ]," NEWLINE
                 << "          \"exclude_dirs\": []" NEWLINE
                 << "      }," NEWLINE;

    auto compiler_target = crate.CompilerTarget();
    if (compiler_target.has_value()) {
      rust_project << "      \"target\": \"" << compiler_target.value()
                   << "\"," NEWLINE;
    }

    auto compiler_args = crate.CompilerArgs();
    if (!compiler_args.empty()) {
      rust_project << "      \"compiler_args\": [";
      bool first_arg = true;
      for (auto& arg : crate.CompilerArgs()) {
        if (!first_arg)
          rust_project << ", ";
        first_arg = false;

        std::string escaped_arg;
        base::EscapeJSONString(arg, false, &escaped_arg);

        rust_project << "\"" << escaped_arg << "\"";
      }
      rust_project << "]," << NEWLINE;
    }

    rust_project << "      \"deps\": [";
    bool first_dep = true;
    for (auto& dep : crate.dependencies()) {
      if (!first_dep)
        rust_project << ",";
      first_dep = false;

      rust_project << NEWLINE << "        {" NEWLINE
                   << "          \"crate\": " << dep.first << "," NEWLINE
                   << "          \"name\": \"" << dep.second << "\"" NEWLINE
                   << "        }";
    }
    rust_project << NEWLINE "      ]," NEWLINE;  // end dep list

    rust_project << "      \"edition\": \"" << crate.edition() << "\"," NEWLINE;

    auto proc_macro_target = crate.proc_macro_path();
    if (proc_macro_target.has_value()) {
      rust_project << "      \"is_proc_macro\": true," NEWLINE;
      auto so_location = FilePathToUTF8(build_settings->GetFullPath(
          proc_macro_target->AsSourceFile(build_settings)));
      rust_project << "      \"proc_macro_dylib_path\": \"" << so_location
                   << "\"," NEWLINE;
    }

    rust_project << "      \"cfg\": [";
    bool first_cfg = true;
    for (const auto& cfg : crate.configs()) {
      if (!first_cfg)
        rust_project << ",";
      first_cfg = false;

      std::string escaped_config;
      base::EscapeJSONString(cfg, false, &escaped_config);

      rust_project << NEWLINE;
      rust_project << "        \"" << escaped_config << "\"";
    }
    rust_project << NEWLINE;
    rust_project << "      ]";  // end cfgs

    if (!crate.rustenv().empty()) {
      rust_project << "," NEWLINE;
      rust_project << "      \"env\": {";
      bool first_env = true;
      for (const auto& env : crate.rustenv()) {
        if (!first_env)
          rust_project << ",";
        first_env = false;
        std::string escaped_key, escaped_val;
        base::EscapeJSONString(env.first, false, &escaped_key);
        base::EscapeJSONString(env.second, false, &escaped_val);
        rust_project << NEWLINE;
        rust_project << "        \"" << escaped_key << "\": \"" << escaped_val
                     << "\"";
      }

      rust_project << NEWLINE;
      rust_project << "      }" NEWLINE;  // end env vars
    } else {
      rust_project << NEWLINE;
    }
    rust_project << "    }";  // end crate
  }
  rust_project << NEWLINE "  ]" NEWLINE;  // end crate list
  rust_project << "}" NEWLINE;
}

void RustProjectWriter::RenderJSON(const BuildSettings* build_settings,
                                   const Label& default_toolchain,
                                   std::vector<const Target*>& all_targets,
                                   std::ostream& rust_project) {
  // Collect all Rust targets in the project and group them by crate.
  CrateInfoMap lookup;
  for (const auto* target : all_targets) {
    if (!target->IsBinary() || !target->source_types_used().RustSourceUsed())
      continue;

    auto crate_root = target->rust_values().crate_root();
    lookup[crate_root].targets.push_back(target);
  }

  // Generate the crate list.
  SysrootIndexMap sysroot_lookup;
  CrateList crate_list;
  for (auto& [crate_root, info] : lookup) {
    AddCrate(build_settings, default_toolchain, crate_root, info, lookup,
             sysroot_lookup, crate_list);
  }

  // Write rust-project.json.
  WriteCrates(build_settings, crate_list, rust_project);
}
