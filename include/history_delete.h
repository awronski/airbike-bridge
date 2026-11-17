#pragma once
#include "ride_session.h"

namespace session {
enum class DeleteResult { SAVED, NOT_FOUND, STORAGE_ERROR };
// Candidate/buffer are caller-owned to keep the ESP32 loop stack bounded.
template<class Store> DeleteResult deleteSaved(Store& store,History& history,uint32_t id,
                                               History& candidate,uint8_t* bytes){
    size_t index=0;while(index<history.count&&history.records[index].id!=id)++index;
    if(!id||index==history.count)return DeleteResult::NOT_FOUND;
    candidate=history;
    for(size_t i=index+1;i<candidate.count;++i)candidate.records[i-1]=candidate.records[i];
    candidate.records[--candidate.count]=Record{};
    encode(candidate,bytes);
    const uint32_t expected=rides::checksum(bytes,HISTORY_BYTES-4);
    if(store.putBytes("history-v2",bytes,HISTORY_BYTES)!=HISTORY_BYTES||
       store.getBytes("history-v2",bytes,HISTORY_BYTES)!=HISTORY_BYTES||
       rides::readLE(bytes+HISTORY_BYTES-4,4)!=expected||
       rides::checksum(bytes,HISTORY_BYTES-4)!=expected)return DeleteResult::STORAGE_ERROR;
    history=candidate;return DeleteResult::SAVED;
}
}
