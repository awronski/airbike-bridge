"""Run the production recovery, save and clear paths against a fault-injected flash store."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUBS = r'''
#include <cassert>
#include <map>
#include <string>
#include <vector>
#include "history_recovery.h"

struct Store {
    std::map<std::string,std::vector<uint8_t>> blobs;
    std::string failRead, failWrite, corruptRead;
    unsigned writes=0;
    size_t getBytesLength(const char* key) {return blobs[key].size();}
    size_t getBytes(const char* key,void* out,size_t size) {
        const auto& blob=blobs[key];
        if(key==failRead||blob.size()>size)return 0;
        memcpy(out,blob.data(),blob.size());
        if(key==corruptRead&&!blob.empty()&&(corruptRead!="history-v2"||blob.size()==session::HISTORY_BYTES))static_cast<uint8_t*>(out)[0]^=1;
        return blob.size();
    }
    size_t putBytes(const char* key,const uint8_t* data,size_t size) {
        if(key==failWrite)return 0;
        blobs[key]={data,data+size};++writes;
        return size;
    }
} ridePreferences;
session::Tracker rideTracker;
session::History historyCandidate;
uint8_t historyBytes[session::HISTORY_BYTES];
bool historyStorageOK=true, historyDirty=false, historyBackupVerified=false;
bool recoveryRideWriteUnverified=false;
const char* corruptHistoryKey=nullptr;
uint32_t historyVersion=0, historyRetryMs=0;
int response=0;
uint32_t millis(){return 1000;}
uint64_t esp_timer_get_time(){return 1000000;}
uint32_t esp_random(){return 12345;}
namespace diagnostic {void flash(int,uint64_t,bool){} void finishCapture(){}}
struct Server {
    std::string confirmation;
    std::string arg(const char*){return confirmation;}
} server;
void apiError(int code,const char*){response=code;}
void sendRides(){response=200;}
void sendJSON(const char*){response=200;}
bool allowAction(const char*){return true;}
void updateDashboardRide(){}
'''
CASES = r'''
int main(int argc,char** argv) {
    const bool legacy=argc>1&&strcmp(argv[1],"legacy")==0;
    const char* originalKey=legacy?"history":"history-v2";
    const char* backupKey=legacy?"history-bad-v1":"history-bad-v2";
    const std::vector<uint8_t> broken={0x41,0x42,0x53,0,0xff};
    ridePreferences.blobs[originalKey]=broken;
    corruptHistoryKey=originalKey;

    rideTracker.start(app::Rider{},0);
    rideTracker.current.durationMs=500;
    recoverHistory();assert(response==400&&ridePreferences.writes==0);
    server.confirmation="1";
    recoverHistory();assert(response==409&&ridePreferences.writes==0);
    rideTracker.finish(session::DISCONNECTED,500);
    assert(!persistHistory(true)&&ridePreferences.writes==0&&rideTracker.pending);

    ridePreferences.failRead=originalKey;
    recoverHistory();assert(response==503&&ridePreferences.writes==0);
    ridePreferences.failRead.clear();
    ridePreferences.failWrite=backupKey;
    recoverHistory();assert(response==503&&ridePreferences.writes==0);
    ridePreferences.failWrite.clear();
    ridePreferences.corruptRead=backupKey;
    recoverHistory();
    assert(response==503&&!historyBackupVerified&&rideTracker.pending);
    assert(ridePreferences.blobs[originalKey]==broken);
    assert(ridePreferences.blobs[backupKey]==broken);

    clearRide();
    assert(response==200&&!rideTracker.hasRide()); // The failed backup never wrote this ride.
    rideTracker.start(app::Rider{},0);
    rideTracker.current.durationMs=500;
    rideTracker.finish(session::DISCONNECTED,500);

    ridePreferences.corruptRead="history-v2";
    recoverHistory();
    assert(response==503&&historyBackupVerified&&rideTracker.pending&&corruptHistoryKey);
    assert(ridePreferences.blobs[backupKey]==broken);
    const auto replacement=ridePreferences.blobs["history-v2"];
    const auto writes=ridePreferences.writes;

    assert(!canClearRide());
    clearRide();
    assert(response==409&&rideTracker.pending&&rideTracker.current.durationMs==500);
    assert(ridePreferences.writes==writes&&ridePreferences.blobs["history-v2"]==replacement);

    ridePreferences.corruptRead.clear();
    recoverHistory();
    assert(response==200&&!corruptHistoryKey&&historyStorageOK&&!historyBackupVerified);
    assert(ridePreferences.writes==writes+1&&ridePreferences.blobs[backupKey]==broken);
    assert(ridePreferences.blobs["history-v2"]==replacement);
    if(legacy)assert(ridePreferences.blobs[originalKey]==broken);
    session::History restarted;
    assert(session::decode(restarted,replacement.data(),replacement.size()));
    assert(restarted.count==1&&restarted.records[0].durationMs==500);
    assert(restarted.records[0].reason==session::DISCONNECTED);
    assert(rideTracker.history.count==1&&!rideTracker.hasRide());
    assert(rideTracker.history.records[0].id==restarted.records[0].id);

    rideTracker.start(app::Rider{},1000);
    rideTracker.current.durationMs=500;
    rideTracker.finish(session::FINISHED,1500);
    assert(persistHistory(true)&&rideTracker.history.count==2&&!rideTracker.pending);

    // A valid read after a transient problem must retain all existing rides.
    corruptHistoryKey="history-v2";
    rideTracker.history.count=0;
    const auto previousBackup=ridePreferences.blobs["history-bad-v2"];
    recoverHistory();
    assert(response==200&&!corruptHistoryKey&&rideTracker.history.count==2);
    assert(ridePreferences.blobs["history-bad-v2"]==previousBackup);
}
'''


class HistoryRecoveryTests(unittest.TestCase):
    def test_corruption_and_flash_failures_preserve_raw_backup_and_pending_ride(self):
        source = (ROOT / 'src/main.cpp').read_text()
        storage = source[source.index('bool writeHistory('):source.index('app::Rider chosenRider()')]
        recovery = source[source.index('void recoverHistory()'):source.index('void configureRideHTTP()')]
        clear_start = source.index('[]{', source.index('server.on("/api/rides/clear"')) + len('[]{')
        clear_body = source[clear_start:source.index('    });', clear_start)]
        with tempfile.TemporaryDirectory(prefix='airbike-recovery-') as directory:
            cpp = Path(directory) / 'recovery.cpp'
            binary = cpp.with_suffix('')
            cpp.write_text(STUBS + storage + recovery + 'void clearRide(){' + clear_body + '}\n' + CASES)
            subprocess.run(['clang++', '-std=c++11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I', str(ROOT / 'include'),
                            str(cpp), '-o', str(binary)], check=True)
            for version in ('legacy', 'modern'):
                with self.subTest(version=version):
                    subprocess.run([str(binary), version], check=True)
