#define main UnusedFullLocomotionSuite
#include "locomotion_selftest.cpp"
#undef main
int main() {
    TestJumpArms();
    std::printf("PASS: %d TR1-3 jump arm continuity checks\n",checks);
}
