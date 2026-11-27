"""Exercise the firmware's HTTP routing with fake AP and LAN connections."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUBS = r'''
#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "setup_window.h"
#include "wifi_credentials.h"
using String=std::string;
enum HTTPMethod { HTTP_GET, HTTP_POST, HTTP_HEAD };
struct IPAddress {
    uint32_t value;
    IPAddress(unsigned a=0,unsigned b=0,unsigned c=0,unsigned d=0):value(a<<24|b<<16|c<<8|d){}
    operator uint32_t() const {return value;}
    String toString() const {
        return std::to_string(value>>24)+'.'+std::to_string((value>>16)&255)+'.'+
            std::to_string((value>>8)&255)+'.'+std::to_string(value&255);
    }
};
struct Connection { IPAddress local, peer;
    IPAddress localIP() const {return local;}
    IPAddress remoteIP() const {return peer;}
};
struct WebServer;
struct RequestHandler {
    virtual ~RequestHandler()=default;
    virtual bool canHandle(HTTPMethod,String)=0;
    virtual bool handle(WebServer&,HTTPMethod,String)=0;
};
struct WebServer {
    Connection connection;
    int status=0;
    String body;
    std::map<String,String> fields, headers;
    std::vector<std::unique_ptr<RequestHandler>> custom;
    std::vector<std::function<bool(HTTPMethod,const String&)>> routes;
    std::function<void()> missing;
    Connection client(){return connection;}
    String arg(const char* key){return fields[key];}
    String header(const char* key){return headers[key];}
    void send(int code,const char*,const String& data){status=code;body=data;}
    void sendHeader(const char*,const char*){}
    void send_P(int code,const char* type,const char* data){send(code,type,data);}
    void send_P(int code,const char* type,const char* data,size_t len){send(code,type,String(data,len));}
    void addHandler(RequestHandler* handler){
        custom.emplace_back(handler);
        routes.push_back([this,handler](HTTPMethod method,const String& uri){
            return handler->canHandle(method,uri)&&handler->handle(*this,method,uri);
        });
    }
    void on(const char* path,HTTPMethod method,std::function<void()> route){
        routes.push_back([path,method,route](HTTPMethod actual,const String& uri){
            if(actual!=method||uri!=path)return false;
            route();return true;
        });
    }
    void onNotFound(std::function<void()> fn){missing=fn;}
    void collectHeaders(const char*[],size_t){}
    void begin(){}
    int call(Connection request,HTTPMethod method,const char* uri){
        connection=request;status=0;body.clear();
        for(const auto& route:routes)if(route(method,uri))return status;
        missing();return status;
    }
} server;
constexpr int WIFI_MODE_AP=2,WL_CONNECTED=3;
struct Wifi {
    IPAddress ap{192,168,77,1}, sta{192,0,2,42}, mask{255,255,255,0};
    bool apEnabled=true;
    int status(){return WL_CONNECTED;}
    int getMode(){return apEnabled?WIFI_MODE_AP:0;}
    IPAddress softAPIP(){return ap;}
    IPAddress softAPSubnetMask(){return mask;}
    IPAddress localIP(){return sta;}
} WiFi;
bool setupAP=true,joining=false,pendingJoin=false,manualSetup=false,storeOnSuccess=false;
uint32_t queuedJoinMs=0;
const char* setupPhase="idle";
String joiningSSID,joiningPassword;
SetupWindow setupWindow;
constexpr char SETUP_ORIGIN[]="http://192.168.77.1";
constexpr char SETUP_PAGE[]="setup page";
constexpr unsigned char WEB_PAGE_GZIP[]={'d','a','s','h'};
uint32_t millis(){return 0;}
String jsonString(const char* value){return '"'+String(value)+'"';}
void sendJSON(const String& text){server.send(200,"application/json",text);}
unsigned dashboardCalls=0;
void dashboard(){++dashboardCalls;sendJSON("dashboard data");}
void sendLive(){dashboard();}
void sendRides(){dashboard();}
void sendTrend(){dashboard();}
namespace diagnostic {
void configureHTTP(WebServer& web,bool(*authorize)(const char*)){
    web.on("/api/diagnostics/export",HTTP_GET,dashboard);
    web.on("/api/diagnostics/settings",HTTP_POST,[authorize]{if(authorize("diagnostics"))dashboard();});
}
}
struct {bool retryRequested=false;} state;
int stateMux=0;
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
void configureRideHTTP(){server.on("/api/riders",HTTP_GET,dashboard);}
'''
CASES = r'''
int main(){
    configureHTTP();
    const IPAddress setupPeer{192,168,77,20}, lanPeer{192,0,2,20};
    const Connection hotspot{WiFi.ap,setupPeer}, lan{WiFi.sta,lanPeer};
    const Connection hotspotAtStationAddress{WiFi.sta,setupPeer};
    assert(server.call(hotspot,HTTP_GET,"/")==200&&server.body=="setup page");
    assert(server.call(hotspot,HTTP_GET,"/api/setup")==200);

    for(const char* route:{"/api/live","/api/rides","/api/trend","/api/riders","/api/diagnostics/export"}){
        const auto calls=dashboardCalls;
        assert(server.call(hotspot,HTTP_GET,route)==404&&dashboardCalls==calls);
        assert(server.call(hotspotAtStationAddress,HTTP_GET,route)==404&&dashboardCalls==calls);
        assert(server.call(lan,HTTP_GET,route)==200&&dashboardCalls==calls+1);
    }
    assert(server.call(hotspotAtStationAddress,HTTP_GET,"/")==404);
    assert(server.call(lan,HTTP_GET,"/")==200&&server.body=="dash");
    assert(server.call(hotspot,HTTP_HEAD,"/")==404);
    assert(server.call(hotspot,HTTP_GET,"/wifi")==404);
    assert(server.call(hotspot,HTTP_POST,"/api/setup")==404);

    server.headers={{"Origin","http://airbike-atom.local"},{"X-AirBike-Action","retry"}};
    assert(server.call(hotspot,HTTP_POST,"/api/retry")==404&&!state.retryRequested);
    assert(server.call(lan,HTTP_POST,"/api/retry")==200&&state.retryRequested);
    server.headers["X-AirBike-Action"]="diagnostics";
    const auto calls=dashboardCalls;
    assert(server.call(hotspot,HTTP_POST,"/api/diagnostics/settings")==404&&dashboardCalls==calls);
    assert(server.call(lan,HTTP_POST,"/api/diagnostics/settings")==200&&dashboardCalls==calls+1);

    assert(server.call(lan,HTTP_GET,"/api/setup")==403);
    server.fields={{"ssid","synthetic-network"},{"password","test-password"}};
    server.headers["Origin"]=SETUP_ORIGIN;
    assert(server.call(lan,HTTP_POST,"/wifi")==403&&!pendingJoin);
    assert(server.call(hotspot,HTTP_POST,"/wifi")==200&&pendingJoin);
    assert(server.call(hotspot,HTTP_GET,"/api/setup")==200&&server.body.find("queued")!=String::npos);

    setupAP=false;  // The interface can still be up if disabling the hotspot failed.
    assert(server.call(hotspot,HTTP_GET,"/api/rides")==404);
    assert(server.call(hotspotAtStationAddress,HTTP_GET,"/api/rides")==404);
    WiFi.apEnabled=false;
    assert(server.call(hotspot,HTTP_GET,"/api/rides")==404);
    assert(server.call(lan,HTTP_GET,"/api/rides")==200);
}
'''


class SetupHTTPTests(unittest.TestCase):
    def test_setup_routes_are_isolated_while_lan_keeps_dashboard_access(self):
        source = (ROOT / 'src/main.cpp').read_text()
        access = source[source.index('bool fromSetupAP()'):source.index('void startSetupAP(')]
        actions = source[source.index('bool allowAction('):source.index('void apiError(')]
        http = source[source.index('void configureHTTP()'):source.index('bool saveWiFiCredentials(')]
        with tempfile.TemporaryDirectory(prefix='airbike-http-test-') as directory:
            cpp = Path(directory) / 'setup_http.cpp'
            cpp.write_text(STUBS + access + actions + http + CASES)
            binary = cpp.with_suffix('')
            subprocess.run(['clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'include'),
                            str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
