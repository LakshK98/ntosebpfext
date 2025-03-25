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

// The following line is optional, but is used to verify
// that the NetEventMonitor prototype is correct or the compiler
// would complain when the function is actually defined below.
netevent_event_hook_t NetEventMonitor;

SEC("netevent_monitor")
int
NetEventMonitor(netevent_event_md_t* ctx)
{
    int result = -1;

    if (ctx != NULL && ctx->payload_start != NULL && ctx->payload_end != NULL &&
        ctx->payload_end > ctx->payload_start) {

        // Push the event to the netevent_events_map.
        // TODO: switch to perf_event_output when it is available.
        result =
            bpf_ringbuf_output(&netevent_events_map, ctx->payload_start, (ctx->payload_end - ctx->payload_start), 0);
    }

    return result;
}
