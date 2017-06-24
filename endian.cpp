#include <iostream>
#include <cstdint>
#include <cstring>

int main() {
    uint32_t val = 0x01020304;
    auto const* ptr = reinterpret_cast<char const*>(&val);
    if (std::memcmp(ptr, "\x01\x02\x03\x04", 4) == 0) {
        std::cout << "#define LSB_FIRST 0\n";
    } else if (std::memcmp(ptr, "\x04\x03\x02\x01", 4) == 0) {
        std::cout << "#define LSB_FIRST 1\n";
    } else {
        // We don't support PDP, so there
        std::cout << "static_assert(false, \"Cannot determine endianness\")\n";
        return 1;
    }
    return 0;
}
