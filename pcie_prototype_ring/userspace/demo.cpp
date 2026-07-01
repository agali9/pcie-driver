#include "proto_pcie.hpp"
#include <iostream>
#include <vector>

static void print_ring_state(const proto_pcie_ring_state& s, const char* label)
{
    std::cout << label
              << " cmd_head=" << s.cmd_head
              << " cmd_tail=" << s.cmd_tail
              << " cmd_pending=" << s.cmd_pending
              << " comp_head=" << s.comp_head
              << " comp_tail=" << s.comp_tail
              << " comp_pending=" << s.comp_pending
              << " completed_total=" << s.completed_total
              << " last_opcode=" << s.last_opcode
              << " last_length=" << s.last_length
              << " last_status=" << s.last_status
              << " next_seqno=" << s.next_seqno
              << '\n';
}

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

        print_ring_state(dev.ring_state(), "before reset:");
        dev.reset();
        print_ring_state(dev.ring_state(), "after reset:");

        dev.send_command(1, {0x10, 0x20, 0x30, 0x40});
        dev.send_command(2, {0x55, 0x66, 0x77, 0x88, 0x99});
        print_ring_state(dev.ring_state(), "after queueing:");
        std::cout << "status(before process)=" << dev.status() << "\n";

        dev.process_pending();
        print_ring_state(dev.ring_state(), "after process:");
        std::cout << "status(after process)=" << dev.status() << "\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    return 0;
}
