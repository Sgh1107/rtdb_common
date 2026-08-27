#include "tests/unit/spawn_self.hpp"

#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rtdb_test {

SpawnOutcome SpawnSelfWithFlag(const std::string& exe,
                               const std::string& extra_arg) {
  SpawnOutcome out;
  // CreateProcessA 第二参要求可变缓冲。
  std::string s = "\"" + exe + "\" \"" + kCrashLockFlag + "\" \"" +
                  extra_arg + "\"";
  std::vector<char> cmdline(s.begin(), s.end());
  cmdline.push_back('\0');
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessA(exe.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                     0, nullptr, nullptr, &si, &pi) == 0)
    return out;
  WaitForSingleObject(pi.hProcess, 20000);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  out.spawned = true;
  out.exit_code = static_cast<int>(code);
  return out;
}

}  // namespace rtdb_test

#else

#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace rtdb_test {

SpawnOutcome SpawnSelfWithFlag(const std::string& exe,
                               const std::string& extra_arg) {
  SpawnOutcome out;
  const pid_t pid = ::fork();
  if (pid < 0) return out;
  if (pid == 0) {
    ::execl(exe.c_str(), exe.c_str(), kCrashLockFlag, extra_arg.c_str(),
            static_cast<char*>(nullptr));
    ::_exit(127);  // exec 失败
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  out.spawned = true;
  out.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return out;
}

}  // namespace rtdb_test

#endif
