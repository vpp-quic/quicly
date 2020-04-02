/*
 * Copyright (c) 2017 Fastly, Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <assert.h>
#include <stdlib.h>
#include "picotls.h"
#include "quicly/sentmap.h"


quicly_sent_packet_t sentmap_packet__end = {
    NULL, UINT64_MAX, INT64_MAX
};


void quicly_sentmap_init(quicly_sentmap_t *map)
{
    *map = (quicly_sentmap_t){NULL};
}

void quicly_sentmap_commit(quicly_sentmap_t *map, uint16_t bytes_in_flight)
{
    assert(quicly_sentmap_is_open(map));

    if (bytes_in_flight != 0) {
        map->tail->ack_eliciting = 1;
        map->tail->bytes_in_flight = bytes_in_flight;
        map->bytes_in_flight += bytes_in_flight;
    }
    map->is_open = 0;
}

quicly_sent_frame_t *quicly_sentmap_allocate_frame(quicly_sentmap_t *map, quicly_sent_acked_cb acked)
{
    assert(quicly_sentmap_is_open(map));

    quicly_sent_packet_t *p = map->tail;
    assert(p->used_frames < p->frame_capacity);
    quicly_sent_frame_t *frame = &p->frames[p->used_frames];
    p->used_frames ++;
    frame->acked = acked;
    return frame;
}

static inline quicly_sent_packet_t *allocate_packet()
{
    quicly_sent_packet_t *packet = calloc(1, sizeof(quicly_sent_packet_t) + SENTMAP_FRAMES_PER_PACKET * sizeof(quicly_sent_frame_t));
    if (packet == NULL)
        return NULL;
    packet->frame_capacity = SENTMAP_FRAMES_PER_PACKET;
    return packet;
}

int quicly_sentmap_prepare(quicly_sentmap_t *map, uint64_t packet_number, int64_t now, uint8_t ack_epoch)
{
    assert(!quicly_sentmap_is_open(map));

    quicly_sent_packet_t *new_packet = allocate_packet();
    if (new_packet == NULL) {
        return PTLS_ERROR_NO_MEMORY;
    }
    new_packet->packet_number = packet_number;
    new_packet->sent_at = now;
    new_packet->ack_epoch = ack_epoch;
    if (map->tail != NULL)
    {
        map->tail->next = new_packet;
        map->tail = new_packet;
    } else {
        map->head = map->tail = new_packet;
    }
    map->is_open = 1;
    return 0;
}

static void discard_packet(quicly_sentmap_t *map, quicly_sent_packet_t *packet)
{
    assert(packet != &sentmap_packet__end);
    if (packet == map->head) {
        map->head = packet->next;
        if (packet == map->tail) {
            map->tail = map->head;
            assert((map->head == NULL) && (map->tail == NULL));
        }
        free(packet);
        return;
    }
    quicly_sent_packet_t *p = map->head;
    while (p != NULL && p->next != packet)
        p = p->next;
    assert(p != NULL);
    p->next = packet->next;
    if (map->tail == packet)
        map->tail = p;
    free(packet);
}

int quicly_sentmap_update(quicly_sentmap_t *map, quicly_sentmap_iter_t *iter, quicly_sentmap_event_t event,
                          struct st_quicly_conn_t *conn)
{
    quicly_sent_packet_t *packet;
    int notify_lost = 0, ret = 0, i = 0;

    assert(!quicly_sentmap_iter_is_end(iter));

    /* save packet pointer */
    packet = iter->p;

    /* update packet-level metrics (make adjustments to notify the loss when discarding a packet that is still deemed inflight) */
    if (packet->bytes_in_flight != 0) {
        if (event == QUICLY_SENTMAP_EVENT_EXPIRED)
            notify_lost = 1;
        assert(map->bytes_in_flight >= packet->bytes_in_flight);
        map->bytes_in_flight -= packet->bytes_in_flight;
    }
    packet->bytes_in_flight = 0;

    /* move iterator to next packet */
    quicly_sentmap_skip(iter);

    /* iterate through the frames */
    for (i = 0; i < packet->used_frames; i ++) {
        quicly_sent_frame_t *frame = &packet->frames[i];
        if (notify_lost && ret == 0)
            ret = frame->acked(conn, packet, frame, QUICLY_SENTMAP_EVENT_LOST);
        if (ret == 0)
            ret = frame->acked(conn, packet, frame, event);
    }

    if (event != QUICLY_SENTMAP_EVENT_LOST)
        discard_packet(map, packet);

    return ret;
}

void quicly_sentmap_dispose(quicly_sentmap_t *map)
{
    quicly_sent_packet_t *packet;

    while ((packet = map->head) != NULL) {
        discard_packet(map, packet);
    }
}
