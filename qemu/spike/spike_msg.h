/*
 * spike_msg.h — shared message layout for the QNX native IPC spike.
 *
 * Deliberately shaped like the VitalGuard vg.h payloads: a small
 * header (type + timestamp) followed by a fixed payload, so the demo
 * exercises the same rendezvous the ipc_qnx.c port will use.
 */
#ifndef SPIKE_MSG_H
#define SPIKE_MSG_H

#include <stdint.h>
#include <sys/iomsg.h>

#define SPIKE_NAME        "vg_spike"
#define SPIKE_TYPE_REQ    0x1001u   /* client -> server request        */
#define SPIKE_TYPE_EVENT  0x1002u   /* client -> server "alarm" event  */

typedef struct {
    uint16_t type;                  /* _IO_MAX + n style discriminator */
    uint32_t seq;
    uint64_t timestamp_ms;
    char     text[64];
} spike_req_t;

typedef struct {
    int32_t  status;                /* 0 = ok                          */
    uint32_t seq;                   /* echoed sequence                 */
    uint32_t server_counter;        /* messages the server has handled */
    char     text[64];
} spike_reply_t;

#endif /* SPIKE_MSG_H */
