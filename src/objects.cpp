#include "stv/objects.hpp"
namespace stv {
void NativeObjects::initialize(){zero(this,sizeof *this);head=no_object;free_head=0;for(unsigned i=0;i<object_capacity;++i)cells[i].next=i+1<object_capacity?ObjectHandle(i+1):no_object;}
Status NativeObjects::allocate(ObjectHandle& out){out=no_object;if(free_head==no_object)return Status::fail("Original 420-object pool exhausted");auto h=free_head;auto& o=cells[h];free_head=o.next;o.clip=0;o.dma_xy=0;o.process=no_process;o.sprite.flags=0;o.allocated=true;o.listed=false;o.next=no_object;++count;out=h;return {};}
Status NativeObjects::insert(ObjectHandle h){if(h>=object_capacity||!cells[h].allocated||cells[h].listed)return Status::fail("Invalid INSOBJ handle",h);auto& o=cells[h];ObjectHandle* link=&head;unsigned iterations=0;while(*link!=no_object){if(*link>=object_capacity||++iterations>object_capacity)return Status::fail("Corrupt INSOBJ list");auto& next=cells[*link];i32 z=signed16(o.z),nz=signed16(next.z);if(z<nz||(z==nz&&signed32(o.sprite.position.y)<=signed32(next.sprite.position.y)))break;link=&next.next;}o.next=*link;*link=h;o.listed=true;return {};}
Status NativeObjects::erase(ObjectHandle h){if(h>=object_capacity||!cells[h].allocated)return Status::fail("DELOBJ inactive object",h);auto& o=cells[h];if(o.listed){ObjectHandle* link=&head;unsigned i=0;while(*link!=h){if(*link==no_object||*link>=object_capacity||++i>object_capacity)return Status::fail("DELOBJ object not on active list",h);link=&cells[*link].next;}*link=o.next;}o.next=free_head;free_head=h;o.allocated=false;o.listed=false;o.sprite.flags=u16(o.sprite.flags&~0x8000u);--count;return {};}
Status NativeObjects::validate()const{bool seen[object_capacity]{};unsigned active=0,free=0;auto h=head;while(h!=no_object){if(h>=object_capacity||seen[h]||!cells[h].allocated||!cells[h].listed)return Status::fail("Corrupt object active chain",h);seen[h]=true;++active;h=cells[h].next;}h=free_head;while(h!=no_object){if(h>=object_capacity||seen[h]||cells[h].allocated)return Status::fail("Corrupt object free chain",h);seen[h]=true;++free;h=cells[h].next;}unsigned unlisted=0;for(unsigned i=0;i<object_capacity;++i)if(!seen[i]){if(!cells[i].allocated||cells[i].listed)return Status::fail("Lost object slot",i);++unlisted;}if(active+unlisted!=count||active+unlisted+free!=object_capacity)return Status::fail("Object count mismatch");return {};}
bool NativeObjects::sort_pass(){if(head==no_object)return false;bool changed=false;ObjectHandle* preceding=&head;ObjectHandle current=head;while(cells[current].next!=no_object){ObjectHandle next=cells[current].next;auto& a=cells[current];auto& b=cells[next];i32 za=signed16(a.z),zb=signed16(b.z);if(zb<za||(zb==za&&signed32(b.sprite.position.y)<signed32(a.sprite.position.y))){a.next=b.next;b.next=current;*preceding=next;preceding=&b.next;changed=true;}else{preceding=&a.next;current=next;}}return changed;}
void NativeObjects::integrate(){for(auto h=head;h!=no_object;h=cells[h].next){auto& o=cells[h];advance_position(o.sprite.position); // NDSP1 DVEL: B_NOMOV test is commented out in supplied source.
}}
Status NativeObjects::draw(Surface out)const{for(auto h=head;h!=no_object;h=cells[h].next){auto s=draw_sprite(cells[h].sprite,out);if(!s)return s;}return {};}
Status source_ohulk_beginobj2(NativeProcesses& processes,NativeObjects& objects,ProcessHandle process,
                              const EditorImages& art,u16 hulk_type,
                              const SourceOhulkCoordinates& coordinates,ObjectHandle& out){
 out=no_object;
 if(process>=process_capacity||!processes.cells[process].active||
    processes.cells[process].id!=source_hulk_process_id)
  return Status::fail("OHULK BEGINOBJ2 requires an active HULKPID process");
 const char* image_name=hulk_type==0?"GB6W5A":"SNG6A";
 const int image=art.find(image_name);
 if(image<0)return Status::fail("IH1 source Hulk image is missing or ambiguous");
 EditorImage image_header;
 auto status=art.image(unsigned(image),image_header);if(!status)return status;
 const unsigned palette_index=image_header.selector&255u;
 if(palette_index<3)return Status::fail("IH1 source image has an invalid palette selector",palette_index);
 EditorPalette palette;
 status=art.palette(palette_index-3,palette);if(!status)return status;

 ObjectHandle handle;
 status=objects.allocate(handle);if(!status)return status;
 auto& object=objects.cells[handle];
 object.sprite={&art,unsigned(image),int(palette_index-3),source_hulk_initial_flags,{0,0,0,0}};
 object.process=process;object.id=source_hulk_object_id;object.z=source_hulk_initial_z;
 status=place_sprite_anchor(object.sprite,coordinates.a0_x_16_16,coordinates.a1_value);
 if(!status){objects.erase(handle);return status;}
 status=objects.insert(handle);
 if(!status){objects.erase(handle);return status;}
 // NDSP1:BEGINOBJ2 returns the newly inserted object in A8.
 processes.cells[process].saved[0]=handle;
 out=handle;
 return {};
}
Status source_pixel_overlap(const NativeSprite& a,const NativeSprite& b,bool& hit){hit=false;if(!a.art||!b.art)return Status::fail("Pixel scan needs source images");EditorImage ai,bi;auto s=sprite_image(a,ai);if(!s)return s;s=sprite_image(b,bi);if(!s)return s;i32 ax=signed16(u16(a.position.x>>16)),ay=signed16(u16(a.position.y>>16)),bx=signed16(u16(b.position.x>>16)),by=signed16(u16(b.position.y>>16));i32 l=ax>bx?ax:bx,t=ay>by?ay:by,r=ax+ai.width<bx+bi.width?ax+ai.width:bx+bi.width,bot=ay+ai.height<by+bi.height?ay+ai.height:by+bi.height;if(l>=r||t>=bot||r-l>=128||bot-t>=128)return {};
 // COLL2 PIXSCANG bounds use header width, but flipped source addressing uses
 // the rounded-up four-byte row width. This is deliberately NOT generic AABB.
 for(i32 y=t;y<bot;++y)for(i32 x=l;x<r;++x){unsigned x0=unsigned(x-ax),y0=unsigned(y-ay),x1=unsigned(x-bx),y1=unsigned(y-by);if(a.flags&16)x0=ai.stride-1-x0;if(a.flags&32)y0=ai.height-1-y0;if(b.flags&16)x1=bi.stride-1-x1;if(b.flags&32)y1=bi.height-1-y1;if(ai.pixels.data[usize(y0)*ai.stride+x0]&&bi.pixels.data[usize(y1)*bi.stride+x1]){hit=true;return {};}}return {};}
}
