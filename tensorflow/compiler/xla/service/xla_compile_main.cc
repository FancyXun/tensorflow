/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/DialectRegistry.h"  // from @llvm-project
#include "mlir/Parser/Parser.h"  // from @llvm-project
#include "tensorflow/compiler/xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "tensorflow/compiler/xla/pjrt/mlir_to_hlo.h"
#include "tensorflow/compiler/xla/service/compiler.h"
#include "tensorflow/compiler/xla/service/cpu/cpu_compiler.h"
#include "tensorflow/compiler/xla/service/cpu/cpu_executable.h"
#include "tensorflow/compiler/xla/service/gpu/executable.pb.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_compiler.h"
#include "tensorflow/compiler/xla/service/gpu/nvptx_compiler.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/tsl/platform/env.h"
#include "tensorflow/tsl/platform/init_main.h"
#include "tensorflow/tsl/platform/protobuf.h"
#include "tensorflow/tsl/util/command_line_flags.h"

namespace xla {
namespace xla_compile {

const char kUsageHeader[] =
    "xla_compile performs ahead-of-time compilation of a MHLO module,\n"
    "resulting in an AotCompilationResult compiled for CPU.\n"
    "A typical invocation looks like this:\n"
    "\n"
    "   $ xla_compile --mhlo_file=mymhlo.mlir --output_file=output "
    "--platform=cpu"
    "\n";

StatusOr<std::string> AotCompileCpuExecutable(
    std::unique_ptr<HloModule> hlo_module) {
  cpu::CpuCompiler cpu_compiler;
  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<cpu::CpuExecutable> cpu_executable,
      cpu_compiler.CompileXlaRuntimeCpuExecutable(std::move(hlo_module)));
  TF_ASSIGN_OR_RETURN(std::unique_ptr<AotCompilationResult> aot_result,
                      cpu_compiler.Export(cpu_executable.get()));
  TF_ASSIGN_OR_RETURN(std::string result, aot_result->SerializeAsString());
  return result;
}

StatusOr<std::string> AotCompileGpuExecutable(
    std::unique_ptr<HloModule> hlo_module,
    const gpu::GpuTargetConfig& gpu_target_config) {
  gpu::NVPTXCompiler nvptx_compiler;
  auto module_group = std::make_unique<HloModuleGroup>(std::move(hlo_module));
  AotCompilationOptions aot_options(nvptx_compiler.PlatformId());
  aot_options.set_target_config(gpu_target_config);
  TF_ASSIGN_OR_RETURN(
      std::vector<std::unique_ptr<AotCompilationResult>> aot_results,
      nvptx_compiler.CompileAheadOfTime(std::move(module_group), aot_options));
  TF_ASSIGN_OR_RETURN(std::string result, aot_results[0]->SerializeAsString());
  return result;
}

xla::Status XlaCompileMain(const std::string& mhlo_path,
                           const std::string& output_path,
                           const std::string& platform,
                           const std::string& gpu_target_config_path) {
  std::string mhlo_string;
  TF_RETURN_IF_ERROR(
      tsl::ReadFileToString(tsl::Env::Default(), mhlo_path, &mhlo_string));

  mlir::DialectRegistry dialects;
  // TODO(b/248362914): Register all required dialects.
  dialects.insert<mlir::arith::ArithDialect>();
  dialects.insert<mlir::mhlo::MhloDialect>();
  dialects.insert<mlir::func::FuncDialect>();

  // Parse MHLO module.
  auto threading = mlir::MLIRContext::Threading::DISABLED;
  auto ctx = std::make_unique<mlir::MLIRContext>(dialects, threading);
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::parseSourceString<mlir::ModuleOp>(mhlo_string, ctx.get());

  // Convert Mhlo to Hlo Module.
  XlaComputation xla_computation;
  TF_RETURN_IF_ERROR(
      MlirToXlaComputation(*module, xla_computation, false, false));
  HloModuleProto hlo_module_proto = xla_computation.proto();

  TF_ASSIGN_OR_RETURN(ProgramShape shape, xla_computation.GetProgramShape());
  DebugOptions debug_options;
  debug_options.set_xla_gpu_enable_xla_runtime_executable(true);
  debug_options.set_xla_backend_optimization_level(2);
  HloModuleConfig config(shape);
  config.set_debug_options(debug_options);
  TF_ASSIGN_OR_RETURN(std::unique_ptr<HloModule> hlo_module,
                      HloModule::CreateFromProto(hlo_module_proto, config));

  // Run AOT compilation.
  std::string result;
  if (platform == "cpu") {
    TF_ASSIGN_OR_RETURN(result, AotCompileCpuExecutable(std::move(hlo_module)));
  } else if (platform == "gpu") {
    // Parse GpuTargetConfig.
    std::string gpu_target_config_string;
    TF_RETURN_IF_ERROR(tsl::ReadFileToString(tsl::Env::Default(),
                                             gpu_target_config_path,
                                             &gpu_target_config_string));
    stream_executor::GpuTargetConfigProto gpu_target_config_proto;
    bool ok = tsl::protobuf::TextFormat::ParseFromString(
        gpu_target_config_string, &gpu_target_config_proto);
    if (!ok) return FailedPrecondition("Failed to parse GpuTargetConfigProto");
    gpu::GpuTargetConfig gpu_target_config(gpu_target_config_proto);

    TF_ASSIGN_OR_RETURN(result, AotCompileGpuExecutable(std::move(hlo_module),
                                                        gpu_target_config));
  } else {
    return Unimplemented("platform %s not supported", platform);
  }

  TF_RETURN_IF_ERROR(
      tsl::WriteStringToFile(tsl::Env::Default(), output_path, result));
  return OkStatus();
}

}  // end namespace xla_compile
}  // end namespace xla

// Read the input file containing the MHLO module, and write a Serialized
// AotCompilationResult to the output file.
int main(int argc, char* argv[]) {
  std::string mhlo_path;
  std::string output_path;
  std::string platform;
  std::string gpu_target_config_path;
  std::vector<tsl::Flag> flag_list = {
      tsl::Flag("mhlo_file", &mhlo_path, "The path to MHLO file"),
      tsl::Flag("output_file", &output_path, "The path to the output file"),
      tsl::Flag("platform", &platform,
                "The platform on which the built executable runs"),
      tsl::Flag("gpu_target_config", &gpu_target_config_path,
                "The path to serialized GpuTargetConfig")};

  tsl::string usage = xla::xla_compile::kUsageHeader;
  usage += tsl::Flags::Usage(argv[0], flag_list);
  if (argc > 1 && absl::string_view(argv[1]) == "--help") {
    std::cerr << usage << "\n";
    return 0;
  }

  bool parsed_flags_ok = tsl::Flags::Parse(&argc, argv, flag_list);
  QCHECK(parsed_flags_ok) << "\n" << usage;

  tsl::port::InitMain(usage.c_str(), &argc, &argv);

  xla::Status result = xla::xla_compile::XlaCompileMain(
      mhlo_path, output_path, platform, gpu_target_config_path);
  if (!result.ok()) {
    LOG(ERROR) << "Compilation failed: " << result.error_message();
    return 1;
  }

  LOG(INFO) << "Compilation succeeded";
  return 0;
}
