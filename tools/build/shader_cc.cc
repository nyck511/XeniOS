// Build-time shader compiler for Xenia's built-in shaders.
//
// Usage: xenia-shader-cc [--slang-msl] [--depfile <path>]
//                       [--metal-sdk <sdk>] [--metal-std <std>]
//                       [--metal-min-version-flag <flag>]
//                       <input> <output.h>
//
// Default: GLSL/XeSL -> SPIR-V, linked in-process via glslang and SPIRV-Tools,
// emitted as `const uint32_t <id>[]`. Stage is parsed from the filename stem
// (foo.cs.glsl -> compute); XeSL sources are compiled with the XeSL wrapper
// and with includes resolved relative to the input file's directory.
//
// --slang-msl (Apple only): .slang -> .metal source via slangc, then xcrun
// metal/metallib to a metallib, emitted as
// `const uint8_t <id>_metallib[]`.
//
// --depfile writes a Make-style dependency file listing every source the
// compile transitively read.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "DirStackFileIncluder.h"
#include "SPIRV/GlslangToSpv.h"
#include "glslang/Public/ResourceLimits.h"
#include "glslang/Public/ShaderLang.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"

#ifdef XE_SHADER_CC_METAL
#if defined(_WIN32)
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif
#endif

namespace {

struct StageInfo {
  const char* key;
  EShLanguage language;
};

constexpr StageInfo kStages[] = {
    {"vs", EShLangVertex},         {"hs", EShLangTessControl},
    {"ds", EShLangTessEvaluation}, {"gs", EShLangGeometry},
    {"ps", EShLangFragment},       {"cs", EShLangCompute},
};

constexpr const char* kXeslWrapperPrefix =
    "#version 460\n"
    "#extension GL_EXT_control_flow_attributes : require\n"
    "#extension GL_EXT_samplerless_texture_functions : require\n"
    "#extension GL_GOOGLE_include_directive : require\n";

bool ParseStage(const std::string& filename, EShLanguage* out,
                std::string* stage_key_out) {
  // Strip extension, then take last 2 chars after the final dot.
  auto dot = filename.rfind('.');
  if (dot == std::string::npos || dot < 3 || filename[dot - 3] != '.') {
    return false;
  }
  std::string key = filename.substr(dot - 2, 2);
  for (const auto& stage : kStages) {
    if (key == stage.key) {
      *out = stage.language;
      *stage_key_out = key;
      return true;
    }
  }
  return false;
}

std::string IdentifierFromFilename(const std::string& filename) {
  // foo.bar.cs.glsl -> foo_bar_cs
  auto last_dot = filename.rfind('.');
  std::string stem = filename.substr(0, last_dot);
  std::replace(stem.begin(), stem.end(), '.', '_');
  return stem;
}

bool ReadFile(const std::filesystem::path& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "failed to open %s\n", path.string().c_str());
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  *out = buf.str();
  return true;
}

// Escape spaces in Make-style depfile paths (Ninja's parser requires this).
std::string EscapeDepPath(const std::string& path) {
  std::string escaped;
  escaped.reserve(path.size());
  for (char c : path) {
    if (c == ' ' || c == '\t') {
      escaped.push_back('\\');
    }
    escaped.push_back(c);
  }
  return escaped;
}

bool WriteDepfile(const std::filesystem::path& depfile_path,
                  const std::filesystem::path& target,
                  const std::vector<std::string>& dependencies) {
  std::error_code ec;
  std::filesystem::create_directories(depfile_path.parent_path(), ec);
  std::ofstream out(depfile_path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "failed to open depfile %s\n",
                 depfile_path.string().c_str());
    return false;
  }
  out << EscapeDepPath(target.string()) << ":";
  for (const auto& dependency : dependencies) {
    out << " \\\n  " << EscapeDepPath(dependency);
  }
  out << "\n";
  return bool(out);
}

// Slang names its temporary MSL output as the depfile target. Retarget it to
// the generated header so CMake and incremental generator users track the
// actual build edge rather than treating it as perpetually dirty.
bool RetargetDepfile(const std::filesystem::path& depfile_path,
                     const std::filesystem::path& target) {
  std::string content;
  if (!ReadFile(depfile_path, &content)) {
    return false;
  }
  size_t colon = std::string::npos;
  for (size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\\') {
      ++i;
    } else if (content[i] == ':' &&
               !(i == 1 &&
                 std::isalpha(static_cast<unsigned char>(content[0])))) {
      colon = i;
      break;
    }
  }
  if (colon == std::string::npos) {
    std::fprintf(stderr, "malformed depfile %s\n",
                 depfile_path.string().c_str());
    return false;
  }

  std::vector<std::string> dependencies;
  std::string dependency;
  for (size_t i = colon + 1; i < content.size(); ++i) {
    char c = content[i];
    if (c == '\\' && i + 1 < content.size()) {
      char next = content[i + 1];
      if (next == '\n' || next == '\r') {
        ++i;
        continue;
      }
      if (next == ' ' || next == '\t') {
        dependency.push_back(next);
        ++i;
        continue;
      }
      dependency.push_back(c);
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (!dependency.empty()) {
        dependencies.push_back(dependency);
        dependency.clear();
      }
    } else {
      dependency.push_back(c);
    }
  }
  if (!dependency.empty()) {
    dependencies.push_back(dependency);
  }
  return WriteDepfile(depfile_path, target, dependencies);
}

std::string DisassembleSpirv(const std::vector<uint32_t>& spirv) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string text;
  const uint32_t options = SPV_BINARY_TO_TEXT_OPTION_INDENT |
                           SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES;
  if (!tools.Disassemble(spirv, &text, options)) {
    text.clear();
  }
  return text;
}

bool WriteSpirvHeader(const std::filesystem::path& path,
                      const std::string& identifier,
                      const std::vector<uint32_t>& spirv) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "failed to open output %s\n", path.string().c_str());
    return false;
  }
  out << "// Generated by xenia-shader-cc. Do not edit.\n#if 0\n";
  std::string disasm = DisassembleSpirv(spirv);
  if (!disasm.empty()) {
    out << disasm;
    if (disasm.back() != '\n') {
      out << '\n';
    }
  }
  out << "#endif\n\n";
  out << "const uint32_t " << identifier << "[] = {";
  for (size_t i = 0; i < spirv.size(); ++i) {
    out << (i % 6 == 0 ? "\n    " : " ");
    char word[16];
    std::snprintf(word, sizeof(word), "0x%08X,", spirv[i]);
    out << word;
  }
  out << "\n};\n";
  return bool(out);
}

#ifdef XE_SHADER_CC_METAL

#if defined(_WIN32)
// Quote an argument for the MSVCRT command-line parser used by _spawnvp.
std::string QuoteForSpawn(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
    return arg;
  }
  std::string quoted;
  quoted.push_back('"');
  for (size_t i = 0; i < arg.size(); ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++backslashes;
      ++i;
    }
    if (i == arg.size()) {
      quoted.append(backslashes * 2, '\\');
      break;
    }
    if (arg[i] == '"') {
      quoted.append(backslashes * 2 + 1, '\\');
      quoted.push_back('"');
    } else {
      quoted.append(backslashes, '\\');
      quoted.push_back(arg[i]);
    }
  }
  quoted.push_back('"');
  return quoted;
}
#endif

int RunCommand(const std::vector<std::string>& args) {
  if (args.empty()) {
    return -1;
  }
#if defined(_WIN32)
  std::vector<std::string> quoted_args;
  quoted_args.reserve(args.size());
  for (const auto& arg : args) {
    quoted_args.push_back(QuoteForSpawn(arg));
  }
  std::vector<char*> argv;
  argv.reserve(quoted_args.size() + 1);
  for (auto& arg : quoted_args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);
  intptr_t result = _spawnvp(_P_WAIT, args[0].c_str(), argv.data());
  if (result < 0) {
    std::fprintf(stderr, "_spawnvp(%s) failed: %s\n", args[0].c_str(),
                 std::strerror(errno));
    return -1;
  }
  return static_cast<int>(result);
#else
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  int result =
      posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
  if (result != 0) {
    std::fprintf(stderr, "posix_spawnp(%s) failed: %s\n", argv[0],
                 std::strerror(result));
    return -1;
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    std::fprintf(stderr, "waitpid failed for %s\n", argv[0]);
    return -1;
  }
  if (!WIFEXITED(status)) {
    return -1;
  }
  return WEXITSTATUS(status);
#endif
}

bool ReadBinaryFile(const std::filesystem::path& path,
                    std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  in.seekg(0, std::ios::end);
  auto size = in.tellg();
  if (size < 0) {
    return false;
  }
  in.seekg(0, std::ios::beg);
  out->resize(static_cast<size_t>(size));
  in.read(reinterpret_cast<char*>(out->data()), size);
  return bool(in);
}

bool WriteMetallibHeader(const std::filesystem::path& path,
                         const std::string& identifier,
                         const std::vector<uint8_t>& bytes) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "failed to open output %s\n", path.string().c_str());
    return false;
  }
  out << "// Generated by xenia-shader-cc. Do not edit.\n";
  out << "const uint8_t " << identifier << "_metallib[] = {";
  for (size_t i = 0; i < bytes.size(); ++i) {
    out << (i % 16 == 0 ? "\n    " : " ");
    char byte[8];
    std::snprintf(byte, sizeof(byte), "0x%02X,", bytes[i]);
    out << byte;
  }
  out << "\n};\n";
  return bool(out);
}

const char* GetSlangcPath() {
  const char* path = std::getenv("SLANGC_PATH");
  if (!path || !path[0]) {
    std::fprintf(stderr,
                 "SLANGC_PATH is not set - point it at the slangc executable "
                 "to compile .slang shaders.\n");
    std::exit(1);
  }
  return path;
}

const char* SlangStageName(const std::string& stage_key) {
  if (stage_key == "vs") {
    return "vertex";
  }
  if (stage_key == "ps") {
    return "pixel";
  }
  if (stage_key == "cs") {
    return "compute";
  }
  if (stage_key == "hs") {
    return "hull";
  }
  if (stage_key == "ds") {
    return "domain";
  }
  if (stage_key == "gs") {
    return "geometry";
  }
  return nullptr;
}

bool RewriteSlangMsl(const std::filesystem::path& path) {
  std::string source;
  if (!ReadFile(path, &source)) {
    return false;
  }

  // Slang renames the source entry point from main to main_0 in MSL. Xenia's
  // Metal loaders use the legacy entry_xe name for all built-in shaders.
  const std::string entry_from = " main_0(";
  const std::string entry_to = " entry_xe(";
  size_t entry_rewrite_count = 0;
  for (size_t pos = 0;
       (pos = source.find(entry_from, pos)) != std::string::npos;
       pos += entry_to.size()) {
    source.replace(pos, entry_from.size(), entry_to);
    ++entry_rewrite_count;
  }
  if (!entry_rewrite_count) {
    std::fprintf(stderr, "Slang MSL output has no main_0 entry point: %s\n",
                 path.string().c_str());
    return false;
  }

  // Slang lowers Texture2DMS.Load with a signed int2 coordinate, but
  // texture2d_ms::read requires uint2 in MSL.
  std::set<std::string> multisampled_textures;
  const std::string declaration = "texture2d_ms<";
  for (size_t pos = 0;
       (pos = source.find(declaration, pos)) != std::string::npos;) {
    size_t close_angle = source.find('>', pos);
    if (close_angle == std::string::npos) {
      break;
    }
    size_t name_begin = close_angle + 1;
    while (name_begin < source.size() &&
           std::isspace(static_cast<unsigned char>(source[name_begin]))) {
      ++name_begin;
    }
    size_t name_end = name_begin;
    while (name_end < source.size() &&
           (std::isalnum(static_cast<unsigned char>(source[name_end])) ||
            source[name_end] == '_')) {
      ++name_end;
    }
    if (name_end > name_begin) {
      multisampled_textures.insert(
          source.substr(name_begin, name_end - name_begin));
    }
    pos = name_end;
  }
  for (const std::string& texture : multisampled_textures) {
    const std::string read_from = texture + ").read((";
    const std::string read_to = texture + ").read(uint2(";
    for (size_t pos = 0;
         (pos = source.find(read_from, pos)) != std::string::npos;
         pos += read_to.size()) {
      source.replace(pos, read_from.size(), read_to);
    }
  }

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "failed to rewrite %s\n", path.string().c_str());
    return false;
  }
  out.write(source.data(), static_cast<std::streamsize>(source.size()));
  return bool(out);
}

#endif  // XE_SHADER_CC_METAL

}  // namespace

int main(int argc, char** argv) {
  bool slang_msl_mode = false;
#ifdef XE_SHADER_CC_METAL
  std::string metal_sdk = "macosx";
  std::string metal_std = "macos-metal2.3";
  std::string metal_min_version_flag =
      std::string("-mmacosx-version-min=") + XE_METAL_MIN_OS;
#endif
  std::string depfile_path;
  int arg_index = 1;
  while (arg_index < argc && argv[arg_index][0] == '-') {
    if (std::strcmp(argv[arg_index], "--slang-msl") == 0) {
#ifndef XE_SHADER_CC_METAL
      std::fprintf(stderr,
                   "--slang-msl not available (built without "
                   "XE_SHADER_CC_METAL)\n");
      return 1;
#endif
      slang_msl_mode = true;
      ++arg_index;
    } else if (std::strcmp(argv[arg_index], "--depfile") == 0) {
      if (arg_index + 1 >= argc) {
        std::fprintf(stderr, "--depfile requires a path\n");
        return 1;
      }
      depfile_path = argv[arg_index + 1];
      arg_index += 2;
    } else if (std::strcmp(argv[arg_index], "--metal-sdk") == 0) {
      if (arg_index + 1 >= argc) {
        std::fprintf(stderr, "--metal-sdk requires an SDK name\n");
        return 1;
      }
#ifdef XE_SHADER_CC_METAL
      metal_sdk = argv[arg_index + 1];
#endif
      arg_index += 2;
    } else if (std::strcmp(argv[arg_index], "--metal-std") == 0) {
      if (arg_index + 1 >= argc) {
        std::fprintf(stderr, "--metal-std requires a standard name\n");
        return 1;
      }
#ifdef XE_SHADER_CC_METAL
      metal_std = argv[arg_index + 1];
#endif
      arg_index += 2;
    } else if (std::strcmp(argv[arg_index], "--metal-min-version-flag") == 0) {
      if (arg_index + 1 >= argc) {
        std::fprintf(stderr,
                     "--metal-min-version-flag requires a compiler flag\n");
        return 1;
      }
#ifdef XE_SHADER_CC_METAL
      metal_min_version_flag = argv[arg_index + 1];
#endif
      arg_index += 2;
    } else {
      std::fprintf(stderr, "unknown flag: %s\n", argv[arg_index]);
      return 1;
    }
  }
  if (argc - arg_index != 2) {
    std::fprintf(stderr,
                 "Usage: %s [--slang-msl] [--depfile <path>] "
                 "[--metal-sdk <sdk>] [--metal-std <std>] "
                 "[--metal-min-version-flag <flag>] <input> <output.h>\n",
                 argv[0]);
    return 1;
  }

  std::filesystem::path input_path = argv[arg_index];
  std::filesystem::path output_path = argv[arg_index + 1];
  std::string input_filename = input_path.filename().string();
  std::string identifier = IdentifierFromFilename(input_filename);

  EShLanguage stage;
  std::string stage_key;
  if (!ParseStage(input_filename, &stage, &stage_key)) {
    std::fprintf(stderr, "cannot determine shader stage from filename: %s\n",
                 input_filename.c_str());
    return 1;
  }

  if (slang_msl_mode) {
#ifdef XE_SHADER_CC_METAL
    const char* slang_stage = SlangStageName(stage_key);
    if (!slang_stage) {
      std::fprintf(stderr, "unsupported stage %s for --slang-msl\n",
                   stage_key.c_str());
      return 1;
    }

    std::filesystem::path temporary_directory =
        std::filesystem::temp_directory_path();
    std::string tag = identifier + "_" +
                      std::to_string(
#ifdef _WIN32
                          static_cast<int>(_getpid())
#else
                          static_cast<int>(::getpid())
#endif
                      );
    std::filesystem::path metal_source_path =
        temporary_directory / (tag + ".metal");
    auto cleanup_metal_source = [&] {
      std::error_code ec;
      std::filesystem::remove(metal_source_path, ec);
    };

    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);

    std::vector<std::string> slang_command = {
        GetSlangcPath(),
        input_path.string(),
        "-target",
        "metal",
        "-stage",
        slang_stage,
        "-entry",
        "main",
        "-I",
        input_path.parent_path().string(),
        "-o",
        metal_source_path.string(),
        "-Wno-40100",
        "-DXE_SLANG_MSL=1",
        "-Wno-39029",
        "-Wno-30056",
        "-DSHADING_LANGUAGE_HLSL_XE=1",
        "-DSHADING_LANGUAGE_GLSL_XE=0",
        "-DSHADING_LANGUAGE_MSL_XE=0",
        "-DCOMBINED_TEXTURE_SAMPLER_XE=0",
        "-DXE_SLANG=1",
    };
    if (!depfile_path.empty()) {
      slang_command.push_back("-depfile");
      slang_command.push_back(depfile_path);
    }
    if (RunCommand(slang_command) != 0) {
      std::fprintf(stderr, "slangc failed for %s\n",
                   input_path.string().c_str());
      cleanup_metal_source();
      return 1;
    }
    if (!depfile_path.empty() && !RetargetDepfile(depfile_path, output_path)) {
      cleanup_metal_source();
      return 1;
    }
    if (!RewriteSlangMsl(metal_source_path)) {
      cleanup_metal_source();
      return 1;
    }

    std::filesystem::path air_path = temporary_directory / (tag + ".air");
    std::filesystem::path metallib_path =
        temporary_directory / (tag + ".metallib");
    auto cleanup_apple_outputs = [&] {
      std::error_code cleanup_error;
      std::filesystem::remove(air_path, cleanup_error);
      std::filesystem::remove(metallib_path, cleanup_error);
    };

    std::vector<std::string> metal_command = {
        "xcrun",
        "-sdk",
        metal_sdk,
        "metal",
        "-x",
        "metal",
        "-std=" + metal_std,
        metal_min_version_flag,
        "-w",
        "-c",
        metal_source_path.string(),
        "-o",
        air_path.string(),
    };
    if (RunCommand(metal_command) != 0) {
      std::fprintf(stderr, "metal failed for %s\n",
                   input_path.string().c_str());
      cleanup_metal_source();
      cleanup_apple_outputs();
      return 1;
    }
    if (RunCommand({"xcrun", "-sdk", metal_sdk, "metallib", air_path.string(),
                    "-o", metallib_path.string()}) != 0) {
      std::fprintf(stderr, "metallib failed for %s\n",
                   input_path.string().c_str());
      cleanup_metal_source();
      cleanup_apple_outputs();
      return 1;
    }

    std::vector<uint8_t> bytes;
    if (!ReadBinaryFile(metallib_path, &bytes)) {
      std::fprintf(stderr, "failed to read %s\n",
                   metallib_path.string().c_str());
      cleanup_metal_source();
      cleanup_apple_outputs();
      return 1;
    }
    cleanup_metal_source();
    cleanup_apple_outputs();
    if (!WriteMetallibHeader(output_path, identifier, bytes)) {
      return 1;
    }
    return 0;
#else
    std::fprintf(stderr,
                 "--slang-msl not available (built without "
                 "XE_SHADER_CC_METAL)\n");
    return 1;
#endif
  }

  std::string source;
  if (!ReadFile(input_path, &source)) {
    return 1;
  }

  // .xesl files rely on a wrapper prepending #version / extensions and then
  // #include'ing the file itself. glslang needs the include directive to come
  // from an actual source string, not from disk, so we synthesize the wrapper
  // and let the includer fetch the real file.
  const bool is_xesl =
      input_filename.size() > 5 &&
      input_filename.compare(input_filename.size() - 5, 5, ".xesl") == 0;
  std::string wrapper;
  std::string wrapper_name;
  if (is_xesl) {
    wrapper = kXeslWrapperPrefix;
    wrapper += "#include \"";
    wrapper += input_filename;
    wrapper += "\"\n";
  }

  glslang::InitializeProcess();

  glslang::TShader shader(stage);
  const char* source_strings[1];
  const char* source_names[1];
  int source_lengths[1];
  if (is_xesl) {
    source_strings[0] = wrapper.c_str();
    source_lengths[0] = static_cast<int>(wrapper.size());
    wrapper_name = "xesl_wrapper";
    source_names[0] = wrapper_name.c_str();
  } else {
    source_strings[0] = source.c_str();
    source_lengths[0] = static_cast<int>(source.size());
    source_names[0] = input_filename.c_str();
  }
  shader.setStringsWithLengthsAndNames(source_strings, source_lengths,
                                       source_names, 1);
  shader.setPreamble("#define SHADING_LANGUAGE_GLSL_XE 1\n");
  // Match the old `glslangValidator -V` default: Vulkan 1.0 / SPV 1.0, which
  // stays compatible with the Vulkan 1.0 devices the runtime still supports.
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan,
                     100);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  DirStackFileIncluder includer;
  std::string input_dir = input_path.parent_path().string();
  if (!input_dir.empty()) {
    includer.pushExternalLocalDirectory(input_dir);
  }

  const int default_version = 460;
  const EShMessages messages = static_cast<EShMessages>(
      EShMsgDefault | EShMsgSpvRules | EShMsgVulkanRules);

  if (!shader.parse(GetDefaultResources(), default_version, false, messages,
                    includer)) {
    std::fprintf(stderr, "glslang parse failed for %s:\n%s%s",
                 input_path.string().c_str(), shader.getInfoLog(),
                 shader.getInfoDebugLog());
    glslang::FinalizeProcess();
    return 1;
  }

  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    std::fprintf(stderr, "glslang link failed for %s:\n%s%s",
                 input_path.string().c_str(), program.getInfoLog(),
                 program.getInfoDebugLog());
    glslang::FinalizeProcess();
    return 1;
  }

  // Emit raw SPIR-V from glslang; optimization is done explicitly below to
  // match the previous `spirv-opt -O -O --canonicalize-ids` behavior.
  glslang::SpvOptions spv_options;
  spv_options.generateDebugInfo = false;
  spv_options.stripDebugInfo = true;
  spv_options.disableOptimizer = true;
  spv_options.validate = false;

  std::vector<uint32_t> spirv;
  glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &spv_options);

  if (!depfile_path.empty()) {
    std::vector<std::string> dependencies;
    dependencies.push_back(input_path.string());
    for (const auto& included_file : includer.getIncludedFiles()) {
      dependencies.push_back(included_file);
    }
    if (!WriteDepfile(depfile_path, output_path, dependencies)) {
      glslang::FinalizeProcess();
      return 1;
    }
  }

  glslang::FinalizeProcess();

  if (spirv.empty()) {
    std::fprintf(stderr, "GlslangToSpv produced empty output for %s\n",
                 input_path.string().c_str());
    return 1;
  }

  {
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_2);
    optimizer.SetMessageConsumer(
        [](spv_message_level_t, const char*, const spv_position_t&,
           const char* message) { std::fprintf(stderr, "%s\n", message); });
    optimizer.RegisterPerformancePasses();
    optimizer.RegisterPerformancePasses();
    optimizer.RegisterPass(spvtools::CreateCanonicalizeIdsPass());
    std::vector<uint32_t> optimized;
    if (!optimizer.Run(spirv.data(), spirv.size(), &optimized)) {
      std::fprintf(stderr, "spirv-opt failed for %s\n",
                   input_path.string().c_str());
      return 1;
    }
    spirv = std::move(optimized);
  }

  if (!WriteSpirvHeader(output_path, identifier, spirv)) {
    return 1;
  }
  return 0;
}
