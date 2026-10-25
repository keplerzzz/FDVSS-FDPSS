#include <exception>
#include <iostream>

void reference_algebra_tests();
void fdvss1_reference_tests();
void fdvss2_reference_tests();
void fdpss_reference_tests();
void payload_tests();

int main() {
    try {
        reference_algebra_tests();
        fdvss1_reference_tests();
        fdvss2_reference_tests();
        fdpss_reference_tests();
        payload_tests();
        std::cout << "connected reference tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "reference test failed: " << error.what() << '\n';
        return 1;
    }
}
