#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum { RETRY_WAIT, RETRY_SEND, RETRY_REVERT } retry_action_t;
typedef struct {
    uint32_t command_id;
    int64_t started_ms;
    unsigned attempts;
    bool done;
} retry_budget_t;

// A command gets at most one retry, even if its target remains unreached.
// Motion or resolution consumes the budget; only a new request can re-arm it.
//
// retry_allowed is false on a toggle-only opener (Security+ 1.0): there is
// no idempotent Open/Close there, only TOGGLE, so re-sending against door
// state that may be stale (a missed status frame, or the door already moving
// from the first command) can stop or reverse the door rather than finish
// the request. Such a request skips straight to the revert.
static inline retry_action_t retry_budget_step(retry_budget_t *b, uint32_t id,
                                               bool pending, bool retry_allowed,
                                               int64_t now, uint32_t timeout)
{
    if (!id) return RETRY_WAIT;
    if (b->command_id != id) {
        b->command_id = id;
        b->started_ms = now;
        b->attempts = 0;
        b->done = false;
    }
    if (!pending) b->done = true;
    if (b->done || now - b->started_ms <= timeout) return RETRY_WAIT;
    b->started_ms = now;
    if (retry_allowed && b->attempts++ == 0) return RETRY_SEND;
    b->done = true;
    return RETRY_REVERT;
}
