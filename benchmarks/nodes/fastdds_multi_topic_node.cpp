// Fast DDS 2.x BEST_EFFORT benchmark. Same workload as CitoQ/Cyclone DDS.
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/dds/core/status/PublicationMatchedStatus.hpp>
#include <fastdds/dds/core/status/StatusMask.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.h>
#include "BenchMsg.h"
#include "BenchMsgPubSubTypes.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace eprosima::fastdds::dds;
using clock_type = std::chrono::steady_clock;
static uint64_t ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count();}
static uint64_t p95(std::vector<uint64_t> v){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[(95*v.size()+99)/100-1];}
int main(int argc,char**argv){
 try{
  if(argc!=11)throw std::runtime_error("usage: role topics bytes samples hz unused lifetime_ms index nodes percent");
  std::string role=argv[1];int topics=std::stoi(argv[2]),size=std::stoi(argv[3]),samples=std::stoi(argv[4]),hz=std::stoi(argv[5]);
  int life=std::stoi(argv[7]),index=std::stoi(argv[8]),nodes=std::stoi(argv[9]),pct=std::stoi(argv[10]);
  bool pub=(role=="publisher"),negative=(role=="negative");
  if(topics<1||topics>256||size<12||samples<1||hz<1||nodes<3||pct<1||pct>100)throw std::runtime_error("invalid args");
  int subscribed=std::max(1,(topics*pct+99)/100);
  int active=pub?topics:(negative?1:subscribed);
  uint64_t expected=(uint64_t)subscribed*samples;
  DomainParticipantQos pqos=PARTICIPANT_QOS_DEFAULT;
  // Each netns is a separate virtual node on one Linux host.
  // Disable Fast DDS shared memory so every sample actually traverses veth.
  pqos.transport().use_builtin_transports=false;
  pqos.transport().user_transports.push_back(
      std::make_shared<eprosima::fastdds::rtps::UDPv4TransportDescriptor>());
  DomainParticipant*part=DomainParticipantFactory::get_instance()->create_participant(0,pqos);
  if(!part)throw std::runtime_error("participant");
  TypeSupport type(new Bench::MsgPubSubType());
  type.register_type(part);
  Publisher* publisher=pub?part->create_publisher(PUBLISHER_QOS_DEFAULT):nullptr;
  Subscriber*subscriber=!pub?part->create_subscriber(SUBSCRIBER_QOS_DEFAULT):nullptr;
  if(pub&&!publisher)throw std::runtime_error("publisher");
  if(!pub&&!subscriber)throw std::runtime_error("subscriber");
  DataWriterQos wq=DATAWRITER_QOS_DEFAULT;
  wq.reliability().kind=BEST_EFFORT_RELIABILITY_QOS;
  wq.history().kind=KEEP_LAST_HISTORY_QOS;wq.history().depth=32;
  DataReaderQos rq=DATAREADER_QOS_DEFAULT;
  rq.reliability().kind=BEST_EFFORT_RELIABILITY_QOS;
  rq.history().kind=KEEP_LAST_HISTORY_QOS;rq.history().depth=2048;
  std::vector<DataWriter*>writers;
  std::vector<DataReader*>readers;
  for(int t=0;t<active;++t){
   char name[96];std::snprintf(name,sizeof(name),"CitoQBench_%06d",negative?999999:t);
   Topic*tp=part->create_topic(name,type.get_type_name(),TOPIC_QOS_DEFAULT);
   if(!tp)throw std::runtime_error("topic");
   if(pub){auto*w=publisher->create_datawriter(tp,wq);if(!w)throw std::runtime_error("writer");writers.push_back(w);}
   else{auto*r=subscriber->create_datareader(tp,rq);if(!r)throw std::runtime_error("reader");readers.push_back(r);}
  }
  uint64_t start=ns(),first=0,received=0,bad=0,duplicate=0,sent=0;
  bool ready=false;
  std::vector<uint8_t>seen((size_t)topics*samples,0);
  std::vector<uint64_t>latencies;
  while((ns()-start)/1000000ull<(uint64_t)life){
   if(pub){
    if(!ready){
     ready=true;
     for(int t=0;t<topics;++t){
      PublicationMatchedStatus matched;
      if(writers[t]->get_publication_matched_status(matched)!=ReturnCode_t::RETCODE_OK)throw std::runtime_error("match");
      if(t<subscribed && matched.current_count<nodes-2){ready=false;break;}
     }
     if(ready){first=ns()+500000000ull;std::cout<<"BENCH_DISCOVERY_READY elapsed_ms="<<((ns()-start)/1000000ull)<<std::endl;}
    }
    if(ready&&ns()>=first){
     for(int burst=0;burst<16 && sent<(uint64_t)topics*samples;++burst){
      auto target=first+sent*1000000000ull/(uint64_t)(hz*topics);
      if(ns()<target)break;
      int t=(int)(sent%topics);
      Bench::Msg m;
      m.seq((uint32_t)(sent/topics));
      m.sent_ns(ns());
      m.payload(std::string(size-12,(char)(65+t%23)));
      const auto rc=writers[t]->write(&m);
      if(rc!=ReturnCode_t::RETCODE_OK){
       std::cerr<<"FAST_DDS_WRITE_FAILURE index="<<sent<<" topic="<<t
                <<" timeout="<<(rc==ReturnCode_t::RETCODE_TIMEOUT)
                <<" resources="<<(rc==ReturnCode_t::RETCODE_OUT_OF_RESOURCES)
                <<" precondition="<<(rc==ReturnCode_t::RETCODE_PRECONDITION_NOT_MET)
                <<std::endl;
       throw std::runtime_error("write");
      }
      ++sent;
     }
    }
    if(ready && sent==(uint64_t)topics*samples && ns()>first+(uint64_t)samples*1000000000ull/hz+500000000ull)break;
   }else{
    for(int t=0;t<active;++t){
     for(;;){
      Bench::Msg m;SampleInfo info;
      if(readers[t]->take_next_sample(&m,&info)!=ReturnCode_t::RETCODE_OK)break;
      if(!info.valid_data)continue;
      if(negative||m.seq()>=(uint32_t)samples||m.sent_ns()>ns()||m.payload().size()!=(size_t)(size-12)){
       ++bad;continue;
      }
      bool match=true;
      for(auto ch:m.payload())if(ch!=char(65+t%23)){match=false;break;}
      if(!match){++bad;continue;}
      size_t seq=(size_t)t*samples+m.seq();
      if(seen[seq]++)++duplicate;
      else {++received;latencies.push_back((ns()-m.sent_ns())/1000);}
     }
    }
    if(!negative && received>=expected)break;
   }
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::cout<<"{\"role\":\""<<role<<"\",\"topics\":"<<topics<<",\"bytes\":"<<size<<",\"samples\":"<<samples
           <<",\"sent\":"<<sent<<",\"received\":"<<received<<",\"bad\":"<<bad
           <<",\"duplicate\":"<<duplicate<<",\"p95_us\":"<<p95(latencies)
           <<",\"ready\":"<<(ready?1:0)<<",\"index\":"<<index<<",\"active_topics\":"<<subscribed<<"}"<<std::endl;
  part->delete_contained_entities();
  DomainParticipantFactory::get_instance()->delete_participant(part);
  if(pub)return ready&&sent==(uint64_t)topics*samples?0:3;
  if(negative)return received==0&&bad==0?0:4;
  return received<=expected&&bad==0&&duplicate==0?0:5;
 }catch(const std::exception&e){std::cerr<<"Fast DDS bench error: "<<e.what()<<std::endl;return 20;}
}
