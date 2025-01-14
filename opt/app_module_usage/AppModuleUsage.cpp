/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AppModuleUsage.h"

#include <algorithm>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <sstream>
#include <string>

#include "ConfigFiles.h"
#include "DexAnnotation.h"
#include "DexClass.h"
#include "DexStore.h"
#include "DexUtil.h"
#include "IRInstruction.h"
#include "PassManager.h"
#include "ReflectionAnalysis.h"
#include "Show.h"
#include "Trace.h"
#include "Walkers.h"

namespace {
constexpr const char* APP_MODULE_USAGE_OUTPUT_FILENAME =
    "redex-app-module-usage.csv";
constexpr const char* APP_MODULE_COUNT_OUTPUT_FILENAME =
    "redex-app-module-count.csv";
constexpr const char* USES_AM_ANNO_VIOLATIONS_FILENAME =
    "redex-app-module-annotation-violations.csv";
constexpr const char* SUPER_VERBOSE_DETIALS_FILENAME =
    "redex-app-module-verbose-details.txt";
// @UsesAppModule DexType descriptor
// returns potential type for an AbstractObject
boost::optional<DexType*> type_used(const reflection::AbstractObject& o) {
  DexClass* clazz = nullptr;
  if (o.dex_type) {
    clazz = type_class(o.dex_type);
  }
  switch (o.obj_kind) {
  case reflection::OBJECT:
    TRACE(APP_MOD_USE, 8,
          "Reflection with result kind of OBJECT found as type ");
    if (o.dex_type) {
      TRACE(APP_MOD_USE, 8, "%s\n", SHOW(o.dex_type));
      return o.dex_type;
    } else {
      TRACE(APP_MOD_USE, 8, "undetermined\n");
    }
    break;
  case reflection::INT:
    [[fallthrough]];
  case reflection::STRING:
    break;
  case reflection::CLASS:
    TRACE(APP_MOD_USE, 8,
          "Reflection with result kind of CLASS found as class ");
    if (o.dex_type) {
      TRACE(APP_MOD_USE, 8, "%s\n", SHOW(o.dex_type));
      return o.dex_type;
    } else {
      TRACE(APP_MOD_USE, 8, "undetermined\n");
    }
    break;
  case reflection::FIELD:
    TRACE(APP_MOD_USE,
          8,
          "Reflection with result kind of FIELD (%s) from class ",
          o.dex_string->c_str());
    if (clazz && !clazz->is_external() && o.dex_string) {
      auto field = clazz->find_field_from_simple_deobfuscated_name(
          o.dex_string->c_str());
      if (field) {
        TRACE(APP_MOD_USE, 8, "%s\n", field->get_type()->c_str());
        return field->get_type();
      } else {
        TRACE(APP_MOD_USE, 8, "undetermined; could not find field\n");
      }
    } else {
      TRACE(APP_MOD_USE,
            8,
            "undetermined; source class could not be created or is external\n");
    }
    break;
  case reflection::METHOD:
    TRACE(APP_MOD_USE,
          8,
          "Reflection with result kind of METHOD (%s) from class ",
          o.dex_string->c_str());
    if (clazz && !clazz->is_external() && o.dex_string) {
      auto reflective_method = clazz->find_method_from_simple_deobfuscated_name(
          o.dex_string->c_str());
      if (reflective_method && reflective_method->get_class()) {
        TRACE(APP_MOD_USE, 8, "%s\n", reflective_method->get_class()->c_str());
        return reflective_method->get_class();
      } else {
        TRACE(APP_MOD_USE, 8, "undetermined; could not find method\n");
      }
    } else {
      TRACE(APP_MOD_USE,
            8,
            "undetermined; source class could not be created or is external\n");
    }
    break;
  }
  return boost::none;
}
} // namespace

void AppModuleUsagePass::run_pass(DexStoresVector& stores,
                                  ConfigFiles& conf,
                                  PassManager& mgr) {
  const auto& full_scope = build_class_scope(stores);
  // To quickly look up wich DexStore ("module") a name represents
  std::unordered_map<std::string, DexStore*> name_store_map;
  reflection::MetadataCache refl_metadata_cache;
  for (auto& store : stores) {
    Scope scope = build_class_scope(store.get_dexen());
    name_store_map.emplace(store.get_name(), &store);
    walk::parallel::classes(scope, [&](DexClass* cls) {
      m_type_store_map.emplace(cls->get_type(), &store);
    });
  }
  walk::parallel::methods(full_scope, [&](DexMethod* method) {
    m_stores_method_uses_map.emplace(method, std::unordered_set<DexStore*>{});
    m_stores_method_uses_reflectively_map.emplace(
        method, std::unordered_set<DexStore*>{});
  });

  load_allow_list(stores, name_store_map);

  auto verbose_path = conf.metafile(SUPER_VERBOSE_DETIALS_FILENAME);

  analyze_direct_app_module_usage(full_scope, verbose_path);
  TRACE(APP_MOD_USE, 4, "*** Direct analysis done\n");
  analyze_reflective_app_module_usage(full_scope, verbose_path);
  TRACE(APP_MOD_USE, 4, "*** Reflective analysis done\n");
  TRACE(APP_MOD_USE, 2, "See %s for full details.\n", verbose_path.c_str());

  auto report_path = conf.metafile(USES_AM_ANNO_VIOLATIONS_FILENAME);
  auto module_use_path = conf.metafile(APP_MODULE_USAGE_OUTPUT_FILENAME);
  auto module_count_path = conf.metafile(APP_MODULE_COUNT_OUTPUT_FILENAME);

  auto num_violations = generate_report(full_scope, report_path, mgr);
  TRACE(APP_MOD_USE, 4, "*** Report done\n");

  if (m_output_entrypoints_to_modules) {
    TRACE(APP_MOD_USE, 4, "*** Outputting module use at %s\n",
          APP_MODULE_USAGE_OUTPUT_FILENAME);
    output_usages(stores, module_use_path);
  }
  if (m_output_module_use_count) {
    TRACE(APP_MOD_USE, 4, "*** Outputting module use count at %s\n",
          APP_MODULE_COUNT_OUTPUT_FILENAME);
    output_use_count(stores, module_count_path);
  }

  unsigned int num_methods_access_app_module = 0;
  for (const auto& pair : m_stores_method_uses_map) {
    auto reflective_references =
        m_stores_method_uses_reflectively_map.at(pair.first);
    if (!pair.second.empty() || !reflective_references.empty()) {
      num_methods_access_app_module++;
    }
  }
  mgr.set_metric("num_methods_access_app_module",
                 num_methods_access_app_module);

  if (m_crash_with_violations) {
    always_assert_log(num_violations == 0,
                      "There are @UsesAppModule violations. See %s \n",
                      report_path.c_str());
  }
}

void AppModuleUsagePass::load_allow_list(
    DexStoresVector& stores,
    const std::unordered_map<std::string, DexStore*>& name_store_map) {
  if (m_allow_list_filepath.empty()) {
    TRACE(APP_MOD_USE, 1, "WARNING: No violation allow list file provided");
    return;
  }
  std::ifstream ifs(m_allow_list_filepath);
  std::string line;
  if (ifs.is_open()) {
    while (getline(ifs, line)) {
      auto comma = line.find_first_of(',');
      const auto& entrypoint = line.substr(0, comma);
      auto asterisk = entrypoint.find_first_of('*');
      do {
        const auto& last_comma = comma;
        const auto& store_start =
            line.find_first_not_of(" ,\"", last_comma + 1);
        comma = line.find_first_of(',', comma + 1); // next comma or end of line
        const auto& store_name = line.substr(store_start, comma - store_start);
        DexStore* store = nullptr;
        if (name_store_map.count(store_name)) {
          store = name_store_map.at(store_name);
        }
        TRACE(APP_MOD_USE, 6,
              "adding allowlist entry \"%s\" uses module \"%s\"\n",
              entrypoint.c_str(), store_name.c_str());
        if (asterisk == std::string::npos) {
          // no asterisk => no prefix/pattern
          if (m_allow_list_map.count(entrypoint) == 0) {
            m_allow_list_map.emplace(entrypoint,
                                     std::unordered_set<DexStore*>{});
          }
          if (store_name.find_first_of('*') != std::string::npos) {
            TRACE(APP_MOD_USE, 6, "entrypoint %s is allowed any store \n",
                  entrypoint.c_str());
            // allow any store to be used
            for (const auto& pair : name_store_map) {
              m_allow_list_map.at(entrypoint).emplace(pair.second);
            }
          } else if (store != nullptr) {
            m_allow_list_map.at(entrypoint).emplace(store);
          }
        } else {
          // asterisk => prefix behavior
          const auto& prefix = entrypoint.substr(0, asterisk);
          TRACE(APP_MOD_USE, 6,
                "entrypoint name is a prefix: \"%s\" => \"%s\"\n",
                entrypoint.c_str(), prefix.c_str());
          if (m_allow_list_prefix_map.count(prefix) == 0) {
            m_allow_list_prefix_map.emplace(prefix,
                                            std::unordered_set<DexStore*>{});
          }
          if (store_name.find_first_of('*') != std::string::npos) {
            // allow any store to be used
            TRACE(APP_MOD_USE, 6,
                  "entrypoints prefixed %s are allowed to use any store \n",
                  prefix.c_str());
            for (const auto& pair : name_store_map) {
              m_allow_list_prefix_map.at(prefix).emplace(pair.second);
            }
          } else if (store != nullptr) {
            m_allow_list_prefix_map.at(prefix).emplace(store);
          }
        }
      } while (comma != std::string::npos);
    }
    ifs.close();
  } else {
    fprintf(stderr,
            "WARNING: Could not open violation allow list at \"%s\"\n",
            m_allow_list_filepath.c_str());
  }
}

void AppModuleUsagePass::analyze_direct_app_module_usage(
    const Scope& scope, const std::string& path) {
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  walk::parallel::opcodes(scope, [&](DexMethod* method, IRInstruction* insn) {
    std::unordered_set<DexType*> types_referenced;
    auto method_class = method->get_class();
    always_assert_log(m_type_store_map.count(method_class) > 0,
                      "%s is missing from m_type_store_map",
                      SHOW(method_class));
    const auto method_store = m_type_store_map.at(method_class);
    if (insn->has_method()) {
      types_referenced.emplace(insn->get_method()->get_class());
    }
    if (insn->has_field()) {
      types_referenced.emplace(insn->get_field()->get_class());
    }
    if (insn->has_type()) {
      types_referenced.emplace(insn->get_type());
    }
    for (DexType* type : types_referenced) {
      if (m_type_store_map.count(type) > 0) {
        const auto store = m_type_store_map.at(type);
        if (!store->is_root_store() && store != method_store) {
          // App module reference!
          // add the store for the referenced type to the map
          m_stores_method_uses_map.update(
              method,
              [store](DexMethod* /* method */,
                      std::unordered_set<DexStore*>& stores_used,
                      bool /* exists */) { stores_used.emplace(store); });
          m_stores_use_count.update(store,
                                    [](DexStore* /*store*/,
                                       AppModuleUsage::UseCount& count,
                                       bool /* exists */) {
                                      count.direct_count =
                                          count.direct_count + 1;
                                    });
          ofs << SHOW(method) << " from module \"" << method_store->get_name()
              << "\" references app module \"" << store->get_name()
              << "\" by using the class \"" << type->str() << "\"\n";
        }
      }
    }
  });
}

void AppModuleUsagePass::analyze_reflective_app_module_usage(
    const Scope& scope, const std::string& path) {
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  // Reflective Reference
  reflection::MetadataCache refl_metadata_cache;
  walk::parallel::code(scope, [&](DexMethod* method, IRCode& code) {
    const auto method_store = m_type_store_map.at(method->get_class());
    std::unique_ptr<reflection::ReflectionAnalysis> analysis =
        std::make_unique<reflection::ReflectionAnalysis>(
            /* dex_method */ method,
            /* context (interprocedural only) */ nullptr,
            /* summary_query_fn (interprocedural only) */ nullptr,
            /* metadata_cache */ &refl_metadata_cache);
    for (auto& mie : InstructionIterable(code)) {
      IRInstruction* insn = mie.insn;
      boost::optional<DexType*> type = boost::none;
      if (!opcode::is_an_invoke(insn->opcode())) {
        TRACE(APP_MOD_USE, 6, "Investigating reflection \n");
        // If an object type is from refletion it will be in the RESULT_REGISTER
        // for some instruction
        const auto& o = analysis->get_abstract_object(RESULT_REGISTER, insn);
        if (o &&
            (o.get().obj_kind != reflection::CLASS ||
             (analysis->get_class_source(RESULT_REGISTER, insn).has_value() &&
              analysis->get_class_source(RESULT_REGISTER, insn).get() ==
                  reflection::REFLECTION))) {
          // If the obj is a CLASS then it must have a class source of
          // REFLECTION
          TRACE(APP_MOD_USE, 6, "Found an abstract object \n");
          type = type_used(o.get());
        }
      }
      if (type.has_value() && m_type_store_map.count(type.get()) > 0) {
        const auto store = m_type_store_map.at(type.get());
        if (!store->is_root_store() && store != method_store) {
          // App module reference!
          // add the store for the referenced type to the map
          m_stores_method_uses_reflectively_map.update(
              method,
              [store](DexMethod* /* method */,
                      std::unordered_set<DexStore*>& stores_used,
                      bool /* exists */) { stores_used.emplace(store); });
          TRACE(APP_MOD_USE,
                5,
                "%s used reflectively by %s\n",
                SHOW(type.get()),
                SHOW(method));
          m_stores_use_count.update(store,
                                    [](DexStore* /*store*/,
                                       AppModuleUsage::UseCount& count,
                                       bool /* exists */) {
                                      count.reflective_count =
                                          count.reflective_count + 1;
                                    });

          ofs << SHOW(method) << " from module \"" << method_store->get_name()
              << "\" *reflectively* references app module \""
              << store->get_name() << "\" by using the class \""
              << type.get()->str() << "\"\n";
        }
      }
    }
  });
}

template <typename T>
std::unordered_set<std::string> AppModuleUsagePass::get_modules_used(
    T* entrypoint, DexType* annotation_type) {
  std::unordered_set<std::string> modules = {};
  auto anno_set = entrypoint->get_anno_set();
  if (anno_set) {
    for (DexAnnotation* annotation : anno_set->get_annotations()) {
      if (annotation->type() == annotation_type) {
        for (const DexAnnotationElement& anno_elem : annotation->anno_elems()) {
          always_assert(anno_elem.string->str() == "value");
          always_assert(anno_elem.encoded_value->evtype() == DEVT_ARRAY);
          const auto* array =
              static_cast<const DexEncodedValueArray*>(anno_elem.encoded_value);
          for (const auto* value : *(array->evalues())) {
            always_assert(value->evtype() == DEVT_STRING);
            modules.emplace(((DexEncodedValueString*)value)->string()->str());
          }
        }
        break;
      }
    }
  }
  return modules;
}

template std::unordered_set<std::string>
AppModuleUsagePass::get_modules_used<DexMethod>(DexMethod*, DexType*);

template std::unordered_set<std::string>
AppModuleUsagePass::get_modules_used<DexField>(DexField*, DexType*);

template std::unordered_set<std::string>
AppModuleUsagePass::get_modules_used<DexClass>(DexClass*, DexType*);

size_t AppModuleUsagePass::generate_report(const Scope& scope,
                                           const std::string& path,
                                           PassManager& mgr) {
  size_t violation_count = 0;
  auto annotation_type =
      DexType::make_type(m_uses_app_module_annotation_descriptor.c_str());
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  // Method violations
  for (const auto& pair : m_stores_method_uses_map) {
    auto method = pair.first;
    std::string method_name = show(method);
    auto store_from = m_type_store_map.at(method->get_class());
    bool print_name = true;
    auto annotated_module_names = get_modules_used(method, annotation_type);
    // combine annotations from class
    annotated_module_names.merge(
        get_modules_used(type_class(method->get_class()), annotation_type));
    // check for violations
    auto violation_check = [&](auto store) {
      const auto& used_module_name = store->get_name();
      if (annotated_module_names.count(used_module_name) == 0 &&
          !violation_is_in_allowlist(method_name, store)) {
        violation(method, store_from->get_name(), used_module_name, ofs,
                  print_name);
        print_name = false;
        violation_count++;
      }
    };
    std::for_each(pair.second.begin(), pair.second.end(), violation_check);
    std::for_each(m_stores_method_uses_reflectively_map.at(method).begin(),
                  m_stores_method_uses_reflectively_map.at(method).end(),
                  [&](const auto& store) {
                    if (pair.second.count(store) == 0) {
                      violation_check(store);
                    }
                  });
    if (!print_name) {
      ofs << "\n";
    }
  }
  // Field violations
  walk::fields(scope, [&](DexField* field) {
    auto annotated_module_names = get_modules_used(field, annotation_type);
    std::string field_name = show(field);
    // combine annotations from class
    annotated_module_names.merge(
        get_modules_used(type_class(field->get_class()), annotation_type));
    bool print_name = true;
    if (m_type_store_map.count(field->get_type()) > 0 &&
        m_type_store_map.count(field->get_class()) > 0) {
      // get_type is the type of the field, the app module that class is from is
      // referenced by the field
      auto store_used = m_type_store_map.at(field->get_type());
      // get_class is the contatining class of the field, the app module that
      // class is in is the module the field is in
      auto store_from = m_type_store_map.at(field->get_class());
      if (!store_used->is_root_store() &&
          store_used->get_name() != store_from->get_name() &&
          annotated_module_names.count(store_used->get_name()) == 0 &&
          !violation_is_in_allowlist(field_name, store_used)) {
        violation(field, store_from->get_name(), store_used->get_name(), ofs,
                  print_name);
        print_name = false;
        violation_count++;
      }
    }
    if (!print_name) {
      ofs << "\n";
    }
  });
  mgr.set_metric("num_violations", violation_count);
  return violation_count;
}

bool AppModuleUsagePass::violation_is_in_allowlist(
    const std::string& entrypoint_name, DexStore* store_used) {
  if (m_allow_list_map.count(entrypoint_name) == 0) {
    // ruled out the exact item in the map so look for a prefix that matches
    // non optimized for now
    for (const auto& pair : m_allow_list_prefix_map) {
      const auto& prefix = pair.first;
      if (entrypoint_name.find_first_of(prefix) == 0) {
        // entrypoint_name matches prefix
        return true;
      }
    }
  } else {
    return m_allow_list_map.at(entrypoint_name).count(store_used) > 0;
  }
  return false;
}

template <typename T>
void AppModuleUsagePass::violation(T* entrypoint,
                                   const std::string& from_module,
                                   const std::string& to_module,
                                   std::ofstream& ofs,
                                   bool print_name) {
  if (print_name) {
    ofs << SHOW(entrypoint);
  }
  ofs << ", " << to_module;
  int level = 4;
  if (m_crash_with_violations) {
    level = 0;
  }
  TRACE(APP_MOD_USE,
        level,
        "%s (from module \"%s\") uses app module \"%s\" without annotation\n",
        SHOW(entrypoint),
        from_module.c_str(),
        to_module.c_str());
}

template void AppModuleUsagePass::violation(
    DexMethod*, const std::string&, const std::string&, std::ofstream&, bool);
template void AppModuleUsagePass::violation(
    DexField*, const std::string&, const std::string&, std::ofstream&, bool);

void AppModuleUsagePass::output_usages(const DexStoresVector& stores,
                                       const std::string& path) {
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  for (const auto& pair : m_stores_method_uses_map) {
    auto reflective_references =
        m_stores_method_uses_reflectively_map.at(pair.first);
    if (!pair.second.empty() || !reflective_references.empty()) {
      const auto method = pair.first;
      if (m_type_store_map.count(method->get_class()) > 0) {
        ofs << "\""
            << m_type_store_map.at(method->get_class())->get_name().c_str()
            << "\", ";
      } else {
        ofs << "\"\", ";
      }
      ofs << "\"" << SHOW(method) << "\"";
      for (auto store : pair.second) {
        if (reflective_references.count(store) > 0) {
          ofs << ", \"(d&r)" << store->get_name().c_str() << "\"";
        } else {
          ofs << ", \"" << store->get_name() << "\"";
        }
      }
      for (auto store : reflective_references) {
        if (pair.second.count(store) == 0) {
          ofs << ", \"(r)" << store->get_name().c_str() << "\"";
        }
      }
      ofs << "\n";
    }
  }
}

void AppModuleUsagePass::output_use_count(const DexStoresVector& stores,
                                          const std::string& path) {
  std::ofstream ofs(path, std::ofstream::out | std::ofstream::trunc);
  for (const auto& pair : m_stores_use_count) {
    ofs << "\"" << pair.first->get_name() << "\", " << pair.second.direct_count
        << ", " << pair.second.reflective_count << "\n";
  }
}

static AppModuleUsagePass s_pass;
