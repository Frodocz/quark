#include <iostream>

#include "quark/Attributes.h"

int main() {
    std::cout << "Hello Quark(" << QUARK_VERSION_MAJOR << "." << QUARK_VERSION_MINOR
              << "." << QUARK_VERSION_PATCH << "), kVersion " << quark::kVersion << std::endl;
    return 0;
}