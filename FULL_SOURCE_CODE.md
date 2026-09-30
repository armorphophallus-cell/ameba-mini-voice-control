# AMB82 Voice Control 完整文字原始碼

以下依檔案收錄專案的韌體、介面、PCM 模組及資料處理腳本。二進位模型、WAV、ZIP、建置輸出與虛擬環境不屬於文字原始碼，未嵌入本文件。

## AMB82_VoiceControl.ino

``cpp
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
````

## RawAudioSink.h

``cpp
#pragma once

#include <Arduino.h>
#include "AudioStream.h"

typedef void (*RawAudioCallback)(const int16_t *samples, size_t count);

class RawAudioSink : public MMFModule {
public:
    RawAudioSink();
    ~RawAudioSink();
    bool begin(RawAudioCallback callback);
    void end();
};
````

## RawAudioSink.cpp

``cpp
#include "RawAudioSink.h"

extern "C" {
#include "mmf2_module.h"
}

struct RawSinkContext {
    RawAudioCallback callback;
};

static void *rawCreate(void *)
{
    return calloc(1, sizeof(RawSinkContext));
}

static void *rawDestroy(void *context)
{
    free(context);
    return NULL;
}

static int rawControl(void *context, int command, int argument)
{
    RawSinkContext *sink = (RawSinkContext *)context;
    if (command == MM_MODULE_CMD(0)) {
        sink->callback = (RawAudioCallback)argument;
        return 0;
    }
    return -1;
}

static int rawHandle(void *context, void *input, void *)
{
    RawSinkContext *sink = (RawSinkContext *)context;
    mm_queue_item_t *item = (mm_queue_item_t *)input;
    if (sink && sink->callback && item && item->data_addr && item->size >= sizeof(int16_t)) {
        sink->callback((const int16_t *)item->data_addr, item->size / sizeof(int16_t));
    }
    return 0;
}

static mm_module_t rawSinkModule = {
    .create = rawCreate,
    .destroy = rawDestroy,
    .control = rawControl,
    .handle = rawHandle,
    .new_item = NULL,
    .del_item = NULL,
    .rsz_item = NULL,
    .vrelease_item = NULL,
    .output_type = MM_TYPE_NONE,
    .module_type = MM_TYPE_ASINK,
    .name = "RAW_PCM"
};

RawAudioSink::RawAudioSink() {}

RawAudioSink::~RawAudioSink()
{
    end();
}

bool RawAudioSink::begin(RawAudioCallback callback)
{
    if (_p_mmf_context == NULL) _p_mmf_context = mm_module_open(&rawSinkModule);
    if (_p_mmf_context == NULL) return false;
    return mm_module_ctrl(_p_mmf_context, MM_MODULE_CMD(0), (int)callback) == 0;
}

void RawAudioSink::end()
{
    if (_p_mmf_context != NULL) {
        mm_module_close(_p_mmf_context);
        _p_mmf_context = NULL;
    }
}
````

## spectral_templates.h

``cpp
#pragma once
constexpr int SPECTRAL_FRAMES=32;
constexpr int SPECTRAL_BANDS=12;
constexpr int SPECTRAL_FEATURES=SPECTRAL_FRAMES*SPECTRAL_BANDS;
constexpr float SPECTRAL_MAX_DISTANCE=0.047989544f;
constexpr float SPECTRAL_MIN_MARGIN=0.001474325f;
static const int SPECTRAL_FREQUENCIES[SPECTRAL_BANDS]={250,400,550,700,900,1100,1350,1650,2000,2400,2900,3500};
static const float LEFT_TEMPLATE[SPECTRAL_FEATURES] = {
    0.019880533f, 0.21316983f, -0.027199054f, 0.071408413f, -0.09803275f, -0.041611914f, -0.034905784f, -0.021303862f,
    -0.016890004f, -0.0038457513f, 0.17401046f, -0.23468113f, 0.43231896f, 0.28394637f, 0.21183413f, 0.13234805f,
    0.034794379f, 0.14546037f, -0.076959997f, -0.24625401f, -0.26807651f, -0.12780747f, -0.18638919f, -0.3352156f,
    0.35840616f, 0.46738395f, 0.1719846f, 0.11577433f, 0.17143612f, -0.059248552f, -0.13727252f, -0.23755954f,
    -0.22001259f, -0.080556311f, -0.14743191f, -0.40290406f, 0.33702239f, 0.43987215f, 0.15515365f, 0.28635484f,
    0.21323454f, -0.033863831f, -0.20181392f, -0.22652525f, -0.23531444f, -0.16980873f, -0.11038258f, -0.45392826f,
    0.37847748f, 0.41565242f, 0.10910714f, 0.27250236f, 0.257238f, -0.18153264f, -0.0856914f, -0.1604996f,
    -0.18309878f, -0.18013316f, -0.24574773f, -0.39627382f, 0.42521396f, 0.4303228f, 0.11202749f, 0.1660748f,
    0.12271408f, 0.025248652f, 0.03345019f, -0.16330628f, -0.17379653f, -0.27418104f, -0.31989476f, -0.38387322f,
    0.34705737f, 0.39761198f, 0.32254818f, 0.12078694f, 0.11781145f, -0.025798881f, -0.083897017f, -0.09445671f,
    -0.15169184f, -0.31950751f, -0.28308403f, -0.34737945f, 0.51454782f, 0.27128157f, 0.19673133f, 0.17783469f,
    0.091922931f, -0.0094052004f, 0.010476884f, -0.11674959f, -0.15234567f, -0.21492469f, -0.38053963f, -0.38883045f,
    0.32264677f, 0.4012793f, 0.24953729f, 0.1281115f, -0.029121364f, -0.071644485f, 0.041409437f, 0.010097961f,
    -0.16461951f, -0.11644021f, -0.30016887f, -0.47108769f, 0.42645851f, 0.30671728f, 0.25990921f, 0.097627304f,
    0.047947783f, -0.20168859f, -0.1313435f, -0.034954373f, -0.067751892f, -0.10568196f, -0.33282709f, -0.26441273f,
    0.53159595f, 0.21952999f, 0.15285651f, 0.058383029f, -0.0021975513f, -0.22701991f, -0.084135801f, 0.093268782f,
    -0.14255485f, -0.10028222f, -0.15135486f, -0.34808955f, 0.46730873f, 0.36515689f, 0.13861674f, 0.14553775f,
    -0.052155416f, 0.0066050873f, 0.00079616904f, -0.081184469f, -0.19845621f, -0.095518917f, -0.26814201f, -0.42856359f,
    0.56354314f, 0.39752793f, 0.05453052f, 0.080172032f, 0.084970467f, -0.072265975f, -0.061921328f, -0.23466879f,
    -0.23726673f, -0.1040349f, -0.19716753f, -0.27341953f, 0.55931282f, 0.22155456f, 0.20548688f, -0.029495383f,
    0.15229978f, -0.0026384629f, 0.027932733f, -0.10925647f, -0.2567617f, -0.17032854f, -0.20463498f, -0.39347208f,
    0.47931644f, 0.25835124f, 0.13142027f, 0.062537998f, 0.027426615f, 0.075282566f, 0.10161062f, -0.10455432f,
    -0.15674616f, -0.20127888f, -0.34048262f, -0.33288276f, 0.3829135f, 0.21195997f, 0.22293152f, 0.11027653f,
    0.1268876f, -0.031942535f, 0.092643887f, 0.0014054514f, -0.2845242f, -0.20222086f, -0.32561848f, -0.30471209f,
    0.36668992f, 0.35471249f, 0.32077369f, 0.097825699f, 0.11526909f, -0.066978104f, -0.10184944f, -0.12170527f,
    -0.23432355f, -0.13155453f, -0.26462761f, -0.33423284f, 0.33685017f, 0.22871442f, 0.33000639f, 0.13306695f,
    0.12517765f, -0.069522634f, 0.056082547f, -0.1999424f, -0.24395575f, -0.17139709f, -0.23119438f, -0.29388678f,
    0.43052256f, 0.21122311f, 0.32746962f, 0.19427808f, 0.0086967498f, -0.0032276486f, -0.11170703f, -0.10702919f,
    -0.28737512f, -0.19234417f, -0.23808295f, -0.2324248f, 0.44370839f, 0.37912834f, 0.30242452f, 0.16012552f,
    -0.020528063f, -0.13614009f, -0.086977728f, -0.013699499f, -0.19270211f, -0.091936611f, -0.3929874f, -0.35041544f,
    0.55375916f, 0.28611371f, 0.21614496f, 0.12752153f, -0.058759447f, -0.054213434f, -0.14068455f, 0.010622284f,
    -0.16643186f, -0.19896208f, -0.24643911f, -0.32867059f, 0.52871823f, 0.18972658f, 0.2860412f, 0.096816994f,
    0.087532818f, -0.00081789744f, -0.074984215f, -0.043581352f, -0.1924367f, -0.17432092f, -0.29533717f, -0.40735781f,
    0.46861506f, 0.31613666f, 0.297308f, 0.10100132f, -0.031410635f, -0.019414749f, 0.059487384f, -0.058660299f,
    -0.32631746f, -0.23863672f, -0.26835021f, -0.29975936f, 0.33940172f, 0.15606001f, 0.33453622f, 0.17135032f,
    0.042070869f, -0.015549981f, 0.10476799f, -0.057034116f, -0.25858855f, -0.16192827f, -0.33797193f, -0.31711379f,
    0.30770996f, 0.37898102f, 0.32272461f, 0.075824119f, 0.010575364f, 0.0501003f, 0.069285266f, -0.0059338175f,
    -0.19652934f, -0.24395882f, -0.39447406f, -0.3743054f, 0.3507081f, 0.28632602f, 0.33739352f, 0.071336426f,
    0.082975514f, 0.050131973f, 0.086668529f, -0.13414665f, -0.31281909f, -0.17281605f, -0.27027506f, -0.37548423f,
    0.28027588f, 0.34054741f, 0.34584144f, 0.1069231f, 0.031949218f, 0.057778653f, 0.070658423f, -0.093484841f,
    -0.30384064f, -0.23216742f, -0.30771026f, -0.29677203f, 0.45426965f, 0.41299221f, 0.28604606f, 0.03219137f,
    0.048811302f, -0.054021548f, 0.017680762f, -0.17516293f, -0.21501423f, -0.16137537f, -0.27414009f, -0.37227654f,
    0.55476391f, 0.31541592f, 0.19580524f, 0.028004548f, 0.095198132f, 0.026243158f, 0.02424722f, -0.2667664f,
    -0.26187739f, -0.14948858f, -0.22007556f, -0.34146985f, 0.53978902f, 0.28309992f, 0.17243211f, 0.048883718f,
    0.16162501f, 0.068511568f, -0.091944873f, -0.19343334f, -0.25876203f, -0.20803945f, -0.1254465f, -0.39671409f,
    0.46216488f, 0.29974151f, 0.27311224f, 0.042193402f, 0.044026185f, 0.027967714f, -0.046317052f, -0.18290929f,
    -0.12929547f, -0.23000145f, -0.15849562f, -0.40218708f, 0.30313638f, 0.32141909f, 0.28915039f, 0.13780022f,
    -0.083216451f, 0.051701769f, 0.051844675f, -0.16399197f, -0.11751137f, -0.24424978f, -0.22458802f, -0.32149509f
};
static const float RIGHT_TEMPLATE[SPECTRAL_FEATURES] = {
    0.65427238f, 0.31728843f, -0.020821916f, -0.12282056f, 0.061038017f, -0.0064223185f, -0.14716192f, -0.15360202f,
    0.042849261f, -0.2997019f, -0.2081853f, -0.11673284f, 0.5510574f, 0.43266836f, 0.15785037f, -0.035095949f,
    -0.018465919f, -0.22809672f, -0.16431148f, -0.051866233f, -0.051189926f, -0.095924817f, -0.28240934f, -0.21421473f,
    0.48540449f, 0.36648628f, 0.14926337f, -0.08300855f, -0.02617413f, 0.016044797f, 0.041831661f, -0.052531097f,
    -0.061200183f, -0.15166755f, -0.33408141f, -0.35036859f, 0.47670627f, 0.33178535f, 0.14489453f, 0.058623176f,
    0.073741488f, -0.039197098f, -0.068535753f, -0.10311171f, -0.11703033f, -0.038386986f, -0.25713277f, -0.46235645f,
    0.47242212f, 0.38656613f, 0.19923306f, 0.063116021f, 0.13178523f, -0.14475475f, -0.070562713f, -0.14640313f,
    -0.13874529f, -0.1686365f, -0.20738728f, -0.37663308f, 0.48507866f, 0.43036446f, 0.18621087f, 0.0065286285f,
    0.16034907f, -0.014023885f, -0.11210831f, -0.24975412f, -0.15223448f, -0.1056997f, -0.25157115f, -0.38314101f,
    0.43970516f, 0.33245948f, 0.25416711f, 0.047270566f, 0.16759962f, -0.1018429f, -0.048541967f, -0.1192437f,
    -0.15789224f, -0.11613652f, -0.27229974f, -0.42524481f, 0.3896884f, 0.43177903f, 0.29110327f, 0.10439994f,
    0.035140116f, -0.092011951f, -0.015903518f, -0.083946966f, -0.14394726f, -0.24815042f, -0.32123706f, -0.34691337f,
    0.46117011f, 0.30610752f, 0.26915833f, 0.016891029f, 0.10135611f, -0.053086203f, -0.031244418f, -0.03554127f,
    -0.10693747f, -0.24383031f, -0.31908667f, -0.364957f, 0.38020024f, 0.26460639f, 0.26499566f, 0.086967997f,
    0.11037461f, -0.068041481f, -0.11599556f, 0.017721483f, -0.13830626f, -0.099564351f, -0.29628417f, -0.40667441f,
    0.49001956f, 0.28469291f, 0.26209399f, 0.039799254f, 0.086894356f, -0.032697935f, -0.092249162f, -0.055966947f,
    -0.1386147f, -0.18173482f, -0.31868824f, -0.34354722f, 0.4695448f, 0.33491585f, 0.27894616f, 0.081105173f,
    0.11821425f, -0.0048238165f, -0.16289243f, -0.035908286f, -0.21019423f, -0.10215426f, -0.37495467f, -0.3917987f,
    0.36471191f, 0.38769987f, 0.3168782f, 0.039051417f, 0.080364935f, 0.049351782f, -0.07478445f, -0.057795614f,
    -0.26893768f, -0.25293741f, -0.28953588f, -0.29406738f, 0.48826873f, 0.39586902f, 0.28409705f, 0.068181276f,
    0.064306416f, -0.0018721999f, -0.081520081f, -0.18831088f, -0.24640451f, -0.19567806f, -0.27050424f, -0.31643206f,
    0.36376628f, 0.29616973f, 0.16204478f, 0.13948497f, 0.20102425f, 0.029516272f, 0.033597317f, -0.1958738f,
    -0.13972175f, -0.11351422f, -0.41405344f, -0.36244079f, 0.33044288f, 0.16017149f, 0.33768332f, 0.086018331f,
    0.003022901f, 0.054620683f, 0.21287803f, -0.12303957f, -0.15154923f, -0.13621508f, -0.40317407f, -0.37086034f,
    0.36470428f, 0.22442676f, 0.26883832f, 0.17512594f, 0.13039593f, 0.067284904f, 0.067525513f, -0.1145431f,
    -0.16694836f, -0.22384733f, -0.50663632f, -0.28632626f, 0.45905438f, 0.2451034f, 0.21074088f, 0.17133389f,
    0.14385168f, 0.015244116f, -0.073649079f, -0.13996287f, -0.2426458f, -0.18851699f, -0.33752406f, -0.2630302f,
    0.29615474f, 0.29579499f, 0.38844785f, 0.078339688f, 0.085673667f, 0.07113409f, -0.044392396f, -0.02022171f,
    -0.16439557f, -0.19864868f, -0.35303095f, -0.43485609f, 0.33305237f, 0.40592265f, 0.2450365f, 0.20742095f,
    -0.012495766f, -0.17370182f, 0.124159f, -0.20357125f, -0.18063897f, -0.1441519f, -0.27180347f, -0.32922894f,
    0.34969711f, 0.41149378f, 0.31933919f, 0.051879764f, -0.037944999f, -0.075094663f, 0.034645002f, -0.020559287f,
    -0.17065997f, -0.15066282f, -0.37364268f, -0.33849144f, 0.46055996f, 0.43511569f, 0.27304468f, -0.025503218f,
    -0.044325177f, -0.081538901f, 0.01476905f, -0.01894475f, -0.17549306f, -0.18294294f, -0.32184562f, -0.3328959f,
    0.39543462f, 0.44859764f, 0.1858511f, 0.096659474f, 0.065676317f, -0.11454561f, -0.023192301f, -0.037905689f,
    -0.20143235f, -0.13274372f, -0.35145199f, -0.33094752f, 0.35081634f, 0.39615321f, 0.15841196f, 0.10340646f,
    0.11184481f, -0.12991507f, 0.063151218f, 0.036358997f, -0.16329631f, -0.24262112f, -0.35418323f, -0.33012757f,
    0.45446512f, 0.44316041f, 0.27359751f, 0.13703503f, 0.011128549f, -0.038180217f, 0.052980196f, -0.22491316f,
    -0.26290163f, -0.21295321f, -0.3584089f, -0.27501023f, 0.34195116f, 0.43832245f, 0.41843387f, -0.062770136f,
    -0.0025685516f, -0.02929298f, -0.050442401f, -0.12201885f, -0.25885394f, -0.12592725f, -0.25744447f, -0.28938881f,
    0.3727729f, 0.4656941f, 0.33289251f, 0.076210789f, -0.042699412f, -0.029840672f, -0.0057329335f, -0.18262972f,
    -0.26128498f, -0.14989905f, -0.36498094f, -0.21050292f, 0.40556979f, 0.43982002f, 0.20103778f, 0.096326254f,
    0.071583994f, -0.083605163f, 0.049325723f, -0.14324014f, -0.24683608f, -0.20166588f, -0.31305987f, -0.27525654f,
    0.5018732f, 0.38200769f, 0.1773528f, 0.12612857f, 0.040831897f, -0.015516699f, -0.0040238337f, -0.090845883f,
    -0.32664177f, -0.18375832f, -0.19999683f, -0.40741089f, 0.65749079f, 0.36180663f, 0.16172747f, 0.022199569f,
    -0.01320815f, -0.080477871f, -0.010177652f, -0.17295326f, -0.16939425f, -0.12045064f, -0.2849746f, -0.35158777f,
    0.62755722f, 0.35538754f, 0.07261879f, 0.098970719f, 0.064451687f, -0.014338446f, -0.048012752f, -0.21551655f,
    -0.12077343f, -0.13382231f, -0.31135559f, -0.37516674f, 0.64821857f, 0.22674853f, 0.17472915f, 0.014447115f,
    0.091059029f, 0.012258898f, 0.014455631f, -0.090388454f, -0.16415764f, -0.18241112f, -0.40308407f, -0.34187654f
};
````

## page.h

``cpp
#pragma once

const char PAGE[] = R"HTML(<!doctype html>
<html lang="zh-Hant"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AMB82 語音燈光控制</title>
<style>
body{font-family:system-ui,sans-serif;max-width:640px;margin:32px auto;padding:0 18px;background:#f4f7fb;color:#14213d}
.card{background:#fff;border-radius:16px;padding:22px;box-shadow:0 8px 28px #20305018}.row{display:flex;gap:16px;flex-wrap:wrap}
.item{flex:1;min-width:150px;background:#eef3f9;padding:14px;border-radius:12px}.lamp{display:inline-block;width:18px;height:18px;border-radius:50%;background:#9aa4b2;margin-right:8px}
.blue.on{background:#168cff;box-shadow:0 0 14px #168cff}.green.on{background:#22b455;box-shadow:0 0 14px #22b455}
#resultPanel{margin:18px 0;padding:20px;border-radius:14px;background:#eef3f9;border:2px solid transparent;text-align:center}
#resultPanel.success{background:#e9f8ef;border-color:#35a65f}#resultPanel.unknown{background:#fff3e0;border-color:#ef8c22}#resultPanel.timeout{background:#edf2f8;border-color:#7890a8}#resultPanel.error{background:#fdecec;border-color:#d63b3b}
#resultTitle{display:block;font-size:1.6rem;margin-bottom:6px}#resultDetail{margin:0}.badge{display:inline-block;padding:5px 10px;border-radius:999px;background:#dfe7f1;font-weight:700}
button{width:100%;margin:16px 0;padding:13px;border:0;border-radius:12px;background:#244b78;color:white;font-size:1rem;font-weight:700;cursor:pointer}button.paused{background:#18834b}button:disabled{opacity:.55}
#connection.ok{color:#167a39}#connection.bad{color:#c62828}small{color:#596579}
</style></head><body><div class="card">
<h1>語音燈光控制</h1><p id="connection">正在連線…</p>
<div id="resultPanel"><strong id="resultTitle">等待語音指令</strong><p id="resultDetail">請說「左邊開燈」或「右邊開燈」</p></div>
<button id="pauseButton" type="button">暫停語音辨識</button>
<div class="row"><div class="item">辨識結果<br><strong id="recognition" class="badge">尚無</strong></div><div class="item">信心分數<br><strong id="score">—</strong></div></div>
<p>指令執行情況：<strong id="executed">尚無指令</strong></p>
<div class="row"><div class="item"><i id="blueLamp" class="lamp blue"></i>左邊／藍燈：<strong id="blue">關</strong></div><div class="item"><i id="greenLamp" class="lamp green"></i>右邊／綠燈：<strong id="green">關</strong></div></div>
<p><small>LED 狀態來自開發板 GPIO 回讀。無法辨識、非控制指令或低於門檻時不改變 LED。</small></p>
</div><script>
let failures=0;
pauseButton.onclick=async()=>{pauseButton.disabled=true;try{const r=await fetch('/api/pause',{method:'POST'});if(!r.ok)throw Error();await refresh();}catch(e){connection.textContent='操作失敗：無法連線至開發板';connection.className='bad';}finally{pauseButton.disabled=false;}};
async function refresh(){try{const r=await fetch('/api/state',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const s=await r.json();
connection.textContent='已連線至開發板';connection.className='ok';pauseButton.textContent=s.paused?'繼續語音辨識':'暫停語音辨識';pauseButton.className=s.paused?'paused':'';recognition.textContent=({left:'左邊',right:'右邊',unknown:'無法辨識',timeout:'10 秒無指令',paused:'辨識已暫停'})[s.recognition]||'尚無';score.textContent=s.score>=0?s.score:'—';
executed.textContent=s.recognition==='timeout'?'已自動關閉左右燈':(s.executed?'已執行 '+(s.recognition==='left'?'左邊藍燈':'右邊綠燈'):(s.sequence?'未執行，LED 維持原狀':'尚無指令'));
resultPanel.className='';if(s.recognition==='left'||s.recognition==='right'){resultPanel.classList.add('success');resultTitle.textContent=s.recognition==='left'?'辨識成功：左邊':'辨識成功：右邊';resultDetail.textContent=s.executed?'指令已執行，狀態已由 GPIO 確認':'指令處理中';}
else if(s.recognition==='unknown'&&s.sequence){resultPanel.classList.add('unknown');resultTitle.textContent='無法辨識';resultDetail.textContent='不是有效控制指令，LED 維持原狀';}
else if(s.recognition==='timeout'){resultPanel.classList.add('timeout');resultTitle.textContent='10 秒沒有有效指令';resultDetail.textContent='左右 LED 已自動關閉';}
else if(s.recognition==='paused'){resultPanel.classList.add('timeout');resultTitle.textContent='語音辨識已暫停';resultDetail.textContent='左右 LED 已關閉，按「繼續語音辨識」恢復';}
blue.textContent=s.blue?'開':'關';green.textContent=s.green?'開':'關';blueLamp.classList.toggle('on',s.blue);greenLamp.classList.toggle('on',s.green);failures=0;
}catch(e){if(++failures>=2){connection.textContent='通訊失敗：請確認仍連著 AMB82-Voice 熱點';connection.className='bad';resultPanel.className='error';resultTitle.textContent='通訊失敗';resultDetail.textContent='無法取得開發板回傳狀態';}}}
setInterval(refresh,700);refresh();
</script></body></html>)HTML";
````

## scripts/Split-VoiceCommands.ps1

``powershell
param(
    [string]$DatasetRoot = (Join-Path $PSScriptRoot '..\dataset')
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

public static class VoiceCommandSplitter
{
    public sealed class SegmentInfo
    {
        public string FileName;
        public double StartSeconds;
        public double EndSeconds;
        public double DurationSeconds;
        public double PeakRms;
    }

    private static short[] ReadPcm16Mono(string path, out int sampleRate)
    {
        using (var stream = File.OpenRead(path))
        using (var reader = new BinaryReader(stream))
        {
            if (new string(reader.ReadChars(4)) != "RIFF") throw new InvalidDataException("Missing RIFF");
            reader.ReadUInt32();
            if (new string(reader.ReadChars(4)) != "WAVE") throw new InvalidDataException("Missing WAVE");
            ushort format = 0, channels = 0, bits = 0;
            int rate = 0;
            byte[] pcm = null;
            while (stream.Position + 8 <= stream.Length)
            {
                string id = new string(reader.ReadChars(4));
                int size = reader.ReadInt32();
                long next = stream.Position + size + (size & 1);
                if (id == "fmt ")
                {
                    format = reader.ReadUInt16();
                    channels = reader.ReadUInt16();
                    rate = reader.ReadInt32();
                    reader.ReadInt32(); reader.ReadUInt16();
                    bits = reader.ReadUInt16();
                }
                else if (id == "data") pcm = reader.ReadBytes(size);
                stream.Position = next;
            }
            if (format != 1 || channels != 1 || bits != 16 || rate != 16000 || pcm == null)
                throw new InvalidDataException("Expected PCM 16 kHz mono 16-bit WAV");
            var samples = new short[pcm.Length / 2];
            Buffer.BlockCopy(pcm, 0, samples, 0, pcm.Length);
            sampleRate = rate;
            return samples;
        }
    }

    private static void WriteWav(string path, short[] samples, int start, int count, int rate)
    {
        using (var writer = new BinaryWriter(File.Create(path)))
        {
            int bytes = count * 2;
            writer.Write(System.Text.Encoding.ASCII.GetBytes("RIFF"));
            writer.Write(36 + bytes);
            writer.Write(System.Text.Encoding.ASCII.GetBytes("WAVEfmt "));
            writer.Write(16); writer.Write((ushort)1); writer.Write((ushort)1);
            writer.Write(rate); writer.Write(rate * 2); writer.Write((ushort)2); writer.Write((ushort)16);
            writer.Write(System.Text.Encoding.ASCII.GetBytes("data")); writer.Write(bytes);
            var buffer = new byte[bytes];
            Buffer.BlockCopy(samples, start * 2, buffer, 0, bytes);
            writer.Write(buffer);
        }
    }

    public static SegmentInfo[] Split(string input, string outputDir, string prefix)
    {
        int rate;
        short[] samples = ReadPcm16Mono(input, out rate);
        int frame = rate / 50; // 20 ms
        int frames = (samples.Length + frame - 1) / frame;
        var rms = new double[frames];
        for (int f = 0; f < frames; f++)
        {
            int start = f * frame, end = Math.Min(start + frame, samples.Length);
            double sum = 0;
            for (int i = start; i < end; i++) { double s = samples[i]; sum += s * s; }
            rms[f] = Math.Sqrt(sum / Math.Max(1, end - start));
        }
        var sorted = (double[])rms.Clone(); Array.Sort(sorted);
        double noise = sorted[(int)(sorted.Length * 0.20)];
        double peak = sorted[sorted.Length - 1];
        double threshold = Math.Max(250.0, Math.Max(noise * 3.0, peak * 0.07));

        int hangover = 12; // merge gaps up to 240 ms
        int pad = 8;       // 160 ms at both ends
        int minFrames = 12; // 240 ms
        var raw = new List<Tuple<int,int>>();
        int activeStart = -1, lastActive = -1;
        for (int f = 0; f < frames; f++)
        {
            if (rms[f] >= threshold)
            {
                if (activeStart < 0) activeStart = f;
                lastActive = f;
            }
            else if (activeStart >= 0 && f - lastActive > hangover)
            {
                if (lastActive - activeStart + 1 >= minFrames)
                    raw.Add(Tuple.Create(Math.Max(0, activeStart - pad), Math.Min(frames - 1, lastActive + pad)));
                activeStart = lastActive = -1;
            }
        }
        if (activeStart >= 0 && lastActive - activeStart + 1 >= minFrames)
            raw.Add(Tuple.Create(Math.Max(0, activeStart - pad), Math.Min(frames - 1, lastActive + pad)));

        Directory.CreateDirectory(outputDir);
        foreach (string old in Directory.GetFiles(outputDir, prefix + "_*.wav")) File.Delete(old);
        var result = new List<SegmentInfo>();
        int index = 1;
        foreach (var seg in raw)
        {
            int startSample = seg.Item1 * frame;
            int endSample = Math.Min(samples.Length, (seg.Item2 + 1) * frame);
            double duration = (endSample - startSample) / (double)rate;
            if (duration > 3.0) continue; // likely continuous noise, not one command
            string fileName = prefix + "_" + index.ToString("D3") + ".wav";
            WriteWav(Path.Combine(outputDir, fileName), samples, startSample, endSample - startSample, rate);
            double segmentPeak = 0;
            for (int f = seg.Item1; f <= seg.Item2; f++) segmentPeak = Math.Max(segmentPeak, rms[f]);
            result.Add(new SegmentInfo {
                FileName = fileName,
                StartSeconds = Math.Round(startSample / (double)rate, 3),
                EndSeconds = Math.Round(endSample / (double)rate, 3),
                DurationSeconds = Math.Round(duration, 3),
                PeakRms = Math.Round(segmentPeak, 1)
            });
            index++;
        }
        return result.ToArray();
    }
}
'@

$wavRoot = Join-Path $DatasetRoot 'wav'
$sampleRoot = Join-Path $DatasetRoot 'samples'
$leftRoot = Join-Path $sampleRoot 'left'
$rightRoot = Join-Path $sampleRoot 'right'
$left = [VoiceCommandSplitter]::Split((Join-Path $wavRoot 'left_source.wav'), $leftRoot, 'left')
$right = [VoiceCommandSplitter]::Split((Join-Path $wavRoot 'right_source.wav'), $rightRoot, 'right')

$manifest = @()
$manifest += $left | ForEach-Object { [PSCustomObject]@{ label='left'; file=('samples/left/' + $_.FileName); start_s=$_.StartSeconds; end_s=$_.EndSeconds; duration_s=$_.DurationSeconds; peak_rms=$_.PeakRms } }
$manifest += $right | ForEach-Object { [PSCustomObject]@{ label='right'; file=('samples/right/' + $_.FileName); start_s=$_.StartSeconds; end_s=$_.EndSeconds; duration_s=$_.DurationSeconds; peak_rms=$_.PeakRms } }
$manifest | Export-Csv -LiteralPath (Join-Path $DatasetRoot 'manifest.csv') -NoTypeInformation -Encoding UTF8

[PSCustomObject]@{
    LeftSamples = $left.Count
    RightSamples = $right.Count
    LeftDurationRange = if ($left.Count) { '{0:N3}-{1:N3}' -f (($left.DurationSeconds | Measure-Object -Minimum).Minimum), (($left.DurationSeconds | Measure-Object -Maximum).Maximum) } else { 'none' }
    RightDurationRange = if ($right.Count) { '{0:N3}-{1:N3}' -f (($right.DurationSeconds | Measure-Object -Minimum).Minimum), (($right.DurationSeconds | Measure-Object -Maximum).Maximum) } else { 'none' }
}
````

## scripts/build_spectral_templates.py

``python
"""Generate compact spectral templates for converter-free AMB82 matching."""

from __future__ import annotations

import json
import wave
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parents[1]
FRAMES = 32
FREQUENCIES = np.array([250, 400, 550, 700, 900, 1100, 1350, 1650, 2000, 2400, 2900, 3500])
SAMPLE_RATE = 16000


def read(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as wav:
        return np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float64)


def features(audio: np.ndarray) -> np.ndarray:
    output = []
    for frame in range(FRAMES):
        start = frame * len(audio) // FRAMES
        end = (frame + 1) * len(audio) // FRAMES
        samples = audio[start:end]
        samples = samples - samples.mean()
        window = np.hanning(len(samples))
        spectrum = np.fft.rfft(samples * window)
        bins = np.rint(FREQUENCIES * len(samples) / SAMPLE_RATE).astype(int)
        values = np.log1p(np.abs(spectrum[bins]) ** 2)
        values -= values.mean()
        norm = np.sqrt(np.sum(values * values)) + 1e-9
        output.extend(values / norm)
    return np.asarray(output, dtype=np.float32)


def cpp_array(name: str, values: np.ndarray) -> str:
    lines = []
    for start in range(0, len(values), 8):
        literals = []
        for value in values[start:start + 8]:
            number = f"{value:.8g}"
            if "." not in number and "e" not in number:
                number += ".0"
            literals.append(number + "f")
        lines.append("    " + ", ".join(literals))
    return f"static const float {name}[SPECTRAL_FEATURES] = {{\n" + ",\n".join(lines) + "\n};\n"


def main() -> None:
    groups = {}
    for label in ("left", "right"):
        groups[label] = np.stack([features(read(path)) for path in sorted((ROOT / "dataset" / "samples" / label).glob("*.wav"))])
    templates = {label: values.mean(axis=0) for label, values in groups.items()}

    own, other, margins = [], [], []
    confusion = [[0, 0], [0, 0]]
    for actual, label in enumerate(("left", "right")):
        for vector in groups[label]:
            distances = [float(np.mean((vector - templates[name]) ** 2)) for name in ("left", "right")]
            predicted = int(np.argmin(distances))
            confusion[actual][predicted] += 1
            own.append(distances[actual])
            other.append(distances[1 - actual])
            margins.append(distances[1 - actual] - distances[actual])

    # Conservative gates; unknown speech must be close to a template and clearly closer than the other command.
    max_distance = float(np.quantile(own, 0.98) * 1.35)
    min_margin = float(max(0.0005, np.quantile(margins, 0.05) * 0.45))
    header = f"""#pragma once
// Generated spectral command templates; no custom neural-network conversion required.
constexpr int SPECTRAL_FRAMES = {FRAMES};
constexpr int SPECTRAL_BANDS = {len(FREQUENCIES)};
constexpr int SPECTRAL_FEATURES = SPECTRAL_FRAMES * SPECTRAL_BANDS;
constexpr float SPECTRAL_MAX_DISTANCE = {max_distance:.8g}f;
constexpr float SPECTRAL_MIN_MARGIN = {min_margin:.8g}f;
static const int SPECTRAL_FREQUENCIES[SPECTRAL_BANDS] = {{{', '.join(map(str, FREQUENCIES))}}};
"""
    header += cpp_array("LEFT_TEMPLATE", templates["left"])
    header += cpp_array("RIGHT_TEMPLATE", templates["right"])
    (ROOT / "spectral_templates.h").write_text(header, encoding="ascii")
    report = {
        "feature_shape": [FRAMES, len(FREQUENCIES)],
        "training_confusion": confusion,
        "own_distance_range": [float(min(own)), float(max(own))],
        "other_distance_range": [float(min(other)), float(max(other))],
        "margin_range": [float(min(margins)), float(max(margins))],
        "max_distance": max_distance,
        "min_margin": min_margin,
        "limit": "Thresholds are based on supplied recordings; live board microphone testing is required.",
    }
    (ROOT / "model" / "artifacts" / "spectral_template_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
````

## scripts/build_live_templates.py

``python
"""Replace spectral templates with features captured from the AMB82 microphone."""
from pathlib import Path
import json
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
rows=[]
for line in (ROOT/'model/artifacts/live_calibration.csv').read_text(encoding='utf-8-sig').splitlines():
    parts=line.strip().split(',')
    if len(parts)==387 and parts[0]=='CAL': rows.append((parts[1],int(parts[2]),np.array(parts[3:],dtype=np.float32)))
groups={label:np.stack([v for lab,_,v in rows if lab==label]) for label in ('L','R')}
templates={label:values.mean(axis=0) for label,values in groups.items()}
checks=[]
for label in ('L','R'):
    for i,v in enumerate(groups[label]):
        own=float(np.mean((v-templates[label])**2)); other=float(np.mean((v-templates['R' if label=='L' else 'L'])**2))
        checks.append({'label':label,'index':i+1,'own':own,'other':other,'margin':other-own,'correct':own<other})
own=[x['own'] for x in checks]; margins=[x['margin'] for x in checks]
max_distance=max(own)*1.6
positive=[m for m in margins if m>0]
min_margin=max(0.001,min(positive)*0.35) if positive else 1.0
def array(name,values):
    out=[]
    for i in range(0,len(values),8): out.append('    '+', '.join((f'{x:.8g}' if '.' in f'{x:.8g}' or 'e' in f'{x:.8g}' else f'{x:.8g}.0')+'f' for x in values[i:i+8]))
    return f'static const float {name}[SPECTRAL_FEATURES] = {{\n'+',\n'.join(out)+'\n};\n'
header=f'''#pragma once
constexpr int SPECTRAL_FRAMES=32;
constexpr int SPECTRAL_BANDS=12;
constexpr int SPECTRAL_FEATURES=SPECTRAL_FRAMES*SPECTRAL_BANDS;
constexpr float SPECTRAL_MAX_DISTANCE={max_distance:.8g}f;
constexpr float SPECTRAL_MIN_MARGIN={min_margin:.8g}f;
static const int SPECTRAL_FREQUENCIES[SPECTRAL_BANDS]={{250,400,550,700,900,1100,1350,1650,2000,2400,2900,3500}};
'''+array('LEFT_TEMPLATE',templates['L'])+array('RIGHT_TEMPLATE',templates['R'])
(ROOT/'spectral_templates.h').write_text(header,encoding='ascii')
report={'samples':{k:len(v) for k,v in groups.items()},'checks':checks,'max_distance':max_distance,'min_margin':min_margin,'all_closer_to_own':all(x['correct'] for x in checks)}
(ROOT/'model/artifacts/live_template_report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
````

## scripts/train_score_classifier.py

``python
"""Train a tiny CPU classifier from quantized stock-YAMNet class scores.

The generated C++ header runs after AMB82's bundled DEFAULT_YAMNET model and
therefore needs no custom .nb conversion.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import tensorflow as tf
import tensorflow_hub as hub

import train_yamnet_head as data_source


ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "model" / "artifacts"
HEADER = ROOT / "score_classifier.h"
LABELS = ["unknown", "left", "right"]
SEED = 82


def score_vector(yamnet, audio: np.ndarray) -> np.ndarray:
    scores, _, _ = yamnet(tf.convert_to_tensor(audio, dtype=tf.float32))
    mean = tf.reduce_mean(scores, axis=0).numpy()
    # AMB82 exposes integer percentages. Mirror that loss of precision here.
    return np.floor(np.clip(mean, 0.0, 1.0) * 100.0).astype(np.float32) / 100.0


def cpp_array(name: str, values: np.ndarray) -> str:
    flat = values.reshape(-1)
    rows = []
    for start in range(0, len(flat), 8):
        literals = []
        for value in flat[start:start + 8]:
            number = f"{value:.8g}"
            if "." not in number and "e" not in number:
                number += ".0"
            literals.append(number + "f")
        rows.append("    " + ", ".join(literals))
    return f"static const float {name}[{len(flat)}] = {{\n" + ",\n".join(rows) + "\n};\n"


def main() -> None:
    np.random.seed(SEED)
    tf.random.set_seed(SEED)
    rows = data_source.command_rows()
    audio_items, targets = [], []
    for row in rows:
        audio_items.append(data_source.fit_window(data_source.read_wav(data_source.DATASET / row["file"])))
        targets.append(LABELS.index(row["label"]))
    unknown = data_source.background_windows(rows)
    audio_items.extend(unknown)
    targets.extend([0] * len(unknown))

    yamnet = hub.load(data_source.YAMNET_URL)
    x = np.stack([score_vector(yamnet, audio) for audio in audio_items])
    y = np.asarray(targets, dtype=np.int32)

    model = tf.keras.Sequential([
        tf.keras.layers.Input((521,)),
        tf.keras.layers.Dense(24, activation="relu"),
        tf.keras.layers.Dense(3, activation="softmax"),
    ])
    model.compile(tf.keras.optimizers.Adam(2e-3), "sparse_categorical_crossentropy", metrics=["accuracy"])
    history = model.fit(x, y, epochs=240, batch_size=16, verbose=0)
    probability = model.predict(x, verbose=0)
    predicted = probability.argmax(axis=1)

    w1, b1 = model.layers[0].get_weights()
    w2, b2 = model.layers[1].get_weights()
    content = """#pragma once
// Generated by scripts/train_score_classifier.py. Input is 521 YAMNet scores in [0,1].
constexpr int SCORE_INPUTS = 521;
constexpr int SCORE_HIDDEN = 24;
constexpr int SCORE_OUTPUTS = 3;
"""
    content += cpp_array("SCORE_W1", w1)
    content += cpp_array("SCORE_B1", b1)
    content += cpp_array("SCORE_W2", w2)
    content += cpp_array("SCORE_B2", b2)
    HEADER.write_text(content, encoding="ascii")

    report = {
        "architecture": "bundled DEFAULT_YAMNET scores -> Dense(24, relu) -> Dense(3, softmax)",
        "labels": LABELS,
        "counts": {label: int((y == i).sum()) for i, label in enumerate(LABELS)},
        "training_accuracy": float((predicted == y).mean()),
        "final_loss": float(history.history["loss"][-1]),
        "confusion": tf.math.confusion_matrix(y, predicted, num_classes=3).numpy().tolist(),
        "deployment": "No custom .nb. Uses AMB82 DEFAULT_YAMNET plus CPU classifier header.",
        "limit": "Training-set check only; live board thresholds still require calibration.",
    }
    (ARTIFACTS / "score_classifier_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
````

## scripts/train_yamnet_head.py

``python
"""Train a three-class prototype on frozen YAMNet embeddings.

This produces a classifier head and a reproducible training report. The head
is not by itself an AMB82 .nb model; Realtek conversion still needs a complete
compatible audio model.
"""

from __future__ import annotations

import csv
import json
import random
import wave
from pathlib import Path

import numpy as np
import tensorflow as tf
import tensorflow_hub as hub


ROOT = Path(__file__).resolve().parents[1]
DATASET = ROOT / "dataset"
MODEL_DIR = ROOT / "model" / "artifacts"
YAMNET_URL = "https://tfhub.dev/google/yamnet/1"
LABELS = ["unknown", "left", "right"]
TARGET_SAMPLES = 15_600  # 0.975 s, matching the AMB82 YAMNet input window
SEED = 82


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as wav:
        if (wav.getframerate(), wav.getnchannels(), wav.getsampwidth()) != (16000, 1, 2):
            raise ValueError(f"Unexpected WAV format: {path}")
        audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2")
    return audio.astype(np.float32) / 32768.0


def fit_window(audio: np.ndarray) -> np.ndarray:
    if len(audio) >= TARGET_SAMPLES:
        start = (len(audio) - TARGET_SAMPLES) // 2
        return audio[start : start + TARGET_SAMPLES]
    left = (TARGET_SAMPLES - len(audio)) // 2
    return np.pad(audio, (left, TARGET_SAMPLES - len(audio) - left))


def command_rows() -> list[dict[str, str]]:
    with (DATASET / "manifest.csv").open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def background_windows(rows: list[dict[str, str]]) -> list[np.ndarray]:
    windows: list[np.ndarray] = []
    for label, source_name in (("left", "left_source.wav"), ("right", "right_source.wav")):
        source = read_wav(DATASET / "wav" / source_name)
        occupied = [(float(r["start_s"]), float(r["end_s"])) for r in rows if r["label"] == label]
        cursor = 0.0
        gaps: list[tuple[float, float]] = []
        for start, end in occupied:
            if start - cursor >= 1.05:
                gaps.append((cursor, start))
            cursor = max(cursor, end)
        total_seconds = len(source) / 16000.0
        if total_seconds - cursor >= 1.05:
            gaps.append((cursor, total_seconds))
        for start, end in gaps:
            first = start + 0.03
            while first + TARGET_SAMPLES / 16000.0 <= end - 0.03:
                a = int(first * 16000)
                windows.append(source[a : a + TARGET_SAMPLES])
                first += 1.05
    # Add low-level synthetic room noise and silence so quiet input maps unknown.
    rng = np.random.default_rng(SEED)
    for _ in range(20):
        windows.append(rng.normal(0.0, rng.uniform(0.0002, 0.003), TARGET_SAMPLES).astype(np.float32))
    return windows


def yamnet_embedding(yamnet, audio: np.ndarray) -> np.ndarray:
    _, embeddings, _ = yamnet(tf.convert_to_tensor(audio, dtype=tf.float32))
    return tf.reduce_mean(embeddings, axis=0).numpy()


def main() -> None:
    random.seed(SEED)
    np.random.seed(SEED)
    tf.random.set_seed(SEED)
    MODEL_DIR.mkdir(parents=True, exist_ok=True)

    rows = command_rows()
    audio_items: list[np.ndarray] = []
    targets: list[int] = []
    sources: list[str] = []
    for row in rows:
        audio_items.append(fit_window(read_wav(DATASET / row["file"])))
        targets.append(LABELS.index(row["label"]))
        sources.append(row["file"])
    unknown = background_windows(rows)
    audio_items.extend(unknown)
    targets.extend([0] * len(unknown))
    sources.extend([f"generated/background_{i:03d}" for i in range(len(unknown))])

    print(f"Loading YAMNet from {YAMNET_URL}")
    yamnet = hub.load(YAMNET_URL)
    features = np.stack([yamnet_embedding(yamnet, audio) for audio in audio_items]).astype(np.float32)
    y = np.asarray(targets, dtype=np.int32)

    model = tf.keras.Sequential([
        tf.keras.layers.Input(shape=(features.shape[1],), name="yamnet_embedding"),
        tf.keras.layers.Dense(96, activation="relu"),
        tf.keras.layers.Dropout(0.20),
        tf.keras.layers.Dense(len(LABELS), activation="softmax", name="command"),
    ], name="voice_command_head")
    model.compile(optimizer=tf.keras.optimizers.Adam(1e-3),
                  loss="sparse_categorical_crossentropy", metrics=["accuracy"])
    history = model.fit(features, y, epochs=80, batch_size=16, verbose=0)

    probabilities = model.predict(features, verbose=0)
    predicted = probabilities.argmax(axis=1)
    confusion = tf.math.confusion_matrix(y, predicted, num_classes=len(LABELS)).numpy().tolist()
    train_accuracy = float((predicted == y).mean())

    model.save(MODEL_DIR / "voice_command_head.keras")
    model.save(MODEL_DIR / "voice_command_head.h5")
    np.savez_compressed(MODEL_DIR / "training_embeddings.npz",
                        features=features, labels=y, sources=np.asarray(sources))
    report = {
        "labels": LABELS,
        "sample_counts": {label: int((y == i).sum()) for i, label in enumerate(LABELS)},
        "embedding_size": int(features.shape[1]),
        "epochs": 80,
        "final_training_loss": float(history.history["loss"][-1]),
        "final_training_accuracy": float(history.history["accuracy"][-1]),
        "recomputed_training_accuracy": train_accuracy,
        "training_confusion_matrix_rows_actual_columns_predicted": confusion,
        "important_limit": "Training-set metrics only; no independent test set was requested.",
        "deployment_limit": "Classifier head is not an AMB82 .nb model. A compatible full audio model and Realtek conversion are still required.",
    }
    (MODEL_DIR / "training_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    (MODEL_DIR / "labels.txt").write_text("\n".join(LABELS) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
````

## scripts/build_custom_yamnet.py

``python
"""Build a three-output YAMNet H5 candidate for Realtek conversion.

The convolutional YAMNet body uses TensorFlow's official pretrained weights.
Only a new 3-class logistic layer is trained from the saved embeddings.
"""

from __future__ import annotations

import json
import sys
from dataclasses import replace
from pathlib import Path

import numpy as np
import tensorflow as tf
import tf_keras


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "model" / "yamnet_source"
ARTIFACTS = ROOT / "model" / "artifacts"
sys.path.insert(0, str(SOURCE))

import params as yamnet_params  # noqa: E402
import yamnet as yamnet_model  # noqa: E402


SEED = 82
LABELS = ["unknown", "left", "right"]


def main() -> None:
    np.random.seed(SEED)
    tf.random.set_seed(SEED)
    data = np.load(ARTIFACTS / "training_embeddings.npz")
    features = data["features"].astype(np.float32)
    labels = data["labels"].astype(np.int32)
    one_hot = tf.one_hot(labels, len(LABELS))

    # Realtek's audio path is based on YAMNet's single logistic output layer.
    head = tf_keras.Sequential([
        tf_keras.layers.Input(shape=(1024,)),
        tf_keras.layers.Dense(len(LABELS), activation="sigmoid", name="command_logits"),
    ])
    head.compile(optimizer=tf_keras.optimizers.Adam(2e-3),
                 loss="binary_crossentropy", metrics=["accuracy"])
    history = head.fit(features, one_hot, epochs=160, batch_size=16, verbose=0)

    original_params = yamnet_params.Params()
    original = yamnet_model.yamnet_frames_model(original_params)
    original.load_weights(str(SOURCE / "yamnet.h5"))

    custom_params = replace(original_params, num_classes=len(LABELS), classifier_activation="sigmoid")
    custom = yamnet_model.yamnet_frames_model(custom_params)

    copied, skipped = [], []
    for old_layer, new_layer in zip(original.layers, custom.layers):
        old_weights, new_weights = old_layer.get_weights(), new_layer.get_weights()
        if old_weights and len(old_weights) == len(new_weights) and all(a.shape == b.shape for a, b in zip(old_weights, new_weights)):
            new_layer.set_weights(old_weights)
            copied.append(new_layer.name)
        elif new_weights:
            skipped.append(new_layer.name)

    dense_layers = [layer for layer in custom.layers if isinstance(layer, tf_keras.layers.Dense)]
    if len(dense_layers) != 1:
        raise RuntimeError(f"Expected one Dense layer, found {len(dense_layers)}")
    dense_layers[0].set_weights(head.layers[-1].get_weights())

    weights_path = ARTIFACTS / "voice_command_yamnet.h5"
    full_path = ARTIFACTS / "voice_command_yamnet_full.h5"
    custom.save_weights(str(weights_path))
    custom.save(str(full_path), include_optimizer=False)

    predicted = head.predict(features, verbose=0).argmax(axis=1)
    report = {
        "labels": LABELS,
        "architecture": "Official YAMNet body plus a 3-output sigmoid layer",
        "copied_pretrained_layers": len(copied),
        "skipped_replaced_layers": skipped,
        "head_training_accuracy": float((predicted == labels).mean()),
        "head_final_loss": float(history.history["loss"][-1]),
        "weights_h5": weights_path.name,
        "full_model_h5": full_path.name,
        "conversion_status": "Candidate only; Realtek converter acceptance has not yet been verified.",
    }
    (ARTIFACTS / "custom_yamnet_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
````

## scripts/validate_custom_yamnet.py

``python
"""Load the custom YAMNet H5 and run a basic inference sanity check."""

from __future__ import annotations

import json
import sys
import wave
from pathlib import Path

import numpy as np
import tf_keras


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "model" / "yamnet_source"
ARTIFACTS = ROOT / "model" / "artifacts"
sys.path.insert(0, str(SOURCE))

import params as yamnet_params  # noqa: E402
import yamnet as yamnet_model  # noqa: E402


LABELS = ["unknown", "left", "right"]


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 16000):
            raise ValueError(f"Unsupported WAV format: {path}")
        return np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float32) / 32768.0


def predict(model: tf_keras.Model, path: Path) -> dict:
    scores, _, _ = model(read_wav(path))
    mean_scores = np.asarray(scores).mean(axis=0)
    class_id = int(mean_scores.argmax())
    return {
        "file": str(path.relative_to(ROOT)),
        "class_id": class_id,
        "label": LABELS[class_id],
        "scores": {label: float(score) for label, score in zip(LABELS, mean_scores)},
    }


def main() -> None:
    model = yamnet_model.yamnet_frames_model(
        yamnet_params.Params(num_classes=3, classifier_activation="sigmoid")
    )
    model.load_weights(str(ARTIFACTS / "voice_command_yamnet.h5"))

    checks = []
    for label in ("left", "right"):
        samples = sorted((ROOT / "dataset" / "samples" / label).glob("*.wav"))
        for path in (samples[0], samples[len(samples) // 2], samples[-1]):
            checks.append(predict(model, path))

    silence = np.zeros(16000, dtype=np.float32)
    scores, _, _ = model(silence)
    mean_scores = np.asarray(scores).mean(axis=0)
    class_id = int(mean_scores.argmax())
    checks.append({
        "file": "generated one-second silence",
        "class_id": class_id,
        "label": LABELS[class_id],
        "scores": {label: float(score) for label, score in zip(LABELS, mean_scores)},
    })

    report = {"checks": checks, "all_expected": all(
        (item["label"] in item["file"]) or
        (item["file"].startswith("generated") and item["label"] == "unknown")
        for item in checks
    )}
    (ARTIFACTS / "inference_sanity_report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
````
