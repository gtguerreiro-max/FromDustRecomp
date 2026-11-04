#include "operations.h"
#include "replay_frame.h"
#include "renderer.h"
#include "agility_exports.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/xenia/third_party/stb/stb_image.h"
using namespace gpu::native;
std::string current;
struct Reader {
    std::vector<uint8_t> bytes;size_t at=0;
    std::span<const uint8_t> Take(size_t size){if(at>bytes.size()||size>bytes.size()-at)throw std::runtime_error("Truncated operation fixture");auto s=std::span(bytes).subspan(at,size);at+=size;return s;}
    uint32_t Word(){uint32_t result;auto b=Take(4);memcpy(&result,b.data(),4);return result;}
};
void Check(bool condition){if(!condition)throw std::runtime_error("Native operation mismatch: "+current);}
int main(int argc,char**argv){
    try{
        const bool render=argc>2&&std::string(argv[1])=="--render";
        const int first=render?3:1;
        if(argc<=first)return 2;
        if(render)Check(_putenv_s("FD_REPLAY_DEBUG","1")==0);
        Renderer renderer;
        if(render&&!renderer.LoadShaderCatalog(argv[2]))return 1;
        uint64_t clears=0,resolves=0,parsed=0,explicitRegisters=0;
        for(int arg=first;arg<argc;++arg){
            const std::string directory=argv[arg];auto frame=ReplayFrame::Load(directory);auto view=frame.View();
            std::vector<Draw> rebuilt(view.draws.begin(),view.draws.end());
            std::ifstream file(directory+"/operation-tests.bin",std::ios::binary);
            Reader r{{std::istreambuf_iterator<char>(file),{}}};
            Check(r.Word()==0x504F4446&&r.Word()==1);
            for(uint32_t count=r.Word();count;--count){
                const uint32_t index=r.Word(),kind=r.Word(),flags=r.Word(),color=r.Word(),stencil=r.Word();
                current=directory+" record "+std::to_string(index)+(kind?" resolve":" clear");
                float depth;auto b=r.Take(4);memcpy(&depth,b.data(),4);
                const uint32_t rectangleCount=r.Word(),parse=r.Word(),size=r.Word();const auto device=r.Take(size);
                std::vector<RenderRect> rectangles;
                for(uint32_t i=0;i<rectangleCount;++i){const int32_t l=int32_t(r.Word()),t=int32_t(r.Word()),rr=int32_t(r.Word()),bb=int32_t(r.Word());rectangles.emplace_back(l,t,rr,bb);}
                std::map<uint32_t,std::span<const uint8_t>> surfaces;
                for(uint32_t i=0;i<5;++i){const auto address=r.Word(),length=r.Word();surfaces[address]=r.Take(length);}
                std::array<uint32_t,6> fetch;for(auto& w:fetch)w=r.Word();
                CopyRegisters copy{r.Word(),r.Word(),r.Word(),r.Word()};const auto commands=r.Take(r.Word());
                auto source=SnapshotSurfaceState(device,[&](uint32_t address,uint32_t)->std::span<const uint8_t>{return surfaces.at(address);});
                const auto& expected=view.draws[index];Draw actual;
                if(!kind){
                    actual=SnapshotClear(std::move(source),flags,color,depth,stencil,rectangles);++clears;
                    for(auto reg:{RB_SURFACE_INFO,RB_COLOR_INFO,RB_COLOR1_INFO,RB_COLOR2_INFO,RB_COLOR3_INFO,RB_DEPTH_INFO,PA_SC_WINDOW_SCISSOR_TL,PA_SC_WINDOW_SCISSOR_BR,VPORT_XSCALE,VPORT_XOFFSET,VPORT_YSCALE,VPORT_YOFFSET,VPORT_ZSCALE,VPORT_ZOFFSET}) {
                        if((reg==RB_COLOR_INFO&&!(expected.clearFlags&1))||(reg==RB_COLOR1_INFO&&!(expected.clearFlags&2))||
                           (reg==RB_COLOR2_INFO&&!(expected.clearFlags&4))||(reg==RB_COLOR3_INFO&&!(expected.clearFlags&8))||
                           (reg==RB_DEPTH_INFO&&!(expected.clearFlags&0x30)))continue;
                        if(actual.regs[reg]!=expected.regs[reg])fprintf(stderr,"reg %u actual %08X expected %08X\n",reg,actual.regs[reg],expected.regs[reg]);
                        Check(actual.regs[reg]==expected.regs[reg]);
                    }
                    if(actual.clearFlags!=expected.clearFlags||actual.clearColor!=expected.clearColor||actual.clearDepth!=expected.clearDepth||actual.clearStencil!=expected.clearStencil||actual.clearRects.size()!=expected.clearRects.size())
                        fprintf(stderr,"flags %X/%X color %X/%X depth %g/%g stencil %u/%u rects %zu/%zu\n",actual.clearFlags,expected.clearFlags,actual.clearColor,expected.clearColor,actual.clearDepth,expected.clearDepth,actual.clearStencil,expected.clearStencil,actual.clearRects.size(),expected.clearRects.size());
                    Check(actual.clearFlags==expected.clearFlags&&actual.clearColor==expected.clearColor&&actual.clearDepth==expected.clearDepth&&actual.clearStencil==expected.clearStencil&&actual.clearRects.size()==expected.clearRects.size());
                    if(!rectangles.empty())Check(!memcmp(actual.clearRects.data(),expected.clearRects.data(),rectangles.size()*sizeof(RenderRect)));
                }else{
                    if(parse){const auto decoded=ParseCopyRegisters(commands);Check(!memcmp(&decoded,&copy,sizeof(copy)));++parsed;}else ++explicitRegisters;
                    actual=SnapshotResolve(std::move(source),flags,fetch,copy);++resolves;
                    const bool colorUsed=(flags&7)!=4;
                    if((colorUsed&&actual.copyColor!=expected.copyColor)||actual.copyDepth!=expected.copyDepth||actual.copySurface!=expected.copySurface||actual.copyDestInfo!=expected.copyDestInfo||actual.copyX!=expected.copyX||actual.copyY!=expected.copyY)
                        fprintf(stderr,"surface %X/%X color %X/%X depth %X/%X info %X/%X x %u/%u y %u/%u\n",actual.copySurface,expected.copySurface,actual.copyColor,expected.copyColor,actual.copyDepth,expected.copyDepth,actual.copyDestInfo,expected.copyDestInfo,actual.copyX,expected.copyX,actual.copyY,expected.copyY);
                    Check(actual.copyDest==expected.copyDest&&actual.copyControl==expected.copyControl&&actual.copySurface==expected.copySurface&&(!colorUsed||actual.copyColor==expected.copyColor)&&actual.copyDepth==expected.copyDepth&&actual.copyDestInfo==expected.copyDestInfo&&actual.copyWidth==expected.copyWidth&&actual.copyHeight==expected.copyHeight&&actual.copyX==expected.copyX&&actual.copyY==expected.copyY&&actual.copyMip==expected.copyMip&&actual.copyMipCount==expected.copyMipCount);
                }
                rebuilt.at(index)=std::move(actual);
            }
            if(render){
                view.draws=rebuilt;
                FrameOutput output;
                if(renderer.RenderFrame(view,{},&output))return 1;
                int width,height,channels;
                auto* baseline=stbi_load((directory+"/frame.png").c_str(),&width,&height,&channels,3);
                if(!baseline)throw std::runtime_error("Missing native baseline PNG");
                const bool equal=output.width==width&&output.height==height&&output.rgb.size()==size_t(width)*height*3&&
                    !memcmp(output.rgb.data(),baseline,output.rgb.size());
                stbi_image_free(baseline);
                Check(equal&&output.debugErrors==0);
            }
        }
        Check(clears&&resolves);printf("Native operations: %llu clears, %llu resolves match; %llu contiguous packets, %llu explicit GPU register fixtures\n",clears,resolves,parsed,explicitRegisters);
        if(render)printf("Native rebuilt operation frames: %llu frames, %llu draws, exact baseline RGB and zero debug errors\n",renderer.CompletedFrames(),renderer.CompletedDraws());
        return 0;
    }catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
}
