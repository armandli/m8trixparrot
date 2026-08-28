#include <core/package_installer.h>

#include <cstdlib>
#include <filesystem>

#include <core/tools.h>       // kVenvDir, ensure_python_ready()
#include <core/tools_util.h>  // shell_quote(), run_shell_capture()

namespace agent {

namespace {

// The venv's own python, not whatever interpreter this process happens to be
// running under — ensure_python_ready() guarantees it exists before this path
// is used.
std::string venv_python() {
  return (std::filesystem::absolute(kVenvDir) / "bin" / "python3").string();
}

// `pip show` is a local metadata lookup — no index needed, so it runs under
// the same PIP_NO_INDEX lockdown as everything else.
bool is_installed(const std::string& python, const std::string& package) {
  const std::string command = shell_quote(python) + " -m pip show --quiet " +
                              shell_quote(package) + " >/dev/null 2>&1";
  return std::system(command.c_str()) == 0;
}

// The real installer the worker thread runs by default. Checks first so a
// package that's already there costs one `pip show`, not a redundant install;
// re-checks after so success is "importable now", not "pip exited 0" (popen
// doesn't cheaply expose pip's own exit code alongside captured output).
PackageInstallResult pip_install(const std::string& package) {
  ensure_python_ready();

  PackageInstallResult result;
  const std::string python = venv_python();

  if (is_installed(python, package)) {
    result.ok = true;
    result.already_installed = true;
    result.output = "'" + package + "' is already installed";
    return result;
  }

  // PIP_NO_INDEX/PIP_NO_INPUT are set process-wide (see tools_python.cpp) so a
  // script's own subprocess pip call can't reach an index. This worker is the
  // one sanctioned installer: unset PIP_NO_INDEX for just this child process,
  // keep PIP_NO_INPUT so pip never blocks on a prompt.
  const std::string command =
      "env -u PIP_NO_INDEX " + shell_quote(python) +
      " -m pip install --disable-pip-version-check --quiet " +
      shell_quote(package) + " 2>&1";
  result.output = run_shell_capture(command);
  result.ok = is_installed(python, package);
  if (not result.ok) {
    result.error = "pip install failed for '" + package + "'";
  }
  return result;
}

}  // namespace

PackageInstaller& PackageInstaller::instance() {
  static PackageInstaller instance;
  return instance;
}

PackageInstaller::PackageInstaller() : mRunner(pip_install) {
  mWorker = std::thread([this] { worker_loop(); });
}

PackageInstaller::~PackageInstaller() {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mShutdown = true;
  }
  mCv.notify_all();
  if (mWorker.joinable()) mWorker.join();
}

void PackageInstaller::set_runner_for_test(Runner runner) {
  instance().mRunner = std::move(runner);
}

PackageInstallResult PackageInstaller::install(const std::string& package) {
  std::shared_future<PackageInstallResult> future;
  {
    std::unique_lock<std::mutex> lock(mMutex);
    if (mInstalled.count(package) != 0) {
      PackageInstallResult result;
      result.ok = true;
      result.already_installed = true;
      result.output = "'" + package + "' is already installed";
      return result;
    }

    auto in_flight = mInFlight.find(package);
    if (in_flight != mInFlight.end()) {
      future = in_flight->second;  // Join the existing job; don't enqueue one.
    } else {
      auto promise = std::make_shared<std::promise<PackageInstallResult>>();
      future = promise->get_future().share();
      mInFlight.emplace(package, future);
      mQueue.push_back(Job{package, promise});
      mCv.notify_one();
    }
  }
  return future.get();
}

void PackageInstaller::worker_loop() {
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mMutex);
      mCv.wait(lock, [this] { return not mQueue.empty() or mShutdown; });
      if (mQueue.empty()) return;
      job = std::move(mQueue.front());
      mQueue.pop_front();
    }

    PackageInstallResult result = mRunner(job.package);

    {
      std::lock_guard<std::mutex> lock(mMutex);
      if (result.ok) mInstalled.insert(job.package);
      mInFlight.erase(job.package);
    }
    job.promise->set_value(result);
  }
}

}  // namespace agent
