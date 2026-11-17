#pragma once
#include "diagnostics_model.h"
namespace diagnostic {
// New report (~66), one spare NVS page (126), next largest critical blob (~84), metadata margin.
// History/settings update sequentially and release their old entries for reclamation.
constexpr size_t STORAGE_RESERVE_ENTRIES=320;
enum class SaveResult:uint8_t {OK,NO_SPACE,WRITE_FAILED,VERIFY_FAILED};
template<class Store> SaveResult save(Store& store,const Compact& report,uint8_t* bytes){
    if(store.freeEntries()<STORAGE_RESERVE_ENTRIES)return SaveResult::NO_SPACE;
    encode(report,bytes);const uint32_t expected=rides::checksum(bytes,STORAGE_BYTES-4);
    if(store.putBytes("report-v1",bytes,STORAGE_BYTES)!=STORAGE_BYTES)return SaveResult::WRITE_FAILED;
    if(store.getBytes("report-v1",bytes,STORAGE_BYTES)!=STORAGE_BYTES||rides::checksum(bytes,STORAGE_BYTES-4)!=expected||
       rides::readLE(bytes+STORAGE_BYTES-4,4)!=expected)return SaveResult::VERIFY_FAILED;
    return SaveResult::OK;
}
}
