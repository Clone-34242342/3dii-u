"""Fractional row/column copies cover adjacent regions without gaps."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
cpp=r'''
#include <cassert>
#include <cmath>
#include "scaled_copy.h"
int main(){
 for(float scale:{0.75f,1.0f,1.9230769f,2.8846154f,3.0f}){
  for(unsigned total:{240u,400u,416u,512u}){
   for(unsigned chunk:{1u,7u,16u,64u,128u}){
    unsigned end=0,sum=0;
    for(unsigned first=0;first<total;first+=chunk){
     auto span=gx2_copy::span(first,std::min(chunk,total-first),scale);
     assert(span.first==end);sum+=span.size();end=span.last;
    }
    assert(sum==static_cast<unsigned>(total*scale));
   }
  }
 }
 // Source padding uses rounded physical extents, not logical tex_h ratios.
 const float scale=1.9230769f;unsigned height=416,texheight=512;
 unsigned sh=texheight*scale,row0=sh-static_cast<unsigned>(height*scale);
 auto s=gx2_copy::span(0,height,scale);
 assert(gx2_copy::uv(row0+s.last,sh)==1.0f);
 assert(gx2_copy::uv(row0+s.first,sh)>0.0f);
 assert(std::abs(gx2_copy::uv(row0,sh)-float(texheight-height)/texheight)>0.0001f);
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.cpp').write_text('#include <algorithm>\n#include <initializer_list>\n'+cpp)
 subprocess.run(['c++','-std=c++20','-I'+str(root/'port/wiiu'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS: adjacent fractional copies cover their complete extent; source UVs respect rounded padding')
