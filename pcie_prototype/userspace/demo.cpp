#include "proto_pcie.hpp"
#include <iostream>

int main()
{
    try {
        ProtoPcie dev("/dev/proto_pcie");
        auto info = dev.info();

        std::cout << "bar0_len=" << info.bar0_len
                  << " irq=" << info.irq
                  << " ring_entries=" << info.ring_entries
                  << " ring_entry_size=" << info.ring_entry_size
                  << "\n";

        dev.reset();
        dev.send_command(1, {0x10, 0x20, 0x30, 0x40});
        std::cout << "status=" << dev.status() << "\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    return 0;
}
