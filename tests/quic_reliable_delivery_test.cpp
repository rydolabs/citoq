#include <citoq/detail/quic_reliable_delivery.hpp>
#include <array>
#include <cassert>
#include <stdexcept>
#include <vector>
int main() {
    namespace q = citoq::detail::quic_reliable;
    const std::array<std::uint8_t,6> ctd1{{'C','T','D','1',1,2}};
    const auto frame = q::encode_data(123,456,ctd1);
    assert(frame.size()==ctd1.size()+24);
    auto data=q::decode_data(frame);
    assert(data.epoch==123 && data.sequence==456);
    assert(data.original_ctd1.size()==ctd1.size());
    const auto reply=q::encode_ack({data.epoch,data.sequence});
    auto ack=q::decode_ack(reply);
    assert(ack.epoch==123 && ack.sequence==456);
    q::Dedup receiver;
    assert(!receiver.contains(123,456));
    receiver.commit(123,456);
    assert(receiver.contains(123,456));
    receiver.commit(123,456);
    assert(receiver.contains(123,456));
    assert(!receiver.contains(124,456));
    assert(!receiver.contains(123,457));
    try {
        auto bad=frame; bad[23]=0xff;
        (void)q::decode_data(bad);
        assert(false);
    } catch (const std::runtime_error&) {}
    try {
        auto bad=reply; bad.push_back(1);
        (void)q::decode_ack(bad);
        assert(false);
    } catch (const std::runtime_error&) {}
    for (std::uint64_t n=1;n<=4500;++n) receiver.commit(555,n);
    assert(receiver.contains(555,4500));
    assert(!receiver.contains(555,1));
    for (std::uint64_t e=1000;e<1140;++e) receiver.commit(e,1);
    assert(receiver.contains(1139,1));
    return 0;
}
