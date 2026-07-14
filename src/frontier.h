/*
 *  frontier.h - Pareto-frontier parse states for block-framed codecs
 *
 *  A block header's count code makes a token's cost depend on the length
 *  of the same-type run it sits in, so a single cost per position is not
 *  enough state.  Each (position, arriving token type) instead keeps a
 *  Pareto frontier of states (run, cost), where run is the current run
 *  length and cost excludes the current run's still-pending header; a
 *  state's realized bits are cost + header(run).  A state survives only
 *  if no other state at the same position and type has both run <= and
 *  cost <= : a smaller run is never worse when the header cost is
 *  monotone non-decreasing in run length (exact for Elias gamma).
 *  Frontiers stay small because the header takes few distinct values
 *  over run <= 256.
 *
 *  Back-links are safe against dominance pruning: every edge into
 *  position i comes from an earlier position, so a frontier is complete
 *  before it is processed, and only states alive at processing time can
 *  acquire children.
 */

#ifndef FRONTIER_H_
#define FRONTIER_H_

#include "richc/array/u32.h"

#include "matches.h"

typedef struct rc_arena rc_arena;

typedef struct frontier_node {
    uint32_t next;          // next state in this frontier's list
    uint32_t from_index;    // predecessor position (RC_INDEX_NONE at seed)
    uint32_t cost;          // bits excluding the pending run header
    uint16_t run;           // current run length, 1..256
    uint16_t from_run;      // predecessor state's run length
    uint8_t  from_type;     // predecessor state's token type (lit/match)
    token    tok;           // token taken to reach this state
} frontier_node;

#define RC_ARRAY_TYPE frontier_node
#define RC_ARRAY_NAME node
#include "richc/template/array.h"

// Insert state (run, cost) into the frontier at heads[pos] unless it is
// dominated; removes any states it dominates.  Lists sorted by run.
void frontier_insert(rc_array_node *pool, rc_array_u32 *heads, uint32_t pos,
                     uint32_t run, uint32_t cost,
                     uint32_t from_index, uint32_t from_type, uint32_t from_run,
                     token tok, rc_arena *arena);

// The state with exactly this run length, or RC_INDEX_NONE
uint32_t frontier_find(rc_view_node pool, uint32_t head, uint32_t run);

#endif // FRONTIER_H_
