#include <core/tools.h>

#include <optional>
#include <string>
#include <utility>

#include <core/package_installer.h>
#include <core/tools_util.h>

namespace agent {

std::string PackageInstallTool::description() const {
  return R"json({"name":"package_install","description":"Install a single Python package so it becomes importable from the python tool. Requests are deduplicated across concurrent callers: a package that is already installed, or already being installed by another in-flight request, is not installed a second time — the call simply waits for that outcome.","parameters":{"type":"object","properties":{"package":{"type":"string","description":"The package name to install, exactly as pip expects it (e.g. \"numpy\" or \"requests==2.31.0\")"}},"required":["package"]}})json";
}

ToolResult PackageInstallTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> package = string_arg(args, "package");
  if (not package or package->empty()) {
    result.error = "package_install: missing required string argument 'package'";
    return result;
  }

  const PackageInstallResult install = PackageInstaller::instance().install(*package);
  if (not install.ok) {
    result.error = "package_install: " +
                   (install.error.empty()
                        ? "failed to install '" + *package + "'"
                        : install.error);
    if (not install.output.empty()) result.error += "\n" + install.output;
    return result;
  }

  std::string output = install.already_installed
                            ? "'" + *package + "' is already installed"
                            : "installed '" + *package + "'";
  if (not install.already_installed and not install.output.empty()) {
    output += "\n" + install.output;
  }

  TruncatedOutput truncated = truncate_output(std::move(output), "package_install");
  result.ok = true;
  result.output = std::move(truncated.text);
  result.output += truncation_note(truncated);
  result.truncated = truncated.truncated;
  result.overflow_path = std::move(truncated.overflow_path);
  return result;
}

}  // namespace agent
