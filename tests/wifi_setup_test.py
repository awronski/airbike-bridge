"""Fault-inject the real Wi-Fi completion loop with native peripheral doubles."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUBS = r'''
#include <string>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cassert>
#include "setup_window.h"
using String=std::string;
constexpr int WL_CONNECTED=3,WIFI_STA=1;
constexpr char HOSTNAME[]="test-atom";
uint32_t currentMs=2000;
uint32_t millis(){return currentMs;}
struct Address{String toString(){return "192.0.2.1";}};
struct Wifi{int state=WL_CONNECTED;String ssid="test-network";int retries=0,closed=0;
int status(){return state;} String SSID(){return ssid;} Address localIP(){return {};}
void disconnect(bool,bool){} void begin(const char*,const char*){} void reconnect(){++retries;}
void softAPdisconnect(bool){++closed;} void mode(int){}
} WiFi;
struct Preferences{size_t putString(const char*,const String&){return 0;} bool isKey(const char*){return false;}} preferences;
struct Mdns{bool begin(const char*){return true;} void addService(const char*,const char*,int){}} MDNS;
struct Logger{void println(const char*){} template<typename... T>void printf(const char*,T...){}} Serial;
struct Http{int restarts=0;void begin(){++restarts;}} server;
bool pendingJoin=false,joining=true,storeOnSuccess=true,setupAP=true,manualSetup=true,mdnsStarted=false;
bool wifiCredentialsStored=true, persistOK=false;
uint32_t queuedJoinMs=0,wifiAttemptMs=1000;
std::atomic<bool> stationGotIP{true};
String joiningSSID="test-network",joiningPassword="test-password";
const char* setupPhase="joining";
SetupWindow setupWindow;
void startSetupAP(){setupAP=true;setupWindow.reopen();}
bool saveWiFiCredentials(const String&,const String&){return persistOK;}
'''
MAIN = r'''
int main(int argc,char** argv){
    if(argc>1&&(!strcmp(argv[1],"fallback")||!strcmp(argv[1],"manual"))){
        const bool explicitSetup=!strcmp(argv[1],"manual");
        setupAP=explicitSetup;manualSetup=explicitSetup;storeOnSuccess=false;
        WiFi.state=0;stationGotIP=false;currentMs=31001;
        updateWiFi();
        assert(setupAP&&!joining&&!setupWindow.completed);
        assert(WiFi.retries==int(!explicitSetup));
        WiFi.state=WL_CONNECTED;updateWiFi();assert(setupAP); // Require GOT_IP.
        stationGotIP=true;WiFi.ssid="another-network";updateWiFi();assert(setupAP);
        WiFi.ssid=joiningSSID;updateWiFi();
        assert(setupAP==explicitSetup && WiFi.closed==int(!explicitSetup));
        assert(server.restarts==WiFi.closed);
        return 0;
    }
    persistOK=argc>1&&!strcmp(argv[1],"success");
    updateWiFi();
    assert(!joining && !storeOnSuccess && joiningPassword.empty());
    if(persistOK){assert(!strcmp(setupPhase,"connected"));assert(setupWindow.completed);}
    else{assert(!strcmp(setupPhase,"save_failed"));assert(!setupWindow.completed);}
    currentMs=62000;updateWiFi();
    assert(setupAP!=persistOK);
    assert(server.restarts==int(persistOK));
}
'''


class WiFiSetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="airbike-wifi-test-")
        source = (ROOT / "src/main.cpp").read_text()
        function = source[source.index("void updateWiFi() {"):source.index("void updateButton() {")]
        path = Path(cls.temp.name) / "wifi.cpp"
        path.write_text(STUBS + function + MAIN)
        cls.binary = Path(cls.temp.name) / "wifi-test"
        subprocess.run(["clang++", "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-I", str(ROOT / "include"),
                        str(path), "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_failed_save_keeps_setup_open(self):
        result = subprocess.run([str(self.binary), "failure"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_verified_save_closes_setup_after_grace_period(self):
        result = subprocess.run([str(self.binary), "success"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_fallback_closes_after_saved_network_returns(self):
        result = subprocess.run([str(self.binary), "fallback"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_explicit_setup_stays_open_when_network_returns(self):
        result = subprocess.run([str(self.binary), "manual"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
