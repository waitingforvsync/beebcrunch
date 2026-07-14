/*
 *  frontier.c - Pareto-frontier parse states for block-framed codecs
 */

#include "frontier.h"

#include "richc/arena.h"
#include "richc/macros.h"

void frontier_insert(rc_array_node *pool, rc_array_u32 *heads, uint32_t pos,
                     uint32_t run, uint32_t cost,
                     uint32_t from_index, uint32_t from_type, uint32_t from_run,
                     token tok, rc_arena *arena)
{
    uint32_t head = rc_array_u32_get(heads, pos);

    // Dominated by an existing state: nothing to do.
    for (uint32_t c = head; c != RC_INDEX_NONE; ) {
        const frontier_node *node = rc_array_node_at(pool, c);
        if (node->run <= run && node->cost <= cost) {
            return;
        }
        c = node->next;
    }

    // Unlink every state the new one dominates.
    uint32_t prev = RC_INDEX_NONE;
    for (uint32_t c = head; c != RC_INDEX_NONE; ) {
        frontier_node *node = rc_array_node_at(pool, c);
        uint32_t next = node->next;
        if (node->run >= run && node->cost >= cost) {
            if (prev == RC_INDEX_NONE) {
                head = next;
            }
            else {
                rc_array_node_at(pool, prev)->next = next;
            }
        }
        else {
            prev = c;
        }
        c = next;
    }

    // Link the new state in ascending run order.
    uint32_t ni = rc_array_node_push(pool, (frontier_node) {
        .next = RC_INDEX_NONE,
        .from_index = from_index,
        .cost = cost,
        .run = (uint16_t)run,
        .from_run = (uint16_t)from_run,
        .from_type = (uint8_t)from_type,
        .tok = tok,
    }, arena);
    prev = RC_INDEX_NONE;
    uint32_t c = head;
    while (c != RC_INDEX_NONE && rc_array_node_at(pool, c)->run < run) {
        prev = c;
        c = rc_array_node_at(pool, c)->next;
    }
    rc_array_node_at(pool, ni)->next = c;
    if (prev == RC_INDEX_NONE) {
        head = ni;
    }
    else {
        rc_array_node_at(pool, prev)->next = ni;
    }
    rc_array_u32_set(heads, pos, head);
}

uint32_t frontier_find(rc_view_node pool, uint32_t head, uint32_t run)
{
    for (uint32_t c = head; c != RC_INDEX_NONE; ) {
        const frontier_node *node = rc_view_node_at(pool, c);
        if (node->run == run) {
            return c;
        }
        c = node->next;
    }
    return RC_INDEX_NONE;
}
