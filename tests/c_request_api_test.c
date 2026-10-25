#include <citoq/citoq_c.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct request_state {
    citoq_context_t* provider;
    uint64_t request_id;
    int handler_calls;
    int completion_calls;
    citoq_request_status_t status;
    uint8_t response[8];
    size_t response_size;
    int auto_reply;
};

static void handler_cb(
    void* user,
    uint64_t request_id,
    uint64_t request_type_id,
    const char* resource,
    size_t resource_size,
    const uint8_t* payload,
    size_t payload_size) {
    struct request_state* s =
        (struct request_state*)user;

    ++s->handler_calls;
    s->request_id = request_id;

    assert(request_type_id == 500u ||
           request_type_id == 501u);
    assert(resource != NULL);
    assert(resource_size > 0u);
    assert(payload_size == 1u);

    if (!s->auto_reply) {
        return;
    }

    const uint8_t response[] = {
        (uint8_t)(payload[0] + 1u)
    };
    int accepted = 0;

    assert(citoq_context_reply(
        s->provider,
        request_id,
        CITOQ_REQUEST_OK,
        response,
        sizeof response,
        &accepted) == CITOQ_STATUS_OK);
    assert(accepted == 1);
}

static void completion_cb(
    void* user,
    citoq_request_status_t status,
    const uint8_t* payload,
    size_t payload_size) {
    struct request_state* s =
        (struct request_state*)user;

    ++s->completion_calls;
    s->status = status;
    s->response_size = payload_size;

    if (payload_size > 0u) {
        assert(payload != NULL);
        assert(payload_size <=
               sizeof s->response);
        memcpy(
            s->response,
            payload,
            payload_size);
    }
}

int main(void) {
    citoq_context_t* provider = NULL;
    citoq_context_t* requester = NULL;
    citoq_context_t* isolated = NULL;

    assert(citoq_context_create(
        "acme", 4,
        &provider) == CITOQ_STATUS_OK);
    assert(citoq_context_create(
        "acme", 4,
        &requester) == CITOQ_STATUS_OK);
    assert(citoq_context_create(
        "other", 5,
        &isolated) == CITOQ_STATUS_OK);

    int has_handler = 99;
    assert(citoq_context_has_handler(
        requester,
        500,
        "robot_1", 7,
        &has_handler) ==
        CITOQ_STATUS_OK);
    assert(has_handler == 0);

    struct request_state state = {
        .provider = provider,
        .auto_reply = 1
    };

    citoq_handler_t* handler = NULL;
    assert(citoq_context_handle(
        provider,
        500,
        "robot_1", 7,
        handler_cb,
        &state,
        &handler) ==
        CITOQ_STATUS_OK);

    assert(citoq_context_has_handler(
        requester,
        500,
        "robot_1", 7,
        &has_handler) ==
        CITOQ_STATUS_OK);
    assert(has_handler == 1);

    assert(citoq_context_has_handler(
        isolated,
        500,
        "robot_1", 7,
        &has_handler) ==
        CITOQ_STATUS_OK);
    assert(has_handler == 0);

    const uint8_t request_payload[] = {7};
    citoq_request_t* request = NULL;

    assert(citoq_context_request(
        requester,
        500,
        "robot_1", 7,
        request_payload,
        sizeof request_payload,
        completion_cb,
        &state,
        &request) ==
        CITOQ_STATUS_OK);

    assert(request != NULL);
    assert(state.handler_calls == 1);
    assert(state.completion_calls == 1);
    assert(state.status ==
           CITOQ_REQUEST_OK);
    assert(state.response_size == 1u);
    assert(state.response[0] == 8u);

    int cancelled = 99;
    assert(citoq_request_cancel(
        request,
        &cancelled) ==
        CITOQ_STATUS_OK);
    assert(cancelled == 0);
    citoq_request_destroy(request);

    /* Raw C API still reports Unavailable if the caller skips precheck. */
    state.completion_calls = 0;
    request = NULL;

    assert(citoq_context_request(
        requester,
        500,
        "missing", 7,
        request_payload,
        sizeof request_payload,
        completion_cb,
        &state,
        &request) ==
        CITOQ_STATUS_OK);

    assert(state.completion_calls == 1);
    assert(state.status ==
           CITOQ_REQUEST_UNAVAILABLE);
    citoq_request_destroy(request);

    /* Cancellation completes exactly once and rejects a late reply. */
    struct request_state slow = {
        .provider = provider,
        .auto_reply = 0
    };
    citoq_handler_t* slow_handler = NULL;

    assert(citoq_context_handle(
        provider,
        501,
        "slow", 4,
        handler_cb,
        &slow,
        &slow_handler) ==
        CITOQ_STATUS_OK);

    request = NULL;
    assert(citoq_context_request(
        requester,
        501,
        "slow", 4,
        request_payload,
        sizeof request_payload,
        completion_cb,
        &slow,
        &request) ==
        CITOQ_STATUS_OK);

    assert(slow.handler_calls == 1);
    assert(slow.completion_calls == 0);
    assert(slow.request_id != 0u);

    cancelled = 0;
    assert(citoq_request_cancel(
        request,
        &cancelled) ==
        CITOQ_STATUS_OK);
    assert(cancelled == 1);
    assert(slow.completion_calls == 1);
    assert(slow.status ==
           CITOQ_REQUEST_CANCELLED);

    int accepted = 99;
    const uint8_t late[] = {9};

    assert(citoq_context_reply(
        provider,
        slow.request_id,
        CITOQ_REQUEST_OK,
        late,
        sizeof late,
        &accepted) ==
        CITOQ_STATUS_OK);
    assert(accepted == 0);

    citoq_request_destroy(request);

    citoq_handler_destroy(slow_handler);
    citoq_handler_destroy(handler);

    assert(citoq_context_has_handler(
        requester,
        500,
        "robot_1", 7,
        &has_handler) ==
        CITOQ_STATUS_OK);
    assert(has_handler == 0);

    citoq_context_destroy(isolated);
    citoq_context_destroy(requester);
    citoq_context_destroy(provider);

    return 0;
}
