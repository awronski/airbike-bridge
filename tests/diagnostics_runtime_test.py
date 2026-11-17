"""Run the real diagnostic HTTP/storage runtime with deterministic ESP32 peripherals.

No device, existing preview, real rider data or network is touched.
"""
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUB = r'''
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <fstream>
using String=std::string;
namespace fake {
inline uint64_t us=0;inline bool failSetting=false,failReport=false,corruptRead=false,allowed=true;
inline size_t freeEntries=500,maxChunk=0;inline unsigned reportWrites=0,settingWrites=0;
inline std::map<std::string,std::vector<uint8_t>> data;
}
inline uint32_t millis(){return fake::us/1000;}
inline int64_t esp_timer_get_time(){return fake::us;}
inline int esp_reset_reason(){return 1;}
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
inline uint32_t uxTaskGetStackHighWaterMark(void*){return 4096;}
constexpr int MALLOC_CAP_8BIT=1,WL_CONNECTED=3;
inline size_t heap_caps_get_largest_free_block(int){return 56000;}
struct Esp{uint32_t getFreeHeap(){return 80000;}uint32_t getMinFreeHeap(){return 70000;}};inline Esp ESP;
using arduino_event_id_t=int;
struct arduino_event_info_t{struct{int reason;}wifi_sta_disconnected;};
constexpr int ARDUINO_EVENT_WIFI_STA_DISCONNECTED=1,ARDUINO_EVENT_WIFI_STA_GOT_IP=2;
struct Wifi{int status(){return WL_CONNECTED;}int RSSI(){return -49;}template<class F>void onEvent(F){}};inline Wifi WiFi;
class Preferences {
    std::string ns;
    std::string key(const char* k){return ns+":"+k;}
public:
    bool begin(const char* name,bool){ns=name;return true;}
    size_t freeEntries(){return fake::freeEntries;}
    size_t getBytesLength(const char* k){return fake::data[key(k)].size();}
    uint8_t getUChar(const char* k,uint8_t fallback){const auto& d=fake::data[key(k)];return d.size()==1?d[0]:fallback;}
    size_t putUChar(const char* k,uint8_t v){++fake::settingWrites;if(fake::failSetting)return 0;fake::data[key(k)]={v};return 1;}
    size_t putBytes(const char* k,const void* p,size_t n){++fake::reportWrites;if(fake::failReport)return 0;const auto* b=static_cast<const uint8_t*>(p);fake::data[key(k)]={b,b+n};return n;}
    size_t getBytes(const char* k,void* p,size_t n){const auto& d=fake::data[key(k)];if(d.size()!=n)return 0;memcpy(p,d.data(),n);if(fake::corruptRead)static_cast<uint8_t*>(p)[100]^=1;return n;}
};
constexpr int HTTP_GET=1,HTTP_POST=2,CONTENT_LENGTH_UNKNOWN=-1;
class WebServer {
public:
    std::map<std::pair<std::string,int>,std::function<void()>> routes;
    std::map<std::string,std::string> args;int status=0;std::string body;
    template<class F>void on(const char* url,int method,F fn){routes[{url,method}]=fn;}
    String arg(const char* k){return args[k];}
    void sendHeader(const char*,const char*){}
    void setContentLength(int){}
    void send(int code,const char*,const String& value){status=code;body=value;}
    void sendContent(const char* chunk){fake::maxChunk=std::max(fake::maxChunk,strlen(chunk));body+=chunk;}
    void sendContent(const char* chunk,size_t n){fake::maxChunk=std::max(fake::maxChunk,n);body.append(chunk,n);}
    void call(const char* path,int method=HTTP_GET){body.clear();status=0;routes.at({path,method})();}
};
'''
MAIN = r'''
#include "diagnostics.cpp"
using namespace diagnostic;
WebServer web;
bool allow(const char* action){assert(!strcmp(action,"diagnostics"));if(!fake::allowed)web.send(403,"text/plain","denied");return fake::allowed;}
void at(uint32_t ms){fake::us=uint64_t(ms)*1000;}
void checkStatus(int code){assert(web.status==code);}
void json(){checkStatus(200);puts(web.body.c_str());}
int main(int argc,char** argv){
    const std::string mode=argc>1?argv[1]:"default";
    if(mode=="restore"){
        std::ifstream input(argv[2],std::ios::binary);
        auto& data=fake::data["airbike-diag:report-v1"];data.assign(std::istreambuf_iterator<char>(input),{});
        fake::data["airbike-diag:enabled"]={0};
    }
    begin();configureHTTP(web,allow);
    if(mode=="default"||mode=="restore"){
        web.call("/api/diagnostics");json();
        if(mode=="restore"){web.args["source"]="saved";web.call("/api/diagnostics/export");json();}
        return 0;
    }
    if(mode=="settings"){
        fake::allowed=false;web.args["enabled"]="0";web.call("/api/diagnostics/settings",HTTP_POST);checkStatus(403);assert(fake::settingWrites==0);
        fake::allowed=true;web.args["enabled"]="invalid";web.call("/api/diagnostics/settings",HTTP_POST);checkStatus(400);assert(fake::settingWrites==0);
        web.args["enabled"]="0";fake::failSetting=true;web.call("/api/diagnostics/settings",HTTP_POST);checkStatus(503);assert(isEnabled());
        fake::failSetting=false;web.call("/api/diagnostics/settings",HTTP_POST);json();assert(!isEnabled());
        web.args["enabled"]="1";web.call("/api/diagnostics/settings",HTTP_POST);json();assert(isEnabled());
        at(1000);packet(BIKE,true);tick(true,1,2,1);web.call("/api/diagnostics");json();return 0;
    }
    at(100);connection(BIKE,true);at(1000);packet(BIKE,true);tick(true,1,2,1);
    at(3600);tick(false,UINT32_MAX,2,2);at(5000);connection(BIKE,false,0x208);tick(false,UINT32_MAX,2,0);
    at(6000);connection(BIKE,true);at(6200);packet(BIKE,true);tick(true,1,2,1);
    at(301000);packet(BIKE,true);tick(true,1,2,1);assert(fake::reportWrites==1);
    if(mode=="capture"){
        web.args["source"]="current";web.call("/api/diagnostics/export");json();
        web.args["source"]="saved";web.call("/api/diagnostics/export");json();
        const auto& bytes=fake::data["airbike-diag:report-v1"];
        std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        assert(fake::maxChunk<=768);return 0;
    }
    if(mode=="space"||mode=="failure"){
        const auto previous=fake::data["airbike-diag:report-v1"];
        if(mode=="space")fake::freeEntries=100;else fake::failReport=true;
        at(601001);packet(BIKE,true);tick(true,1,2,1);web.call("/api/diagnostics");json();
        assert(fake::data["airbike-diag:report-v1"]==previous);
        const auto writes=fake::reportWrites;at(601101);tick(true,1,2,1);assert(fake::reportWrites==writes);
        return 0;
    }
    if(mode=="corrupt"){
        fake::data["airbike-diag:report-v1"][100]^=1;web.args["source"]="saved";web.call("/api/diagnostics/export");checkStatus(404);return 0;
    }
    return 2;
}
'''


class DiagnosticsRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="airbike-diagnostic-runtime-")
        folder = Path(cls.temp.name)
        (folder / 'fake.h').write_text(STUB)
        for name in ['Arduino.h','WebServer.h','Preferences.h','WiFi.h','esp_system.h','esp_heap_caps.h','esp_timer.h']:
            (folder / name).write_text('#include "fake.h"\n')
        (folder / 'diagnostics_build.h').write_text('#define DIAGNOSTIC_BUILD "test-build"\n')
        (folder / 'runtime.cpp').write_text(MAIN)
        cls.binary = folder / 'runtime'
        cls.report = folder / 'saved.bin'
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                        '-I',str(folder),'-I',str(ROOT/'include'),'-I',str(ROOT/'src'),str(folder/'runtime.cpp'),'-o',str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, name):
        output = subprocess.run([str(self.binary),name,str(self.report)],capture_output=True,text=True,check=True)
        return [json.loads(line) for line in output.stdout.splitlines()]

    def test_default_on_and_no_browser_or_ride_required(self):
        data, = self.run_case('default')
        self.assertTrue(data['enabled'])
        self.assertFalse(data['saved_available'])
        self.assertEqual(data['events'][0]['kind'],'boot')

    def test_actual_endpoints_and_five_minute_checkpoint_survive_restart(self):
        current, saved = self.run_case('capture')
        self.assertTrue(current['active'])
        self.assertEqual(current['stats']['bike_disconnects'],1)
        self.assertEqual(saved['stats']['bike_disconnects'],1)
        self.assertEqual(saved['session_id'],1)
        self.assertTrue(current['health'])
        self.assertFalse(saved['health'])
        status, restored = self.run_case('restore')
        self.assertFalse(status['enabled'])
        self.assertTrue(status['saved_available'])
        self.assertFalse(status['saved_this_boot'])
        self.assertEqual(restored['stats'],saved['stats'])
        self.assertEqual(restored['events'],saved['events'])
        text=json.dumps(saved)
        for secret in ['password','ssid','band_address','rider_name']:
            self.assertNotIn(secret,text)

    def test_preference_validation_failure_and_enabling_midride(self):
        off,on,partial=self.run_case('settings')
        self.assertFalse(off['enabled'])
        self.assertTrue(on['enabled'])
        self.assertTrue(partial['partial'])

    def test_low_storage_keeps_previous_report_and_backs_off(self):
        data,=self.run_case('space')
        self.assertEqual(data['save_status'],'insufficient_storage')
        self.assertTrue(data['saved_available'])

    def test_failed_checkpoint_keeps_previous_report_and_backs_off(self):
        data,=self.run_case('failure')
        self.assertEqual(data['save_status'],'write_failed')

    def test_corrupt_saved_report_is_not_downloaded_as_valid(self):
        self.assertEqual(self.run_case('corrupt'),[])


if __name__=='__main__':
    unittest.main()
