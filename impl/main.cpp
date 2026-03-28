#ifdef KALIDOUS_WASM
#include <iostream>
    int main() {
        std::cout << "Thank you for installing Kalidous, however this version is opaque for WASM rigth now\n";
        std::cout << "Im doing my best in order to have more things to show, WASM will come soon";
        return -1;
    }
#else
#include <kalidous/kalidous.hpp>
    int main(int argc, const char** argv) {
        return kalidous_run(argc, argv);
    }
#endif
