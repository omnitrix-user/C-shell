#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

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

void run_external(const std::vector<std::string>& args) {
  const std::string& program = args[0];
  const std::string path = find_in_path(program);
  if (path.empty()) {
    std::cout << program << ": command not found\n";
    return;
  }

  const pid_t pid = fork();
  if (pid == 0) {
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

  while (true) {
    std::cout << "$ ";
    std::string input;
    if (!std::getline(std::cin, input)) {
      break;
    }

    const std::vector<std::string> args = parse_args(input);
    if (args.empty()) {
      continue;
    }

    const std::string& cmd = args[0];

    if (cmd == "exit") {
      return 0;
    }
    if (cmd == "echo") {
      run_echo(args);
    } else if (cmd == "type") {
      run_type(args);
    } else if (cmd == "pwd") {
      run_pwd();
    } else if (cmd == "cd") {
      run_cd(args);
    } else {
      run_external(args);
    }
  }

  return 0;
}
