// Native Fast DDS 2.x BEST_EFFORT Pub/Sub driver for the M26 Linux netns harness.
// Generated type comes from the same BenchMsg.idl used by CycloneDDS.
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include "BenchMsg.h"
#include "BenchMsgPubSubTypes.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace eprosima::fastdds::dds;
static std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static std::uint64_t percentile95(std::vector<std::uint64_t>& v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[(95 * v.size() + 99) / 100 - 1];
}
static void must(bool condition, const char* what) {
    if (!condition) throw std::runtime_error(what);
}
int main(int argc, char** argv) {
    try {
        if (argc != 8 && argc != 11)
            throw std::runtime_error("args: role topics bytes samples hz unused lifetime_ms [index nodes percent]");
        std::string role = argv[1];
        int ntopics = std::stoi(argv[2]);
        int nbytes = std::stoi(argv[3]);
        int nsamples = std::stoi(argv[4]);
        int hz = std::stoi(argv[5]);
        long lifetime = std::stol(argv[7]);
        int index = argc == 11 ? std::stoi(argv[8]) : (role=="publisher"?0:(role=="negative"?2:1));
        int nodes = argc == 11 ? std::stoi(argv[9]) : 3;
        int percent = argc == 11 ? std::stoi(argv[10]) : 100;
        must(ntopics>=1 && ntopics<=256 && nbytes>=12 && nbytes<=60000
             && nsamples>=1 && nsamples<=10000 && hz>=1 && hz<=2000
             && nodes>=3 && percent>=1 && percent<=100 && index>=0 && index<nodes,
             "invalid workload");
        bool pub = role=="publisher", negative = role=="negative";
        must(pub || role=="subscriber" || negative,"role");
        int nactive = negative?1:(pub?ntopics:(ntopics*percent+99)/100);
        int subscribed = (ntopics*percent+99)/100;
        auto* factory = DomainParticipantFactory::get_instance();
        DomainParticipantQos participantQos;
        participantQos.name(("citoq-fastdds-"+std::to_string(index)).c_str());
        auto* participant = factory->create_participant(0, participantQos);
        must(participant != nullptr, "participant");
        TypeSupport support(new Bench::MsgPubSubType());
        must(support.register_type(participant)==ReturnCode_t::RETCODE_OK, "register type");
        auto* publisher = pub ? participant->create_publisher(PUBLISHER_QOS_DEFAULT,nullptr) : nullptr;
        auto* subscriber = pub ? nullptr : participant->create_subscriber(SUBSCRIBER_QOS_DEFAULT,nullptr);
        must(pub ? publisher != nullptr : subscriber != nullptr, "endpoint");
        DataWriterQos wqos;
        DataReaderQos rqos;
        if (pub) {
            publisher->get_default_datawriter_qos(wqos);
            wqos.reliability().kind=BEST_EFFORT_RELIABILITY_QOS;
            wqos.history().kind=KEEP_LAST_HISTORY_QOS;
            wqos.history().depth=2048;
        } else {
            subscriber->get_default_datareader_qos(rqos);
            rqos.reliability().kind=BEST_EFFORT_RELIABILITY_QOS;
            rqos.history().kind=KEEP_LAST_HISTORY_QOS;
            rqos.history().depth=2048;
        }
        std::vector<DataWriter*> writers;
        std::vector<DataReader*> readers;
        for (int t=0;t<nactive;++t) {
            char name[64];
            std::snprintf(name,sizeof(name),"CitoBench_%06d",negative?999999:t);
            auto* topic = participant->create_topic(name,support.get_type_name(),TOPIC_QOS_DEFAULT);
            must(topic!=nullptr, "topic");
            if (pub) {
                auto* writer=publisher->create_datawriter(topic,wqos);
                must(writer!=nullptr,"writer");
                writers.push_back(writer);
            } else {
                auto* reader=subscriber->create_datareader(topic,rqos);
                must(reader!=nullptr,"reader");
                readers.push_back(reader);
            }
        }
        auto started=now_ns();
        std::uint64_t first=0,sent=0,received=0,bad=0,dup=0;
        bool ready=false;
        std::vector<unsigned char> seen((std::size_t)ntopics*nsamples,0);
        std::vector<std::uint64_t> latencies;
        while ((now_ns()-started)/1000000ull < (std::uint64_t)lifetime) {
            const auto now=now_ns();
            if (pub) {
                if (!ready) {
                    ready=true;
                    for (int t=0;t<subscribed;++t) {
                        PublicationMatchedStatus matched;
                        must(writers[t]->get_publication_matched_status(matched)==ReturnCode_t::RETCODE_OK, "match");
                        if (matched.current_count < nodes-2) {ready=false;break;}
                    }
                    if (ready) {
                        first=now_ns()+500000000ull;
                        std::cout<<"BENCH_DISCOVERY_READY elapsed_ms="<<(now_ns()-started)/1000000ull<<std::endl;
                    }
                }
                if (ready && sent<(std::uint64_t)ntopics*nsamples && now>=first
                    && now-first>=sent*1000000000ull/(std::uint64_t)(hz*ntopics)) {
                    int t=sent%ntopics;
                    Bench::Msg m;
                    m.seq(static_cast<std::uint32_t>(sent/ntopics));
                    m.sent_ns(now_ns());
                    m.payload(std::string(nbytes-12,static_cast<char>(65+t%23)));
                    must(writers[t]->write(&m),"write");
                    ++sent;
                }
                if (ready && sent==(std::uint64_t)ntopics*nsamples
                    && now-first>(std::uint64_t)nsamples*1000000000ull/hz+500000000ull)
                    break;
            } else {
                for (int t=0;t<nactive;++t) {
                    for (;;) {
                        Bench::Msg m;
                        SampleInfo info;
                        if (readers[t]->take_next_sample(&m,&info)!=ReturnCode_t::RETCODE_OK) break;
                        if (!info.valid_data) continue;
                        const auto arrival=now_ns();
                        if (negative || m.seq()>=(std::uint32_t)nsamples
                            || m.payload().size()!=(std::size_t)nbytes-12
                            || m.sent_ns()>arrival
                            || m.payload()!=std::string(nbytes-12,static_cast<char>(65+t%23))) {
                            ++bad;continue;
                        }
                        const auto at=(std::size_t)t*nsamples+m.seq();
                        if (seen[at]++) ++dup;
                        else {
                            ++received;
                            latencies.push_back((arrival-m.sent_ns())/1000);
                        }
                    }
                }
                if (!negative && received==(std::uint64_t)subscribed*nsamples) break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(pub?200:800));
        }
        std::cout<<"{\"role\":\""<<role<<"\",\"topics\":"<<ntopics
                 <<",\"bytes\":"<<nbytes<<",\"samples\":"<<nsamples
                 <<",\"sent\":"<<sent<<",\"received\":"<<received
                 <<",\"bad\":"<<bad<<",\"duplicate\":"<<dup
                 <<",\"p95_us\":"<<percentile95(latencies)
                 <<",\"ready\":"<<(ready?1:0)<<",\"index\":"<<index
                 <<",\"active_topics\":"<<subscribed<<"}"<<std::endl;
        factory->delete_participant(participant);
        if (pub) return ready && sent==(std::uint64_t)ntopics*nsamples?0:3;
        if (negative) return received==0 && bad==0?0:4;
        return received<=(std::uint64_t)subscribed*nsamples && bad==0 && dup==0?0:5;
    } catch(const std::exception& e) {
        std::cerr<<"FASTDDS_DRIVER_ERROR: "<<e.what()<<std::endl;
        return 20;
    }
}
