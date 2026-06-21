#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <readline/history.h>
#include <readline/readline.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

struct BackgroundJob {
  int job_number;
  pid_t pid;
  std::string command;
  bool done = false;
};

std::vector<BackgroundJob>& bg_jobs() {
  static std::vector<BackgroundJob> jobs;
  return jobs;
}

int next_job_number() {
  for (int num = 1;; ++num) {
    bool used = false;
    for (const BackgroundJob& job : bg_jobs()) {
      if (job.job_number == num) {
        used = true;
        break;
      }
    }
    if (!used) {
      return num;
    }
  }
}

void sigchld_handler(int /*sig*/) {
  const int saved_errno = errno;
  int status = 0;
  pid_t pid = 0;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    for (BackgroundJob& job : bg_jobs()) {
      if (job.pid == pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
        job.done = true;
        break;
      }
    }
  }
  errno = saved_errno;
}

void poll_zombies() {
  int status = 0;
  pid_t pid = 0;
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    for (BackgroundJob& job : bg_jobs()) {
      if (job.pid == pid && (WIFEXITED(status) || WIFSIGNALED(status))) {
        job.done = true;
        break;
      }
    }
  }
}

void reap_jobs() {
  poll_zombies();

  std::vector<size_t> done_indices;
  for (size_t i = 0; i < bg_jobs().size(); ++i) {
    if (bg_jobs()[i].done) {
      done_indices.push_back(i);
    }
  }

  const size_t job_count = bg_jobs().size();
  for (size_t i = 0; i < job_count; ++i) {
    if (std::find(done_indices.begin(), done_indices.end(), i) == done_indices.end()) {
      continue;
    }

    const BackgroundJob& job = bg_jobs()[i];
    char marker = ' ';
    if (i == job_count - 1) {
      marker = '+';
    } else if (i == job_count - 2) {
      marker = '-';
    }

    std::string status = "Done";
    status.resize(24, ' ');
    std::string cmd = job.command;
    if (cmd.size() >= 2 && cmd.substr(cmd.size() - 2) == " &") {
      cmd = cmd.substr(0, cmd.size() - 2);
    }
    std::cout << '[' << job.job_number << ']' << marker << ' ' << status << cmd << '\n';
  }

  for (auto it = done_indices.rbegin(); it != done_indices.rend(); ++it) {
    bg_jobs().erase(bg_jobs().begin() + static_cast<std::ptrdiff_t>(*it));
  }
}

struct RedirectInfo {
  std::string stdout_file;
  bool stdout_append = false;
  std::string stderr_file;
  bool stderr_append = false;
};

RedirectInfo extract_redirects(std::vector<std::string>& args) {
  RedirectInfo redirect;
  for (size_t i = 0; i < args.size();) {
    const std::string& arg = args[i];
    if ((arg == ">" || arg == "1>") && i + 1 < args.size()) {
      redirect.stdout_file = args[i + 1];
      redirect.stdout_append = false;
      args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                 args.begin() + static_cast<std::ptrdiff_t>(i + 2));
      continue;
    }
    if ((arg == ">>" || arg == "1>>") && i + 1 < args.size()) {
      redirect.stdout_file = args[i + 1];
      redirect.stdout_append = true;
      args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                 args.begin() + static_cast<std::ptrdiff_t>(i + 2));
      continue;
    }
    if (arg == "2>" && i + 1 < args.size()) {
      redirect.stderr_file = args[i + 1];
      redirect.stderr_append = false;
      args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                 args.begin() + static_cast<std::ptrdiff_t>(i + 2));
      continue;
    }
    if (arg == "2>>" && i + 1 < args.size()) {
      redirect.stderr_file = args[i + 1];
      redirect.stderr_append = true;
      args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
                 args.begin() + static_cast<std::ptrdiff_t>(i + 2));
      continue;
    }
    ++i;
  }
  return redirect;
}

bool apply_redirects(const RedirectInfo& redirect) {
  if (!redirect.stdout_file.empty()) {
    const int flags = O_WRONLY | O_CREAT | (redirect.stdout_append ? O_APPEND : O_TRUNC);
    const int fd = open(redirect.stdout_file.c_str(), flags, 0644);
    if (fd < 0) {
      return false;
    }
    dup2(fd, STDOUT_FILENO);
    close(fd);
  }

  if (!redirect.stderr_file.empty()) {
    const int flags = O_WRONLY | O_CREAT | (redirect.stderr_append ? O_APPEND : O_TRUNC);
    const int fd = open(redirect.stderr_file.c_str(), flags, 0644);
    if (fd < 0) {
      return false;
    }
    dup2(fd, STDERR_FILENO);
    close(fd);
  }

  return true;
}

class RedirectScope {
 public:
  explicit RedirectScope(const RedirectInfo& redirect) {
    if (!redirect.stdout_file.empty()) {
      saved_stdout_ = dup(STDOUT_FILENO);
      const int flags = O_WRONLY | O_CREAT | (redirect.stdout_append ? O_APPEND : O_TRUNC);
      const int fd = open(redirect.stdout_file.c_str(), flags, 0644);
      if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        close(fd);
      }
    }
    if (!redirect.stderr_file.empty()) {
      saved_stderr_ = dup(STDERR_FILENO);
      const int flags = O_WRONLY | O_CREAT | (redirect.stderr_append ? O_APPEND : O_TRUNC);
      const int fd = open(redirect.stderr_file.c_str(), flags, 0644);
      if (fd >= 0) {
        dup2(fd, STDERR_FILENO);
        close(fd);
      }
    }
  }

  ~RedirectScope() {
    if (saved_stdout_ >= 0) {
      dup2(saved_stdout_, STDOUT_FILENO);
      close(saved_stdout_);
    }
    if (saved_stderr_ >= 0) {
      dup2(saved_stderr_, STDERR_FILENO);
      close(saved_stderr_);
    }
  }

 private:
  int saved_stdout_ = -1;
  int saved_stderr_ = -1;
};

std::vector<std::string> parse_args(const std::string& input) {
  std::vector<std::string> args;
  std::string current;
  bool in_single_quotes = false;
  bool in_double_quotes = false;

  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];

    if (in_single_quotes) {
      if (c == '\'') {
        in_single_quotes = false;
      } else {
        current += c;
      }
      continue;
    }

    if (in_double_quotes) {
      if (c == '"') {
        in_double_quotes = false;
      } else if (c == '\\' && i + 1 < input.size()) {
        const char next = input[++i];
        if (next == '\\' || next == '$' || next == '`' || next == '\n' || next == '"') {
          current += next;
        } else {
          current += '\\';
          current += next;
        }
      } else {
        current += c;
      }
      continue;
    }

    if (c == '\'') {
      in_single_quotes = true;
    } else if (c == '"') {
      in_double_quotes = true;
    } else if (c == '\\' && i + 1 < input.size()) {
      current += input[++i];
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (!current.empty()) {
        args.push_back(current);
        current.clear();
      }
    } else {
      current += c;
    }
  }

  if (!current.empty()) {
    args.push_back(current);
  }

  return args;
}

static void trim_stage(std::string& stage) {
  while (!stage.empty() && std::isspace(static_cast<unsigned char>(stage.front()))) {
    stage.erase(stage.begin());
  }
  while (!stage.empty() && std::isspace(static_cast<unsigned char>(stage.back()))) {
    stage.pop_back();
  }
}

std::vector<std::string> split_pipeline(const std::string& input) {
  std::vector<std::string> stages;
  std::string current;
  bool in_single_quotes = false;
  bool in_double_quotes = false;

  for (size_t i = 0; i < input.size(); ++i) {
    const char c = input[i];

    if (in_single_quotes) {
      if (c == '\'') {
        in_single_quotes = false;
      }
      current += c;
      continue;
    }

    if (in_double_quotes) {
      if (c == '"') {
        in_double_quotes = false;
      } else if (c == '\\' && i + 1 < input.size()) {
        current += input[++i];
      } else {
        current += c;
      }
      continue;
    }

    if (c == '\'') {
      in_single_quotes = true;
      current += c;
    } else if (c == '"') {
      in_double_quotes = true;
      current += c;
    } else if (c == '|') {
      trim_stage(current);
      stages.push_back(current);
      current.clear();
    } else {
      current += c;
    }
  }

  trim_stage(current);
  if (!current.empty() || !stages.empty()) {
    stages.push_back(current);
  }

  return stages;
}

bool is_builtin(const std::string& cmd) {
  return cmd == "echo" || cmd == "exit" || cmd == "type" || cmd == "pwd" || cmd == "cd" ||
         cmd == "jobs";
}

const std::vector<std::string>& builtin_commands() {
  static const std::vector<std::string> commands = {"cd", "echo", "exit", "jobs", "pwd", "type"};
  return commands;
}

std::vector<std::string> collect_path_executables(const std::string& prefix) {
  std::vector<std::string> results;
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return results;
  }

  std::stringstream ss(path_env);
  std::string dir;
  while (std::getline(ss, dir, ':')) {
    if (!fs::exists(dir)) {
      continue;
    }

    try {
      for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
          continue;
        }

        const std::string filename = entry.path().filename().string();
        if (filename.compare(0, prefix.size(), prefix) != 0) {
          continue;
        }
        if (access(entry.path().c_str(), X_OK) != 0) {
          continue;
        }
        if (std::find(results.begin(), results.end(), filename) == results.end()) {
          results.push_back(filename);
        }
      }
    } catch (const fs::filesystem_error&) {
      continue;
    }
  }

  return results;
}

char* command_generator(const char* text, int state) {
  static size_t builtin_index = 0;
  static std::vector<std::string> path_matches;
  static size_t path_index = 0;
  static bool path_phase = false;
  static std::string prefix;

  if (state == 0) {
    prefix = text != nullptr ? text : "";
    builtin_index = 0;
    path_index = 0;
    path_phase = false;
    path_matches = collect_path_executables(prefix);
  }

  if (!path_phase) {
    while (builtin_index < builtin_commands().size()) {
      const std::string& cmd = builtin_commands()[builtin_index++];
      if (cmd.compare(0, prefix.size(), prefix) == 0) {
        return strdup(cmd.c_str());
      }
    }
    path_phase = true;
  }

  while (path_index < path_matches.size()) {
    return strdup(path_matches[path_index++].c_str());
  }

  return nullptr;
}

char** command_completion(const char* text, int start, int /*end*/) {
  if (start == 0) {
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, command_generator);
  }
  return nullptr;
}

std::string find_in_path(const std::string& command) {
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr) {
    return "";
  }

  std::stringstream ss(path_env);
  std::string dir;
  while (std::getline(ss, dir, ':')) {
    const fs::path full_path = fs::path(dir) / command;
    if (fs::exists(full_path) && fs::is_regular_file(full_path) &&
        access(full_path.c_str(), X_OK) == 0) {
      return full_path.string();
    }
  }
  return "";
}

void run_echo(const std::vector<std::string>& args) {
  for (size_t i = 1; i < args.size(); ++i) {
    if (i > 1) {
      std::cout << ' ';
    }
    std::cout << args[i];
  }
  std::cout << '\n';
}

void run_type(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    return;
  }

  const std::string& cmd = args[1];
  if (is_builtin(cmd)) {
    std::cout << cmd << " is a shell builtin\n";
    return;
  }

  const std::string path = find_in_path(cmd);
  if (!path.empty()) {
    std::cout << cmd << " is " << path << '\n';
  } else {
    std::cout << cmd << ": not found\n";
  }
}

void run_pwd() {
  char cwd[4096];
  if (getcwd(cwd, sizeof(cwd)) != nullptr) {
    std::cout << cwd << '\n';
  }
}

[[noreturn]] void run_builtin_in_child(const std::vector<std::string>& args) {
  const std::string& cmd = args[0];
  if (cmd == "echo") {
    run_echo(args);
  } else if (cmd == "type") {
    run_type(args);
  } else if (cmd == "pwd") {
    run_pwd();
  }
  std::exit(0);
}

void run_cd(const std::vector<std::string>& args) {
  if (args.size() < 2) {
    return;
  }

  std::string path = args[1];
  if (path == "~") {
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      path = home;
    }
  }

  if (chdir(path.c_str()) != 0) {
    std::cout << "cd: " << path << ": No such file or directory\n";
  }
}

void run_jobs() {
  poll_zombies();

  std::vector<size_t> done_indices;
  for (size_t i = 0; i < bg_jobs().size(); ++i) {
    if (bg_jobs()[i].done) {
      done_indices.push_back(i);
    }
  }

  const size_t job_count = bg_jobs().size();
  for (size_t i = 0; i < job_count; ++i) {
    const BackgroundJob& job = bg_jobs()[i];
    char marker = ' ';
    if (i == job_count - 1) {
      marker = '+';
    } else if (i == job_count - 2) {
      marker = '-';
    }

    const bool is_done =
        std::find(done_indices.begin(), done_indices.end(), i) != done_indices.end();
    std::string status = is_done ? "Done" : "Running";
    status.resize(24, ' ');
    std::string cmd = job.command;
    if (is_done && cmd.size() >= 2 && cmd.substr(cmd.size() - 2) == " &") {
      cmd = cmd.substr(0, cmd.size() - 2);
    }
    std::cout << '[' << job.job_number << ']' << marker << ' ' << status << cmd << '\n';
  }

  for (auto it = done_indices.rbegin(); it != done_indices.rend(); ++it) {
    bg_jobs().erase(bg_jobs().begin() + static_cast<std::ptrdiff_t>(*it));
  }
}

struct PipelineStage {
  std::vector<std::string> args;
  RedirectInfo redirect;
};

void close_all_pipes(std::vector<std::array<int, 2>>& pipes) {
  for (auto& pipe_fds : pipes) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
  }
}

void run_pipeline(const std::vector<std::string>& stage_inputs, bool background,
                  const std::string& original_command) {
  std::vector<PipelineStage> stages;
  for (const std::string& stage_input : stage_inputs) {
    std::vector<std::string> args = parse_args(stage_input);
    RedirectInfo redirect = extract_redirects(args);
    if (args.empty()) {
      return;
    }
    stages.push_back({args, redirect});
  }

  const int num_commands = static_cast<int>(stages.size());
  std::vector<std::array<int, 2>> pipes(static_cast<size_t>(num_commands - 1));
  for (int i = 0; i < num_commands - 1; ++i) {
    if (pipe(pipes[static_cast<size_t>(i)].data()) == -1) {
      return;
    }
  }

  std::vector<pid_t> pids;
  for (int i = 0; i < num_commands; ++i) {
    const PipelineStage& stage = stages[static_cast<size_t>(i)];
    const std::string& program = stage.args[0];
    const bool builtin = is_builtin(program);
    std::string path;

    if (!builtin) {
      path = find_in_path(program);
      if (path.empty()) {
        std::cout << program << ": command not found\n";
        close_all_pipes(pipes);
        for (pid_t pid : pids) {
          waitpid(pid, nullptr, 0);
        }
        return;
      }
    }

    const pid_t pid = fork();
    if (pid == 0) {
      if (i > 0) {
        dup2(pipes[static_cast<size_t>(i - 1)][0], STDIN_FILENO);
      }
      if (i < num_commands - 1) {
        dup2(pipes[static_cast<size_t>(i)][1], STDOUT_FILENO);
      } else {
        apply_redirects(stage.redirect);
      }
      if (i < num_commands - 1 && !stage.redirect.stderr_file.empty()) {
        RedirectInfo stderr_only;
        stderr_only.stderr_file = stage.redirect.stderr_file;
        stderr_only.stderr_append = stage.redirect.stderr_append;
        apply_redirects(stderr_only);
      }
      close_all_pipes(pipes);

      if (builtin) {
        run_builtin_in_child(stage.args);
      }

      std::vector<std::string> arg_storage = stage.args;
      std::vector<char*> argv;
      argv.reserve(arg_storage.size() + 1);
      for (auto& arg : arg_storage) {
        argv.push_back(arg.data());
      }
      argv.push_back(nullptr);

      execv(path.c_str(), argv.data());
      std::exit(1);
    }

    if (pid > 0) {
      pids.push_back(pid);
    }
  }

  close_all_pipes(pipes);

  if (background && !pids.empty()) {
    const int job_number = next_job_number();
    bg_jobs().push_back({job_number, pids.back(), original_command});
    std::cout << '[' << job_number << "] " << pids.back() << '\n';
    return;
  }

  for (pid_t pid : pids) {
    int status = 0;
    waitpid(pid, &status, 0);
  }
}

void run_external(std::vector<std::string> args, const RedirectInfo& redirect, bool background,
                  const std::string& original_command) {
  const std::string& program = args[0];
  const std::string path = find_in_path(program);
  if (path.empty()) {
    std::cout << program << ": command not found\n";
    return;
  }

  const pid_t pid = fork();
  if (pid == 0) {
    apply_redirects(redirect);

    std::vector<std::string> arg_storage = args;
    std::vector<char*> argv;
    argv.reserve(arg_storage.size() + 1);
    for (auto& arg : arg_storage) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    execv(path.c_str(), argv.data());
    std::exit(1);
  }

  if (pid > 0) {
    if (background) {
      const int job_number = next_job_number();
      bg_jobs().push_back({job_number, pid, original_command});
      std::cout << '[' << job_number << "] " << pid << '\n';
    } else {
      int status = 0;
      waitpid(pid, &status, 0);
    }
  }
}

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  struct sigaction sa {};
  sa.sa_handler = sigchld_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, nullptr);

  rl_attempted_completion_function = command_completion;
  rl_completion_append_character = ' ';

  while (true) {
    reap_jobs();

    char* raw_line = readline("$ ");
    if (raw_line == nullptr) {
      break;
    }

    const std::string input(raw_line);
    free(raw_line);

    if (input.empty()) {
      continue;
    }

    bool background = false;
    std::string command_input = input;
    if (!command_input.empty() && command_input.back() == '&') {
      background = true;
      command_input.pop_back();
      trim_stage(command_input);
    }

    const std::vector<std::string> pipeline_stages = split_pipeline(command_input);
    if (pipeline_stages.size() > 1) {
      run_pipeline(pipeline_stages, background, input);
      continue;
    }

    std::vector<std::string> args = parse_args(command_input);
    if (args.empty()) {
      continue;
    }

    if (background && !args.empty() && args.back() == "&") {
      args.pop_back();
    }
    if (args.empty()) {
      continue;
    }

    const RedirectInfo redirect = extract_redirects(args);
    if (args.empty()) {
      continue;
    }

    const std::string& cmd = args[0];

    if (cmd == "exit") {
      return 0;
    }
    if (cmd == "echo") {
      RedirectScope redirect_scope(redirect);
      run_echo(args);
    } else if (cmd == "type") {
      RedirectScope redirect_scope(redirect);
      run_type(args);
    } else if (cmd == "pwd") {
      RedirectScope redirect_scope(redirect);
      run_pwd();
    } else if (cmd == "cd") {
      run_cd(args);
    } else if (cmd == "jobs") {
      run_jobs();
    } else {
      run_external(args, redirect, background, input);
    }
  }

  return 0;
}
