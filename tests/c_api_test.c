#include <citoq/citoq_c.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
struct state { int calls; uint64_t type_id; char resource[32]; uint8_t payload[32]; size_t size; };
static void cb(void* user, uint64_t type_id, const char* resource, size_t resource_size,
               const uint8_t* payload, size_t payload_size) {
    struct state* s = (struct state*)user;
    ++s->calls; s->type_id = type_id;
    assert(resource_size < sizeof(s->resource));
    memcpy(s->resource, resource, resource_size); s->resource[resource_size] = '\0';
    assert(payload_size <= sizeof(s->payload));
    memcpy(s->payload, payload, payload_size); s->size = payload_size;
}
int main(void) {
    assert(citoq_abi_version() == 1u);
    citoq_context_t* ctx = NULL;
    assert(citoq_context_create("acme", 4, &ctx) == CITOQ_STATUS_OK);
    struct state s = {0};
    citoq_subscription_t* sub = NULL;
    assert(citoq_context_subscribe(ctx, 100, "drone_17", 8, cb, &s, &sub) == CITOQ_STATUS_OK);
    int has_demand = 99;
    assert(citoq_context_has_demand(
        ctx, 100, "drone_18", 8,
        &has_demand) == CITOQ_STATUS_OK);
    assert(has_demand == 0);
    assert(citoq_context_has_demand(
        ctx, 100, "drone_17", 8,
        &has_demand) == CITOQ_STATUS_OK);
    assert(has_demand == 1);

    /* A local subscription must not count as a remote route. This
       observation API also rejects null output pointers. */
    size_t matched_remote = 17;
    assert(citoq_context_remote_match_count(
        ctx, 100, "drone_17", 8, &matched_remote) == CITOQ_STATUS_OK);
    assert(matched_remote == 0);
    assert(citoq_context_remote_match_count(
        ctx, 100, "drone_17", 8, NULL) == CITOQ_STATUS_INVALID_ARGUMENT);

    const uint8_t hello[] = {'h','e','l','l','o'};
    size_t delivered = 99;

    /* A second Context in the same process shares host demand state. */
    citoq_context_t* peer = NULL;
    citoq_context_t* isolated = NULL;
    assert(citoq_context_create(
        "acme", 4,
        &peer) == CITOQ_STATUS_OK);
    assert(citoq_context_create(
        "other", 5,
        &isolated) == CITOQ_STATUS_OK);

    assert(citoq_context_has_demand(
        peer, 100, "drone_17", 8,
        &has_demand) == CITOQ_STATUS_OK);
    assert(has_demand == 1);

    assert(citoq_context_has_demand(
        isolated, 100, "drone_17", 8,
        &has_demand) == CITOQ_STATUS_OK);
    assert(has_demand == 0);

    assert(citoq_context_publish(
        peer, 100, "drone_17", 8,
        hello, sizeof hello,
        &delivered) == CITOQ_STATUS_OK);
    assert(delivered == 1);
    assert(s.calls == 1);

    citoq_context_destroy(peer);
    citoq_context_destroy(isolated);
    assert(citoq_context_publish(ctx, 100, "drone_18", 8, hello, sizeof hello, &delivered) == CITOQ_STATUS_OK);
    assert(delivered == 0 && s.calls == 1);
    assert(citoq_context_publish(ctx, 100, "drone_17", 8, hello, sizeof hello, &delivered) == CITOQ_STATUS_OK);
    assert(delivered == 1 && s.calls == 2 && s.type_id == 100);
    assert(strcmp(s.resource, "drone_17") == 0);
    assert(s.size == sizeof hello && memcmp(s.payload, hello, sizeof hello) == 0);
    citoq_subscription_destroy(sub);
    assert(citoq_context_has_demand(
        ctx, 100, "drone_17", 8,
        &has_demand) == CITOQ_STATUS_OK);
    assert(has_demand == 0);
    assert(citoq_context_publish(ctx, 100, "drone_17", 8, hello, sizeof hello, &delivered) == CITOQ_STATUS_OK);
    assert(delivered == 0);
    assert(citoq_context_run(ctx) == CITOQ_STATUS_OK);
    citoq_context_destroy(ctx);

    /* Subscription destruction after Context destruction must be safe. */
    citoq_context_t* short_ctx = NULL;
    citoq_subscription_t* long_sub = NULL;
    assert(citoq_context_create(
        "acme", 4,
        &short_ctx) == CITOQ_STATUS_OK);
    assert(citoq_context_subscribe(
        short_ctx, 200,
        "late", 4,
        cb, &s,
        &long_sub) == CITOQ_STATUS_OK);
    citoq_context_destroy(short_ctx);
    citoq_subscription_destroy(long_sub);

    return 0;
}
