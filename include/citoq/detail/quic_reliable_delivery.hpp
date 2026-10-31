#pragma once
// Experimental native-QUIC DATA ACK protocol, intentionally independent of
// CTD1 best-effort and of the existing (not-yet-integrated) CRD1/NACK model.
// All state is owned by one CitoQ event loop. Sender epoch is random at startup;
// the receiver deduplicates within a bounded retry horizon, NOT persistent
// exactly-once delivery across a receiver process restart.
#include <cstdint>
#include <cstddef>
#include <deque>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <limits>

namespace citoq::detail::quic_reliable {

inline void append_u64(std::vector<std::uint8_t>& out, std::uint64_t n) {
    for (int i=0;i<8;++i) out.push_back(static_cast<std::uint8_t>(n>>(8*i)));
}
inline void append_u32(std::vector<std::uint8_t>& out, std::uint32_t n) {
    for (int i=0;i<4;++i) out.push_back(static_cast<std::uint8_t>(n>>(8*i)));
}
inline std::uint64_t read_u64(std::span<const std::uint8_t> data, std::size_t pos) {
    if (pos > data.size() || data.size()-pos < 8) throw std::runtime_error("truncated reliable u64");
    std::uint64_t value=0;
    for(int i=0;i<8;++i) value |= std::uint64_t(data[pos+i])<<(8*i);
    return value;
}
inline std::uint32_t read_u32(std::span<const std::uint8_t> data, std::size_t pos) {
    if (pos > data.size() || data.size()-pos < 4) throw std::runtime_error("truncated reliable u32");
    std::uint32_t value=0;
    for(int i=0;i<4;++i) value |= std::uint32_t(data[pos+i])<<(8*i);
    return value;
}
inline bool magic(std::span<const std::uint8_t> bytes, char kind) {
    return bytes.size() >= 4 && bytes[0]=='C' && bytes[1]=='T' &&
        bytes[2]==static_cast<std::uint8_t>(kind) && bytes[3]=='1';
}
struct DataView {
    std::uint64_t epoch{};
    std::uint64_t sequence{};
    std::span<const std::uint8_t> original_ctd1{};
};
struct Ack {
    std::uint64_t epoch{};
    std::uint64_t sequence{};
};
inline std::vector<std::uint8_t> encode_data(
    std::uint64_t epoch, std::uint64_t sequence,
    std::span<const std::uint8_t> original) {
    if (!epoch || !sequence || !magic(original,'D') ||
        original.size() > 8u*1024u*1024u)
        throw std::invalid_argument("invalid reliable QUIC DATA");
    std::vector<std::uint8_t> out={'C','T','Y','1'};
    out.reserve(24+original.size());
    append_u64(out,epoch); append_u64(out,sequence);
    append_u32(out,static_cast<std::uint32_t>(original.size()));
    out.insert(out.end(),original.begin(),original.end());
    return out;
}
inline DataView decode_data(std::span<const std::uint8_t> bytes) {
    if (!magic(bytes,'Y') || bytes.size()<24) throw std::runtime_error("invalid reliable DATA magic");
    DataView out{read_u64(bytes,4),read_u64(bytes,12),{}};
    const auto size=read_u32(bytes,20);
    if (!out.epoch || !out.sequence || size!=bytes.size()-24 ||
        size>8u*1024u*1024u || !magic(bytes.subspan(24),'D'))
        throw std::runtime_error("invalid reliable DATA body");
    out.original_ctd1=bytes.subspan(24);
    return out;
}
inline std::vector<std::uint8_t> encode_ack(Ack ack) {
    if (!ack.epoch || !ack.sequence) throw std::invalid_argument("invalid ACK");
    std::vector<std::uint8_t> out={'C','T','K','1'};
    append_u64(out,ack.epoch); append_u64(out,ack.sequence);
    return out;
}
inline Ack decode_ack(std::span<const std::uint8_t> bytes) {
    if (!magic(bytes,'K') || bytes.size()!=20)
        throw std::runtime_error("invalid reliable ACK");
    Ack ack{read_u64(bytes,4),read_u64(bytes,12)};
    if (!ack.epoch || !ack.sequence) throw std::runtime_error("zero ACK id");
    return ack;
}
// Bounded receiver deduplication. Mark only AFTER application dispatch succeeds.
// Monotonic data epochs from a sender process are scoped by a random epoch.
class Dedup {
public:
    bool contains(std::uint64_t epoch, std::uint64_t seq) const {
        const auto it=epochs_.find(epoch);
        return it!=epochs_.end() && it->second.seen.contains(seq);
    }
    void commit(std::uint64_t epoch, std::uint64_t seq) {
        if (!epoch || !seq) throw std::invalid_argument("invalid delivered sequence");
        auto it=epochs_.find(epoch);
        if(it==epochs_.end()) {
            if (epochs_.size()==128) {
                epochs_.erase(order_.front()); order_.pop_front();
            }
            order_.push_back(epoch);
            it=epochs_.emplace(epoch,History{}).first;
        }
        auto& h=it->second;
        if(!h.seen.insert(seq).second) return;
        h.order.push_back(seq);
        if(h.order.size()>4096) {
            h.seen.erase(h.order.front()); h.order.pop_front();
        }
    }
private:
    struct History {
        std::unordered_set<std::uint64_t> seen;
        std::deque<std::uint64_t> order;
    };
    std::unordered_map<std::uint64_t,History> epochs_;
    std::deque<std::uint64_t> order_;
};
} // namespace citoq::detail::quic_reliable
