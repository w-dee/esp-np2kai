#include <assert.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "np2audio86_fixture.h"

static void print_sha256(const uint8_t digest[NP2_SHA256_DIGEST_SIZE])
{
    size_t i;
    for (i = 0U; i < NP2_SHA256_DIGEST_SIZE; ++i) {
        printf("%02x", digest[i]);
    }
}

static void test_event_ring(void)
{
    struct np2audio86_event_ring ring;
    struct np2audio86_event event;
    unsigned i;
    np2audio86_event_ring_init(&ring);
    assert(np2audio86_event_ring_dequeue(&ring, &event) ==
           NP2_AUDIO86_TRANSPORT_EMPTY);
    for (i = 0U; i < NP2_AUDIO86_ASYNC_EVENT_CAPACITY; ++i) {
        event.frame_timestamp = i * 240U;
        event.sequence = i;
        event.opcode = NP2_AUDIO86_EVENT_FM_KEY;
        event.payload = i;
        assert(np2audio86_event_ring_enqueue(&ring, &event) ==
               NP2_AUDIO86_TRANSPORT_OK);
    }
    assert(np2audio86_event_ring_occupancy(&ring) == 128U);
    event.sequence = 128U;
    assert(np2audio86_event_ring_enqueue(&ring, &event) ==
           NP2_AUDIO86_TRANSPORT_FULL);
    for (i = 0U; i < 64U; ++i) {
        assert(np2audio86_event_ring_dequeue(&ring, &event) ==
               NP2_AUDIO86_TRANSPORT_OK);
        assert(event.sequence == i);
    }
    for (i = 128U; i < 192U; ++i) {
        event.frame_timestamp = i * 240U;
        event.sequence = i;
        event.opcode = NP2_AUDIO86_EVENT_PSG_REGISTER;
        event.payload = i;
        assert(np2audio86_event_ring_enqueue(&ring, &event) ==
               NP2_AUDIO86_TRANSPORT_OK);
    }
    for (i = 64U; i < 192U; ++i) {
        assert(np2audio86_event_ring_dequeue(&ring, &event) ==
               NP2_AUDIO86_TRANSPORT_OK);
        assert(event.sequence == i);
    }
    assert(np2audio86_event_ring_occupancy(&ring) == 0U);

    /* The counters are uint32_t monotonic positions; exercise the exact
     * usable-capacity boundary across a counter wrap. */
    np2audio86_event_ring_init(&ring);
    atomic_store_explicit(&ring.head, UINT32_MAX - 63U, memory_order_relaxed);
    atomic_store_explicit(&ring.tail, UINT32_MAX - 63U, memory_order_relaxed);
    for (i = 0U; i < NP2_AUDIO86_ASYNC_EVENT_CAPACITY; ++i) {
        event.sequence = i;
        assert(np2audio86_event_ring_enqueue(&ring, &event) ==
               NP2_AUDIO86_TRANSPORT_OK);
    }
    assert(np2audio86_event_ring_occupancy(&ring) ==
           NP2_AUDIO86_ASYNC_EVENT_CAPACITY);
    assert(np2audio86_event_ring_enqueue(&ring, &event) ==
           NP2_AUDIO86_TRANSPORT_FULL);
    for (i = 0U; i < NP2_AUDIO86_ASYNC_EVENT_CAPACITY; ++i) {
        assert(np2audio86_event_ring_dequeue(&ring, &event) ==
               NP2_AUDIO86_TRANSPORT_OK);
        assert(event.sequence == i);
    }
    assert(np2audio86_event_ring_occupancy(&ring) == 0U);
}

static void test_byte_ring(void)
{
    struct np2audio86_byte_ring ring;
    uint8_t expected[NP2_AUDIO86_ASYNC_BYTE_CAPACITY];
    uint8_t input[NP2_AUDIO86_ASYNC_BYTE_CAPACITY];
    uint8_t output[NP2_AUDIO86_ASYNC_BYTE_CAPACITY];
    size_t expected_count = 0U;
    size_t i;
    np2audio86_byte_ring_init(&ring);
    assert(np2audio86_byte_ring_pop(&ring, output, 1U) ==
           NP2_AUDIO86_TRANSPORT_EMPTY);
    for (i = 0U; i < sizeof(input); ++i) {
        input[i] = (uint8_t)((i * 37U + 11U) & 0xffU);
    }
    assert(np2audio86_byte_ring_push(&ring, input, 32768U) ==
           NP2_AUDIO86_TRANSPORT_OK);
    memcpy(expected + expected_count, input, 32768U);
    expected_count += 32768U;
    assert(np2audio86_byte_ring_pop(&ring, output, 20000U) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(memcmp(output, expected, 20000U) == 0);
    memmove(expected, expected + 20000U, expected_count - 20000U);
    expected_count -= 20000U;
    assert(np2audio86_byte_ring_push(&ring, input, 50000U) ==
           NP2_AUDIO86_TRANSPORT_OK);
    memcpy(expected + expected_count, input, 50000U);
    expected_count += 50000U;
    assert(np2audio86_byte_ring_push(&ring, input + 50000U, 2768U) ==
           NP2_AUDIO86_TRANSPORT_OK);
    memcpy(expected + expected_count, input + 50000U, 2768U);
    expected_count += 2768U;
    assert(expected_count == NP2_AUDIO86_ASYNC_BYTE_CAPACITY);
    assert(np2audio86_byte_ring_push(&ring, input, 1U) ==
           NP2_AUDIO86_TRANSPORT_FULL);
    assert(np2audio86_byte_ring_pop(&ring, output, expected_count) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(memcmp(output, expected, expected_count) == 0);
    assert(np2audio86_byte_ring_occupancy(&ring) == 0U);

    np2audio86_byte_ring_init(&ring);
    atomic_store_explicit(&ring.head, UINT32_MAX - 32767U,
                          memory_order_relaxed);
    atomic_store_explicit(&ring.tail, UINT32_MAX - 32767U,
                          memory_order_relaxed);
    assert(np2audio86_byte_ring_push(&ring, input,
                                     NP2_AUDIO86_ASYNC_BYTE_CAPACITY) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(np2audio86_byte_ring_occupancy(&ring) ==
           NP2_AUDIO86_ASYNC_BYTE_CAPACITY);
    assert(np2audio86_byte_ring_pop(&ring, output,
                                    NP2_AUDIO86_ASYNC_BYTE_CAPACITY) ==
           NP2_AUDIO86_TRANSPORT_OK);
    assert(memcmp(output, input, NP2_AUDIO86_ASYNC_BYTE_CAPACITY) == 0);
}

static int exact_result(const struct np2audio86_async_result *left,
                        const struct np2audio86_async_result *right)
{
    return left->passed == right->passed &&
           left->oracle.frames == right->oracle.frames &&
           left->oracle.bytes == right->oracle.bytes &&
           left->oracle.quanta == right->oracle.quanta &&
           left->oracle.pcm_crc32 == right->oracle.pcm_crc32 &&
           memcmp(left->oracle.pcm_sha256, right->oracle.pcm_sha256,
                  NP2_SHA256_DIGEST_SIZE) == 0 &&
           left->oracle.control_crc32 == right->oracle.control_crc32 &&
           memcmp(left->oracle.control_sha256, right->oracle.control_sha256,
                  NP2_SHA256_DIGEST_SIZE) == 0 &&
           left->oracle.source_crc32 == right->oracle.source_crc32 &&
           memcmp(left->oracle.source_sha256, right->oracle.source_sha256,
                  NP2_SHA256_DIGEST_SIZE) == 0 &&
           left->oracle.pcm86_bytes_supplied == right->oracle.pcm86_bytes_supplied &&
           left->oracle.pcm86_bytes_consumed == right->oracle.pcm86_bytes_consumed &&
           left->oracle.pcm86_refills == right->oracle.pcm86_refills &&
           left->oracle.pcm86_fifo_min == right->oracle.pcm86_fifo_min &&
           left->oracle.pcm86_fifo_max == right->oracle.pcm86_fifo_max &&
           left->oracle.mix_peak_abs == right->oracle.mix_peak_abs &&
           left->oracle.clamped_samples == right->oracle.clamped_samples &&
           left->oracle.fm_contribution == right->oracle.fm_contribution &&
           left->oracle.psg_contribution == right->oracle.psg_contribution &&
           left->oracle.rhythm_contribution == right->oracle.rhythm_contribution &&
           left->oracle.pcm86_contribution == right->oracle.pcm86_contribution &&
           left->transport_event_count == right->transport_event_count &&
           left->transport_event_crc32 == right->transport_event_crc32 &&
           memcmp(left->transport_event_sha256,
                  right->transport_event_sha256,
                  NP2_SHA256_DIGEST_SIZE) == 0;
}

int main(void)
{
    struct np2audio86_async_result runs[4][2];
    unsigned mode;
    test_event_ring();
    test_byte_ring();
    printf("AUDIO86_ASYNC_RING_TEST event_ring=PASS byte_ring=PASS\n");
    printf("AUDIO86_ASYNC_CONFIG modes=4 lifecycles_per_mode=2 event_capacity=128 byte_capacity=65536 max_data_run=32768\n");
    for (mode = 0U; mode < 4U; ++mode) {
        unsigned repetition;
        for (repetition = 0U; repetition < 2U; ++repetition) {
            int status = np2audio86_async_run(
                (enum np2audio86_async_mode)mode, &runs[mode][repetition]);
            printf("AUDIO86_ASYNC_MODE mode=%s lifecycle=%c pass=%u first_error=%s event_full_wait=%" PRIu64 " byte_full_wait=%" PRIu64 " event_high_water=%u byte_high_water=%u\n",
                   np2audio86_async_mode_name((enum np2audio86_async_mode)mode),
                   repetition == 0U ? 'A' : 'B',
                   status == 0 && runs[mode][repetition].passed,
                   np2audio86_async_error_name(runs[mode][repetition].first_error),
                   runs[mode][repetition].event_full_wait_count,
                   runs[mode][repetition].pcm86_byte_full_wait_count,
                   runs[mode][repetition].event_high_water,
                   runs[mode][repetition].pcm86_byte_high_water);
            if (status != 0 || !runs[mode][repetition].passed) {
                return 1;
            }
            if (repetition != 0U &&
                !exact_result(&runs[mode][0], &runs[mode][repetition])) {
                return 1;
            }
        }
    }
    for (mode = 1U; mode < 4U; ++mode) {
        if (!exact_result(&runs[0][0], &runs[mode][0]) ||
            !exact_result(&runs[0][0], &runs[mode][1])) {
            return 1;
        }
    }
    printf("AUDIO86_ASYNC_ORACLE pcm_crc32=%08" PRIx32 " pcm_sha256=",
           runs[0][0].oracle.pcm_crc32);
    print_sha256(runs[0][0].oracle.pcm_sha256);
    printf("\n");
    printf("AUDIO86_ASYNC_TRANSPORT events=%" PRIu64 " crc32=%08" PRIx32
           " sha256=",
           runs[0][0].transport_event_count,
           runs[0][0].transport_event_crc32);
    print_sha256(runs[0][0].transport_event_sha256);
    printf("\n");
    printf("AUDIO86_ASYNC_PCM86 data_runs=%" PRIu64 " supplied=%" PRIu64 " consumed=%" PRIu64 " fifo_min=%" PRId32 " fifo_max=%" PRId32 " underrun=%u\n",
           runs[0][0].pcm86_data_run_count,
           runs[0][0].oracle.pcm86_bytes_supplied,
           runs[0][0].oracle.pcm86_bytes_consumed,
           runs[0][0].oracle.pcm86_fifo_min,
           runs[0][0].oracle.pcm86_fifo_max,
           runs[0][0].oracle.pcm86_fifo_underrun);
    printf("AUDIO86_ASYNC_CROSS_MODE_EXACTNESS=PASS\n");
    printf("AUDIO86_ASYNC_RESULT=PASS\n");
    return 0;
}
