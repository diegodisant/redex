/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "CommonSubexpressionElimination.h"
#include "ConstantEnvironment.h"
#include "ConstantPropagationTransform.h"
#include "CopyPropagation.h"
#include "DedupBlocks.h"
#include "DexClass.h"
#include "DexStore.h"
#include "IPConstantPropagationAnalysis.h"
#include "LocalDce.h"
#include "MethodProfiles.h"
#include "ShrinkerConfig.h"

namespace shrinker {

class Shrinker {
 public:
  Shrinker(
      DexStoresVector& stores,
      const Scope& scope,
      const ShrinkerConfig& config,
      const std::unordered_set<DexMethodRef*>& configured_pure_methods,
      const std::unordered_set<DexString*>& configured_finalish_field_names);
  void shrink_method(DexMethod* method);
  const constant_propagation::Transform::Stats& get_const_prop_stats() const {
    return m_const_prop_stats;
  }
  const cse_impl::Stats& get_cse_stats() const { return m_cse_stats; }
  const copy_propagation_impl::Stats& get_copy_prop_stats() const {
    return m_copy_prop_stats;
  }
  const LocalDce::Stats& get_local_dce_stats() const {
    return m_local_dce_stats;
  }
  const dedup_blocks_impl::Stats& get_dedup_blocks_stats() const {
    return m_dedup_blocks_stats;
  }
  size_t get_methods_shrunk() const { return m_methods_shrunk; }

  bool enabled() const { return m_enabled; }

  const std::unordered_set<const DexField*>* get_finalizable_fields() const {
    return m_cse_shared_state ? &m_cse_shared_state->get_finalizable_fields()
                              : nullptr;
  }

  const XStoreRefs& get_xstores() const { return m_xstores; }

 private:
  const XStoreRefs m_xstores;
  const ShrinkerConfig m_config;
  const bool m_enabled;
  std::unique_ptr<cse_impl::SharedState> m_cse_shared_state;

  std::unordered_set<DexMethodRef*> m_pure_methods;
  std::unordered_set<DexString*> m_finalish_field_names;

  // THe mutex protects all other mutable (stats) fields.
  std::mutex m_stats_mutex;
  constant_propagation::Transform::Stats m_const_prop_stats;
  cse_impl::Stats m_cse_stats;
  copy_propagation_impl::Stats m_copy_prop_stats;
  LocalDce::Stats m_local_dce_stats;
  dedup_blocks_impl::Stats m_dedup_blocks_stats;
  size_t m_methods_shrunk{0};
};

} // namespace shrinker