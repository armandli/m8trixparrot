#ifndef PACKAGE_INSTALLER_H
#define PACKAGE_INSTALLER_H

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace agent {

// Outcome of one install request. `already_installed` distinguishes "nothing
// to do" from "we ran pip and it worked" so PackageInstallTool can word its
// result accordingly; both count as ok.
struct PackageInstallResult {
  bool ok = false;
  bool already_installed = false;
  std::string output;  // pip's captured stdout+stderr, on either path.
  std::string error;
};

// Singleton that serialises `pip install` requests behind one worker thread
// and a FIFO queue, modeled on OllamaClient. AgentPool runs each subagent on
// its own thread, so without this, N subagents asking for the same package at
// once would each shell out to pip independently; that duplication is exactly
// what this exists to prevent.
//
// A request for a package already confirmed installed (this process) is
// answered immediately with no new job. A request for a package that already
// has a job in flight — queued or currently running — joins that job's future
// instead of enqueueing a second one; every caller for that package is woken
// with the same result once it settles. That is what "reject a duplicate
// install" means here: no redundant pip invocation, not an error back to a
// caller who only wanted the package to end up installed.
struct PackageInstaller {
  static PackageInstaller& instance();

  // Enqueues (or joins an in-flight) install of `package` and blocks the
  // calling thread until it settles. Safe to call from any thread.
  PackageInstallResult install(const std::string& package);

  // Replaces the function the worker calls to actually perform an install.
  // Test-only seam so dedup/queueing behavior can be verified without
  // shelling out to real pip; production code never calls this.
  using Runner = std::function<PackageInstallResult(const std::string&)>;
  static void set_runner_for_test(Runner runner);

  ~PackageInstaller();

  PackageInstaller(const PackageInstaller&) = delete;
  PackageInstaller& operator=(const PackageInstaller&) = delete;

private:
  PackageInstaller();

  void worker_loop();

  struct Job {
    std::string package;
    std::shared_ptr<std::promise<PackageInstallResult>> promise;
  };

  Runner mRunner;
  std::deque<Job> mQueue;
  std::unordered_map<std::string, std::shared_future<PackageInstallResult>>
      mInFlight;
  std::unordered_set<std::string> mInstalled;
  std::mutex mMutex;
  std::condition_variable mCv;
  std::thread mWorker;
  bool mShutdown = false;
};

}  // namespace agent

#endif  // PACKAGE_INSTALLER_H
