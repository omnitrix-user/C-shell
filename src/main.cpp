#include <algorithm>
#include <cctype>
#include <cstdlib>
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

bool is_builtin(const std::string& cmd) {
  return cmd == "echo" || cmd == "exit" || cmd == "type" || cmd == "pwd" || cmd == "cd";
}

const std::vector<std::string>& builtin_commands() {
  static const std::vector<std::string> commands = {"cd", "echo", "exit", "pwd", "type"};
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

void run_external(std::vector<std::string> args, const RedirectInfo& redirect) {
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
    int status = 0;
    waitpid(pid, &status, 0);
  }
}

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  rl_attempted_completion_function = command_completion;
  rl_completion_append_character = ' ';

  while (true) {
    char* raw_line = readline("$ ");
    if (raw_line == nullptr) {
      break;
    }

    std::string input(raw_line);
    free(raw_line);

    if (input.empty()) {
      continue;
    }

    std::vector<std::string> args = parse_args(input);
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
    } else {
      run_external(args, redirect);
    }
  }

  return 0;
}
