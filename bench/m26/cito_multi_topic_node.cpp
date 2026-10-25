#include <citoq/detail/udp_runtime_adapter.hpp>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using Clock=std::chrono::steady_clock;
static uint64_t nsecs(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
static int sock(uint16_t port, bool broadcast=false){int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)throw std::runtime_error("socket");int one=1;setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));if(broadcast)setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&one,sizeof(one));sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=INADDR_ANY;if(bind(fd,(sockaddr*)&a,sizeof(a))<0)throw std::runtime_error("bind");return fd;}
static citoq::detail::UdpEndpoint ep(sockaddr_in a){return {ntohl(a.sin_addr.s_addr),ntohs(a.sin_port)};}
static bool send(int fd,citoq::detail::UdpEndpoint to,std::span<const uint8_t> bytes){sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(to.port);a.sin_addr.s_addr=htonl(to.ipv4);return sendto(fd,bytes.data(),bytes.size(),0,(sockaddr*)&a,sizeof(a))==(ssize_t)bytes.size();}
static void sends(int fd,const std::vector<citoq::detail::UdpSend>& actions){for(const auto& a:actions)if(!send(fd,a.endpoint,a.bytes))throw std::runtime_error("send_action");}
static std::vector<uint8_t> recvmsg(int fd,citoq::detail::UdpEndpoint* from=nullptr){std::vector<uint8_t> bytes(65535);sockaddr_in source{};socklen_t len=sizeof(source);auto n=recvfrom(fd,bytes.data(),bytes.size(),0,(sockaddr*)&source,&len);if(n<=0)return {};bytes.resize(n);if(from)*from=ep(source);return bytes;}
static citoq::InterestKey key(int i){return {7,17,uint64_t(1000+i)};}
static void put32(std::vector<uint8_t>& b,uint32_t x){for(int j=0;j<4;++j)b[j]=(x>>(8*j))&255;}
static void put64(std::vector<uint8_t>& b,uint64_t x){for(int j=0;j<8;++j)b[4+j]=(x>>(8*j))&255;}
static uint64_t get64(std::span<const uint8_t> b){uint64_t v=0;for(int j=0;j<8;++j)v|=uint64_t(b[4+j])<<(8*j);return v;}
static uint32_t get32(std::span<const uint8_t> b){uint32_t v=0;for(int j=0;j<4;++j)v|=uint32_t(b[j])<<(8*j);return v;}
static uint64_t p95(std::vector<uint64_t> v){if(v.empty())return 0;std::sort(v.begin(),v.end());return v[(95*v.size()+99)/100-1];}
int main(int argc,char** argv){
try{
// role topics payload_size samples hz announce_ip lifetime_ms
if(argc!=8 && argc!=11)throw std::runtime_error("args: role topics bytes samples hz announce_ip lifetime_ms [index nodes percent]");
const auto benchmark_start=Clock::now();
std::string role=argv[1];int topics=std::stoi(argv[2]),size=std::stoi(argv[3]),samples=std::stoi(argv[4]),hz=std::stoi(argv[5]),lifetime=std::stoi(argv[7]);
if(topics<1||topics>256||size<12||size>60000||samples<1||samples>10000||hz<1||hz>2000)throw std::runtime_error("bounds");
int index=argc==11?std::stoi(argv[8]):(role=="publisher"?0:(role=="subscriber"?1:2));
int nodes=argc==11?std::stoi(argv[9]):3;
int pct=argc==11?std::stoi(argv[10]):100;
if(nodes<3||nodes>120||index<0||index>=nodes||pct<1||pct>100)throw std::runtime_error("layout");
int active=std::max(1,(topics*pct+99)/100);
int node=100+index;int ctl=47000+node,dat=48000+node;
uint64_t expected_peer_count=uint64_t(nodes-2);
uint64_t expected_received=uint64_t(active)*samples;
citoq::detail::HostRuntimeCore host;
std::vector<std::vector<uint8_t>> seen(topics,std::vector<uint8_t>(samples,0));
uint64_t received=0,bad=0,dup=0,data_datagrams=0;std::vector<uint64_t> latencies;
if(role!="publisher"){
 host.register_app(node,[&](auto type,std::string_view resource,std::span<const uint8_t> payload)->size_t{
  int idx=int(type)-1000;
  if(role=="negative"||idx<0||idx>=active||resource!="bench"||payload.size()!=size){++bad;return 0;}
  uint32_t seq=get32(payload);
  if(seq>=uint32_t(samples)||get64(payload)>nsecs()){++bad;return 0;}
  for(int j=12;j<size;++j)if(payload[j]!=uint8_t(65+idx%23)){++bad;return 0;}
  if(seen[idx][seq]++)++dup;else{++received;latencies.push_back((nsecs()-get64(payload))/1000);}
  return 1;
 });
 for(int i=0;i<active;++i)if(role=="subscriber")host.add_interest(node,key(i));
 if(role=="negative")host.add_interest(node,{7,17,999999});
}
citoq::detail::UdpRuntimeAdapter udp(host,node,node*1000+1);
std::array<citoq::RouteEndpoint,1> routes{{{uint64_t(node),uint16_t(dat)}}};udp.set_local_route_endpoints(routes);
if(role=="publisher")for(int i=0;i<topics;++i)udp.ensure_routes(key(i));
int fd=sock(ctl,role!="publisher"),data=-1;if(role!="publisher")data=sock(dat);
in_addr ip{};if(inet_pton(AF_INET,argv[6],&ip)!=1)throw std::runtime_error("ip");
citoq::detail::UdpEndpoint announce{ntohl(ip.s_addr),47100};auto announcement=udp.announcement(ctl,1500);
auto started=benchmark_start,next=Clock::now();bool ready=false;uint64_t sent=0;auto last=started;
while(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count()<lifetime){
 auto now=Clock::now();
 if(role!="publisher"&&now>=next){if(!send(fd,announce,announcement))throw std::runtime_error("announce");next=now+std::chrono::milliseconds(150);}
 pollfd fds[2]{{fd,POLLIN,0},{data,POLLIN,0}};int n=poll(fds,data<0?1:2,8);if(n<0)throw std::runtime_error("poll");
 if(fds[0].revents&POLLIN){citoq::detail::UdpEndpoint source{};auto b=recvmsg(fd,&source);if(!b.empty())sends(fd,udp.process_control(b,source,nsecs()/1000000));}
 if(data>=0&&(fds[1].revents&POLLIN)){auto b=recvmsg(data);if(!b.empty()){++data_datagrams;udp.receive_data(b,"bench");}}
 if(role=="publisher"){
  if(!ready){ready=true;for(int i=0;i<topics;++i)if(host.remote_destinations(key(i)).size()!=(i<active?expected_peer_count:0)){ready=false;break;}if(ready){last=Clock::now()+std::chrono::milliseconds(500);std::cout<<"BENCH_DISCOVERY_READY elapsed_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count()<<std::endl;}}
  if(ready&&sent<uint64_t(topics)*samples){for(int burst=0;burst<32&&sent<uint64_t(topics)*samples;++burst){auto seq=sent/topics;int t=sent%topics;auto when=last+std::chrono::microseconds(uint64_t(sent)*1000000/(uint64_t(hz)*topics));
   if(Clock::now()>=when){std::vector<uint8_t> payload(size,uint8_t(65+t%23));put32(payload,seq);put64(payload,nsecs());auto result=udp.publish(key(t),"bench",payload,[&](auto to,auto packet){return send(fd,to,packet);});if(result.remote_sent!=(t<active?expected_peer_count:0))throw std::runtime_error("lost route");++sent;}else break;}}
  if(ready&&sent==uint64_t(topics)*samples&&Clock::now()-last>std::chrono::milliseconds((1000ull*samples/hz)+500))break;
 }
 if(role=="subscriber"&&received==expected_received&&Clock::now()-started>std::chrono::milliseconds(300))break;
 udp.expire(nsecs()/1000000);
}
std::cout<<"{\"role\":\""<<role<<"\",\"topics\":"<<topics<<",\"bytes\":"<<size<<",\"samples\":"<<samples<<",\"sent\":"<<sent<<",\"received\":"<<received<<",\"bad\":"<<bad<<",\"duplicate\":"<<dup<<",\"p95_us\":"<<p95(latencies)<<",\"ready\":"<<(ready?1:0)<<",\"index\":"<<index<<",\"active_topics\":"<<active<<",\"data_datagrams\":"<<data_datagrams<<"}\n";
close(fd);if(data>=0)close(data);
if(role=="publisher")return ready&&sent==uint64_t(topics)*samples?0:3;
if(role=="negative")return received==0&&bad==0&&data_datagrams==0?0:4;
return (bad==0&&dup==0&&received<=expected_received)?0:5;
}catch(const std::exception& e){std::cerr<<"bench_exception: "<<e.what()<<'\n';return 20;}
}
