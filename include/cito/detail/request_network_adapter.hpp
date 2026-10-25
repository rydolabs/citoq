#pragma once

#include <cito/detail/request_transport.hpp>
#include <cito/data_packet.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cito::detail {

using RequestWaitId = std::uint64_t;

struct RequestNetworkStart {
    RequestId request_id{};
    DestinationId destination{};
    bool sent{false};
};

class RequestNetworkAdapter {
public:
    using Clock =
        std::chrono::steady_clock;

    using SessionSend =
        std::function<bool(
            std::span<const std::uint8_t>)>;

    using SendReply =
        RequestTransportCore::SendReply;

    using LazyPayload =
        std::function<
            std::vector<std::uint8_t>()>;

    using DataInterestHook =
        std::function<void(const InterestKey&)>;

    using WaitHook =
        std::function<void(
            const RequestKey&)>;

    RequestNetworkAdapter(
        HostRuntimeCore& host,
        RequestReplyCore& requests)
        : host_(host),
          transport_(
              host,
              requests) {}

    bool attach_session(
        DestinationId destination,
        SessionSend send) {
        if (
            destination == 0 ||
            !send) {
            throw std::invalid_argument(
                "Cito request network: invalid session");
        }

        const auto [it, inserted] =
            sessions_.insert_or_assign(
                destination,
                std::move(send));

        (void)it;
        return inserted;
    }

    bool detach_session(
        DestinationId destination,
        RequestStatus pending_status =
            RequestStatus::Unavailable) {
        const auto removed =
            sessions_.erase(
                destination) != 0;

        transport_.fail_destination(
            destination,
            pending_status);

        return removed;
    }

    // A connected QUIC session transports CTD1 alongside request frames.
    // Invalid and oversized data must never reach the async sender.
    bool send_data(DestinationId destination,
                   std::span<const std::uint8_t> bytes) {
        constexpr std::size_t limit = 8 * 1024 * 1024;
        auto it = sessions_.find(destination);
        if (it == sessions_.end() || bytes.size() > limit) return false;
        try {
            (void)cito::decode_data_packet_view(bytes);
            return it->second(bytes);
        } catch (...) { return false; }
    }

    // Only the transport owner's authenticated QUIC event loop may call this.
    std::size_t receive_data(std::span<const std::uint8_t> bytes) {
        constexpr std::size_t limit = 8 * 1024 * 1024;
        if (bytes.size() > limit) return 0;
        try { return host_.deliver_data_packet(bytes); }
        catch (...) { return 0; }
    }

    bool has_session(
        DestinationId destination)
        const noexcept {
        return
            sessions_.contains(
                destination);
    }

    std::optional<DestinationId>
    ready_destination(
        const RequestKey& key)
        const noexcept {
        const auto capability =
            request_handler_interest_key(
                key);

        const auto& destinations =
            host_.remote_destinations(
                capability);

        std::optional<DestinationId>
            selected;

        for (const auto destination :
             destinations) {
            if (!sessions_.contains(
                    destination)) {
                continue;
            }

            if (
                !selected ||
                destination <
                    *selected) {
                selected =
                    destination;
            }
        }

        return selected;
    }

    bool can_send(
        const RequestKey& key)
        const noexcept {
        return
            ready_destination(
                key)
                .has_value();
    }

    // One registration per publishing Context; shared topics keep a single
    // discovery intent until the final publisher releases it.
    void set_data_interest_hooks(
        DataInterestHook ensure, DataInterestHook release) {
        data_ensure_ = std::move(ensure);
        data_release_ = std::move(release);
    }

    void clear_data_interest_hooks() noexcept {
        data_ensure_ = {};
        data_release_ = {};
        data_intents_.clear();
    }

    bool data_interest_enabled() const noexcept {
        return static_cast<bool>(data_ensure_);
    }

    bool ensure_data_interest(const InterestKey& key) {
        if (!data_ensure_) return false;
        auto [it, added] = data_intents_.try_emplace(key, 0);
        if (added) {
            try {
                data_ensure_(key);
            } catch (...) {
                data_intents_.erase(it);
                throw;
            }
        }
        ++it->second;
        return true;
    }

    void release_data_interest(const InterestKey& key) noexcept {
        const auto it = data_intents_.find(key);
        if (it == data_intents_.end()) return;
        if (--it->second == 0) {
            if (data_release_) {
                try { data_release_(key); } catch (...) {}
            }
            data_intents_.erase(it);
        }
    }

    void set_wait_hook(
        WaitHook hook) {
        wait_hook_ =
            std::move(hook);
    }

    void clear_wait_hook() {
        wait_hook_ = {};
    }

    void discover_request_handler(const RequestKey& key) {
        if (wait_hook_) wait_hook_(key);
    }

    bool waiting_enabled()
        const noexcept {
        return
            static_cast<bool>(
                wait_hook_);
    }

    RequestWaitId defer(
        AppId requester_app,
        const RequestKey& key,
        LazyPayload encode,
        RequestReplyCore::Completion
            completion,
        Clock::time_point deadline) {
        if (requester_app == 0) {
            throw std::invalid_argument(
                "Cito request network: requester app id is zero");
        }

        if (!encode) {
            throw std::invalid_argument(
                "Cito request network: lazy encoder is empty");
        }

        if (!completion) {
            throw std::invalid_argument(
                "Cito request network: completion is empty");
        }

        if (!wait_hook_) {
            completion(
                RequestStatus::
                    Unavailable,
                {});
            return 0;
        }

        const auto id =
            next_wait_id_++;

        waits_.emplace(
            id,
            WaitingRequest{
                requester_app,
                key,
                std::move(encode),
                std::move(completion),
                deadline,
                0,
                false});

        try {
            wait_hook_(key);
        } catch (...) {
            finish_wait(
                id,
                RequestStatus::Internal,
                {});
            return 0;
        }

        return id;
    }

    std::size_t progress(
        Clock::time_point now =
            Clock::now()) {
        std::vector<RequestWaitId>
            ids;

        ids.reserve(
            waits_.size());

        for (const auto& [id, _] :
             waits_) {
            ids.push_back(id);
        }

        std::size_t changed = 0;

        for (const auto id : ids) {
            auto it =
                waits_.find(id);

            if (it == waits_.end()) {
                continue;
            }

            if (now >=
                it->second.deadline) {
                if (expire_wait(id)) {
                    ++changed;
                }
                continue;
            }

            if (it->second.started) {
                continue;
            }

            if (!can_send(
                    it->second.key)) {
                continue;
            }

            std::vector<std::uint8_t>
                payload;

            try {
                payload =
                    it->second.encode();
            } catch (...) {
                finish_wait(
                    id,
                    RequestStatus::Internal,
                    {});
                ++changed;
                continue;
            }

            it =
                waits_.find(id);

            if (it == waits_.end()) {
                continue;
            }

            const auto requester_app =
                it->second.requester_app;
            const auto key =
                it->second.key;

            it->second.started = true;
            it->second.encode = {};

            try {
                const auto started =
                    start(
                        requester_app,
                        key,
                        std::span<
                            const std::uint8_t>(
                            payload.data(),
                            payload.size()),
                        [this, id](
                            RequestStatus status,
                            std::span<
                                const std::uint8_t>
                                response) {
                            finish_wait(
                                id,
                                status,
                                response);
                        });

                it =
                    waits_.find(id);

                if (it != waits_.end()) {
                    it->second.request_id =
                        started.request_id;
                }
            } catch (...) {
                finish_wait(
                    id,
                    RequestStatus::Internal,
                    {});
            }

            ++changed;
        }

        return changed;
    }

    bool cancel_wait(
        RequestWaitId id) {
        const auto it =
            waits_.find(id);

        if (it == waits_.end()) {
            return false;
        }

        if (
            !it->second.started ||
            it->second.request_id == 0) {
            return finish_wait(
                id,
                RequestStatus::Cancelled,
                {});
        }

        const auto request_id =
            it->second.request_id;

        auto packet =
            transport_.cancel(
                request_id);

        if (!packet) {
            return finish_wait(
                id,
                RequestStatus::Cancelled,
                {});
        }

        send_best_effort(
            *packet);

        return true;
    }

    bool expire_wait(
        RequestWaitId id) {
        const auto it =
            waits_.find(id);

        if (it == waits_.end()) {
            return false;
        }

        if (
            !it->second.started ||
            it->second.request_id == 0) {
            return finish_wait(
                id,
                RequestStatus::
                    DeadlineExceeded,
                {});
        }

        const auto request_id =
            it->second.request_id;

        auto packet =
            transport_.expire(
                request_id);

        if (!packet) {
            return finish_wait(
                id,
                RequestStatus::
                    DeadlineExceeded,
                {});
        }

        send_best_effort(
            *packet);

        return true;
    }

    std::size_t waiting_count()
        const noexcept {
        return waits_.size();
    }

    std::optional<Clock::time_point>
    next_deadline()
        const noexcept {
        std::optional<
            Clock::time_point>
            next;

        for (const auto& [_, wait] :
             waits_) {
            if (
                !next ||
                wait.deadline <
                    *next) {
                next = wait.deadline;
            }
        }

        return next;
    }

    std::size_t fail_waiting(
        RequestStatus status =
            RequestStatus::Unavailable) {
        std::vector<RequestWaitId>
            ids;

        ids.reserve(
            waits_.size());

        for (const auto& [id, _] :
             waits_) {
            ids.push_back(id);
        }

        std::size_t failed = 0;

        for (const auto id : ids) {
            const auto it =
                waits_.find(id);

            if (it == waits_.end()) {
                continue;
            }

            if (
                it->second.started &&
                it->second.request_id != 0) {
                const auto request_id =
                    it->second.request_id;

                if (transport_.fail_request(
                        request_id,
                        status)) {
                    ++failed;
                    continue;
                }
            }

            if (finish_wait(
                    id,
                    status,
                    {})) {
                ++failed;
            }
        }

        return failed;
    }

    RequestNetworkStart start(
        AppId requester_app,
        const RequestKey& key,
        std::span<const std::uint8_t>
            payload,
        RequestReplyCore::Completion
            completion) {
        const auto destination =
            ready_destination(
                key);

        if (!destination) {
            if (!completion) {
                throw std::invalid_argument(
                    "Cito request network: completion is empty");
            }

            completion(
                RequestStatus::
                    Unavailable,
                {});
            return {};
        }

        auto packet =
            transport_.start_to(
                requester_app,
                key,
                *destination,
                payload,
                std::move(
                    completion));

        if (!packet) {
            return {};
        }

        const auto request_id =
            packet->request_id;

        const auto session_it =
            sessions_.find(
                *destination);

        if (
            session_it ==
            sessions_.end()) {
            transport_.fail_request(
                request_id);
            return {
                request_id,
                *destination,
                false};
        }

        auto send =
            session_it->second;

        bool sent = false;

        try {
            sent =
                send(packet->bytes);
        } catch (...) {
            sent = false;
        }

        if (!sent) {
            transport_.fail_request(
                request_id);
        }

        return {
            request_id,
            *destination,
            sent};
    }

    std::size_t cancel_requests(
        AppId requester_app) {
        std::vector<RequestWaitId>
            wait_ids;

        for (const auto& [id, wait] :
             waits_) {
            if (
                wait.requester_app ==
                requester_app) {
                wait_ids.push_back(id);
            }
        }

        std::size_t cancelled = 0;

        for (const auto id :
             wait_ids) {
            if (cancel_wait(id)) {
                ++cancelled;
            }
        }

        auto packets =
            transport_.cancel_requests(
                requester_app);

        for (auto& packet : packets) {
            send_best_effort(
                packet);
        }

        return
            cancelled +
            packets.size();
    }

    std::size_t drop_requests(
        AppId requester_app) {
        std::vector<RequestWaitId>
            wait_ids;

        for (const auto& [id, wait] :
             waits_) {
            if (
                wait.requester_app ==
                requester_app) {
                wait_ids.push_back(id);
            }
        }

        for (const auto id :
             wait_ids) {
            waits_.erase(id);
        }

        auto packets =
            transport_.drop_requests(
                requester_app);

        for (auto& packet : packets) {
            send_best_effort(
                packet);
        }

        return
            wait_ids.size() +
            packets.size();
    }

    bool cancel(
        RequestId request_id) {
        auto packet =
            transport_.cancel(
                request_id);

        if (!packet) {
            return false;
        }

        send_best_effort(
            *packet);

        return true;
    }

    bool receive(
        RequestPeerId peer,
        std::span<const std::uint8_t>
            bytes,
        SendReply send_reply = {}) {
        const auto frame =
            decode_request_transport_frame(
                bytes);

        switch (frame.kind) {
            case RequestTransportKind::
                Request:
                return transport_.
                    on_request(
                        peer,
                        bytes,
                        std::move(
                            send_reply));

            case RequestTransportKind::
                Reply:
                return transport_.
                    on_reply(
                        static_cast<
                            DestinationId>(
                            peer),
                        bytes);

            case RequestTransportKind::
                Cancel:
                return transport_.
                    on_cancel(
                        peer,
                        bytes);
        }

        return false;
    }

    std::size_t drop_peer(
        RequestPeerId peer) {
        return transport_.drop_peer(
            peer);
    }

    std::size_t session_count()
        const noexcept {
        return sessions_.size();
    }

    RequestTransportCore& transport()
        noexcept {
        return transport_;
    }

    const RequestTransportCore&
    transport() const noexcept {
        return transport_;
    }

private:
    struct WaitingRequest {
        AppId requester_app{};
        RequestKey key{};
        LazyPayload encode;
        RequestReplyCore::Completion
            completion;
        Clock::time_point deadline{};
        RequestId request_id{};
        bool started{false};
    };

    bool finish_wait(
        RequestWaitId id,
        RequestStatus status,
        std::span<const std::uint8_t>
            response) {
        const auto it =
            waits_.find(id);

        if (it == waits_.end()) {
            return false;
        }

        auto completion =
            std::move(
                it->second.completion);

        waits_.erase(it);

        completion(
            status,
            response);

        return true;
    }

    void send_best_effort(
        const RequestTransportSend&
            packet) {
        const auto session_it =
            sessions_.find(
                packet.destination);

        if (
            session_it ==
            sessions_.end()) {
            return;
        }

        auto send =
            session_it->second;

        try {
            (void)send(
                packet.bytes);
        } catch (...) {
            // Correlation has already completed locally.
        }
    }

    HostRuntimeCore& host_;
    RequestTransportCore transport_;

    std::unordered_map<
        DestinationId,
        SessionSend> sessions_;

    std::unordered_map<
        RequestWaitId,
        WaitingRequest> waits_;

    DataInterestHook data_ensure_;
    DataInterestHook data_release_;
    std::unordered_map<InterestKey, std::size_t, InterestKeyHash>
        data_intents_;

    WaitHook wait_hook_;
    RequestWaitId next_wait_id_{1};
};

} // namespace cito::detail
