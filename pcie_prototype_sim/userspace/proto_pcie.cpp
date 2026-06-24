#include "proto_pcie.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

ProtoPcie::ProtoPcie(const std::string& device_path)
    : fd_(::open(device_path.c_str(), O_RDWR))
{
    if (fd_ < 0) {
        throw std::runtime_error("failed to open device: " + device_path + ": " + std::strerror(errno));
    }
}

ProtoPcie::~ProtoPcie()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

proto_pcie_info ProtoPcie::info() const
{
    proto_pcie_info out{};
    if (::ioctl(fd_, PROTO_PCIE_IOC_GET_INFO, &out) != 0) {
        throw std::runtime_error("ioctl(GET_INFO) failed: " + std::string(std::strerror(errno)));
    }
    return out;
}

void ProtoPcie::reset() const
{
    if (::ioctl(fd_, PROTO_PCIE_IOC_RESET) != 0) {
        throw std::runtime_error("ioctl(RESET) failed: " + std::string(std::strerror(errno)));
    }
}

void ProtoPcie::send_command(std::uint32_t opcode, const std::vector<std::uint8_t>& payload) const
{
    proto_pcie_command cmd{};
    cmd.opcode = opcode;
    cmd.length = static_cast<std::uint32_t>(payload.size() > PROTO_PCIE_MAX_PAYLOAD ? PROTO_PCIE_MAX_PAYLOAD : payload.size());
    if (cmd.length > 0) {
        std::memcpy(cmd.payload, payload.data(), cmd.length);
    }

    if (::ioctl(fd_, PROTO_PCIE_IOC_SEND_CMD, &cmd) != 0) {
        throw std::runtime_error("ioctl(SEND_CMD) failed: " + std::string(std::strerror(errno)));
    }
}

std::uint32_t ProtoPcie::status() const
{
    std::uint32_t out = 0;
    if (::ioctl(fd_, PROTO_PCIE_IOC_GET_STATUS, &out) != 0) {
        throw std::runtime_error("ioctl(GET_STATUS) failed: " + std::string(std::strerror(errno)));
    }
    return out;
}
