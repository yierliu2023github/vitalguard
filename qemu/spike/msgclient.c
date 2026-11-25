/*
 * msgclient.c — QNX native IPC spike, client side.
 *
 * name_open the server, then three synchronous MsgSend round trips:
 * two pings and one "event" (the alarm-path shape).  Prints the
 * replies and a ClockCycles-based round-trip time for each send so
 * the console log is self-evidencing.  Usage: msgclient
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#include "spike_msg.h"

static int round_trip(int coid, uint16_t type, uint32_t seq,
                      const char *text)
{
    spike_req_t req;
    spike_reply_t reply;
    uint64_t t0, t1, cps;
    int rc;

    memset(&req, 0, sizeof(req));
    req.type = type;
    req.seq = seq;
    req.timestamp_ms = 0;
    snprintf(req.text, sizeof(req.text), "%s", text);

    cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    t0 = ClockCycles();
    rc = MsgSend(coid, &req, sizeof(req), &reply, sizeof(reply));
    t1 = ClockCycles();
    if (rc == -1) {
        perror("MsgSend");
        return -1;
    }
    printf("msgclient: seq=%u reply status=%d server_counter=%u "
           "text='%s' roundtrip=%llu us\n",
           seq, reply.status, reply.server_counter, reply.text,
           (unsigned long long)((t1 - t0) * 1000000ull / cps));
    fflush(stdout);
    return 0;
}

int main(void)
{
    int coid, tries;

    coid = -1;
    for (tries = 0; tries < 50 && coid == -1; tries++) {
        coid = name_open(SPIKE_NAME, 0);
        if (coid == -1) {
            struct timespec ts = { 0, 100 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    if (coid == -1) {
        perror("name_open");
        return 1;
    }
    printf("msgclient: connected to '%s' (coid %d) after %d tries\n",
           SPIKE_NAME, coid, tries + 1);

    round_trip(coid, SPIKE_TYPE_REQ,   1, "ping from msgclient");
    round_trip(coid, SPIKE_TYPE_EVENT, 2, "VFIB onset (spike event)");
    round_trip(coid, SPIKE_TYPE_REQ,   3, "second ping");

    ConnectDetach(coid);
    printf("msgclient: done\n");
    return 0;
}
