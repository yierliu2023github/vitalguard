/*
 * vg.h — VitalGuard shared types and wire protocol.
 *
 * VitalGuard is a patient-monitor alarm core for QNX 8.0 (QEMU target).
 * This header defines the messages exchanged between the processes:
 *
 *     vitalgen ──sample blocks──▶ vitaldsp ──vitals + alarm events──▶ alarmgr
 *        ▲                          │                                  │
 *        └──────── fault commands ──┴────── heartbeats ──▶ vgsupervisor ┘
 *
 * On QNX these messages ride native message passing (MsgSend/MsgReceive)
 * with priority inheritance; on the Linux host they ride the POSIX backend
 * of the same IPC abstraction (see src/ipc/).  The payload layouts are
 * identical in both worlds, which is what makes the host build a faithful
 * development model of the target system.
 *
 * Time base: all signal-path timestamps (t0_ms, t_event_ms, ...) are on the
 * "signal clock" — milliseconds since the signal generator started, stamped
 * by vitalgen on each sample block and propagated downstream.  This keeps
 * the DSP and alarm logic deterministic and unit-testable offline.
 * Heartbeats use the wall monotonic clock instead (liveness is a wall
 * clock property).
 */
#ifndef VG_H
#define VG_H

#include <stdint.h>

/* ---- signal geometry -------------------------------------------------- */
#define VG_SAMPLE_RATE_HZ   250                      /* ECG/pleth/resp rate */
#define VG_BLOCK_SAMPLES    25                       /* 100 ms per block    */
#define VG_BLOCK_MS         (1000 * VG_BLOCK_SAMPLES / VG_SAMPLE_RATE_HZ)

/* ---- IPC endpoint names (socket names on host, channel names on QNX) -- */
#define VG_EP_GEN           "vitalgen"
#define VG_EP_DSP           "vitaldsp"
#define VG_EP_ALARM         "alarmgr"
#define VG_EP_SUPERVISOR    "vgsupervisor"

/* ---- process roles (heartbeat sender ids) ------------------------------ */
enum {
    VG_PROC_GEN   = 1,
    VG_PROC_DSP   = 2,
    VG_PROC_ALARM = 3
};

/* ---- message types ----------------------------------------------------- */
typedef enum {
    VG_MSG_SAMPLE_BLOCK = 1,    /* vitalgen -> vitaldsp                    */
    VG_MSG_VITALS       = 2,    /* vitaldsp -> alarmgr (10 Hz)             */
    VG_MSG_ALARM_EVENT  = 3,    /* vitaldsp -> alarmgr (condition change)  */
    VG_MSG_HEARTBEAT    = 4,    /* any child  -> vgsupervisor              */
    VG_MSG_FAULT_CMD    = 5,    /* faultctl  -> vitalgen / vgsupervisor    */
    VG_MSG_ALARM_CMD    = 6     /* faultctl  -> alarmgr (ack / silence)    */
} vg_msg_type_t;

/* ---- payloads ---------------------------------------------------------- */

/* One 100 ms block of synthesised physiology. */
typedef struct {
    uint64_t t0_ms;                         /* signal-clock start of block */
    float    ecg[VG_BLOCK_SAMPLES];         /* mV, lead-II-like            */
    float    pleth[VG_BLOCK_SAMPLES];       /* SpO2 plethysmograph, arb.   */
    float    resp[VG_BLOCK_SAMPLES];        /* respiration waveform, arb.  */
    float    spo2_pct;                      /* instantaneous SpO2 at block end */
} vg_sample_block_t;

/* Alarm conditions evaluated by the DSP. */
typedef enum {
    VG_COND_NONE     = 0,
    VG_COND_VFIB     = 1,   /* ventricular fibrillation — HIGH            */
    VG_COND_ASYSTOLE = 2,   /* no QRS for >= 4 s, flat trace — HIGH       */
    VG_COND_HR_HIGH  = 3,   /* heart rate above limit — MEDIUM            */
    VG_COND_HR_LOW   = 4,   /* heart rate below limit — MEDIUM            */
    VG_COND_SPO2_LOW = 5,   /* SpO2 below limit — MEDIUM (HIGH if < 80 %)  */
    VG_COND_APNEA    = 6,   /* no breath detected — HIGH                  */
    VG_COND_COUNT    = 7
} vg_condition_t;

/* Alarm priorities, IEC 60601-1-8 style. */
typedef enum {
    VG_PRIO_HIGH   = 1,
    VG_PRIO_MEDIUM = 2,
    VG_PRIO_LOW    = 3
} vg_priority_t;

/* Periodic vitals summary, DSP -> alarm manager (also the UI feed). */
typedef struct {
    uint64_t t_ms;              /* signal clock                          */
    float    hr_bpm;            /* -1 when not (yet) measurable          */
    float    spo2_pct;          /* averaged SpO2                         */
    float    resp_rate;         /* breaths/min, -1 when not measurable   */
    uint32_t conditions_mask;   /* bit (1<<cond) per active condition    */
} vg_vitals_t;

/* A condition transition, DSP -> alarm manager. */
typedef struct {
    uint32_t condition;         /* vg_condition_t                        */
    uint32_t priority;          /* vg_priority_t                         */
    int32_t  active;            /* 1 = raised, 0 = cleared               */
    float    value;             /* measured value that tripped the limit */
    uint64_t t_onset_ms;        /* when the raw condition first appeared */
    uint64_t t_event_ms;        /* when persistence elapsed (raise time) */
} vg_alarm_event_t;

/* Liveness, child -> supervisor (wall clock at the supervisor). */
typedef struct {
    uint32_t sender;            /* VG_PROC_*                             */
    uint32_t seq;
} vg_heartbeat_t;

/* Fault-injection / control commands from the faultctl console. */
typedef enum {
    VG_FAULT_RHYTHM   = 1,      /* -> vitalgen: switch ECG rhythm        */
    VG_FAULT_DESAT    = 2,      /* -> vitalgen: SpO2 desaturation event  */
    VG_FAULT_APNEA    = 3,      /* -> vitalgen: respiration pause        */
    VG_FAULT_KILL_DSP = 4       /* -> vgsupervisor: SIGKILL the DSP      */
} vg_fault_kind_t;

typedef struct {
    uint32_t kind;              /* vg_fault_kind_t                       */
    uint32_t rhythm;            /* VG_FAULT_RHYTHM: vg_rhythm_t          */
    float    fparam;            /* DESAT: target SpO2 %                  */
    uint32_t duration_ms;       /* event duration; 0 = until changed     */
} vg_fault_cmd_t;

/* Operator commands to the alarm manager. */
typedef enum {
    VG_ALARM_CMD_ACK     = 1,
    VG_ALARM_CMD_SILENCE = 2    /* audio pause; alarm_id 0 = all alarms  */
} vg_alarm_cmd_action_t;

typedef struct {
    uint32_t alarm_id;
    uint32_t action;            /* vg_alarm_cmd_action_t                 */
} vg_alarm_cmd_t;

#endif /* VG_H */
