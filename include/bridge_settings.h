#pragma once
#include <string.h>
#include <stdio.h>
#include "ride_history.h"
namespace app {
constexpr size_t MAX_RIDERS = 10, NAME_BYTES = 49;
constexpr uint16_t MAX_FTP_WATTS = 2000;
struct Rider {
    uint32_t id = 0; char name[NAME_BYTES] = "Guest";
    uint8_t sex = 0, age = 0; // 0 unspecified, 1 female, 2 male
    uint16_t weightDeciKg = 0, maxHR = 0; // 0 means not supplied
    uint16_t ftpWatts = 0; // 0 means not supplied
    char bandAddress[18] = "", bandName[64] = ""; uint8_t bandType = 0;
    uint64_t createKey = 0; // Optional request identity for retrying creation.
};
inline bool validName(const char* s) {
    const size_t n = strnlen(s, NAME_BYTES);
    if (!n || n >= NAME_BYTES) return false;
    bool visible = false;
    for (size_t i=0; i<n; ++i) {
        const uint8_t c = uint8_t(s[i]);
        if (c < 32 || c == 127) return false;
        if (c != ' ') visible = true;
        if (c < 128) continue;
        unsigned k = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        if (!k || i+k >= n) return false;
        const uint8_t d = uint8_t(s[i+1]);
        if ((c == 0xe0 && d < 0xa0) || (c == 0xed && d >= 0xa0) || (c == 0xf0 && d < 0x90) || (c == 0xf4 && d >= 0x90)) return false;
        while (k--) if ((uint8_t(s[++i]) & 0xc0) != 0x80) return false;
    }
    return visible;
}
struct Settings {
    Rider riders[MAX_RIDERS]; size_t count = 0;
    uint32_t nextID = 1, selectedID = 0;
    bool autoStart = true, autoStop = true, autoRider = false;
    uint16_t startS = 3, stopS = 10;
    char hrAddress[18] = "", hrName[64] = ""; uint8_t hrType = 0;
    const Rider* find(uint32_t id) const { for (size_t i=0;i<count;++i) if(riders[i].id==id)return &riders[i]; return nullptr; }
    const Rider* findCreatedRider(uint64_t key) const {
        if (key) for (size_t i=0;i<count;++i) if (riders[i].createKey==key) return &riders[i];
        return nullptr;
    }
    Rider selected() const { const auto* p=find(selectedID); return p ? *p : Rider{}; }
};
inline bool validRider(const Rider& r) {
    return r.id && validName(r.name) && r.sex <= 2 && r.age <= 120 && (!r.weightDeciKg || (r.weightDeciKg >= 10 && r.weightDeciKg <= 5000)) &&
        (!r.maxHR || (r.maxHR >= 30 && r.maxHR <= 250)) && r.ftpWatts <= MAX_FTP_WATTS;
}
inline bool validAddress(const char* p) {
    if (!p[0]) return true;
    if (strnlen(p,18)!=17) return false;
    for (unsigned i=0;i<17;++i) {
        if (i%3==2) { if(p[i]!=':')return false; }
        else if (!((p[i]>='0'&&p[i]<='9')||(p[i]>='a'&&p[i]<='f')||(p[i]>='A'&&p[i]<='F'))) return false;
    }
    return true;
}
inline bool sameBand(const char* a,uint8_t at,const char* b,uint8_t bt){
    if(!a[0]||!b[0]||at!=bt)return false;
    for(unsigned i=0;i<18;++i){const char x=a[i]>='A'&&a[i]<='F'?a[i]+32:a[i];const char y=b[i]>='A'&&b[i]<='F'?b[i]+32:b[i];if(x!=y)return false;if(!x)return true;}return false;
}
inline const Rider* ownerOf(const Settings& s,const char* address,uint8_t type){
    for(size_t i=0;i<s.count;++i)if(sameBand(s.riders[i].bandAddress,s.riders[i].bandType,address,type))return &s.riders[i];return nullptr;
}
inline Rider selectedBandRider(const Settings& s,uint32_t id){
    const auto* found=s.find(id);if(found)return *found;
    Rider guest;
    // Preserve a previously remembered, unassigned shared band for Guest only.
    if(!id&&s.hrAddress[0]&&!ownerOf(s,s.hrAddress,s.hrType)){
        memcpy(guest.bandAddress,s.hrAddress,18);memcpy(guest.bandName,s.hrName,64);guest.bandType=s.hrType;
    }return guest;
}
inline bool valid(const Settings& s) {
    if (s.count > MAX_RIDERS || !s.nextID || s.startS<1 || s.startS>600 || s.stopS<1 || s.stopS>600 ||
        !validAddress(s.hrAddress) || s.hrType>1 || strnlen(s.hrName,64)>=64 || (s.selectedID && !s.find(s.selectedID))) return false;
    for(size_t i=0;i<s.count;++i) {
        const auto& r=s.riders[i];
        if(!validRider(r)||r.id>=s.nextID||!validAddress(r.bandAddress)||r.bandType>1||strnlen(r.bandName,64)>=64||(!r.bandAddress[0]&&r.bandName[0]))return false;
        for(size_t j=0;j<i;++j)if(s.riders[j].id==r.id||sameBand(s.riders[j].bandAddress,s.riders[j].bandType,r.bandAddress,r.bandType))return false;
    }
    return true;
}
constexpr size_t SETTINGS_V1_BYTES=128+MAX_RIDERS*64+4;
constexpr size_t SETTINGS_BYTES=128+MAX_RIDERS*160+4;
// Earlier version 2 writers zero-filled these reserved fields.
constexpr size_t RIDER_FTP_OFFSET=144, RIDER_CREATE_KEY_OFFSET=146;
static_assert(RIDER_CREATE_KEY_OFFSET+sizeof(uint64_t)<=160,"Rider fields fit the v2 record");
inline void encode(const Settings& s, uint8_t* out) {
    memset(out,0,SETTINGS_BYTES); memcpy(out,"ABCF",4);out[4]=2;out[5]=s.count;out[106]=s.autoRider;
    rides::writeLE(out+8,s.nextID,4);rides::writeLE(out+12,s.selectedID,4);
    out[16]=s.autoStart;out[17]=s.autoStop;rides::writeLE(out+18,s.startS,2);rides::writeLE(out+20,s.stopS,2);
    out[22]=s.hrType;memcpy(out+23,s.hrAddress,18);memcpy(out+42,s.hrName,64);
    for(size_t i=0;i<s.count;++i){const auto& r=s.riders[i];auto* p=out+128+i*160;
        rides::writeLE(p,r.id,4);memcpy(p+4,r.name,NAME_BYTES);p[53]=r.sex;p[54]=r.age;
        rides::writeLE(p+56,r.weightDeciKg,2);rides::writeLE(p+58,r.maxHR,2);memcpy(p+60,r.bandAddress,18);p[78]=r.bandType;memcpy(p+80,r.bandName,64);
        rides::writeLE(p+RIDER_FTP_OFFSET,r.ftpWatts,2);
        rides::writeLE(p+RIDER_CREATE_KEY_OFFSET,r.createKey,sizeof(r.createKey));
    }
    rides::writeLE(out+SETTINGS_BYTES-4,rides::checksum(out,SETTINGS_BYTES-4),4);
}
inline bool decode(Settings& out, const uint8_t* p, size_t n) {
    if(!p || (n!=SETTINGS_BYTES && n!=SETTINGS_V1_BYTES) || memcmp(p,"ABCF",4) ||
        !((p[4]==1&&n==SETTINGS_V1_BYTES)||(p[4]==2&&n==SETTINGS_BYTES)) || (p[4]==2&&p[106]>1) || p[5]>MAX_RIDERS || p[16]>1 || p[17]>1 ||
        rides::readLE(p+n-4,4)!=rides::checksum(p,n-4))return false;
    Settings s;s.count=p[5];s.nextID=rides::readLE(p+8,4);s.selectedID=rides::readLE(p+12,4);
    s.autoStart=p[16];s.autoStop=p[17];s.startS=rides::readLE(p+18,2);s.stopS=rides::readLE(p+20,2);
    s.autoRider=p[4]==2&&p[106];s.hrType=p[22];memcpy(s.hrAddress,p+23,18);memcpy(s.hrName,p+42,64);
    for(size_t i=0;i<s.count;++i){auto& r=s.riders[i];const auto* q=p+128+i*(p[4]==1?64:160);
        r.id=rides::readLE(q,4);memcpy(r.name,q+4,NAME_BYTES);r.sex=q[53];r.age=q[54];
        r.weightDeciKg=rides::readLE(q+56,2);r.maxHR=rides::readLE(q+58,2);
        if(p[4]==2){
            memcpy(r.bandAddress,q+60,18);r.bandType=q[78];memcpy(r.bandName,q+80,64);
            r.ftpWatts=rides::readLE(q+RIDER_FTP_OFFSET,2);
            r.createKey=rides::readLE(q+RIDER_CREATE_KEY_OFFSET,sizeof(r.createKey));
        }
    }
    if(p[4]==1&&s.selectedID&&s.hrAddress[0]){
        for(size_t i=0;i<s.count;++i)if(s.riders[i].id==s.selectedID){
            memcpy(s.riders[i].bandAddress,s.hrAddress,18);memcpy(s.riders[i].bandName,s.hrName,64);s.riders[i].bandType=s.hrType;
        }
    }
    if(!valid(s))return false;out=s;return true;
}
}
