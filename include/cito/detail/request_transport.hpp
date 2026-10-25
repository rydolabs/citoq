#pragma once

#include <cito/detail/request_reply_core.hpp>

#include <algorithm>
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

enum class RequestTransportKind :
    std::uint8_t {
    Request = 1,
    Reply = 2,
    Cancel = 3,
};

struct RequestTransportFrameView {
    RequestTransportKind kind{
        RequestTransportKind::Request};
    RequestStatus status{
        RequestStatus::Ok};
    RequestId request_id{};
    RequestKey key{};
    std::span<const std::uint8_t>
        payload;
};

namespace request_transport_detail {

inline void write_u32(
    std::vector<std::uint8_t>& out,
    std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(
            static_cast<std::uint8_t>(
                (value >> (i * 8)) &
                0xffu));
    }
}

inline void write_u64(
    std::vector<std::uint8_t>& out,
    std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(
            static_cast<std::uint8_t>(
                (value >> (i * 8)) &
                0xffu));
    }
}

inline std::uint32_t read_u32(
    std::span<const std::uint8_t> bytes,
    std::size_t& pos) {
    if (pos + 4 > bytes.size()) {
        throw std::runtime_error(
            "Cito request transport: truncated u32");
    }

    std::uint32_t value = 0;

    for (int i = 0; i < 4; ++i) {
        value |=
            static_cast<std::uint32_t>(
                bytes[pos++]) <<
            (i * 8);
    }

    return value;
}

inline std::uint64_t read_u64(
    std::span<const std::uint8_t> bytes,
    std::size_t& pos) {
    if (pos + 8 > bytes.size()) {
        throw std::runtime_error(
            "Cito request transport: truncated u64");
    }

    std::uint64_t value = 0;

    for (int i = 0; i < 8; ++i) {
        value |=
            static_cast<std::uint64_t>(
                bytes[pos++]) <<
            (i * 8);
    }

    return value;
}

inline bool valid_status(
    RequestStatus status) noexcept {
    switch (status) {
        case RequestStatus::Ok:
        case RequestStatus::Cancelled:
        case RequestStatus::Unavailable:
        case RequestStatus::InvalidArgument:
        case RequestStatus::DeadlineExceeded:
        case RequestStatus::Internal:
            return true;
    }

    return false;
}

inline std::vector<std::uint8_t>
encode(
    RequestTransportKind kind,
    RequestStatus status,
    RequestId request_id,
    const RequestKey& key,
    std::span<const std::uint8_t>
        payload) {
    if (request_id == 0) {
        throw std::invalid_argument(
            "Cito request transport: request id is zero");
    }

    if (!valid_status(status)) {
        throw std::invalid_argument(
            "Cito request transport: invalid status");
    }

    if (
        payload.size() >
        0xffffffffu) {
        throw std::length_error(
            "Cito request transport: payload too large");
    }

    std::vector<std::uint8_t> out;
    out.reserve(
        44 + payload.size());

    out.insert(
        out.end(),
        {'C', 'T', 'X', '1'});

    out.push_back(
        static_cast<std::uint8_t>(
            kind));
    out.push_back(
        static_cast<std::uint8_t>(
            status));

    out.push_back(0);
    out.push_back(0);

    write_u64(
        out,
        request_id);
    write_u64(
        out,
        key.scope);
    write_u64(
        out,
        key.resource);
    write_u64(
        out,
        key.request_type);
    write_u32(
        out,
        static_cast<std::uint32_t>(
            payload.size()));

    out.insert(
        out.end(),
        payload.begin(),
        payload.end());

    return out;
}

} // namespace request_transport_detail

inline std::vector<std::uint8_t>
encode_request_transport_request(
    RequestId request_id,
    const RequestKey& key,
    std::span<const std::uint8_t>
        payload) {
    if (key.request_type == 0) {
        throw std::invalid_argument(
            "Cito request transport: request type is zero");
    }

    return request_transport_detail::encode(
        RequestTransportKind::Request,
        RequestStatus::Ok,
        request_id,
        key,
        payload);
}

inline std::vector<std::uint8_t>
encode_request_transport_reply(
    RequestId request_id,
    RequestStatus status,
    std::span<const std::uint8_t>
        payload) {
    return request_transport_detail::encode(
        RequestTransportKind::Reply,
        status,
        request_id,
        {},
        payload);
}

inline std::vector<std::uint8_t>
encode_request_transport_cancel(
    RequestId request_id) {
    return request_transport_detail::encode(
        RequestTransportKind::Cancel,
        RequestStatus::Cancelled,
        request_id,
        {},
        {});
}

inline RequestTransportFrameView
decode_request_transport_frame(
    std::span<const std::uint8_t>
        bytes) {
    if (
        bytes.size() < 44 ||
        bytes[0] != 'C' ||
        bytes[1] != 'T' ||
        bytes[2] != 'X' ||
        bytes[3] != '1') {
        throw std::runtime_error(
            "Cito request transport: invalid frame");
    }

    const auto kind =
        static_cast<
            RequestTransportKind>(
            bytes[4]);

    if (
        kind !=
            RequestTransportKind::
                Request &&
        kind !=
            RequestTransportKind::
                Reply &&
        kind !=
            RequestTransportKind::
                Cancel) {
        throw std::runtime_error(
            "Cito request transport: invalid frame kind");
    }

    const auto status =
        static_cast<RequestStatus>(
            bytes[5]);

    if (!request_transport_detail::
             valid_status(status)) {
        throw std::runtime_error(
            "Cito request transport: invalid status");
    }

    if (
        bytes[6] != 0 ||
        bytes[7] != 0) {
        throw std::runtime_error(
            "Cito request transport: reserved bits are non-zero");
    }

    std::size_t pos = 8;

    RequestTransportFrameView
        frame;
    frame.kind = kind;
    frame.status = status;
    frame.request_id =
        request_transport_detail::
            read_u64(
                bytes,
                pos);
    frame.key.scope =
        request_transport_detail::
            read_u64(
                bytes,
                pos);
    frame.key.resource =
        request_transport_detail::
            read_u64(
                bytes,
                pos);
    frame.key.request_type =
        request_transport_detail::
            read_u64(
                bytes,
                pos);

    const auto payload_size =
        request_transport_detail::
            read_u32(
                bytes,
                pos);

    if (
        frame.request_id == 0 ||
        pos + payload_size !=
            bytes.size()) {
        throw std::runtime_error(
            "Cito request transport: invalid payload length or id");
    }

    frame.payload =
        bytes.subspan(
            pos,
            payload_size);

    const bool empty_key =
        frame.key.scope == 0 &&
        frame.key.resource == 0 &&
        frame.key.request_type == 0;

    if (
        kind ==
            RequestTransportKind::
                Request) {
        if (
            status !=
                RequestStatus::Ok ||
            frame.key.request_type ==
                0) {
            throw std::runtime_error(
                "Cito request transport: invalid request frame");
        }
    } else if (!empty_key) {
        throw std::runtime_error(
            "Cito request transport: reply/cancel key must be empty");
    }

    if (
        kind ==
            RequestTransportKind::
                Cancel &&
        (status !=
             RequestStatus::
                 Cancelled ||
         !frame.payload.empty())) {
        throw std::runtime_error(
            "Cito request transport: invalid cancel frame");
    }

    return frame;
}

using RequestPeerId = std::uint64_t;

struct RequestTransportSend {
    DestinationId destination{};
    RequestId request_id{};
    std::vector<std::uint8_t> bytes;
};

struct OutboundRequestState {
    DestinationId destination{};
    AppId requester_app{};
};

struct InboundRequestKey {
    RequestPeerId peer{};
    RequestId request_id{};

    bool operator==(
        const InboundRequestKey&) const noexcept =
        default;
};

struct InboundRequestKeyHash {
    std::size_t operator()(
        const InboundRequestKey& key)
        const noexcept {
        auto mix = [](
            std::uint64_t value) {
            value ^= value >> 33;
            value *=
                0xff51afd7ed558ccdULL;
            value ^= value >> 33;
            value *=
                0xc4ceb9fe1a85ec53ULL;
            value ^= value >> 33;
            return value;
        };

        return static_cast<std::size_t>(
            mix(key.peer) ^
            (mix(key.request_id) +
             0x9e3779b97f4a7c15ULL));
    }
};

class RequestTransportCore {
public:
    using Completion =
        RequestReplyCore::Completion;

    using SendReply =
        std::function<bool(
            std::span<const std::uint8_t>)>;

    RequestTransportCore(
        HostRuntimeCore& host,
        RequestReplyCore& requests)
        : host_(host),
          requests_(requests) {}

    std::optional<
        RequestTransportSend>
    start(
        AppId requester_app,
        const RequestKey& key,
        std::span<const std::uint8_t>
            payload,
        Completion completion) {
        const auto capability =
            request_handler_interest_key(
                key);

        const auto& destinations =
            host_.remote_destinations(
                capability);

        if (destinations.empty()) {
            if (!completion) {
                throw std::invalid_argument(
                    "Cito remote request completion is empty");
            }

            completion(
                RequestStatus::
                    Unavailable,
                {});

            return std::nullopt;
        }

        return start_to(
            requester_app,
            key,
            *std::min_element(
                destinations.begin(),
                destinations.end()),
            payload,
            std::move(completion));
    }

    std::optional<
        RequestTransportSend>
    start_to(
        AppId requester_app,
        const RequestKey& key,
        DestinationId destination,
        std::span<const std::uint8_t>
            payload,
        Completion completion) {
        if (!completion) {
            throw std::invalid_argument(
                "Cito remote request completion is empty");
        }

        const auto capability =
            request_handler_interest_key(
                key);

        if (
            destination == 0 ||
            !host_.remote_destinations(
                capability)
                 .contains(
                     destination)) {
            completion(
                RequestStatus::
                    Unavailable,
                {});
            return std::nullopt;
        }

        const auto started =
            requests_.start_remote(
                requester_app,
                std::move(
                    completion));

        try {
            auto bytes =
                encode_request_transport_request(
                    started.id,
                    key,
                    payload);

            outbound_.emplace(
                started.id,
                OutboundRequestState{
                    destination,
                    requester_app});

            return RequestTransportSend{
                destination,
                started.id,
                std::move(bytes)};
        } catch (...) {
            requests_.complete_remote(
                started.id,
                RequestStatus::Internal,
                {});
            throw;
        }
    }

    bool on_request(
        RequestPeerId peer,
        std::span<const std::uint8_t>
            bytes,
        SendReply send_reply) {
        const auto frame =
            decode_request_transport_frame(
                bytes);

        if (
            frame.kind !=
            RequestTransportKind::
                Request) {
            return false;
        }

        if (!send_reply) {
            throw std::invalid_argument(
                "Cito remote reply sender is empty");
        }

        const InboundRequestKey
            inbound_key{
                peer,
                frame.request_id};

        const auto [inbound_it, inserted] =
            inbound_.emplace(
                inbound_key,
                RequestId{0});

        if (!inserted) {
            const auto reply =
                encode_request_transport_reply(
                    frame.request_id,
                    RequestStatus::
                        InvalidArgument,
                    {});

            return send_reply(
                reply);
        }

        auto reply_once =
            [this,
             inbound_key,
             wire_request_id =
                 frame.request_id,
             send_reply =
                 std::move(send_reply)](
                RequestStatus status,
                std::span<
                    const std::uint8_t>
                    payload) mutable {
                if (
                    inbound_.erase(
                        inbound_key) == 0) {
                    return false;
                }

                const auto reply =
                    encode_request_transport_reply(
                        wire_request_id,
                        status,
                        payload);

                try {
                    return send_reply(
                        reply);
                } catch (...) {
                    return false;
                }
            };

        const auto dispatched =
            requests_.dispatch_external(
                frame.key,
                frame.request_id,
                frame.payload,
                std::move(
                    reply_once));

        if (dispatched.dispatched) {
            const auto current =
                inbound_.find(
                    inbound_key);

            if (current !=
                inbound_.end()) {
                current->second =
                    dispatched
                        .handler_request_id;
            }
        } else {
            // Missing-handler dispatch replies synchronously and normally
            // erases the inbound key through reply_once. Be defensive if a
            // custom sender rejected the reply.
            inbound_.erase(
                inbound_key);
        }

        return dispatched.dispatched;
    }

    bool on_reply(
        DestinationId peer,
        std::span<const std::uint8_t>
            bytes) {
        const auto frame =
            decode_request_transport_frame(
                bytes);

        if (
            frame.kind !=
            RequestTransportKind::
                Reply) {
            return false;
        }

        const auto it =
            outbound_.find(
                frame.request_id);

        if (
            it == outbound_.end() ||
            it->second.destination !=
                peer) {
            return false;
        }

        outbound_.erase(it);

        return requests_.
            complete_remote(
                frame.request_id,
                frame.status,
                frame.payload);
    }

    bool on_cancel(
        RequestPeerId peer,
        std::span<const std::uint8_t>
            bytes) {
        const auto frame =
            decode_request_transport_frame(
                bytes);

        if (
            frame.kind !=
            RequestTransportKind::
                Cancel) {
            return false;
        }

        const InboundRequestKey key{
            peer,
            frame.request_id};

        const auto it =
            inbound_.find(key);

        if (it == inbound_.end()) {
            return false;
        }

        const auto handler_request_id =
            it->second;

        inbound_.erase(it);

        if (handler_request_id != 0) {
            requests_.abandon_external(
                handler_request_id);
        }

        return true;
    }

    std::optional<
        RequestTransportSend>
    cancel(
        RequestId request_id) {
        return terminate(
            request_id,
            RequestStatus::Cancelled);
    }

    std::optional<
        RequestTransportSend>
    expire(
        RequestId request_id) {
        return terminate(
            request_id,
            RequestStatus::
                DeadlineExceeded);
    }

    std::vector<
        RequestTransportSend>
    cancel_requests(
        AppId requester_app) {
        std::vector<RequestId> ids;

        for (const auto& [id, state] :
             outbound_) {
            if (
                state.requester_app ==
                requester_app) {
                ids.push_back(id);
            }
        }

        std::vector<
            RequestTransportSend>
            sends;
        sends.reserve(ids.size());

        for (const auto id : ids) {
            auto send =
                cancel(id);

            if (send) {
                sends.push_back(
                    std::move(
                        *send));
            }
        }

        return sends;
    }

    std::vector<
        RequestTransportSend>
    drop_requests(
        AppId requester_app) {
        std::vector<RequestId> ids;

        for (const auto& [id, state] :
             outbound_) {
            if (
                state.requester_app ==
                requester_app) {
                ids.push_back(id);
            }
        }

        std::vector<
            RequestTransportSend>
            sends;
        sends.reserve(ids.size());

        for (const auto id : ids) {
            const auto it =
                outbound_.find(id);

            if (it == outbound_.end()) {
                continue;
            }

            const auto destination =
                it->second.destination;

            outbound_.erase(it);
            requests_.abandon(id);

            sends.push_back(
                RequestTransportSend{
                    destination,
                    id,
                    encode_request_transport_cancel(
                        id)});
        }

        return sends;
    }

    bool fail_request(
        RequestId request_id,
        RequestStatus status =
            RequestStatus::
                Unavailable) {
        const auto it =
            outbound_.find(
                request_id);

        if (it == outbound_.end()) {
            return false;
        }

        outbound_.erase(it);

        return requests_.
            complete_remote(
                request_id,
                status,
                {});
    }

    std::size_t fail_destination(
        DestinationId destination,
        RequestStatus status =
            RequestStatus::
                Unavailable) {
        std::vector<RequestId> ids;

        for (const auto& [id, state] :
             outbound_) {
            if (
                state.destination ==
                destination) {
                ids.push_back(id);
            }
        }

        for (const auto id : ids) {
            outbound_.erase(id);
            requests_.complete_remote(
                id,
                status,
                {});
        }

        return ids.size();
    }

    std::size_t drop_peer(
        RequestPeerId peer) {
        std::vector<RequestId>
            handler_request_ids;

        for (auto it =
                 inbound_.begin();
             it != inbound_.end();) {
            if (it->first.peer != peer) {
                ++it;
                continue;
            }

            if (it->second != 0) {
                handler_request_ids
                    .push_back(
                        it->second);
            }

            it = inbound_.erase(it);
        }

        for (const auto id :
             handler_request_ids) {
            requests_.abandon_external(
                id);
        }

        return
            handler_request_ids.size();
    }

    std::size_t outbound_count()
        const noexcept {
        return outbound_.size();
    }

    std::size_t inbound_count()
        const noexcept {
        return inbound_.size();
    }

private:
    std::optional<
        RequestTransportSend>
    terminate(
        RequestId request_id,
        RequestStatus status) {
        if (
            status !=
                RequestStatus::
                    Cancelled &&
            status !=
                RequestStatus::
                    DeadlineExceeded) {
            throw std::invalid_argument(
                "Cito request transport: invalid terminal status");
        }

        const auto it =
            outbound_.find(
                request_id);

        if (it == outbound_.end()) {
            return std::nullopt;
        }

        const auto destination =
            it->second.destination;

        outbound_.erase(it);

        auto bytes =
            encode_request_transport_cancel(
                request_id);

        if (!requests_.complete_remote(
                request_id,
                status,
                {})) {
            return std::nullopt;
        }

        return RequestTransportSend{
            destination,
            request_id,
            std::move(bytes)};
    }

    HostRuntimeCore& host_;
    RequestReplyCore& requests_;

    std::unordered_map<
        RequestId,
        OutboundRequestState>
        outbound_;

    std::unordered_map<
        InboundRequestKey,
        RequestId,
        InboundRequestKeyHash>
        inbound_;
};

} // namespace cito::detail
