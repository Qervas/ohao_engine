#include "core/window.hpp"
#include <iostream>

int main() {
    try {
        ohao::Window window(800, 600, "OHAO Engine");

        while (!window.shouldClose()) {
            window.pollEvents();
        }

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
