#include <ddsc/dds.h>
#include "BenchMsg.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static void fail(const char*s,dds_return_t code){fprintf(stderr,"dds %s: %d\n",s,code);exit(20);}
static int cmp(const void*a,const void*b){uint64_t x=*(const uint64_t*)a,y=*(const uint64_t*)b;return (x>y)-(x<y);}
int main(int argc,char**argv){
 if(argc!=8&&argc!=11){fprintf(stderr,"args: role topics bytes samples hz unused lifetime_ms [index nodes pct]\n");return 2;}
 const char* role=argv[1];int topics=atoi(argv[2]),size=atoi(argv[3]),samples=atoi(argv[4]),hz=atoi(argv[5]);long life=atol(argv[7]);
 if(topics<1||topics>256||size<12||size>60000||samples<1||samples>10000||hz<1||hz>2000)return 2;
 bool pub=!strcmp(role,"publisher"),negative=!strcmp(role,"negative");
 int index=argc==11?atoi(argv[8]):(pub?0:(negative?2:1));
 int nodes=argc==11?atoi(argv[9]):3, pct=argc==11?atoi(argv[10]):100;
 if(nodes<3||nodes>120||index<0||index>=nodes||pct<1||pct>100)return 2;
 int active=negative?1:(pub?topics:(topics*pct+99)/100);
 int subscribed=(topics*pct+99)/100;
 uint64_t expected_peer_count=nodes-2, expected_received=(uint64_t)subscribed*samples;
 dds_entity_t p=dds_create_participant(DDS_DOMAIN_DEFAULT,NULL,NULL);if(p<0)fail("participant",p);
 dds_qos_t*qos=dds_create_qos();dds_qset_reliability(qos,DDS_RELIABILITY_BEST_EFFORT,DDS_MSECS(100));dds_qset_history(qos,DDS_HISTORY_KEEP_LAST,2048);
 dds_entity_t*ends=calloc(active,sizeof(*ends));
 for(int t=0;t<active;++t){char name[60];snprintf(name,sizeof(name),"CitoBench_%06d",negative?999999:t);dds_entity_t topic=dds_create_topic(p,&Bench_Msg_desc,name,NULL,NULL);if(topic<0)fail("topic",topic);
   ends[t]=pub?dds_create_writer(p,topic,qos,NULL):dds_create_reader(p,topic,qos,NULL);if(ends[t]<0)fail("reader_or_writer",ends[t]);
 }
 dds_delete_qos(qos);
 dds_entity_t waitset=-1;
 if(!pub){waitset=dds_create_waitset(p);if(waitset<0)fail("waitset",waitset);for(int t=0;t<active;++t){dds_entity_t cond=dds_create_readcondition(ends[t],DDS_ANY_STATE);if(cond<0)fail("readcondition",cond);dds_return_t rc=dds_waitset_attach(waitset,cond,(dds_attach_t)t);if(rc<0)fail("attach",rc);}}
 uint8_t *seen=calloc((size_t)topics*samples,1);uint64_t *lat=calloc((size_t)topics*samples,sizeof(uint64_t));uint64_t recv=0,bad=0,dup=0,sent=0;bool ready=false;
 uint64_t start=ns(),first=0;
 char* payload=malloc(size);memset(payload,'A',size-12);payload[size-12]=0;
 Bench_Msg*sample=Bench_Msg__alloc();void*storage[1]={sample};dds_sample_info_t info[1];
 while((ns()-start)/1000000ull<(uint64_t)life){
  if(pub){
   if(!ready){ready=true;for(int t=0;t<topics;++t){dds_publication_matched_status_t matched={0};dds_return_t rc=dds_get_publication_matched_status(ends[t],&matched);if(rc<0)fail("match",rc);if(t<subscribed&&matched.current_count<(int)expected_peer_count){ready=false;break;}}if(ready){first=ns()+500000000ull;printf("BENCH_DISCOVERY_READY elapsed_ms=%llu\n",(unsigned long long)((ns()-start)/1000000ull));fflush(stdout);}}
   if(ready&&sent<(uint64_t)topics*samples&&ns()>=first&&ns()-first>=sent*1000000000ull/(uint64_t)(hz*topics)){
    int t=sent%topics;Bench_Msg m={0};m.seq=sent/topics;m.sent_ns=ns();memset(payload,65+t%23,size-12);m.payload=payload;
    dds_return_t rc=dds_write(ends[t],&m);if(rc<0)fail("write",rc);sent++;
   }
   if(ready&&sent==(uint64_t)topics*samples&&ns()>=first&&ns()-first>(uint64_t)samples*1000000000ull/hz+500000000ull)break;
  }else{
   dds_attach_t triggered[256];dds_return_t wake=dds_waitset_wait(waitset,triggered,(size_t)active,DDS_MSECS(8));if(wake<0)fail("wait",wake);
   for(int t=0;t<active;++t){for(;;){dds_return_t count=dds_take(ends[t],storage,info,1,1);if(count<0)fail("take",count);if(count==0)break;if(count==1&&info[0].valid_data){
     Bench_Msg*m=storage[0];if(negative||m->seq>=(uint32_t)samples||!m->payload||strlen(m->payload)!=(size_t)size-12||m->sent_ns>ns()){bad++;continue;}
     char expected=65+t%23;bool valid=true;for(int j=0;j<size-12;++j)if(m->payload[j]!=expected){valid=false;break;}if(!valid){bad++;continue;}
     size_t index=(size_t)t*samples+m->seq;if(seen[index]++)dup++;else{lat[recv++]=(ns()-m->sent_ns)/1000;}
   }}}
   if(!negative&&recv==expected_received)break;
  }
  if(pub)usleep(200);
 }
 qsort(lat,recv,sizeof(uint64_t),cmp);
 uint64_t p95=recv?lat[(95*recv+99)/100-1]:0;
 printf("{\"role\":\"%s\",\"topics\":%d,\"bytes\":%d,\"samples\":%d,\"sent\":%llu,\"received\":%llu,\"bad\":%llu,\"duplicate\":%llu,\"p95_us\":%llu,\"ready\":%d,\"index\":%d,\"active_topics\":%d}\n",role,topics,size,samples,(unsigned long long)sent,(unsigned long long)recv,(unsigned long long)bad,(unsigned long long)dup,(unsigned long long)p95,ready?1:0,index,subscribed);
 dds_delete(p);Bench_Msg_free(sample,DDS_FREE_ALL);free(payload);free(lat);free(seen);free(ends);
 if(pub)return (ready&&sent==(uint64_t)topics*samples)?0:3;
 if(negative)return (recv==0&&bad==0)?0:4;
 return (recv<=expected_received&&bad==0&&dup==0)?0:5;
}
