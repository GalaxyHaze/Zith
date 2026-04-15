#include "parser/parser.h"
#ifdef ZITH_WASM
#include <iostream>
#include <assert>

int main() {
    std::cout << "Thank you for installing Zith." << std::endl;
    std::cout << "The WASM version is currently under development and opaque." << std::endl;
    std::cout << "I'm doing my best to bring you new features soon. Stay tuned!" << std::endl;
    return 0; // Retorno 0 é geralmente melhor para "successo visual" mesmo que não faça nada
}
#else
#include <zith/zith.hpp>

int main(int argc, const char** string) {
    return zith_run(argc, string);
}
#endif