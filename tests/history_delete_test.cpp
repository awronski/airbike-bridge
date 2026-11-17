#include <cassert>
#include <cstring>
#include <cstdio>
#include "history_delete.h"
struct Store {
    uint8_t stored[session::HISTORY_BYTES]={};bool failWrite=false,failRead=false,corrupt=false;
    size_t putBytes(const char*,const void* p,size_t n){if(failWrite)return 0;memcpy(stored,p,n);return n;}
    size_t getBytes(const char*,void* p,size_t n){if(failRead)return 0;memcpy(p,stored,n);if(corrupt)static_cast<uint8_t*>(p)[20]^=1;return n;}
};
int main(){
    session::History h,candidate;session::Record r;r.reason=session::FINISHED;r.durationMs=10000;
    for(unsigned i=0;i<20;++i)h.add(r);
    Store store;uint8_t bytes[session::HISTORY_BYTES];
    for(unsigned failure=0;failure<3;++failure){
        store.failWrite=failure==0;store.failRead=failure==1;store.corrupt=failure==2;
        assert(session::deleteSaved(store,h,10,candidate,bytes)==session::DeleteResult::STORAGE_ERROR);
        assert(h.count==20&&h.nextID==21&&h.records[10].id==10);
    }
    store.failWrite=store.failRead=store.corrupt=false;
    assert(session::deleteSaved(store,h,10,candidate,bytes)==session::DeleteResult::SAVED);
    assert(h.count==19&&h.nextID==21&&h.records[9].id==11&&h.records[10].id==9);
    session::History reloaded;assert(session::decode(reloaded,store.stored,sizeof(store.stored))&&reloaded.count==19);
    assert(session::deleteSaved(store,h,10,candidate,bytes)==session::DeleteResult::NOT_FOUND);
    assert(session::deleteSaved(store,h,0,candidate,bytes)==session::DeleteResult::NOT_FOUND);
    while(h.count)assert(session::deleteSaved(store,h,h.records[0].id,candidate,bytes)==session::DeleteResult::SAVED);
    h.add(r);assert(h.count==1&&h.records[0].id==21); // Old confirmation cannot target the new ride.
    std::puts("Persistent deletion, readback, failed writes, stable IDs and empty history passed.");
}
