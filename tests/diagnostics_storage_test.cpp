#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "diagnostics_storage.h"
struct Store {
    uint8_t bytes[diagnostic::STORAGE_BYTES]{};
    size_t length=0,free=500;bool fail=false,corrupt=false;unsigned writes=0;
    size_t freeEntries(){return free;}
    size_t putBytes(const char*,const void* p,size_t n){++writes;if(fail)return 0;memcpy(bytes,p,n);length=n;return n;}
    size_t getBytes(const char*,void* p,size_t n){if(n!=length)return 0;memcpy(p,bytes,n);if(corrupt)static_cast<uint8_t*>(p)[100]^=1;return n;}
};
int main(){
    using namespace diagnostic;
    Store store;Recorder r;r.boot(0,1,"test");r.activity(1,true,0);
    Compact report;r.compact(report,10);uint8_t scratch[STORAGE_BYTES];
    assert(save(store,report,scratch)==SaveResult::OK && store.writes==1);
    store.fail=true;assert(save(store,report,scratch)==SaveResult::WRITE_FAILED);
    store.fail=false;store.corrupt=true;assert(save(store,report,scratch)==SaveResult::VERIFY_FAILED);
    store.corrupt=false;store.free=STORAGE_RESERVE_ENTRIES-1;const auto writes=store.writes;
    assert(save(store,report,scratch)==SaveResult::NO_SPACE && store.writes==writes);
    puts("Diagnostic storage verifies writes and reserves history/settings capacity.");
}
