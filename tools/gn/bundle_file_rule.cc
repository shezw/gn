// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/gn/bundle_file_rule.h"

#include "base/strings/stringprintf.h"
#include "tools/gn/output_file.h"
#include "tools/gn/settings.h"
#include "tools/gn/substitution_pattern.h"
#include "tools/gn/substitution_writer.h"
#include "tools/gn/target.h"
#include "tools/gn/variables.h"

namespace {

Err ErrMissingPropertyForExpansion(const Settings* settings,
                                   const Target* target,
                                   const BundleFileRule* bundle_data,
                                   const char* property_name) {
  std::string bundle_data_target_label =
      bundle_data->target()->label().GetUserVisibleName(
          settings->default_toolchain_label());

  return Err(target->defined_from(),
             base::StringPrintf("Property %s is required.", property_name),
             base::StringPrintf(
                 "In order to expand {{%s}} in %s, the "
                 "property needs to be defined in the create_bundle target.",
                 property_name, bundle_data_target_label.c_str()));
}

}  // namespace

BundleFileRule::BundleFileRule(const Target* bundle_data_target,
                               const std::vector<SourceFile> sources,
                               const SubstitutionPattern& pattern)
    : target_(bundle_data_target), sources_(sources), pattern_(pattern) {
  // target_ may be null during testing.
  DCHECK(!target_ || target_->output_type() == Target::BUNDLE_DATA);
}

BundleFileRule::BundleFileRule(const BundleFileRule& other) = default;

BundleFileRule::~BundleFileRule() = default;

bool BundleFileRule::ApplyPatternToSource(const Settings* settings,
                                          const Target* target,
                                          const BundleData& bundle_data,
                                          const SourceFile& source_file,
                                          SourceFile* expanded_source_file,
                                          Err* err) const {
  std::string output_path;
  for (const auto& subrange : pattern_.ranges()) {
    switch (subrange.type) {
      case SUBSTITUTION_LITERAL:
        output_path.append(subrange.literal);
        break;
      case SUBSTITUTION_BUNDLE_ROOT_DIR:
        if (bundle_data.contents_dir().is_null()) {
          *err = ErrMissingPropertyForExpansion(settings, target, this,
                                                variables::kBundleRootDir);
          return false;
        }
        output_path.append(bundle_data.root_dir().value());
        break;
      case SUBSTITUTION_BUNDLE_CONTENTS_DIR:
        if (bundle_data.contents_dir().is_null()) {
          *err = ErrMissingPropertyForExpansion(settings, target, this,
                                                variables::kBundleContentsDir);
          return false;
        }
        output_path.append(bundle_data.contents_dir().value());
        break;
      case SUBSTITUTION_BUNDLE_RESOURCES_DIR:
        if (bundle_data.resources_dir().is_null()) {
          *err = ErrMissingPropertyForExpansion(settings, target, this,
                                                variables::kBundleResourcesDir);
          return false;
        }
        output_path.append(bundle_data.resources_dir().value());
        break;
      case SUBSTITUTION_BUNDLE_EXECUTABLE_DIR:
        if (bundle_data.executable_dir().is_null()) {
          *err = ErrMissingPropertyForExpansion(
              settings, target, this, variables::kBundleExecutableDir);
          return false;
        }
        output_path.append(bundle_data.executable_dir().value());
        break;
      case SUBSTITUTION_BUNDLE_PLUGINS_DIR:
        if (bundle_data.contents_dir().is_null()) {
          *err = ErrMissingPropertyForExpansion(settings, target, this,
                                                variables::kBundlePlugInsDir);
          return false;
        }
        output_path.append(bundle_data.plugins_dir().value());
        break;
      default:
        output_path.append(SubstitutionWriter::GetSourceSubstitution(
            target_, target_->settings(), source_file, subrange.type,
            SubstitutionWriter::OUTPUT_ABSOLUTE, SourceDir()));
        break;
    }
  }
  *expanded_source_file = SourceFile(SourceFile::SWAP_IN, &output_path);
  return true;
}

bool BundleFileRule::ApplyPatternToSourceAsOutputFile(
    const Settings* settings,
    const Target* target,
    const BundleData& bundle_data,
    const SourceFile& source_file,
    OutputFile* expanded_output_file,
    Err* err) const {
  SourceFile expanded_source_file;
  if (!ApplyPatternToSource(settings, target, bundle_data, source_file,
                            &expanded_source_file, err)) {
    return false;
  }

  *expanded_output_file =
      OutputFile(settings->build_settings(), expanded_source_file);
  return true;
}
