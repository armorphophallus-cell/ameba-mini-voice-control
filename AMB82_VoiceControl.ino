#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include "StreamIO.h"
#include "AudioStream.h"
#include "RawAudioSink.h"
#include "spectral_templates.h"
#include "page.h"
extern "C" {
#include "ard_socket.h"
}
#include <lwip/sockets.h>
#undef read
#undef write

enum { UNKNOWN, LEFT, RIGHT, TIMEOUT, PAUSED };
const int BLOCK=320, VAD=250, END_QUIET=12, MIN_SAMPLES=5000, MAX_SAMPLES=24000;
AudioSetting audioConfig(16000,1,USE_AUDIO_AMIC);
Audio audio; RawAudioSink sink; StreamIO audioStream(1,1);
int16_t recording[MAX_SAMPLES], block[BLOCK];
volatile int recordingLength=0; volatile bool recordingNow=false, recordingReady=false;
volatile int latestClass=UNKNOWN, latestScore=-1; volatile bool latestExecuted=false;
volatile unsigned long sequence=0;
volatile bool paused=false;
float lastFeatures[SPECTRAL_FEATURES];
char ssid[]="AMB82-Voice", password[]="voice82x", channel[]="6";
IPAddress apIP(192,168,82,1), subnet(255,255,255,0); int serverSocket=-1;

const char *className(int id){return id==LEFT?"left":id==RIGHT?"right":id==TIMEOUT?"timeout":id==PAUSED?"paused":"unknown";}

void processBlock(const int16_t *data){
  static int quiet=0;
  if(paused){recordingNow=false;recordingReady=false;recordingLength=0;quiet=0;return;}
  long long sum=0; for(int i=0;i<BLOCK;i++) sum+=abs((int)data[i]);
  int level=sum/BLOCK;
  if(!recordingNow && !recordingReady && level>=VAD){recordingNow=true; recordingLength=0; quiet=0;}
  if(recordingNow){
    if(recordingLength+BLOCK<=MAX_SAMPLES){memcpy(recording+recordingLength,data,BLOCK*2); recordingLength+=BLOCK;}
    quiet=level<VAD?quiet+1:0;
    if(quiet>=END_QUIET || recordingLength+BLOCK>MAX_SAMPLES){
      int trimmed=recordingLength-quiet*BLOCK; if(trimmed>0) recordingLength=trimmed;
      recordingNow=false; recordingReady=true;
    }
  }
}

void onAudio(const int16_t *data,size_t count){
  static int used=0;
  for(size_t i=0;i<count;i++){block[used++]=data[i]; if(used==BLOCK){processBlock(block); used=0;}}
}

void extractFeatures(const int16_t *data,int count,float *out){
  for(int frame=0;frame<SPECTRAL_FRAMES;frame++){
    int start=frame*count/SPECTRAL_FRAMES, end=(frame+1)*count/SPECTRAL_FRAMES, nSamples=end-start;
    float mean=0; for(int n=start;n<end;n++) mean+=data[n]; mean/=nSamples;
    float featureMean=0;
    for(int band=0;band<SPECTRAL_BANDS;band++){
      int bin=(int)(SPECTRAL_FREQUENCIES[band]*nSamples/16000.0f+0.5f); float re=0,im=0;
      for(int n=0;n<nSamples;n++){
        float window=0.5f-0.5f*cosf(2*PI*n/(nSamples-1)); float value=(data[start+n]-mean)*window;
        float phase=2*PI*bin*n/nSamples; re+=value*cosf(phase); im-=value*sinf(phase);
      }
      float value=log1pf(re*re+im*im); out[frame*SPECTRAL_BANDS+band]=value; featureMean+=value;
    }
    featureMean/=SPECTRAL_BANDS; float norm=1e-9f;
    for(int band=0;band<SPECTRAL_BANDS;band++){float &v=out[frame*SPECTRAL_BANDS+band];v-=featureMean;norm+=v*v;}
    norm=sqrtf(norm); for(int band=0;band<SPECTRAL_BANDS;band++)out[frame*SPECTRAL_BANDS+band]/=norm;
  }
}

int recognize(const int16_t *data,int count,int &confidence,float &distance,float &margin){
  extractFeatures(data,count,lastFeatures); float dl=0,dr=0;
  for(int i=0;i<SPECTRAL_FEATURES;i++){float l=lastFeatures[i]-LEFT_TEMPLATE[i],r=lastFeatures[i]-RIGHT_TEMPLATE[i];dl+=l*l;dr+=r*r;}
  dl/=SPECTRAL_FEATURES;dr/=SPECTRAL_FEATURES;distance=min(dl,dr);margin=fabsf(dl-dr);
  confidence=min(99,max(0,(int)(50+margin*1000)));
  if(count<MIN_SAMPLES||distance>SPECTRAL_MAX_DISTANCE||margin<SPECTRAL_MIN_MARGIN)return UNKNOWN;
  return dl<dr?LEFT:RIGHT;
}

void reply(WiFiClient &c,const char *status,const char *type,const char *body){
  String h=String("HTTP/1.1 ")+status+"\r\nContent-Type: "+type+"\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: "+String(strlen(body))+"\r\n\r\n";
  c.print(h);c.write((const uint8_t*)body,strlen(body));
}
void serve(WiFiClient &c){
  String h;unsigned long start=millis();while(c.connected()&&millis()-start<1000&&h.length()<1024){uint8_t b;int n=c.read(&b,1);if(n>0){h+=(char)b;if(h.endsWith("\r\n\r\n"))break;}else if(n==0)break;else delay(1);}
  if(h.startsWith("GET / HTTP/"))reply(c,"200 OK","text/html; charset=utf-8",PAGE);
  else if(h.startsWith("GET /api/state HTTP/")){
    String j=String("{\"recognition\":\"")+className(latestClass)+"\",\"score\":"+String((int)latestScore)+",\"executed\":"+(latestExecuted?"true":"false")+",\"paused\":"+(paused?"true":"false")+",\"blue\":"+(digitalRead(LED_B)?"true":"false")+",\"green\":"+(digitalRead(LED_G)?"true":"false")+",\"sequence\":"+String((unsigned long)sequence)+"}";
    reply(c,"200 OK","application/json",j.c_str());
  }else if(h.startsWith("POST /api/pause HTTP/")){
    paused=!paused;recordingNow=false;recordingReady=false;recordingLength=0;digitalWrite(LED_B,LOW);digitalWrite(LED_G,LOW);
    latestClass=paused?PAUSED:UNKNOWN;latestScore=-1;latestExecuted=true;sequence++;
    String j=String("{\"ok\":true,\"paused\":")+(paused?"true":"false")+",\"blue\":false,\"green\":false}";
    printf("{\"event\":\"pause\",\"paused\":%s,\"blue\":false,\"green\":false}\r\n",paused?"true":"false");
    reply(c,"200 OK","application/json",j.c_str());
  }else reply(c,"404 Not Found","text/plain","Not found");
}

void setup(){
  Serial.begin(115200);pinMode(LED_B,OUTPUT);pinMode(LED_G,OUTPUT);digitalWrite(LED_B,LOW);digitalWrite(LED_G,LOW);
  audio.configAudio(audioConfig);audio.begin();
  if(!sink.begin(onAudio))printf("{\"event\":\"error\",\"message\":\"PCM sink failed\"}\r\n");
  audioStream.registerInput(audio);audioStream.registerOutput(sink);
  if(audioStream.begin()!=0)printf("{\"event\":\"error\",\"message\":\"PCM stream failed\"}\r\n");
  WiFi.config(apIP,apIP,apIP,subnet);while(WiFi.apbegin(ssid,password,channel,0)!=WL_CONNECTED)delay(1000);
  serverSocket=start_server(80,TCP_MODE);if(serverSocket<0||sock_listen(serverSocket,4)<0)printf("{\"event\":\"error\",\"message\":\"HTTP server failed\"}\r\n");
  printf("{\"event\":\"ready\",\"mode\":\"pcm-template\"}\r\nVoice interface: http://");Serial.println(WiFi.localIP());
}

void loop(){
  static unsigned long lastCommand=0;
  if(recordingReady&&!paused){
    int count=recordingLength,score=0;float distance=0,margin=0;int result=recognize(recording,count,score,distance,margin);recordingReady=false;
    latestClass=result;latestScore=score;latestExecuted=false;sequence++;
    if(result!=UNKNOWN&&millis()-lastCommand>=1200){digitalWrite(LED_B,result==LEFT);digitalWrite(LED_G,result==RIGHT);latestExecuted=true;lastCommand=millis();sequence++;}
    printf("{\"event\":\"recognition\",\"class\":\"%s\",\"score\":%d,\"samples\":%d,\"distance\":%.5f,\"margin\":%.5f,\"executed\":%s,\"blue\":%s,\"green\":%s}\r\n",className(result),score,count,distance,margin,latestExecuted?"true":"false",digitalRead(LED_B)?"true":"false",digitalRead(LED_G)?"true":"false");
  }
  if((digitalRead(LED_B)||digitalRead(LED_G))&&millis()-lastCommand>=10000){
    digitalWrite(LED_B,LOW);digitalWrite(LED_G,LOW);latestClass=TIMEOUT;latestScore=-1;latestExecuted=true;sequence++;
    printf("{\"event\":\"timeout\",\"seconds\":10,\"executed\":true,\"blue\":false,\"green\":false}\r\n");
  }
  if(serverSocket>=0){fd_set set;FD_ZERO(&set);FD_SET(serverSocket,&set);struct timeval t={0,0};if(lwip_select(serverSocket+1,&set,NULL,NULL,&t)>0){int s=lwip_accept(serverSocket,NULL,NULL);if(s>=0){WiFiClient c((uint8_t)s);c.setRecvTimeout(5);serve(c);c.stop();}}}
  delay(2);
}
