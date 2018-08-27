/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/program_options.hpp>
#include <iostream>

#include "DexClass.h"
#include "PassManager.h"
#include "PassRegistry.h"
#include "Timer.h"
#include "ToolsCommon.h"

namespace {

struct Arguments {
  std::string input_ir_dir;
  std::string output_ir_dir;
  std::vector<std::string> pass_names;
};

Arguments parse_args(int argc, char* argv[]) {
  namespace po = boost::program_options;
  po::options_description desc(
      "Run one pass with dex and IR meta as input and output");
  desc.add_options()("help,h", "produce help message");
  desc.add_options()("input-ir,i", po::value<std::string>(),
                     "input dex and IR meta directory");
  desc.add_options()("output-ir,o",
                     po::value<std::string>(),
                     "output dex and IR meta directory");
  desc.add_options()("pass-name,p", po::value<std::vector<std::string>>(),
                     "pass name");

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);

  if (vm.count("help")) {
    desc.print(std::cout);
    exit(EXIT_SUCCESS);
  }

  Arguments args;

  if (vm.count("input-ir")) {
    args.input_ir_dir = vm["input-ir"].as<std::string>();
  }

  if (vm.count("output-ir")) {
    args.output_ir_dir = vm["output-ir"].as<std::string>();
  }
  if (args.output_ir_dir.empty() ||
      !redex::dir_is_writable(args.output_ir_dir)) {
    std::cerr << "output-dir is empty or not writable\n";
    exit(EXIT_FAILURE);
  }

  if (vm.count("pass-name")) {
    args.pass_names = vm["pass-name"].as<std::vector<std::string>>();
  }

  return args;
}

/**
 * Process entry_data : Load config file and change the passes list
 */
Json::Value process_entry_data(const Json::Value& entry_data,
                               const Arguments& args) {
  Json::Value config_data =
      redex::parse_config(entry_data["config"].asString());
  // Change passes list in config data.
  config_data["redex"]["passes"] = Json::arrayValue;
  Json::Value& passes_list = config_data["redex"]["passes"];
  for (const std::string& pass_name : args.pass_names) {
    passes_list.append(pass_name);
  }
  int len = config_data["redex"]["passes"].size();
  if (len > 0 && passes_list[len - 1].asString() != "RegAllocPass") {
    passes_list.append("RegAllocPass");
  }

  // apk_dir
  if (entry_data.isMember("apk_dir")) {
    config_data["apk_dir"] = entry_data["apk_dir"].asString();
  }

  return config_data;
}
} // namespace

int main(int argc, char* argv[]) {
  Timer opt_timer("Redex-opt");
  Arguments args = parse_args(argc, argv);

  g_redex = new RedexContext();

  Json::Value entry_data;

  DexStoresVector stores;

  redex::load_all_intermediate(args.input_ir_dir, stores, &entry_data);

  Json::Value config_data = process_entry_data(entry_data, args);
  ConfigFiles cfg(std::move(config_data), args.output_ir_dir);

  const auto& passes = PassRegistry::get().get_passes();
  PassManager manager(passes, config_data);
  manager.set_testing_mode();
  manager.run_passes(stores, cfg);

  redex::write_all_intermediate(cfg, args.output_ir_dir, stores, entry_data);

  delete g_redex;
  return 0;
}
