/*
 * msgserver.c — QNX native IPC spike, server side.
 *
 * ChannelCreate + name_attach (global namespace), then a
 * MsgReceive/MsgReply rendezvous loop.  Handles two request types:
 * a plain ping (replies with a counter) and an "event" (treated like
 * an alarm-path message: logged loudly, replied immediately — the
 * receive -> reply discipline ipc_qnx.c is designed around).
 *
 * Runs until killed.  Usage: msgserver &
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>

#include "spike_msg.h"

int main(void)
{
    name_attach_t *attach;
    spike_req_t req;
    spike_reply_t reply;
    uint32_t counter = 0;

    attach = name_attach(NULL, SPIKE_NAME, 0);
    if (!attach) {
        perror("name_attach");
        return 1;
    }
    printf("msgserver: listening on channel %d as /dev/name/local/%s\n",
           attach->chid, SPIKE_NAME);
    fflush(stdout);

    for (;;) {
        int rcvid = MsgReceive(attach->chid, &req, sizeof(req), NULL);
        if (rcvid == -1) {
            perror("MsgReceive");
            continue;
        }
        if (rcvid == 0) {           /* a pulse, not a message */
            printf("msgserver: pulse received (code %d value %d)\n",
                   ((struct _pulse *)&req)->code,
                   ((struct _pulse *)&req)->value.sival_int);
            fflush(stdout);
            continue;
        }
        counter++;
        memset(&reply, 0, sizeof(reply));
        reply.status = 0;
        reply.seq = req.seq;
        reply.server_counter = counter;
        if (req.type == SPIKE_TYPE_EVENT) {
            printf("msgserver: EVENT seq=%u text='%s' -> replying at once\n",
                   req.seq, req.text);
            snprintf(reply.text, sizeof(reply.text),
                     "event accepted: %.40s", req.text);
        } else {
            printf("msgserver: request seq=%u text='%s'\n",
                   req.seq, req.text);
            snprintf(reply.text, sizeof(reply.text),
                     "pong for seq %u", req.seq);
        }
        fflush(stdout);
        if (MsgReply(rcvid, 0, &reply, sizeof(reply)) == -1)
            perror("MsgReply");
    }
    /* NOTREACHED */
    return 0;
}
