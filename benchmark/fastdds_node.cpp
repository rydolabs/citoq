// CitoQ comparative benchmark: Fast DDS 2.11 RELIABLE, UDPv4-only (SHM disabled).
// Uses Bench::Msg generated from bench/m26/BenchMsg.idl.
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.h>
#include "BenchMsg.h"
#include "BenchMsgPubSubTypes.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace eprosima::fastdds::dds;
using Clock=std::chrono::steady_clock;
static std::uint64_t mono_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
static void ensure(bool ok,const char* msg) {if(!ok) throw std::runtime_error(msg);}
static double percentile(std::vector<std::uint64_t> v,double p) {
  if(v.empty()) return -1;
  std::sort(v.begin(),v.end());
  return (double)v[std::min(v.size()-1,(std::size_t)((v.size()-1)*p+0.5))]/1000.;
}
int main(int argc,char**argv) {
  try {
    ensure(argc==6,"args: role count bytes rate output.json");
    const std::string role=argv[1], output=argv[5];
    const int count=std::stoi(argv[2]), payload_size=std::stoi(argv[3]);
    const double hz=std::stod(argv[4]);
    ensure((role=="sender"||role=="receiver")&&count>0&&count<=100000&&payload_size>=16&&payload_size<=60000&&hz>0,"invalid args");
    auto* factory=DomainParticipantFactory::get_instance();
    DomainParticipantQos pqos;
    factory->get_default_participant_qos(pqos);
    pqos.transport().use_builtin_transports=false; // disallow SHM bypassing veth / netem
    pqos.transport().user_transports.push_back(std::make_shared<eprosima::fastdds::rtps::UDPv4TransportDescriptor>());
    auto* dp=factory->create_participant(22,pqos);
    ensure(dp!=nullptr,"participant");
    TypeSupport type(new Bench::MsgPubSubType());
    ensure(type.register_type(dp)==ReturnCode_t::RETCODE_OK,"type");
    auto* topic=dp->create_topic("CitoQBenchmarkFastDDS",type.get_type_name(),TOPIC_QOS_DEFAULT);
    ensure(topic!=nullptr,"topic");
    DataWriter* writer=nullptr;
    DataReader* reader=nullptr;
    if(role=="sender") {
      auto* publisher=dp->create_publisher(PUBLISHER_QOS_DEFAULT,nullptr);
      ensure(publisher!=nullptr,"publisher");
      DataWriterQos qos;publisher->get_default_datawriter_qos(qos);
      qos.reliability().kind=RELIABLE_RELIABILITY_QOS;
      qos.history().kind=KEEP_LAST_HISTORY_QOS;
      qos.history().depth=1024;
      writer=publisher->create_datawriter(topic,qos);
      ensure(writer!=nullptr,"writer");
    } else {
      auto* subscriber=dp->create_subscriber(SUBSCRIBER_QOS_DEFAULT,nullptr);
      ensure(subscriber!=nullptr,"subscriber");
      DataReaderQos qos;subscriber->get_default_datareader_qos(qos);
      qos.reliability().kind=RELIABLE_RELIABILITY_QOS;
      qos.history().kind=KEEP_LAST_HISTORY_QOS;
      qos.history().depth=1024;
      reader=subscriber->create_datareader(topic,qos);
      ensure(reader!=nullptr,"reader");
    }
    const auto deadline=Clock::now()+std::chrono::seconds(18);
    if(writer) {
      PublicationMatchedStatus status;
      while(Clock::now()<deadline) {
        ensure(writer->get_publication_matched_status(status)==ReturnCode_t::RETCODE_OK,"match");
        if(status.current_count>0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      ensure(status.current_count>0,"matched subscriber unavailable");
      auto base=Clock::now()+std::chrono::milliseconds(100);
      for(int i=1;i<=count;i++) {
        std::this_thread::sleep_until(base+std::chrono::nanoseconds((long long)((i-1)*1e9/hz)));
        Bench::Msg msg;
        msg.seq((std::uint32_t)i);
        msg.sent_ns(mono_ns());
        msg.payload(std::string(payload_size-12,'B'));
        ensure(writer->write(&msg)==ReturnCode_t::RETCODE_OK,"write");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(700));
      std::cout<<"SENDER_OK engine=fastdds sent="<<count<<std::endl;
      factory->delete_participant(dp);
      return 0;
    }
    std::vector<unsigned char> seen((std::size_t)count+1,0);
    std::vector<std::uint64_t> lat;
    int bad=0,duplicates=0;
    std::uint64_t first=0,last=0;
    while(Clock::now()<deadline && (int)lat.size()<count) {
      for(;;) {
        Bench::Msg msg;SampleInfo info;
        if(reader->take_next_sample(&msg,&info)!=ReturnCode_t::RETCODE_OK) break;
        if(!info.valid_data) continue;
        const auto received=mono_ns();
        const auto seq=msg.seq();
        if(seq==0||seq>(std::uint32_t)count||msg.payload().size()!=(std::size_t)payload_size-12||msg.sent_ns()>received||msg.sent_ns()==0) {
          bad++;continue;
        }
        if(seen[seq]++) {duplicates++;continue;}
        lat.push_back(received-msg.sent_ns());
        if(!first)first=received;
        last=received;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    const bool complete=(int)lat.size()==count&&!duplicates&&!bad;
    std::ofstream o(output);
    ensure((bool)o,"output file");
    o<<"{"
      <<"\"engine\":\"fastdds\","
      <<"\"expected\":"<<count<<","
      <<"\"received_unique\":"<<lat.size()<<","
      <<"\"duplicates\":"<<duplicates<<","
      <<"\"invalid\":"<<bad<<","
      <<"\"missing\":"<<(count-(int)lat.size())<<","
      <<"\"payload_bytes\":"<<payload_size<<","
      <<"\"latency_us_p50\":"<<percentile(lat,.50)<<","
      <<"\"latency_us_p95\":"<<percentile(lat,.95)<<","
      <<"\"latency_us_p99\":"<<percentile(lat,.99)<<","
      <<"\"delivery_window_ms\":"<<((last>=first)?(last-first)/1000000.:0.)<<","
      <<"\"complete\":"<<(complete?"true":"false")<<"}\n";
    std::cout<<"FAST_DDS_COMPLETE="<<complete<<" received="<<lat.size()<<"/"<<count<<std::endl;
    factory->delete_participant(dp);
    return 0;
  } catch(const std::exception& e) {
    std::cerr<<"FAST_DDS_DRIVER_ERROR "<<e.what()<<std::endl;
    return 2;
  }
}
