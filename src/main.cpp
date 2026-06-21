#include <iostream>
#include <string>

int main() {
  // Flush after every std::cout / std:cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // TODO: Uncomment the code below to pass the first stage
  std::cout << "$ invalid_banana_command";
  std::string input;
  std::getline(std::cin, input);
  std::cout << input << ": command not found" << std::endl;
}
