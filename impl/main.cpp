#ifdef KALIDOUS_WASM
#include <iostream>

int main() {
    std::cout << "Thank you for installing Kalidous." << std::endl;
    std::cout << "The WASM version is currently under development and opaque." << std::endl;
    std::cout << "I'm doing my best to bring you new features soon. Stay tuned!" << std::endl;
    return 0; // Retorno 0 é geralmente melhor para "successo visual" mesmo que não faça nada
}
#else
#include <kalidous/kalidous.hpp>

int main(int argc, const char** argv) {
    return kalidous_run(argc, argv);
}
#endif