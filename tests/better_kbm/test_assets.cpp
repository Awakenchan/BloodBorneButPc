#define main converter_main
#include "../../better-engine/tools/engine/menu_assets.cpp"
#undef main
#include <cassert>
#include <iostream>
Bytes place(int depth,int cid,int x,int y,std::optional<std::string> name,bool update=false){Place2 p;p.flags=4|(update?1:2);p.cid=cid;Matrix m;m.tx=x;m.ty=y;p.mat=m;return tag_bytes(kPlace2,build_place2(p,depth,m,name));}
Bytes sprite(int cid,int caption){Bytes b;app16(b,cid);app16(b,1);app(b,place(1,1,caption,0,"Text_0"));app(b,tag_bytes(kShowFrame,{}));app(b,tag_bytes(kEnd,{}));return tag_bytes(kDefineSprite,b);}
int main(){
 Bytes movie(13);movie[0]='F';movie[1]='W';movie[2]='S';movie[3]=10;app(movie,sprite(100,100));app(movie,sprite(101,200));
 Bytes body;app16(body,200);app16(body,2);
 for(int i=0;i<7;++i)app(body,place(i+1,i==6?101:100,i==6?10000:0,2000+i*1400,"Item_"+std::to_string(i)+"_0"));
 app(body,tag_bytes(kShowFrame,{}));
 for(int i=0;i<7;++i)app(body,place(i+1,0,i==6?10000:0,2100+i*1400,std::nullopt,true));
 app(body,tag_bytes(kShowFrame,{}));app(body,tag_bytes(kEnd,{}));app(movie,tag_bytes(kDefineSprite,body));
 app(movie,place(50,200,0,0,"ControllSetting"));app(movie,tag_bytes(kEnd,{}));put32(movie,4,movie.size());
 auto out=extend_vanilla(movie,"ControllSetting",2,true);auto sp=find_sprite(out,200);assert(sp);
 int frame=0;std::map<int,int> y;std::map<int,Place2> named;int updates=0;
 for(auto t:tags(out,sp->s+4,sp->e)){
  if(t.code==kShowFrame){++frame;continue;}if(t.code!=kPlace2)continue;
  auto p=parse_place2(out,t.s,t.e);assert(p.mat);
  if(!frame){y[p.depth]=p.mat->ty;if(p.name)named[std::stoi(p.name->substr(5))]=p;}
  else{assert(p.mat->ty==y.at(p.depth)+100);++updates;}
 }
 assert(named.size()==9&&updates==9&&frame==2);
 assert(named[7].cid==101&&named[8].cid==101); // true native action buttons
 assert(named[7].mat->ty==named[8].mat->ty);
 for(int i=1;i<7;++i)assert(named[i].mat->ty-named[i-1].mat->ty==1260);
 assert(named[7].mat->tx+caption_x(out,101)==named[0].mat->tx+caption_x(out,100)+600);
 assert(u32(out,4)==out.size());
 auto messages=read_messages();assert(messages.count(126000)&&messages.count(125000));assert(messages[125000].text!="Play with");
 std::cout<<"PASS: native action buttons, first/second columns, row spacing and all animation-frame updates; new IDs preserve author's Colour settings.\n";
}
