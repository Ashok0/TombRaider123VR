// Read installed PDP tables and exercise the actual runtime pose-path builder.
// Usage: switch_pose_asset_tests.exe <game directory>. Does not modify assets.
#include "../src/SwitchPosePath.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
template<class T> T Read(const std::vector<char>& data,size_t at) {
    if(at+sizeof(T)>data.size()) throw std::runtime_error("asset table bounds");
    T value;std::memcpy(&value,data.data()+at,sizeof(value));return value;
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    using namespace tr::firstperson;
    int passed=0,unsupported=0;
    for(int game:{1,2,3}) for(const auto& file:std::filesystem::recursive_directory_iterator(std::filesystem::path(argv[1])/std::to_string(game)/"DATA")) {
        if(file.path().extension()!=".PDP") continue;
        std::ifstream stream(file.path(),std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(stream)),{});
        size_t cursor=0,offsets[7]{};uint32_t counts[7]{};
        const int strides[]={32,6,8,2,4,2,18};
        for(int t=0;t<7;++t) {
            counts[t]=Read<uint32_t>(data,cursor);cursor+=4;offsets[t]=cursor;
            cursor+=size_t(counts[t])*strides[t];if(cursor>data.size()) throw std::runtime_error("asset size");
        }
        std::vector<SwitchAnim> anims(counts[0]);
        for(size_t i=0;i<anims.size();++i) {
            auto& a=anims[i];const auto p=offsets[0]+i*32;
            const auto frame=Read<uint32_t>(data,p);
            if(frame>=counts[5]*2) throw std::runtime_error("frame offset");
            a.frames=reinterpret_cast<const int16_t*>(data.data()+offsets[5]+frame);
            // Disk records have a 32-bit frame offset; runtime expands it to a pointer.
            std::memcpy(reinterpret_cast<char*>(&a)+8,data.data()+p+4,28);
        }
        const auto* changes=reinterpret_cast<const SwitchChange*>(data.data()+offsets[1]);
        const auto* ranges=reinterpret_cast<const SwitchRange*>(data.data()+offsets[2]);
        const auto* commands=reinterpret_cast<const int16_t*>(data.data()+offsets[3]);
        for(uint32_t o=0;o<counts[6];++o) {
            const auto p=offsets[6]+o*18;const auto slot=Read<uint32_t>(data,p);
            if(slot!=(game==1 ? 55u : game==2 ? 104u : 129u)) continue;
            const int base=Read<int16_t>(data,p+16);
            for(int state:{0,1}) {
                int idle=-1;
                for(int i=base;i<base+4 && i<int(anims.size());++i)
                    if(anims[i].state==state && anims[i].first==anims[i].last) { idle=i;break; }
                SwitchPosePath path;
                const bool valid=idle>=0 && path.Build(anims.data(),changes,ranges,commands,
                    {int16_t(idle),anims[idle].first,int16_t(state)},1-state);
                if(!valid) { ++unsupported;std::cout<<"Native fallback: "<<file.path().filename()<<" state "<<state<<"\n";continue; }
                if(path.count<3 || path.poses[path.count-1].state!=1-state) return 1;
                ++passed;
            }
        }
    }
    std::cout<<"PASS: "<<passed<<" installed wall-lever paths; "<<unsupported<<" unsupported variants retain native controls\n";
    return passed>0 ? 0 : 1;
}
