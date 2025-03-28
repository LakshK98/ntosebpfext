// Copyright (c) Microsoft Corporation
// SPDX-License-Identifier: MIT

// This BPF program listens for events from the netevent driver, and stores them into a ring buffer map.

#include "bpf_helpers.h"
#include "ebpf_netevent_hooks.h"

#include <stddef.h>
#include <stdint.h>

// Ring-buffer for netevent_event_md_t.
#define EVENTS_MAP_SIZE (512 * 1024)
struct
{
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, EVENTS_MAP_SIZE);
} netevent_events_map SEC(".maps");
struct
{
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, EVENTS_MAP_SIZE);
} test_events_map SEC(".maps");

// The following line is optional, but is used to verify
// that the NetEventMonitor prototype is correct or the compiler
// would complain when the function is actually defined below.
netevent_event_hook_t NetEventMonitor;

SEC("netevent_monitor")
int
NetEventMonitor(netevent_event_md_t* ctx)
{
    int result = -1;

    if (ctx != NULL && ctx->data_start != NULL && ctx->data_end != NULL && ctx->data_end > ctx->data_start) {

        if (ctx->data_meta != NULL && ctx->data_meta > ctx->data_end) {
            // bpf_printk("NetEventMonitor: data_meta lengt: %u\n", (ctx->data_meta - ctx->data_end));
            bpf_ringbuf_output(&test_events_map, ctx->data_end, 10, 0);
        }
        // Push the event to the netevent_events_map.
        // TODO: switch to perf_event_output when it is available.
        // Issue: https://github.com/microsoft/ntosebpfext/issues/204.
        result = bpf_ringbuf_output(&netevent_events_map, ctx->data_start, (ctx->data_end - ctx->data_start), 0);
    }

    return result;
}
