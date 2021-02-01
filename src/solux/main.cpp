#include <iostream>
#include "solux_main.h"


int main(int argc, char** argv) {
  std::cout << compile_env() << std::endl;
  std::cout << "solux says \"Hello World!\"" << std::endl;
  return solux_main(argc, argv);
}

