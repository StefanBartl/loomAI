#include <iostream>

#include "minitest.hpp"

int main() {
    int ran = 0;
    for (const auto& test : minitest::cases()) {
        const int before = minitest::failures();
        std::cout << "[ RUN  ] " << test.name << "\n";
        test.body();
        ++ran;
        std::cout << (minitest::failures() == before ? "[  OK  ] " : "[ FAIL ] ") << test.name << "\n";
    }
    std::cout << "\n" << ran << " tests, " << minitest::failures() << " failed checks\n";
    return minitest::failures() == 0 ? 0 : 1;
}
