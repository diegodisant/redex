/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include "ApiLevelChecker.h"
#include "Creators.h"
#include "DexAsm.h"
#include "DexUtil.h"
#include "IRAssembler.h"
#include "IRCode.h"
#include "Inliner.h"
#include "InlinerConfig.h"
#include "LegacyInliner.h"
#include "RedexTest.h"

struct MethodInlineTest : public RedexTest {
  MethodInlineTest() {
    DexMethod::make_method("Ljava/lang/Enum;.equals:(Ljava/lang/Object;)Z")
        ->make_concrete(ACC_PUBLIC, true);

    DexField::make_field("Ljava/lang/Boolean;.TRUE:Ljava/lang/Boolean;")
        ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);
    DexField::make_field("Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;")
        ->make_concrete(ACC_PUBLIC | ACC_STATIC | ACC_FINAL);

    DexMethod::make_method("Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;")
        ->make_concrete(ACC_PUBLIC, true);
    DexMethod::make_method("Ljava/lang/Boolean;.booleanValue:()Z")
        ->make_concrete(ACC_PUBLIC, true);
  }
};

void test_inliner(const std::string& caller_str,
                  const std::string& callee_str,
                  const std::string& expected_str) {
  auto caller = assembler::ircode_from_string(caller_str);
  auto callee = assembler::ircode_from_string(callee_str);

  const auto& callsite = std::find_if(
      caller->begin(), caller->end(), [](const MethodItemEntry& mie) {
        return mie.type == MFLOW_OPCODE &&
               opcode::is_an_invoke(mie.insn->opcode());
      });
  legacy_inliner::inline_method_unsafe(
      /*caller_method=*/nullptr, caller.get(), callee.get(), callsite);

  auto expected = assembler::ircode_from_string(expected_str);

  EXPECT_CODE_EQ(expected.get(), caller.get());
}

DexClass* create_a_class(const char* description) {
  ClassCreator cc(DexType::make_type(description));
  cc.set_super(type::java_lang_Object());
  return cc.create();
}

void create_runtime_exception_init() {
  auto init_method = static_cast<DexMethod*>(DexMethod::make_method(
      "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V"));
  init_method->set_external();
}

static void remove_position(IRCode* code) {
  for (auto it = code->begin(); it != code->end();) {
    if (it->type == MFLOW_POSITION) {
      it = code->erase_and_dispose(it);
    } else {
      it++;
    }
  }
}

/**
 * Create a method like
 * void {{name}}() {
 *   const v0 {{val}};
 * }
 */
DexMethod* make_a_method(DexClass* cls, const char* name, int val) {
  auto proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto ref = DexMethod::make_method(cls->get_type(),
                                    DexString::make_string(name), proto);
  MethodCreator mc(ref, ACC_STATIC | ACC_PUBLIC);
  auto main_block = mc.get_main_block();
  auto loc = mc.make_local(type::_int());
  main_block->load_const(loc, val);
  main_block->ret_void();
  auto method = mc.create();
  cls->add_method(method);
  return method;
}

/**
 * Create a method like
 * void {{name}}() {
 *   while (true) {}
 * }
 */
DexMethod* make_loopy_method(DexClass* cls, const char* name) {
  auto proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto ref = DexMethod::make_method(cls->get_type(),
                                    DexString::make_string(name), proto);
  MethodCreator mc(ref, ACC_STATIC | ACC_PUBLIC);
  auto method = mc.create();
  method->set_code(assembler::ircode_from_string("((:begin) (goto :begin))"));
  cls->add_method(method);
  return method;
}

/**
 * Create a method like
 * public static void {{name}}(int x) {
 *   if (x != 0) {
 *     throw new RuntimeException("bla");
 *   }
 * }
 */
DexMethod* make_precondition_method(DexClass* cls, const char* name) {
  auto method_name = cls->get_name()->str() + "." + name;
  auto method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")" + method_name +
                                              R"(:(I)V"
      (
        (load-param v0)
        (if-eqz v0 :fail)
        (return-void)

        (:fail)
        (new-instance "Ljava/lang/RuntimeException;")
        (move-result-pseudo-object v1)
        (const-string "Bla")
        (move-result-pseudo-object v2)
        (invoke-direct (v1 v2) "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V")
        (throw v1)
     )
    )
  )");
  cls->add_method(method);
  return method;
}

/**
 * Create a method like
 * public static void {{name}}(int x) {
 *   if (x+0+0+0+0 != 0) {
 *     throw new RuntimeException("bla");
 *   }
 * }
 */
DexMethod* make_silly_precondition_method(DexClass* cls, const char* name) {
  auto method_name = cls->get_name()->str() + "." + name;
  auto method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")" + method_name +
                                              R"(:(I)V"
      (
        (load-param v0)
        (add-int/lit8 v0 v0 0)
        (add-int/lit8 v0 v0 0)
        (add-int/lit8 v0 v0 0)
        (add-int/lit8 v0 v0 0)
        (if-eqz v0 :fail)
        (return-void)

        (:fail)
        (new-instance "Ljava/lang/RuntimeException;")
        (move-result-pseudo-object v1)
        (const-string "Bla")
        (move-result-pseudo-object v2)
        (invoke-direct (v1 v2) "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V")
        (throw v1)
     )
    )
  )");
  cls->add_method(method);
  return method;
}

/**
 * Create a method like
 * public static void {{name}}(Boolean x) {
 *   if (Boolean.booleanValue() != 0) {
 *     throw new RuntimeException("bla");
 *   }
 * }
 */
DexMethod* make_unboxing_precondition_method(DexClass* cls, const char* name) {
  auto method_name = cls->get_name()->str() + "." + name;
  auto method = assembler::method_from_string(std::string("") + R"(
    (method (public static) ")" + method_name +
                                              R"(:(Ljava/lang/Boolean;)V"
      (
        (load-param-object v0)
        (invoke-virtual (v0) "Ljava/lang/Boolean;.booleanValue:()Z")
        (move-result v0)
        (if-eqz v0 :fail)
        (return-void)

        (:fail)
        (new-instance "Ljava/lang/RuntimeException;")
        (move-result-pseudo-object v1)
        (const-string "Bla")
        (move-result-pseudo-object v2)
        (invoke-direct (v1 v2) "Ljava/lang/RuntimeException;.<init>:(Ljava/lang/String;)V")
        (throw v1)
     )
    )
  )");
  cls->add_method(method);
  return method;
}

/**
 * Create a method calls other methods.
 * void {{name}}() {
 *   other1();
 *   other2();
 *   ...
 * }
 */
DexMethod* make_a_method_calls_others(DexClass* cls,
                                      const char* name,
                                      const std::vector<DexMethod*>& methods) {
  auto proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto ref = DexMethod::make_method(cls->get_type(),
                                    DexString::make_string(name), proto);
  MethodCreator mc(ref, ACC_STATIC | ACC_PUBLIC);
  auto main_block = mc.get_main_block();
  for (auto callee : methods) {
    main_block->invoke(callee, {});
  }
  main_block->ret_void();
  auto method = mc.create();
  cls->add_method(method);
  return method;
}

DexMethod* make_a_method_calls_others_with_arg(
    DexClass* cls,
    const char* name,
    const std::vector<std::pair<DexMethod*, int32_t>>& methods) {
  auto proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto ref = DexMethod::make_method(cls->get_type(),
                                    DexString::make_string(name), proto);
  MethodCreator mc(ref, ACC_STATIC | ACC_PUBLIC);
  auto main_block = mc.get_main_block();
  auto loc = mc.make_local(type::_int());
  for (auto& p : methods) {
    main_block->load_const(loc, p.second);
    main_block->invoke(p.first, {loc});
  }
  main_block->ret_void();
  auto method = mc.create();
  cls->add_method(method);
  return method;
}

DexMethod* make_a_method_calls_others_with_arg(
    DexClass* cls,
    const char* name,
    const std::vector<std::pair<DexMethod*, DexField*>>& methods) {
  auto proto =
      DexProto::make_proto(type::_void(), DexTypeList::make_type_list({}));
  auto ref = DexMethod::make_method(cls->get_type(),
                                    DexString::make_string(name), proto);
  MethodCreator mc(ref, ACC_STATIC | ACC_PUBLIC);
  auto main_block = mc.get_main_block();
  auto loc = mc.make_local(type::_int());
  for (auto& p : methods) {
    main_block->sget(p.second, loc);
    main_block->invoke(p.first, {loc});
  }
  main_block->ret_void();
  auto method = mc.create();
  cls->add_method(method);
  return method;
}

/*
 * Test that we correctly insert move instructions that map caller args to
 * callee params.
 */
TEST_F(MethodInlineTest, insertMoves) {
  using namespace dex_asm;
  auto callee = static_cast<DexMethod*>(DexMethod::make_method(
      "Lfoo;", "testCallee", "V", {"I", "Ljava/lang/Object;"}));
  callee->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  callee->set_code(std::make_unique<IRCode>(callee, 0));

  auto caller = static_cast<DexMethod*>(
      DexMethod::make_method("Lfoo;", "testCaller", "V", {}));
  caller->make_concrete(ACC_PUBLIC | ACC_STATIC, false);
  caller->set_code(std::make_unique<IRCode>(caller, 0));

  auto invoke = dasm(OPCODE_INVOKE_STATIC, callee, {});
  invoke->set_srcs_size(2);
  invoke->set_src(0, 1);
  invoke->set_src(1, 2);

  auto caller_code = caller->get_code();
  caller_code->push_back(dasm(OPCODE_CONST, {1_v, 1_L}));
  caller_code->push_back(dasm(OPCODE_CONST, {2_v, 0_L})); // load null ref
  caller_code->push_back(invoke);
  auto invoke_it = std::prev(caller_code->end());
  caller_code->push_back(dasm(OPCODE_RETURN_VOID));
  caller_code->set_registers_size(3);

  auto callee_code = callee->get_code();
  callee_code->push_back(dasm(OPCODE_CONST, {1_v, 1_L}));
  callee_code->push_back(dasm(OPCODE_RETURN_VOID));

  legacy_inliner::inline_method_unsafe(
      /*caller_method=*/nullptr,
      caller->get_code(),
      callee->get_code(),
      invoke_it);

  auto it = InstructionIterable(caller_code).begin();
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST, {1_v, 1_L}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST, {2_v, 0_L}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_MOVE, {3_v, 1_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_MOVE_OBJECT, {4_v, 2_v}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_CONST, {4_v, 1_L}));
  ++it;
  EXPECT_EQ(*it->insn, *dasm(OPCODE_RETURN_VOID));

  EXPECT_EQ(caller_code->get_registers_size(), 5);
}

TEST_F(MethodInlineTest, debugPositionsAfterReturn) {
  DexMethod* caller =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.caller:()V"));
  caller->make_concrete(ACC_PUBLIC, /* is_virtual */ false);
  DexMethod* callee =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.callee:()V"));
  callee->make_concrete(ACC_PUBLIC, /* is_virtual */ false);
  const auto& caller_str = R"(
    (
      (.pos:dbg_0 "LFoo;.caller:()V" "Foo.java" 10)
      (const v0 0)
      (invoke-static () "LFoo;.bar:()V")
      (return-void)
    )
  )";
  const auto& callee_str = R"(
    (
      (.pos:dbg_0 "LFoo;.callee:()V" "Foo.java" 123)
      (const v0 1)
      (if-eqz v0 :after)

      (:exit)
      (.pos:dbg_1 "LFoo;.callee:()V" "Foo.java" 124)
      (const v1 2)
      (return-void)

      (:after)
      (const v2 3)
      (goto :exit)
    )
  )";
  const auto& expected_str = R"(
    (
      (.pos:dbg_0 "LFoo;.caller:()V" "Foo.java" 10)
      (const v0 0)

      (.pos:dbg_1 "LFoo;.callee:()V" "Foo.java" 123 dbg_0)
      (const v1 1)
      (if-eqz v1 :after)

      (:exit)
      (.pos:dbg_2 "LFoo;.callee:()V" "Foo.java" 124 dbg_0)
      (const v2 2)
      (.pos:dbg_3 "LFoo;.caller:()V" "Foo.java" 10)
      (return-void)

      ; Check that this position was correctly added to the code after the
      ; callee's return
      (.pos:dbg_4 "LFoo;.callee:()V" "Foo.java" 124 dbg_0)
      (:after)
      (const v3 3)
      (goto :exit)
    )
  )";
  test_inliner(caller_str, callee_str, expected_str);
}

TEST_F(MethodInlineTest, test_intra_dex_inlining) {
  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  // Only inline methods within dex.
  bool intra_dex = true;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> canidates;
  std::unordered_set<DexMethod*> expected_inlined;
  auto foo_cls = create_a_class("Lfoo;");
  auto bar_cls = create_a_class("Lbar;");
  {
    // foo is in dex 2, bar is in dex 3.
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    store.add_classes({bar_cls});
    stores.push_back(std::move(store));
  }
  {
    auto foo_m1 = make_a_method(foo_cls, "foo_m1", 1);
    auto bar_m1 = make_a_method(bar_cls, "bar_m1", 2001);
    auto bar_m2 = make_a_method(bar_cls, "bar_m2", 2002);
    canidates.insert(foo_m1);
    canidates.insert(bar_m1);
    canidates.insert(bar_m2);
    // foo_main calls foo_m1 and bar_m2.
    auto foo_main =
        make_a_method_calls_others(foo_cls, "foo_main", {foo_m1, bar_m2});
    // bar_main calls bar_m1.
    auto bar_main = make_a_method_calls_others(bar_cls, "bar_main", {bar_m1});
    // Expect foo_m1 and bar_m1 be inlined if `intra_dex` is true.
    expected_inlined.insert(foo_m1);
    expected_inlined.insert(bar_m1);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);

  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             canidates, concurrent_resolver, inliner_config,
                             intra_dex ? IntraDex : InterDex);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), expected_inlined.size());
  for (auto method : expected_inlined) {
    EXPECT_EQ(inlined.count(method), 1);
  }
}

// Don't inline when it would exceed (configured) size
TEST_F(MethodInlineTest, size_limit) {
  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  DexStoresVector stores;
  std::unordered_set<DexMethod*> canidates;
  std::unordered_set<DexMethod*> expected_inlined;
  auto foo_cls = create_a_class("Lfoo;");
  auto bar_cls = create_a_class("Lbar;");
  {
    // foo is in dex 2, bar is in dex 3.
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    store.add_classes({bar_cls});
    stores.push_back(std::move(store));
  }
  {
    auto foo_m1 = make_a_method(foo_cls, "foo_m1", 1);
    auto bar_m1 = make_a_method(bar_cls, "bar_m1", 2001);
    auto bar_m2 = make_a_method(bar_cls, "bar_m2", 2002);
    canidates.insert(foo_m1);
    canidates.insert(bar_m1);
    canidates.insert(bar_m2);
    // foo_main calls foo_m1 and bar_m2.
    make_a_method_calls_others(foo_cls, "foo_main", {foo_m1, bar_m2});
    // bar_main calls bar_m1.
    make_a_method_calls_others(bar_cls, "bar_main", {bar_m1});
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);

  inliner::InlinerConfig inliner_config;
  inliner_config.soft_max_instruction_size = 0;
  inliner_config.populate(scope);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             canidates, concurrent_resolver, inliner_config,
                             IntraDex);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), 0);
}

TEST_F(MethodInlineTest, minimal_self_loop_regression) {
  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> expected_inlined;
  auto foo_cls = create_a_class("Lfoo;");
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  {
    auto foo_m1 = make_loopy_method(foo_cls, "foo_m1");
    candidates.insert(foo_m1);
    // foo_main calls foo_m1.
    make_a_method_calls_others(foo_cls, "foo_main", {foo_m1});
    expected_inlined.insert(foo_m1);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             candidates, concurrent_resolver, inliner_config,
                             intra_dex ? IntraDex : InterDex);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), expected_inlined.size());
  for (auto method : expected_inlined) {
    EXPECT_EQ(inlined.count(method), 1);
  }
}

TEST_F(MethodInlineTest, non_unique_inlined_registers) {
  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> expected_inlined;
  auto foo_cls = create_a_class("Lfoo;");
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  DexMethod* foo_main;
  {
    auto foo_m1 = make_a_method(foo_cls, "foo_m1", 1);
    auto foo_m2 = make_a_method(foo_cls, "foo_m2", 2);
    candidates.insert(foo_m1);
    candidates.insert(foo_m2);
    // foo_main calls foo_m1 and foo_m2.
    foo_main =
        make_a_method_calls_others(foo_cls, "foo_main", {foo_m1, foo_m2});
    expected_inlined.insert(foo_m1);
    expected_inlined.insert(foo_m2);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  inliner_config.unique_inlined_registers = false;
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             candidates, concurrent_resolver, inliner_config,
                             intra_dex ? IntraDex : InterDex);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), expected_inlined.size());
  for (auto method : expected_inlined) {
    EXPECT_EQ(inlined.count(method), 1);
  }

  // Note: the position is an artifact and may get cleaned up.
  const auto& expected_str = R"(
    (
      (.pos:dbg_0 "Lfoo;.foo_main:()V" UnknownSource 0)
      (const v0 1)
      (const v0 2)
      (return-void)
    )
  )";
  auto actual = foo_main->get_code();
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), actual);
}

TEST_F(MethodInlineTest, inline_beneficial_on_average_after_constant_prop) {
  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> expected_inlined;
  auto foo_cls = create_a_class("Lfoo;");
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  DexMethod *check_method, *foo_main;
  {
    create_runtime_exception_init();
    check_method = make_precondition_method(foo_cls, "check");
    candidates.insert(check_method);
    // foo_main calls check_method a few times.
    foo_main = make_a_method_calls_others_with_arg(foo_cls,
                                                   "foo_main",
                                                   {
                                                       {check_method, 1},
                                                       {check_method, 1},
                                                       {check_method, 1},
                                                       {check_method, 1},
                                                       {check_method, 1},
                                                       {check_method, 1},
                                                   });
    expected_inlined.insert(check_method);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  inliner_config.throws_inline = true;
  inliner_config.shrinker.run_const_prop = true;
  inliner_config.shrinker.run_local_dce = true;
  check_method->get_code()->build_cfg(true);
  foo_main->get_code()->build_cfg(true);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             candidates, concurrent_resolver, inliner_config,
                             intra_dex ? IntraDex : InterDex);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), expected_inlined.size());
  for (auto method : expected_inlined) {
    EXPECT_EQ(inlined.count(method), 1);
  }

  const auto& expected_str = R"(
    (
      (return-void)
    )
  )";
  foo_main->get_code()->clear_cfg();
  auto actual = foo_main->get_code();
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), actual);
}

TEST_F(MethodInlineTest,
       inline_beneficial_for_particular_instance_after_constant_prop) {
  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> expected_inlined;
  auto foo_cls = create_a_class("Lfoo;");
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  DexMethod *check_method, *foo_main;
  {
    create_runtime_exception_init();
    check_method = make_precondition_method(foo_cls, "check");
    candidates.insert(check_method);
    // foo_main calls check_method a few times.
    foo_main = make_a_method_calls_others_with_arg(foo_cls,
                                                   "foo_main",
                                                   {
                                                       {check_method, 0},
                                                       {check_method, 0},
                                                       {check_method, 1},
                                                       {check_method, 0},
                                                       {check_method, 0},
                                                       {check_method, 0},
                                                   });
    expected_inlined.insert(check_method);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  inliner_config.throws_inline = true;
  inliner_config.shrinker.run_const_prop = true;
  inliner_config.shrinker.run_local_dce = true;
  check_method->get_code()->build_cfg(true);
  foo_main->get_code()->build_cfg(true);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             candidates, concurrent_resolver, inliner_config,
                             intra_dex ? IntraDex : InterDex);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), expected_inlined.size());
  for (auto method : expected_inlined) {
    EXPECT_EQ(inlined.count(method), 1);
  }

  const auto& expected_str = R"(
    (
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (return-void)
    )
  )";
  foo_main->get_code()->clear_cfg();
  auto actual = foo_main->get_code();
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), actual);
}

TEST_F(
    MethodInlineTest,
    inline_beneficial_for_particular_instance_after_constant_prop_and_local_dce) {
  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> expected_inlined;
  auto foo_cls = create_a_class("Lfoo;");
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  DexMethod *check_method, *foo_main;
  {
    create_runtime_exception_init();
    check_method = make_silly_precondition_method(foo_cls, "check");
    candidates.insert(check_method);
    // foo_main calls check_method a few times.
    foo_main = make_a_method_calls_others_with_arg(foo_cls,
                                                   "foo_main",
                                                   {
                                                       {check_method, 0},
                                                       {check_method, 0},
                                                       {check_method, 1},
                                                       {check_method, 0},
                                                       {check_method, 0},
                                                       {check_method, 0},
                                                   });
    expected_inlined.insert(check_method);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  inliner_config.throws_inline = true;
  inliner_config.shrinker.run_const_prop = true;
  inliner_config.shrinker.run_local_dce = true;
  check_method->get_code()->build_cfg(true);
  foo_main->get_code()->build_cfg(true);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             candidates, concurrent_resolver, inliner_config,
                             intra_dex ? IntraDex : InterDex);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), expected_inlined.size());
  for (auto method : expected_inlined) {
    EXPECT_EQ(inlined.count(method), 1);
  }

  const auto& expected_str = R"(
    (
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (return-void)
    )
  )";
  foo_main->get_code()->clear_cfg();
  auto actual = foo_main->get_code();
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), actual);
}

TEST_F(MethodInlineTest, throw_after_no_return) {
  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  auto foo_cls = create_a_class("Lfoo;");
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  DexMethod *check_method, *foo_main;
  {
    create_runtime_exception_init();
    check_method = make_silly_precondition_method(foo_cls, "check");
    candidates.insert(check_method);
    // foo_main calls check_method a few times. Already the first call is one
    // that will always throw.
    foo_main = make_a_method_calls_others_with_arg(foo_cls,
                                                   "foo_main",
                                                   {
                                                       {check_method, 0},
                                                       {check_method, 0},
                                                       {check_method, 1},
                                                   });
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  inliner_config.throws_inline = true;
  inliner_config.throw_after_no_return = true;
  check_method->get_code()->build_cfg(true);
  foo_main->get_code()->build_cfg(true);
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             candidates, concurrent_resolver, inliner_config,
                             intra_dex ? IntraDex : InterDex);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), 0);

  const auto& expected_str = R"(
    (
      (const v0 0)
      (invoke-static (v0) "Lfoo;.check:(I)V")
      (const v1 0)
      (throw v1)
    )
  )";
  foo_main->get_code()->clear_cfg();
  auto actual = foo_main->get_code();
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), actual);
}

TEST_F(MethodInlineTest, boxed_boolean) {
  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> expected_inlined;
  auto foo_cls = create_a_class("Lfoo;");
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  DexMethod *check_method, *foo_main;
  {
    create_runtime_exception_init();
    check_method = make_unboxing_precondition_method(foo_cls, "check");
    candidates.insert(check_method);
    // foo_main calls check_method a few times.
    auto FALSE_field = (DexField*)DexField::get_field(
        "Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;");
    always_assert(FALSE_field != nullptr);
    auto TRUE_field = (DexField*)DexField::get_field(
        "Ljava/lang/Boolean;.TRUE:Ljava/lang/Boolean;");
    always_assert(TRUE_field != nullptr);
    foo_main =
        make_a_method_calls_others_with_arg(foo_cls,
                                            "foo_main",
                                            {
                                                {check_method, FALSE_field},
                                                {check_method, FALSE_field},
                                                {check_method, TRUE_field},
                                                {check_method, FALSE_field},
                                                {check_method, FALSE_field},
                                                {check_method, FALSE_field},
                                            });
    expected_inlined.insert(check_method);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  inliner_config.throws_inline = true;
  inliner_config.shrinker.run_const_prop = true;
  inliner_config.shrinker.run_local_dce = true;
  inliner_config.shrinker.compute_pure_methods = false;
  check_method->get_code()->build_cfg(true);
  foo_main->get_code()->build_cfg(true);
  std::unordered_set<DexMethodRef*> pure_methods{
      DexMethod::get_method("Ljava/lang/Boolean;.booleanValue:()Z")};
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             candidates, concurrent_resolver, inliner_config,
                             intra_dex ? IntraDex : InterDex,
                             /* true_virtual_callers */ {},
                             /* inline_for_speed */ nullptr,
                             /* analyze_and_prune_inits */ false, pure_methods);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), expected_inlined.size());
  for (auto method : expected_inlined) {
    EXPECT_EQ(inlined.count(method), 1);
  }

  const auto& expected_str = R"(
    (
      (.pos:dbg_0 "Lfoo;.foo_main:()V" UnknownSource 0)
      (sget-object "Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;")
      (move-result-pseudo-object v0)
      (invoke-static (v0) "Lfoo;.check:(Ljava/lang/Boolean;)V")
      (sget-object "Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;")
      (move-result-pseudo-object v0)
      (invoke-static (v0) "Lfoo;.check:(Ljava/lang/Boolean;)V")
      (sget-object "Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;")
      (move-result-pseudo-object v0)
      (invoke-static (v0) "Lfoo;.check:(Ljava/lang/Boolean;)V")
      (sget-object "Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;")
      (move-result-pseudo-object v0)
      (invoke-static (v0) "Lfoo;.check:(Ljava/lang/Boolean;)V")
      (sget-object "Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;")
      (move-result-pseudo-object v0)
      (invoke-static (v0) "Lfoo;.check:(Ljava/lang/Boolean;)V")
      (return-void)
    )
  )";
  foo_main->get_code()->clear_cfg();
  auto actual = foo_main->get_code();
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), actual);
}

TEST_F(MethodInlineTest, boxed_boolean_without_shrinking) {
  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> expected_inlined;
  auto foo_cls = create_a_class("Lfoo;");
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  DexMethod *check_method, *foo_main;
  {
    create_runtime_exception_init();
    check_method = make_unboxing_precondition_method(foo_cls, "check");
    candidates.insert(check_method);
    // foo_main calls check_method a few times.
    auto FALSE_field = (DexField*)DexField::get_field(
        "Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;");
    always_assert(FALSE_field != nullptr);
    auto TRUE_field = (DexField*)DexField::get_field(
        "Ljava/lang/Boolean;.TRUE:Ljava/lang/Boolean;");
    always_assert(TRUE_field != nullptr);
    foo_main =
        make_a_method_calls_others_with_arg(foo_cls,
                                            "foo_main",
                                            {
                                                {check_method, TRUE_field},
                                                {check_method, FALSE_field},
                                            });
    expected_inlined.insert(check_method);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  inliner_config.throws_inline = true;
  check_method->get_code()->build_cfg(true);
  foo_main->get_code()->build_cfg(true);
  std::unordered_set<DexMethodRef*> pure_methods{
      DexMethod::get_method("Ljava/lang/Boolean;.booleanValue:()Z")};
  init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
      scope, /* create_init_class_insns */ false);
  MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                             candidates, concurrent_resolver, inliner_config,
                             intra_dex ? IntraDex : InterDex,
                             /* true_virtual_callers */ {},
                             /* inline_for_speed */ nullptr,
                             /* analyze_and_prune_inits */ false, pure_methods);
  inliner.inline_methods();
  auto inlined = inliner.get_inlined();
  EXPECT_EQ(inlined.size(), expected_inlined.size());
  for (auto method : expected_inlined) {
    EXPECT_EQ(inlined.count(method), 1);
  }

  const auto& expected_str = R"(
    (
      (.pos:dbg_0 "Lfoo;.foo_main:()V" UnknownSource 0)
      (sget-object "Ljava/lang/Boolean;.TRUE:Ljava/lang/Boolean;")
      (move-result-pseudo-object v0)
      (move-object v1 v0)
      (sget-object "Ljava/lang/Boolean;.FALSE:Ljava/lang/Boolean;")
      (move-result-pseudo-object v0)
      (invoke-static (v0) "Lfoo;.check:(Ljava/lang/Boolean;)V")
      (return-void)
    )
  )";

  foo_main->get_code()->clear_cfg();
  auto actual = foo_main->get_code();
  auto expected = assembler::ircode_from_string(expected_str);
  EXPECT_CODE_EQ(expected.get(), actual);
}

TEST_F(MethodInlineTest, visibility_change_static_invoke) {
  auto foo_cls = create_a_class("LFoo;");
  auto bar_cls = create_a_class("LBar;");

  DexMethod* caller =
      static_cast<DexMethod*>(DexMethod::make_method("LBar;.caller:()V"));
  caller->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);

  DexMethod* callee =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.callee:()V"));
  callee->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);
  DexMethod* nested_callee = static_cast<DexMethod*>(
      DexMethod::make_method("LFoo;.nested_callee:()V"));
  nested_callee->make_concrete(ACC_PRIVATE, /* is_virtual */ false);

  DexMethod* caller_inside = static_cast<DexMethod*>(
      DexMethod::make_method("LFoo;.caller_inside:()V"));
  caller_inside->make_concrete(ACC_PRIVATE,
                               /* is_virtual */ false);

  DexMethod* nested_callee_2 = static_cast<DexMethod*>(
      DexMethod::make_method("LFoo;.nested_callee_2:()V"));
  nested_callee_2->make_concrete(ACC_PRIVATE, /* is_virtual */ false);

  DexMethod* init =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.<init>:()V"));
  init->make_concrete(ACC_CONSTRUCTOR | ACC_PUBLIC, /* is_virtual */ false);

  bar_cls->add_method(caller);

  foo_cls->add_method(init);
  foo_cls->add_method(callee);
  foo_cls->add_method(nested_callee);
  foo_cls->add_method(nested_callee_2);
  foo_cls->add_method(caller_inside);

  const auto& caller_str = R"(
    (
      (const v0 0)
      (invoke-static () "LFoo;.callee:()V")
      (return-void)
    )
  )";

  caller->set_code(assembler::ircode_from_string(caller_str));

  const auto& callee_str = R"(
    (
      (const v0 1)

      (new-instance "LFoo;")
      (move-result-pseudo-object v1)
      (invoke-direct (v1) "LFoo;.<init>:()V")
      (invoke-direct (v1) "LFoo;.nested_callee:()V")

      (if-eqz v0 :after)

      (:exit)
      (const v1 2)
      (return-void)

      (:after)
      (const v2 3)
      (goto :exit)
    )
  )";

  const auto& caller_inside_str = R"(
    (
      (load-param-object v1)
      (invoke-direct (v1) "LFoo;.nested_callee:()V")
      (const v0 0)
      (return-void)
    )
  )";

  const auto& nested_callee_str = R"(
    (
      (load-param-object v1)
      (invoke-direct (v1) "LFoo;.nested_callee_2:()V")
      (const v0 0)
      (return-void)
    )
  )";

  const auto& nested_callee_2_str = R"(
    (
      (load-param-object v1)
      (const v0 0)
      (return-void)
    )
  )";

  const auto& init_str = R"(
    (
      (load-param-object v0)
      (invoke-direct (v0) "Ljava/lang/Object;.<init>:()V")
      (return-void)
    )
  )";

  callee->set_code(assembler::ircode_from_string(callee_str));
  nested_callee->set_code(assembler::ircode_from_string(nested_callee_str));
  caller_inside->set_code(assembler::ircode_from_string(caller_inside_str));
  nested_callee_2->set_code(assembler::ircode_from_string(nested_callee_2_str));
  init->set_code(assembler::ircode_from_string(init_str));

  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> expected_inlined;
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls, bar_cls});
    stores.push_back(std::move(store));
  }
  {
    candidates.insert(callee);
    candidates.insert(nested_callee);
    expected_inlined.insert(callee);
    expected_inlined.insert(nested_callee);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  inliner_config.throws_inline = true;
  inliner_config.shrinker.run_const_prop = false;
  inliner_config.shrinker.run_local_dce = false;
  inliner_config.shrinker.compute_pure_methods = false;

  caller->get_code()->build_cfg(true);
  callee->get_code()->build_cfg(true);
  nested_callee->get_code()->build_cfg(true);
  caller_inside->get_code()->build_cfg(true);
  nested_callee_2->get_code()->build_cfg(true);
  init->get_code()->build_cfg(true);

  {
    init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
        scope, /* create_init_class_insns */ false);
    MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                               candidates, concurrent_resolver, inliner_config,
                               intra_dex ? IntraDex : InterDex,
                               /* true_virtual_callers */ {},
                               /* inline_for_speed */ nullptr,
                               /* analyze_and_prune_inits */ false, {});
    inliner.inline_methods();

    auto inlined = inliner.get_inlined();
    EXPECT_EQ(inlined.size(), expected_inlined.size());
    for (auto method : expected_inlined) {
      EXPECT_EQ(inlined.count(method), 1);
    }
  }

  caller->get_code()->clear_cfg();
  callee->get_code()->clear_cfg();
  nested_callee->get_code()->clear_cfg();
  caller_inside->get_code()->clear_cfg();
  nested_callee_2->get_code()->clear_cfg();
  init->get_code()->clear_cfg();

  EXPECT_TRUE(is_public(nested_callee_2));

  // visibility does not change, as the call to nested_callee is
  // futher inlined to nested_callee's code
  EXPECT_TRUE(is_private(nested_callee));

  const auto& caller_expected_str = R"(
    (
      (.pos:dbg_0 "LBar;.caller:()V" UnknownSource 0)
      (const v0 0)
      (.pos:dbg_1 "LFoo;.callee:()V" UnknownSource 0 dbg_0)
      (const v1 1)
      (new-instance "LFoo;")
      (move-result-pseudo-object v2)
      (invoke-direct (v2) "LFoo;.<init>:()V")
      (move-object v5 v2)
      (invoke-static (v5) "LFoo;.nested_callee_2:(LFoo;)V")
      (const v4 0)
      (if-eqz v1 :L1)
      (:L0)
      (const v2 2)
      (.pos:dbg_2 "LBar;.caller:()V" UnknownSource 0)
      (return-void)
      (:L1)
      (const v3 3)
      (goto :L0)
    )
  )";

  auto caller_actual = caller->get_code();
  auto caller_expected = assembler::ircode_from_string(caller_expected_str);
  EXPECT_CODE_EQ(caller_actual, caller_expected.get());

  const auto& caller_inside_expected_str = R"(
    (
      (load-param-object v1)
      (.pos:dbg_0 "LFoo;.caller_inside:()V" UnknownSource 0)
      (move-object v3 v1)
      (invoke-static (v3) "LFoo;.nested_callee_2:(LFoo;)V")
      (const v2 0)
      (const v0 0)
      (return-void)
    )
  )";

  auto caller_inside_actual = caller_inside->get_code();
  auto caller_inside_expected =
      assembler::ircode_from_string(caller_inside_expected_str);
  EXPECT_CODE_EQ(caller_inside_actual, caller_inside_expected.get());

  const auto& nested_callee_expected_str = R"(
    (
      (load-param-object v1)
      (invoke-static (v1) "LFoo;.nested_callee_2:(LFoo;)V")
      (const v0 0)
      (return-void)
    )
  )";

  auto nested_callee_actual = nested_callee->get_code();
  auto nested_callee_expected =
      assembler::ircode_from_string(nested_callee_expected_str);
  EXPECT_CODE_EQ(nested_callee_actual, nested_callee_expected.get());
}

TEST_F(MethodInlineTest, unused_result) {
  auto foo_cls = create_a_class("LFoo;");
  auto bar_cls = create_a_class("LBar;");

  DexMethod* caller =
      static_cast<DexMethod*>(DexMethod::make_method("LBar;.caller:()V"));
  caller->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);

  DexMethod* callee =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.callee:(I)I"));
  callee->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);

  bar_cls->add_method(caller);

  foo_cls->add_method(callee);

  const auto& caller_str = R"(
    (
      (const v0 0)
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (return-void)
    )
  )";

  caller->set_code(assembler::ircode_from_string(caller_str));

  const auto& callee_str = R"(
    (
      (load-param v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (return v0)
    )
  )";

  callee->set_code(assembler::ircode_from_string(callee_str));

  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> expected_inlined;
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls, bar_cls});
    stores.push_back(std::move(store));
  }
  {
    candidates.insert(callee);
    expected_inlined.insert(callee);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  inliner_config.multiple_callers = true;
  inliner_config.use_call_site_summaries = true;
  inliner_config.shrinker.run_local_dce = true;
  inliner_config.shrinker.compute_pure_methods = false;

  caller->get_code()->build_cfg(true);
  callee->get_code()->build_cfg(true);

  {
    init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
        scope, /* create_init_class_insns */ false);
    MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                               candidates, concurrent_resolver, inliner_config,
                               intra_dex ? IntraDex : InterDex,
                               /* true_virtual_callers */ {},
                               /* inline_for_speed */ nullptr,
                               /* analyze_and_prune_inits */ false, {});
    inliner.inline_methods();

    auto inlined = inliner.get_inlined();
    EXPECT_EQ(inlined.size(), expected_inlined.size());
    for (auto method : expected_inlined) {
      EXPECT_EQ(inlined.count(method), 1);
    }
  }

  caller->get_code()->clear_cfg();
  callee->get_code()->clear_cfg();

  const auto& caller_expected_str = R"(
    (
      (return-void)
    )
  )";

  auto caller_actual = caller->get_code();
  auto caller_expected = assembler::ircode_from_string(caller_expected_str);
  EXPECT_CODE_EQ(caller_actual, caller_expected.get());
}

// top-down call-site analysis will determine that it's beneficial to inline
// across all nested call-sites
TEST_F(MethodInlineTest, caller_caller_callee_call_site) {
  auto foo_cls = create_a_class("LFoo;");

  DexMethod* outer_caller =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.outer_caller:()V"));
  outer_caller->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);

  DexMethod* inner_caller = static_cast<DexMethod*>(
      DexMethod::make_method("LFoo;.inner_caller:(I)V"));
  inner_caller->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);

  DexMethod* callee =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.callee:(I)I"));
  callee->make_concrete(ACC_PUBLIC | ACC_STATIC, /* is_virtual */ false);

  foo_cls->add_method(outer_caller);
  foo_cls->add_method(inner_caller);
  foo_cls->add_method(callee);

  const auto& outer_caller_str = R"(
    (
      (const v0 1)
      (invoke-static (v0) "LFoo;.inner_caller:(I)V")
      (invoke-static (v0) "LFoo;.inner_caller:(I)V")
      (invoke-static (v0) "LFoo;.inner_caller:(I)V")
      (invoke-static (v0) "LFoo;.inner_caller:(I)V")
      (invoke-static (v0) "LFoo;.inner_caller:(I)V")
      (invoke-static (v0) "LFoo;.inner_caller:(I)V")
      (invoke-static (v0) "LFoo;.inner_caller:(I)V")
      (invoke-static (v0) "LFoo;.inner_caller:(I)V")
      (invoke-static (v0) "LFoo;.inner_caller:(I)V")
      (invoke-static (v0) "LFoo;.inner_caller:(I)V")
      (return-void)
    )
  )";

  outer_caller->set_code(assembler::ircode_from_string(outer_caller_str));

  const auto& inner_caller_str = R"(
    (
      (load-param v0)
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (invoke-static (v0) "LFoo;.callee:(I)I")
      (return-void)
    )
  )";

  inner_caller->set_code(assembler::ircode_from_string(inner_caller_str));

  const auto& callee_str = R"(
    (
      (load-param v0)
      (if-nez v0 :exit)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (add-int v0 v0 v0)
      (:exit)
      (return v0)
    )
  )";

  callee->set_code(assembler::ircode_from_string(callee_str));

  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  bool intra_dex = false;

  DexStoresVector stores;
  std::unordered_set<DexMethod*> candidates;
  std::unordered_set<DexMethod*> expected_inlined;
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  {
    candidates.insert(inner_caller);
    candidates.insert(callee);
    expected_inlined.insert(inner_caller);
    expected_inlined.insert(callee);
  }
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);
  inliner_config.multiple_callers = true;
  inliner_config.use_call_site_summaries = true;
  inliner_config.throws_inline = true;
  inliner_config.shrinker.run_local_dce = true;
  inliner_config.shrinker.run_const_prop = true;
  inliner_config.shrinker.compute_pure_methods = false;

  outer_caller->get_code()->build_cfg(true);
  inner_caller->get_code()->build_cfg(true);
  callee->get_code()->build_cfg(true);

  {
    init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
        scope, /* create_init_class_insns */ false);
    MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                               candidates, concurrent_resolver, inliner_config,
                               intra_dex ? IntraDex : InterDex,
                               /* true_virtual_callers */ {},
                               /* inline_for_speed */ nullptr,
                               /* analyze_and_prune_inits */ false, {});
    inliner.inline_methods();

    auto inlined = inliner.get_inlined();
    EXPECT_EQ(inlined.size(), expected_inlined.size());
    for (auto method : expected_inlined) {
      EXPECT_EQ(inlined.count(method), 1);
    }
  }

  outer_caller->get_code()->clear_cfg();
  inner_caller->get_code()->clear_cfg();
  callee->get_code()->clear_cfg();

  const auto& outer_caller_expected_str = R"(
    (
      (return-void)
    )
  )";

  auto outer_caller_actual = outer_caller->get_code();

  // Let's filter out all positions.
  // TODO: Enhance position filtering so that we don't get redundant positions.
  remove_position(outer_caller_actual);

  auto outer_caller_expected =
      assembler::ircode_from_string(outer_caller_expected_str);
  EXPECT_CODE_EQ(outer_caller_actual, outer_caller_expected.get());
}

TEST_F(MethodInlineTest,
       dont_inline_callee_with_tries_and_no_catch_all_at_sketchy_call_site) {
  auto foo_cls = create_a_class("LFoo;");

  DexMethod* caller = static_cast<DexMethod*>(
      DexMethod::make_method("LFoo;.sketchyCaller:()V"));
  caller->make_concrete(ACC_PRIVATE, /* is_virtual */ false);

  DexMethod* callee =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.callee:()V"));
  callee->make_concrete(ACC_PRIVATE, /* is_virtual */ false);

  foo_cls->add_method(caller);
  foo_cls->add_method(callee);

  const auto& caller_str = R"(
    (
      (load-param v0)
      (monitor-enter v0)

      (.try_start a)
      (invoke-static () "LBar;.canThrowInsideTry:()V")
      (.try_end a)
      (invoke-direct (v0) "LFoo;.callee:()V")

      (.catch (a))
      (monitor-exit v0)
      (return-void)
    )
  )";

  caller->set_code(assembler::ircode_from_string(caller_str));

  const auto& callee_str = R"(
    (
      (load-param-object v0)

      (.try_start a)
      (invoke-static () "LBar;.canThrowNotImportant:()V")
      (.try_end a)

      (.catch (a) "LSomeSpecificType;")
      (return-void)
    )
  )";

  callee->set_code(assembler::ircode_from_string(callee_str));

  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  DexStoresVector stores;
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  std::unordered_set<DexMethod*> candidates{callee};
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);

  caller->get_code()->build_cfg(true);
  callee->get_code()->build_cfg(true);

  {
    init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
        scope, /* create_init_class_insns */ false);
    MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                               candidates, concurrent_resolver, inliner_config,
                               IntraDex,
                               /* true_virtual_callers */ {},
                               /* inline_for_speed */ nullptr,
                               /* analyze_and_prune_inits */ false, {});
    inliner.inline_methods();

    auto inlined = inliner.get_inlined();
    EXPECT_EQ(inlined.size(), 0);
  }
}

TEST_F(MethodInlineTest, dont_inline_sketchy_callee_into_into_try) {
  auto foo_cls = create_a_class("LFoo;");

  DexMethod* caller =
      static_cast<DexMethod*>(DexMethod::make_method("LFoo;.caller:()V"));
  caller->make_concrete(ACC_PRIVATE, /* is_virtual */ false);

  DexMethod* callee = static_cast<DexMethod*>(
      DexMethod::make_method("LFoo;.sketchy_callee:()V"));
  callee->make_concrete(ACC_PRIVATE, /* is_virtual */ false);

  foo_cls->add_method(caller);
  foo_cls->add_method(callee);

  const auto& caller_str = R"(
    (
      (load-param-object v0)

      (.try_start a)
      (invoke-direct (v0) "LFoo;.sketchy_callee:()V")
      (.try_end a)

      (.catch (a) "LWhatEver;")
      (return-void)
    )
  )";

  caller->set_code(assembler::ircode_from_string(caller_str));

  const auto& callee_str = R"(
    (
      (load-param v0)
      (monitor-enter v0)

      (.try_start a)
      (invoke-static () "LBar;.canThrowNotImportant:()V")
      (.try_end a)
      (invoke-static () "LBar;.canThrowOutsideTry:()V")

      (.catch (a))
      (monitor-exit v0)
      (return-void)
    )
  )";

  callee->set_code(assembler::ircode_from_string(callee_str));

  ConcurrentMethodRefCache concurrent_resolve_cache;
  auto concurrent_resolver = [&concurrent_resolve_cache](DexMethodRef* method,
                                                         MethodSearch search) {
    return resolve_method(method, search, concurrent_resolve_cache);
  };

  DexStoresVector stores;
  {
    DexStore store("root");
    store.add_classes({});
    store.add_classes({foo_cls});
    stores.push_back(std::move(store));
  }
  std::unordered_set<DexMethod*> candidates{callee};
  auto scope = build_class_scope(stores);
  api::LevelChecker::init(0, scope);
  inliner::InlinerConfig inliner_config;
  inliner_config.populate(scope);

  caller->get_code()->build_cfg(true);
  callee->get_code()->build_cfg(true);

  {
    init_classes::InitClassesWithSideEffects init_classes_with_side_effects(
        scope, /* create_init_class_insns */ false);
    MultiMethodInliner inliner(scope, init_classes_with_side_effects, stores,
                               candidates, concurrent_resolver, inliner_config,
                               IntraDex,
                               /* true_virtual_callers */ {},
                               /* inline_for_speed */ nullptr,
                               /* analyze_and_prune_inits */ false, {});
    inliner.inline_methods();

    auto inlined = inliner.get_inlined();
    EXPECT_EQ(inlined.size(), 0);
  }
}
