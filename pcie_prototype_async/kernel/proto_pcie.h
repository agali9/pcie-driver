#ifndef PROTO_PCIE_H
#define PROTO_PCIE_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define PROTO_PCIE_NAME "proto_pcie"
#define PROTO_PCIE_MAX_PAYLOAD 256
#define PROTO_PCIE_RING_DEPTH 8

#define PROTO_PCIE_IOC_MAGIC 'P'

struct proto_pcie_info {
    __u32 bar0_len;
    __u32 irq;
    __u32 ring_entries;
    __u32 ring_entry_size;
};

struct proto_pcie_command {
    __u32 opcode;
    __u32 length;
    __u8 payload[PROTO_PCIE_MAX_PAYLOAD];
};

struct proto_pcie_ring_state {
    __u32 cmd_head;
    __u32 cmd_tail;
    __u32 cmd_pending;
    __u32 comp_head;
    __u32 comp_tail;
    __u32 comp_pending;
    __u32 completed_total;
    __u32 last_opcode;
    __u32 last_length;
    __u32 last_status;
    __u32 next_seqno;
};

struct proto_pcie_wait {
    __u32 target_completed_total;
    __u32 timeout_ms;
    __u32 observed_completed_total;
    __s32 result;
};

#define PROTO_PCIE_IOC_GET_INFO _IOR(PROTO_PCIE_IOC_MAGIC, 1, struct proto_pcie_info)
#define PROTO_PCIE_IOC_RESET _IO(PROTO_PCIE_IOC_MAGIC, 2)
#define PROTO_PCIE_IOC_SEND_CMD _IOW(PROTO_PCIE_IOC_MAGIC, 3, struct proto_pcie_command)
#define PROTO_PCIE_IOC_GET_STATUS _IOR(PROTO_PCIE_IOC_MAGIC, 4, __u32)
#define PROTO_PCIE_IOC_GET_RING_STATE _IOR(PROTO_PCIE_IOC_MAGIC, 5, struct proto_pcie_ring_state)
#define PROTO_PCIE_IOC_WAIT_COMPLETION _IOWR(PROTO_PCIE_IOC_MAGIC, 6, struct proto_pcie_wait)

#endif
