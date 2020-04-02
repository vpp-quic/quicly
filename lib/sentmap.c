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
#include <string.h>
#include "picotls.h"
#include "quicly/sentmap.h"


quicly_sent_packet_t sentmap_packet__end = {
    UINT64_MAX, INT64_MAX
};

static void show_ring(quicly_sentmap_t *map) {
    printf("map %p ring sz %ld first %ld last %ld\n", map, map->ring_size, map->first_packet, map->last_packet);
    printf("ring state: ");
    for (int i = 0; i < map->ring_size; i ++) {
        printf("%x ", map->packet_ring[i]);
    }
    printf("\n");
}

static void grow_ring(quicly_sentmap_t *map)
{
    uint64_t new_ring_size = 2 * map->ring_size;
    if (!new_ring_size)
        new_ring_size = 2;

    map->packet_ring = realloc(map->packet_ring, new_ring_size * sizeof(quicly_sent_packet_t *));
    memset(&map->packet_ring[map->ring_size], 0, (new_ring_size - map->ring_size) * sizeof(quicly_sent_packet_t *));
    if (!map->ring_size)
      goto done;
    if ((map->last_packet % map->ring_size) < (map->first_packet % map->ring_size)) {
        if ((map->first_packet % map->ring_size) == (map->first_packet % new_ring_size)) {
            /* move the packets at the start of the ring to the start of the new area */
            uint64_t num_to_move = 1 + map->last_packet % map->ring_size;
            memmove(&map->packet_ring[map->ring_size],
                    map->packet_ring,
                    num_to_move * sizeof(quicly_sent_packet_t *));
            memset(map->packet_ring, 0, num_to_move * sizeof(quicly_sent_packet_t *));
        } else {
            /* move the packets at the start of the ring to the start of the new area */
            uint64_t num_to_move = map->ring_size - (map->first_packet % map->ring_size);
            memmove(&map->packet_ring[map->first_packet % new_ring_size],
                    &map->packet_ring[map->first_packet % map->ring_size],
                    num_to_move * sizeof(quicly_sent_packet_t *));
            memset(&map->packet_ring[map->first_packet % map->ring_size], 0,
                    num_to_move * sizeof(quicly_sent_packet_t *));
        }
    } else if ((map->first_packet % map->ring_size) != (map->first_packet % new_ring_size)) {
        /* move all the packets to the new area */
        uint64_t num_to_move = 1 + map->last_packet - map->first_packet;
        memmove(&map->packet_ring[map->first_packet % new_ring_size],
                &map->packet_ring[map->first_packet % map->ring_size],
                num_to_move * sizeof(quicly_sent_packet_t *));
        memset(&map->packet_ring[map->first_packet % map->ring_size], 0,
                num_to_move * sizeof(quicly_sent_packet_t *));
    }
done:
    map->ring_size = new_ring_size;
}

static inline quicly_sent_packet_t *get(quicly_sentmap_t *map, uint64_t packet_id)
{
    return map->packet_ring[packet_id % map->ring_size];
}


void quicly_sentmap_init(quicly_sentmap_t *map)
{
    *map = (quicly_sentmap_t){0};
    grow_ring(map);
    map->first_packet = UINT64_MAX;
}


void quicly_sentmap_commit(quicly_sentmap_t *map, uint16_t bytes_in_flight)
{
    assert(quicly_sentmap_is_open(map));

    if (bytes_in_flight != 0) {
        quicly_sent_packet_t *p = get(map, map->last_packet);
        p->ack_eliciting = 1;
        p->bytes_in_flight = bytes_in_flight;
        map->bytes_in_flight += bytes_in_flight;
    }
    map->is_open = 0;
}

quicly_sent_frame_t *quicly_sentmap_allocate_frame(quicly_sentmap_t *map, quicly_sent_acked_cb acked)
{
    assert(quicly_sentmap_is_open(map));

    quicly_sent_packet_t *p = get(map, map->last_packet);
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
    if (map->first_packet != UINT64_MAX)
        while (packet_number - map->first_packet >= map->ring_size - 1) /* -1 to always leave a null slot between last and first */
            grow_ring(map);
    new_packet->packet_number = packet_number;
    new_packet->sent_at = now;
    new_packet->ack_epoch = ack_epoch;
    map->packet_ring[packet_number % map->ring_size] = new_packet;

    if (map->first_packet == UINT64_MAX)
        map->first_packet = packet_number;
    if (map->last_packet < packet_number)
        map->last_packet = packet_number;
    map->is_open = 1;
    return 0;
}

static void discard_packet(quicly_sentmap_t *map, quicly_sent_packet_t *packet)
{
    uint64_t pnum = packet->packet_number;
    assert(pnum >= map->first_packet);
    assert(pnum <= map->last_packet);

    free(packet);
    map->packet_ring[pnum % map->ring_size] = NULL;
    if (pnum == map->first_packet) {
        if (pnum == map->last_packet) {
            map->first_packet = UINT64_MAX; // Empty map
            map->last_packet = 0;
            return;
        }
        do
            map->first_packet ++;
        while (!get(map, map->first_packet));
    } else if (pnum == map->last_packet) {
        do
            map->last_packet --;
        while (!get(map, map->last_packet));
        /* empty case already treated, we should find a packet */
    }
    assert(map->last_packet >= map->first_packet);
}

int quicly_sentmap_update(quicly_sentmap_t *map, quicly_sentmap_iter_t *iter, quicly_sentmap_event_t event,
                          struct st_quicly_conn_t *conn)
{
    int notify_lost = 0, ret = 0, i = 0;

    assert(!quicly_sentmap_iter_is_end(iter));

    /* save packet pointer */
    quicly_sent_packet_t *packet = quicly_sentmap_get(iter);

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
    while (map->first_packet != UINT64_MAX) {
        discard_packet(map, get(map, map->first_packet));
    }
}
