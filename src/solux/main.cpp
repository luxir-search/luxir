#include <iostream>
#include "solux_main.h"


int main(int argc, char** argv) {
  std::cout << solux_banner() << std::endl;
  std::cout << "solux says \"Hello World!\"" << std::endl;
  return solux_main(argc, argv);
}

