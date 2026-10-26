/*
 * main.c - Main entry point and task orchestration for whattheflip
 *
 * Runs µT-Kernel 3.0 on ESP32-C6 (single-core RV32IMAC).
 * Enforces Barr C standard: zero malloc/free, static task stacks.
 */

#include "wtf_types.h"
#include "wtf_safety.h"
#include "wtf_discovery.h"
#include "wtf_sniffer.h"
#include "wtf_fuzzer.h"
#include "wtf_storage.h"
#include "wtf_telemetry.h"

#define TASK_STACK_SZ           3072
#define STORAGE_STACK_SZ        4096

/* Global Kernel Object Handles */
ID g_mpf_packets       = 0;
ID g_mbf_storage       = 0;
ID g_mbf_telemetry     = 0;
ID g_mtx_probe_access  = 0;
ID g_flg_system        = 0;

/* Static Memory Allocations for Kernel Objects (Zero malloc) */
#define MPF_NUM_BLOCKS          16
#define MPF_BLOCK_SZ            2048
LOCAL UB s_mpf_buffer[MPF_NUM_BLOCKS * MPF_BLOCK_SZ];

#define MBF_STORAGE_DEPTH       16
LOCAL UB s_mbf_storage_buffer[MBF_STORAGE_DEPTH * sizeof(wtf_record_t)];

#define MBF_TELEMETRY_DEPTH     8
LOCAL UB s_mbf_telemetry_buffer[MBF_TELEMETRY_DEPTH * sizeof(wtf_record_t)];

/* Idle task - required by ESP32-C6 µT-Kernel port when all tasks block */
LOCAL void task_idle(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    for (;;) {
        asm volatile ("wfi" ::: "memory");
    }
}

LOCAL BOOL create_kernel_objects(void)
{
    /* 1. Fixed Memory Pool MPF_PACKETS */
    T_CMPF cmpf = {
        .mpfatr = TA_TFIFO | TA_USERBUF,
        .mpfcnt = MPF_NUM_BLOCKS,
        .blfsz  = MPF_BLOCK_SZ,
        .bufptr = s_mpf_buffer,
    };
    g_mpf_packets = tk_cre_mpf(&cmpf);
    if (g_mpf_packets <= E_OK) {
        tm_printf((UB *)"[INIT] FATAL: tk_cre_mpf failed (%d)\n", g_mpf_packets);
        return FALSE;
    }

    /* 2. Message Buffer MBF_STORAGE (depth 16) */
    T_CMBF cmbf_storage = {
        .mbfatr = TA_TFIFO | TA_USERBUF,
        .bufsz  = sizeof(s_mbf_storage_buffer),
        .maxmsz = sizeof(wtf_record_t),
        .bufptr = s_mbf_storage_buffer,
    };
    g_mbf_storage = tk_cre_mbf(&cmbf_storage);
    if (g_mbf_storage <= E_OK) {
        tm_printf((UB *)"[INIT] FATAL: tk_cre_mbf(storage) failed (%d)\n", g_mbf_storage);
        return FALSE;
    }

    /* 3. Message Buffer MBF_TELEMETRY (depth 8) */
    T_CMBF cmbf_telemetry = {
        .mbfatr = TA_TFIFO | TA_USERBUF,
        .bufsz  = sizeof(s_mbf_telemetry_buffer),
        .maxmsz = sizeof(wtf_record_t),
        .bufptr = s_mbf_telemetry_buffer,
    };
    g_mbf_telemetry = tk_cre_mbf(&cmbf_telemetry);
    if (g_mbf_telemetry <= E_OK) {
        tm_printf((UB *)"[INIT] FATAL: tk_cre_mbf(telemetry) failed (%d)\n", g_mbf_telemetry);
        return FALSE;
    }

    /* 4. Mutex MTX_PROBE_ACCESS */
    T_CMTX cmtx = {
        .mtxatr = TA_TFIFO | TA_INHERIT,
    };
    g_mtx_probe_access = tk_cre_mtx(&cmtx);
    if (g_mtx_probe_access <= E_OK) {
        tm_printf((UB *)"[INIT] FATAL: tk_cre_mtx failed (%d)\n", g_mtx_probe_access);
        return FALSE;
    }

    /* 5. Event Flag FLG_SYSTEM */
    T_CFLG cflg = {
        .flgatr = TA_TFIFO | TA_WMUL,
        .iflgptn = 0,
    };
    g_flg_system = tk_cre_flg(&cflg);
    if (g_flg_system <= E_OK) {
        tm_printf((UB *)"[INIT] FATAL: tk_cre_flg failed (%d)\n", g_flg_system);
        return FALSE;
    }

    return TRUE;
}

LOCAL BOOL launch_task(const char *name, FP task_fn, PRI prio, SZ stksz)
{
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG0,
        .task    = task_fn,
        .itskpri = prio,
        .stksz   = stksz,
        .bufptr  = NULL,
    };
    ID tid = tk_cre_tsk(&ctsk);
    if (tid <= E_OK) {
        tm_printf((UB *)"[INIT] FATAL: tk_cre_tsk(%s) failed, er=%d\n", name, tid);
        return FALSE;
    }
    ER er = tk_sta_tsk(tid, 0);
    if (er != E_OK) {
        tm_printf((UB *)"[INIT] FATAL: tk_sta_tsk(%s) failed, er=%d\n", name, er);
        return FALSE;
    }
    return TRUE;
}

EXPORT INT usermain(void)
{
    /* Keep probes high-Z for initial 50 ms (strapping pins GPIO4, GPIO5, GPIO9 invariant) */
    tk_dly_tsk(50);

    tm_printf((UB *)"\n============================================================\n");
    tm_printf((UB *)"  whattheflip - Multi-Protocol Diagnostic & Fuzzing Tool    \n");
    tm_printf((UB *)"  µT-Kernel 3.0 (IEEE 2050-2018) on ESP32-C6 RISC-V        \n");
    tm_printf((UB *)"============================================================\n");

    /* Create lowest-priority idle task */
    if (!launch_task("TSK_IDLE", (FP)task_idle, CNF_MAX_TSKPRI, 1024)) {
        return 1;
    }

    /* Initialize static kernel objects */
    if (!create_kernel_objects()) {
        tm_printf((UB *)"[INIT] FATAL: Kernel object creation failed!\n");
        return 1;
    }
    tm_printf((UB *)"[INIT] Kernel objects created (MPF_PACKETS, MBF_STORAGE, MTX_PROBE_ACCESS)\n");

    /* Launch Subsystem Tasks */
    launch_task("TSK_SAFETY",    (FP)wtf_safety_task,    4,  2048);
    launch_task("TSK_DISCOVERY", (FP)wtf_discovery_task, 7,  TASK_STACK_SZ);
    launch_task("TSK_SNIFFER",   (FP)wtf_sniffer_task,   8,  TASK_STACK_SZ);
    launch_task("TSK_FUZZER",    (FP)wtf_fuzzer_task,    9,  TASK_STACK_SZ);
    launch_task("TSK_STORAGE",   (FP)wtf_storage_task,   10, STORAGE_STACK_SZ);
    launch_task("TSK_TELEMETRY", (FP)wtf_telemetry_task, 11, TASK_STACK_SZ);

    tm_printf((UB *)"[INIT] All 6 subsystem tasks launched. Initial task sleeping forever.\n\n");
    tk_slp_tsk(TMO_FEVR);
    return 0;
}
