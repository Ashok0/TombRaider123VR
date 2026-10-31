// Run only the physical-switch regression, not the full locomotion suite.
#define main UnusedFullLocomotionSuite
#include "locomotion_selftest.cpp"
#undef main
int main() {
    TestPhysicalSwitch();
    std::printf("PASS: %d physical-switch checks (TR1-3)\n",checks);
}
