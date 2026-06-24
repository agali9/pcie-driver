#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <linux/ioctl.h>
}

#define PROTO_PCIE_IOC_MAGIC 'P'
#define PROTO_PCIE_MAX_PAYLOAD 256

struct proto_pcie_info {
    std::uint32_t bar0_len;
    std::uint32_t irq;
    std::uint32_t ring_entries;
    std::uint32_t ring_entry_size;
};

struct proto_pcie_command {
    std::uint32_t opcode;
    std::uint32_t length;
    std::uint8_t payload[PROTO_PCIE_MAX_PAYLOAD];
};

#define PROTO_PCIE_IOC_GET_INFO _IOR(PROTO_PCIE_IOC_MAGIC, 1, struct proto_pcie_info)
#define PROTO_PCIE_IOC_RESET _IO(PROTO_PCIE_IOC_MAGIC, 2)
#define PROTO_PCIE_IOC_SEND_CMD _IOW(PROTO_PCIE_IOC_MAGIC, 3, struct proto_pcie_command)
#define PROTO_PCIE_IOC_GET_STATUS _IOR(PROTO_PCIE_IOC_MAGIC, 4, std::uint32_t)

class ProtoPcie {
public:
    explicit ProtoPcie(const std::string& device_path = "/dev/proto_pcie");
    ~ProtoPcie();

    ProtoPcie(const ProtoPcie&) = delete;
    ProtoPcie& operator=(const ProtoPcie&) = delete;

    proto_pcie_info info() const;
    void reset() const;
    void send_command(std::uint32_t opcode, const std::vector<std::uint8_t>& payload) const;
    std::uint32_t status() const;

private:
    int fd_;
};
