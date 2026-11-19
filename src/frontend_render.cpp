#include "stv/frontend.hpp"
namespace stv {
namespace {
Status valid_surface(Surface s){if(!s.pixels||!s.width||!s.height||s.stride<s.width||usize(s.height-1)*s.stride+s.width>s.pixel_capacity)return Status::fail("Invalid UI framebuffer");return {};}
void dot(Surface s,i32 x,i32 y,u32 color){if(x>=0&&y>=0&&u32(x)<s.width&&u32(y)<s.height)s.pixels[usize(y)*s.stride+u32(x)]=color;}
void rect(Surface s,i32 x,i32 y,i32 w,i32 h,u32 color){if(w<=0||h<=0)return;i32 x0=x<0?0:x,y0=y<0?0:y,x1=x+w,y1=y+h;if(x1>i32(s.width))x1=i32(s.width);if(y1>i32(s.height))y1=i32(s.height);for(i32 yy=y0;yy<y1;++yy)for(i32 xx=x0;xx<x1;++xx)dot(s,xx,yy,color);}
void line(Surface s,i32 x0,i32 y0,i32 x1,i32 y1,u32 color){i32 dx=x1>x0?x1-x0:x0-x1,dy=y1>y0?y0-y1:y1-y0,sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;for(;;){dot(s,x0,y0,color);if(x0==x1&&y0==y1)break;i32 twice=2*err;if(twice>=dy){err+=dy;x0+=sx;}if(twice<=dx){err+=dx;y0+=sy;}}}
void border(Surface s,i32 x0,i32 y0,i32 x1,i32 y1,u32 color,i32 width=3){rect(s,x0,y0,x1-x0+1,width,color);rect(s,x0,y1-width+1,x1-x0+1,width,color);rect(s,x0,y0,width,y1-y0+1,color);rect(s,x1-width+1,y0,width,y1-y0+1,color);}
u32 diag_color(u32 color){unsigned i=color&255;return i<source_ui_diagp_count?editor_color(source_ui_diagp[i]):0;}
void circle(Surface s,i32 cx,i32 cy,i32 radius,u32 color,bool fill){i32 x=0,y=radius,d=3-2*radius;while(x<=y){if(fill){rect(s,cx-x,cy+y,2*x,1,color);rect(s,cx-x,cy-y,2*x,1,color);rect(s,cx-y,cy+x,2*y,1,color);rect(s,cx-y,cy-x,2*y,1,color);}else{dot(s,cx+x,cy+y,color);dot(s,cx-x,cy+y,color);dot(s,cx+x,cy-y,color);dot(s,cx-x,cy-y,color);dot(s,cx+y,cy+x,color);dot(s,cx-y,cy+x,color);dot(s,cx+y,cy-x,color);dot(s,cx-y,cy-x,color);}if(d<0)d+=4*x+6;else{d+=4*(x-y)+10;--y;}++x;}}
Status image_by_name(const EditorImages& bank,const char* name,EditorImage& image){
    // Font7A and Font7a are different original symbols. Case folding would
    // conflate their artwork. Exact duplicate records must still agree.
    bool found=false;
    for(unsigned i=0;i<bank.count;++i){EditorImage candidate;auto status=bank.image(i,candidate);if(!status)return status;if(!text_equal(candidate.name,name))continue;
        if(found&&(image.width!=candidate.width||image.height!=candidate.height||image.anchor_x!=candidate.anchor_x||image.anchor_y!=candidate.anchor_y||image.selector!=candidate.selector||image.pixels.size!=candidate.pixels.size||!equal(image.pixels.data,candidate.pixels.data,image.pixels.size)))return Status::fail("Ambiguous exact-case source UI image",i);
        image=candidate;found=true;
    }
    return found?Status{}:Status::fail("Missing exact-case original UI image");
}
}
Status UiFont::bind(const SourcePack& pack,unsigned kind){if(kind>1)return Status::fail("Unknown original font");auto*e=pack.find(kind?"TROGF15.IMG":"TROGF7.IMG");if(!e)return Status::fail("Original menu font bank missing");auto s=bank.open(e->bytes);if(!s)return s;for(unsigned i=0;i<93;++i){s=image_by_name(bank,source_ui_font_names[kind][i],glyphs[i]);if(!s)return s;}return {};}
Status UiFont::measure(const char* text,unsigned spacing,u32& result)const{if(!text)return Status::fail("Null original font string");i64 total=0;for(usize i=0;text[i];++i){u8 c=u8(text[i]);if(c<32||c>125)return Status::fail("String outside original RD font table",i);total+=(c==32?5:glyphs[c-33].width)+signed16(u16(spacing));if(total<0||total>65535)return Status::fail("Original text extent outside supported range",i);}result=u32(total);return {};}
Status UiFont::draw(Surface s,const char*text,i32 x,i32 y,u32 color,unsigned spacing,unsigned just,bool anchors)const{
    auto v=valid_surface(s);if(!v)return v;u32 width=0;v=measure(text,spacing,width);if(!v)return v;if(just==1)x-=i32(width>>1);else if(just==2)x-=i32(width);else if(just)return Status::fail("Invalid original text justification");
    for(usize n=0;text[n];++n){unsigned c=u8(text[n]);if(c==32){x+=5+signed16(u16(spacing));continue;}const auto& g=glyphs[c-33];i32 gx=x-(anchors?g.anchor_x:0),gy=y-(anchors?g.anchor_y:0);for(unsigned row=0;row<g.height;++row)for(unsigned col=0;col<g.width;++col)if(g.pixels.data[usize(row)*g.stride+col])dot(s,gx+i32(col),gy+i32(row),color);x+=g.width+signed16(u16(spacing));}return {};
}
Status UiScene::bind(const SourcePack& pack,const UiSceneDefinition& d){
    if(!d.header_count||d.header_count>64||d.palette_count>32)return Status::fail("Linked scene limits invalid");
    char name[48]{};usize n=length(d.bank);if(n+5>sizeof name)return Status::fail("Linked scene name too long");copy(name,d.bank,n);copy(name+n,".BDD",5);auto*bdd=pack.find(name);name[n+3]='B';auto*bdb=pack.find(name);if(!bdd||!bdb)return Status::fail("Linked source presentation bank missing");EditorBackground b;auto st=b.open(bdd->bytes,bdb->bytes);if(!st)return st;if(b.block_count!=d.header_count)return Status::fail("Linked presentation block count differs from original editor bank");
    for(unsigned i=0;i<d.header_count;++i){const auto&h=d.headers[i];const auto&v=b.blocks[i];if(h.width!=v.width||h.height!=v.height||v.pixels.size!=usize(h.width)*h.height)return Status::fail("Linked presentation geometry differs from original block",i);images[i]={};images[i].width=h.width;images[i].height=h.height;images[i].stride=h.width;images[i].pixels=v.pixels;}
    for(unsigned i=0;i<d.placement_count;++i){const auto&p=d.placements[i];if(p.block>=d.header_count||p.palette>=d.palette_count)return Status::fail("Linked presentation pointer outside tables",i);for(usize k=0;k<images[p.block].pixels.size;++k)if(images[p.block].pixels.data[k]>=d.palettes[p.palette].count)return Status::fail("Presentation pixel outside its linked palette",i);}
    definition=&d;return {};
}
Status UiScene::draw(Surface out,u32 fade,unsigned age,bool cycles)const{
    if(!definition)return Status::fail("Linked scene not bound");
    auto s=valid_surface(out);if(!s)return s;const auto&d=*definition;u32 colors[32][256]{};
    for(unsigned p=0;p<d.palette_count;++p){const auto&pal=d.palettes[p];for(unsigned c=0;c<pal.count;++c){unsigned source=c;if(cycles&&text_equal(pal.name,"HSC")&&age>=24){unsigned first=0,last=0,rate=1;if(c>=50&&c<55){first=50;last=55;rate=5;}else if(c>=55&&c<58){first=55;last=58;rate=9;}if(last){unsigned rotate=((age-24)/rate)%(last-first);source=first+(c-first+(last-first)-rotate)%(last-first);}}if(!cycles&&text_equal(pal.name,"SDP1")&&age>=4&&c>=41&&c<55){unsigned rotate=((age-4)/4)%14;source=41+(c-41+14-rotate)%14;}colors[p][c]=editor_color(ui_fade_word(pal.words[source],fade));}}
    // Stable linked map depth order; no editor coordinates/palettes substituted.
    for(unsigned z=0;z<256;++z){for(unsigned i=0;i<d.placement_count;++i){const auto&p=d.placements[i];if((p.flags>>8)!=z)continue;const auto&im=images[p.block];s=draw_indexed(out,{im.pixels,im.width,im.height,im.stride},colors[p.palette],d.palettes[p.palette].count,{p.x+d.x,p.y+d.y,bool(p.flags&16),bool(p.flags&32),bool(p.flags&64)});if(!s)return s;}}
    return {};
}
Status ui_diag_text(Surface s,const char*text,i32 x,i32 y,u32 color,bool vertical){auto v=valid_surface(s);if(!v)return v;for(usize n=0;text[n];++n){unsigned c=u8(text[n]);if(c<32||c>=32+source_diag_glyph_count)return Status::fail("Character outside original SYSFONT",n);const auto&g=source_diag_glyphs[c-32];unsigned pitch=(g.width+7)/8;for(unsigned yy=0;yy<g.height;++yy)for(unsigned xx=0;xx<g.width;++xx)if(g.rows[yy*pitch+xx/8]&(1u<<(xx&7)))dot(s,x+i32(xx),y+i32(yy),color);if(vertical)y+=g.height+1;else x+=g.width+2;}return {};}
Status ui_diag_board(Surface s,const u8* states,usize count){auto v=valid_surface(s);if(!v)return v;if(states&&count!=source_diag_chip_count)return Status::fail("Board chip-state array length mismatch");for(unsigned i=0;i<source_cpu_vecs_count;++i){const auto&r=source_cpu_vecs[i];line(s,r.x0,r.y0,r.x1,r.y1,diag_color(r.color));}for(unsigned i=0;i<source_diag_chip_count;++i){const auto&c=source_diag_chips[i];u8 state=c.stuffed?(states?states[i]:0):3;if(state>3)return Status::fail("Invalid diagnostic chip state",i);u32 fill=state==1?diag_color(6):state==2?diag_color(3):state==3?diag_color(1):0;if(state)rect(s,c.x+1,c.y+1,c.w-1,c.h-1,fill);border(s,c.x,c.y,c.x+c.w,c.y+c.h,diag_color(2),1);dot(s,c.x+c.pin_x,c.y+c.pin_y,diag_color(2));v=ui_diag_text(s,c.text,c.x+c.tx,c.y+c.ty,state?0:diag_color(2),c.direction!=0);if(!v)return v;}return {};}
Status ui_switch_screen(Surface s,u32 closed,u8 auxiliary){auto v=valid_surface(s);if(!v)return v;for(unsigned i=0;i<source_but_table_count;++i){const auto&r=source_but_table[i];u32 value=r.extended?auxiliary:closed;bool on=(value&r.mask)!=0;circle(s,r.x,r.y,r.radius,diag_color(r.color),false);if(on)circle(s,r.x,r.y,r.radius-1,diag_color(r.color),true);}
    const char*dirs[4]={"UP","DOWN","LFT","RT"};const i32 dx[4]={-7,-20,-48,20},dy[4]={-27,20,-5,-5};for(unsigned i=0;i<source_joy_table_count;++i){const auto&r=source_joy_table[i];circle(s,r.x,r.y,r.radius,diag_color(r.color),true);for(unsigned k=0;k<4;++k)if(closed&(1u<<(r.shift+k))){v=ui_diag_text(s,dirs[k],r.x+dx[k],r.y+dy[k],diag_color(r.color));if(!v)return v;}}
    for(unsigned i=0;i<source_switch_text_count;++i){const auto&r=source_switch_text[i];v=ui_diag_text(s,r.text,r.x,r.y,diag_color(r.color),r.direction!=0);if(!v)return v;}line(s,0,185,400,185,diag_color(2));return {};
}
Status ui_monitor_pattern(Surface s,unsigned pattern){auto v=valid_surface(s);if(!v)return v;if(pattern<3){const unsigned col[3]={3,6,7};rect(s,0,0,i32(s.width),i32(s.height),diag_color(col[pattern]));return {};}
    if(pattern==3){for(unsigned x=0;x<8;++x){u16 w=source_ui_intensities[x*2];for(unsigned y=0;y<16;++y){rect(s,i32(x*49),i32(y*16),49,16,editor_color(w));w=u16(w-source_ui_intensities[x*2+1]);}}return {};}
    if(pattern==4){for(unsigned i=0;i<source_mon_recs_count;++i){const auto&r=source_mon_recs[i];rect(s,r.x0,r.y0,r.x1,r.y1,diag_color(r.color));}for(unsigned i=0;i<source_mon_vecs_count;++i){const auto&r=source_mon_vecs[i];line(s,r.x0,r.y0,r.x1,r.y1,diag_color(r.color));}for(unsigned i=0;i<source_mon_dot_count;++i){const auto&r=source_mon_dots[i];dot(s,r.x,r.y,diag_color(r.color));}return {};}
    return Status::fail("Unknown original monitor pattern");
}
Status UiFrontend::load(const SourcePack&pack){
    loaded=false;auto s=fonts[0].bind(pack,0);if(!s)return s;s=fonts[1].bind(pack,1);if(!s)return s;if(source_ui_scene_count!=2)return Status::fail("Original presentation scene set mismatch");for(unsigned i=0;i<2;++i){s=scenes[i].bind(pack,source_ui_scenes[i]);if(!s)return s;}
    auto*e=pack.find("SMASHLOG.IMG");if(!e)return Status::fail("Original title logo bank missing");s=title_art.open(e->bytes);if(!s)return s;const char*names[2]={"SMASH","TVBOX"};for(unsigned i=0;i<2;++i){s=image_by_name(title_art,names[i],title_images[i]);if(!s)return s;s=title_art.palette((title_images[i].selector&255)-3,title_palettes[i]);if(!s)return s;}
    e=pack.find("NUARENA3.IMG");if(!e)return Status::fail("Original AFONT score bank missing");s=score_art.open(e->bytes);if(!s)return s;for(unsigned i=0;i<10;++i){char name[7]={'A','F','O','N','T',char('0'+i),0};s=image_by_name(score_art,name,score_digits[i]);if(!s)return s;}s=score_art.palette((score_digits[0].selector&255)-3,score_palette);if(!s)return s;
    for(unsigned i=0;i<source_ui_menu_count;++i){auto&m=source_ui_menus[i];u32 extent=0;s=fonts[0].measure(m.title,2,extent);if(!s)return s;for(unsigned j=0;j<m.count;++j){s=fonts[0].measure(m.items[j].text,1,extent);if(!s)return s;}}
    loaded=true;return {};
}
u32 UiFrontend::ui_color(u32 c)const{unsigned index=c&255;if(index==62)return editor_color(spectrum[0]);if(index==63)return editor_color(spectrum[1]);return index<source_ui_diagp_count?editor_color(source_ui_diagp[index]):0;}
Status UiFrontend::draw_messages(Surface s,const char*key)const{bool found=false;for(unsigned i=0;i<source_ui_message_count;++i){const auto&m=source_ui_messages[i];if(!text_equal(key,m.key))continue;found=true;auto st=fonts[m.font].draw(s,m.text,m.x,m.y,ui_color(m.color),m.spacing,m.justification);if(!st)return st;}return found?Status{}:Status::fail("Compiled original message missing");}
Status UiFrontend::draw_revision(Surface s,const char*title,u32 color)const{border(s,10,9,384,44,ui_color(4));auto st=fonts[0].draw(s,title,200,14,ui_color(color),2,1);if(!st)return st;return fonts[0].draw(s,source_ui_revision,200,30,ui_color(5),2,1);}
Status UiFrontend::draw_menu(Surface out,bool editing)const{
    if(!menu)return Status::fail("Menu is not initialized");
    Status st;if(page!=UiPage::Confirm){st=draw_revision(out,menu->title,u32(menu->header[0]));if(!st)return st;}
    const auto&h=menu->header;i32 cx=h[1],y=h[2],step=h[3],bar_x=cx-h[8]/2-1,left=bar_x-5,right=bar_x+h[8]+4;
    const UiAdjustment*selected=editing?adjustment:ui_adjustment(menu->items[selection-1].routine);
    bool adjustment_menu=text_equal(menu->label,"MEN_ADJ")||text_equal(menu->label,"MEN_CUST");
    if(page==UiPage::Confirm){
        border(out,37,61,362,246,ui_color(4));const char*title=nullptr;
        if(menu_depth)title=menu_stack[menu_depth-1].menu->items[menu_stack[menu_depth-1].selection-1].text;
        if(!title)return Status::fail("Missing original confirmation title");
        st=fonts[0].draw(out,title,200,102,ui_color(62),2,1);if(!st)return st;const char*q=ui_literal("TEST.ASM:M_SURE");if(!q)return Status::fail("Original confirmation question missing");st=fonts[0].draw(out,q,200,128,ui_color(5),2,1);if(!st)return st;
    }else{
        border(out,left,47,right,75,ui_color(14));if(adjustment_menu){st=draw_messages(out,"ADJUST.ASM:ADJ_ADV");if(!st)return st;}else{st=fonts[0].draw(out,"SELECT WITH ANY STICK",cx,52,ui_color(14),1,1);if(!st)return st;st=fonts[0].draw(out,"ACTIVATE WITH ANY BUTTON",cx,62,ui_color(14),1,1);if(!st)return st;}
        if(editing){border(out,left,78,right,128,ui_color(2));border(out,left,125,right,248,ui_color(2));st=draw_messages(out,"ADJUST.ASM:MES_ADJUSTING");if(!st)return st;st=fonts[0].draw(out,menu->items[selection-1].text,cx,104,ui_color(62),1,1);if(!st)return st;st=draw_messages(out,"ADJUST.ASM:INST_TO_ADJ");if(!st)return st;st=draw_messages(out,"ADJUST.ASM:ADJ_ARROW");if(!st)return st;}
        else border(out,left,y+h[6]-5,right,y+i32(menu->count-1)*step+h[6]+h[5]+4,ui_color(u32(h[4])));
    }
    if(!editing){for(unsigned i=0;i<menu->count;++i){i32 row_y=y+i32(i)*step;u32 color=ui_color(u32(h[4]));if(i+1==selection){rect(out,bar_x,row_y+h[6],h[8],h[5],ui_color(button.active?4:2));color=ui_color(button.active?2:u32(h[9]));}st=fonts[0].draw(out,menu->items[i].text,cx,row_y,color,1,1);if(!st)return st;}}
    if(page==UiPage::Confirm)return {};
    if(selected){
        border(out,189,47,384,75,ui_color(13));st=fonts[0].draw(out,selected->title,287,56,ui_color(13),2,1);if(!st)return st;
        if(const auto*help=ui_help(selected->help)){i32 yy=78;const auto color=ui_color(help->color);border(out,189,yy,384,yy+i32(help->count*10+10),color);for(unsigned i=0;i<help->count;++i){st=fonts[0].draw(out,help->lines[i],help->justification?199:286,yy+6+i32(i*10),color,1,help->justification?0:1);if(!st)return st;}}
        if(text_equal(selected->formatter,"COIN_SET")&&settings.adjustments[21])border(out,189,158,384,248,ui_color(5));
        char text[160];ui_adjustment_text(settings,*selected,text,sizeof text);border(out,189,158,384,178,ui_color(5));st=fonts[0].draw(out,text,287,164,ui_color(editing?2:62),2,1);if(!st)return st;
        if(text_equal(selected->formatter,"COIN_SET")&&settings.adjustments[21]){
            const auto&cp=source_ui_coin_presets[settings.adjustments[1]-1];
            for(unsigned i=0;i<cp.line_count;++i){st=fonts[0].draw(out,cp.lines[i],287,188+i32(i*12),ui_color(2),1,1);if(!st)return st;}
            const char*slot=ui_literal("ADJUST.ASM:M_SLOTVAL");if(!slot)return Status::fail("Missing original slot label");
            st=fonts[0].draw(out,slot,287,213,ui_color(2),1,1);if(!st)return st;
            st=fonts[0].draw(out,cp.slots,287,225,ui_color(2),1,1);if(!st)return st;
        }
    }else if(const auto*help=ui_help(menu->items[selection-1].help)){
        i32 hx=right+2,hy=y+i32(selection-1)*step+h[6]+h[5]/2-i32(help->count*10+10)/2;u32 color=ui_color(help->color);border(out,hx,hy,384,hy+i32(help->count*10+10),color);
        for(unsigned i=0;i<help->count;++i){st=fonts[0].draw(out,help->lines[i],help->justification?hx+10:(hx+384)/2,hy+6+i32(i*10),color,1,help->justification?0:1);if(!st)return st;}
    }
    return {};
}
Status UiFrontend::draw_audits(Surface out)const{
    const char*title=ui_literal(audit_page?"MENU.ASM:MES_AUD":"MENU.ASM:MES_BOOK");
    if(!title)return Status::fail("Original audit title missing");
    auto st=draw_revision(out,title,audit_page?14:2);if(!st)return st;
    unsigned first=audit_page?audit_page+1:0,last=audit_page?first+1:2;
    for(unsigned ti=first;ti<last;++ti){
        if(ti>=source_ui_audit_count)return Status::fail("Audit table outside original list");
        const auto&t=source_ui_audits[ti];
        for(unsigned i=0;i<t.count;++i){char value[96];st=ui_audit_text(settings,t.rows[i],value,sizeof value);if(!st)return st;
            const i32 y=t.header[0]+i32(i)*t.header[1];
            st=fonts[0].draw(out,t.rows[i].text,t.header[2],y,ui_color(5),2,0);if(!st)return st;
            st=fonts[0].draw(out,value,t.header[3],y,ui_color(5),2,2);if(!st)return st;
        }
        if(ti==0&&settings.adjustments[9]){char money[96];st=ui_money_text(settings,money,sizeof money);if(!st)return st;
            const char*label=ui_literal("MENU.ASM:T_MONEY");if(!label)return Status::fail("Original money audit label missing");
            st=fonts[0].draw(out,label,10,142,ui_color(2),2,0);if(!st)return st;
            st=fonts[0].draw(out,money,384,142,ui_color(2),2,2);if(!st)return st;
        }
    }
    if(!menu)return Status::fail("Audit continuation menu missing");
    const auto&h=menu->header;
    for(unsigned i=0;i<menu->count;++i){i32 y=h[2]+i32(i)*h[3];u32 color=ui_color(u32(h[4]));
        if(i+1==selection){rect(out,h[1]-h[8]/2-1,y+h[6],h[8],h[5],ui_color(button.active?4:2));color=ui_color(button.active?2:u32(h[9]));}
        st=fonts[0].draw(out,menu->items[i].text,h[1],y,color,1,1);if(!st)return st;
    }
    return {};
}
Status UiFrontend::draw_highscores(Surface out,unsigned age)const{
    u32 fade=age<30?8+8*(age/2):128;if(age>=322)fade=(age-322)/2<16?120-8*((age-322)/2):0;
    auto s=scenes[0].draw(out,fade,age,true);if(!s)return s;if(age<22)return {};
    u32 colors[64];for(unsigned i=0;i<64;++i)colors[i]=editor_color(ui_fade_word(source_ui_nuarenap[i],fade));
    u32 color=source_ui_nup1_count>39?editor_color(ui_fade_word(source_ui_nup1[39],128)):0;
    for(unsigned t=0;t<2;++t){for(unsigned i=1;i<11;++i){const auto&r=settings.scores[t][i];i32 y=58+i32((i-1)*16),x=t?291:101;bool leading=true;for(unsigned d=0;d<8;++d){unsigned digit=(r.bcd>>((7-d)*4))&15;if(digit>9)digit=9;if(digit)leading=false;if(!leading){const auto&im=score_digits[digit];s=draw_indexed(out,{im.pixels,im.width,im.height,im.stride},colors,64,{x+i32(d*11),y,false,false,true});if(!s)return s;}}
        // HSTD INITOUT is direct DMA; it ignores animation origin offsets.
        s=fonts[1].draw(out,r.initials,t?238:49,y-2,color,2,0,false);if(!s)return s;
    }}
    return {};
}
Status UiFrontend::draw_title(Surface out,unsigned age)const{
    auto s=scenes[1].draw(out,128,age,false);if(!s)return s;if(age<150||age>=567)return {};
    i32 x=184,y=-96+5*i32(age<177?age-150:27);if(age>=477)x+=7*i32(age-477);
    for(unsigned i=0;i<2;++i){const auto&im=title_images[i];const auto&pal=title_palettes[i];u32 colors[256]{};for(unsigned c=0;c<pal.count;++c)colors[c]=editor_color(le16(pal.words.data+c*2));s=draw_indexed(out,{im.pixels,im.width,im.height,im.stride},colors,pal.count,{x-im.anchor_x,y-im.anchor_y,false,false,true});if(!s)return s;}return {};
}
Status UiFrontend::draw(Surface out)const{
    if(!loaded)return Status::fail("UI original assets not loaded");
    auto st=valid_surface(out);if(!st)return st;for(unsigned y=0;y<out.height;++y)zero(out.pixels+usize(y)*out.stride,usize(out.width)*4);
    switch(page){
    case UiPage::Window:{
        if(window_previous==UiPage::HighScores)st=draw_highscores(out,window_previous_age);
        else {st=draw_revision(out,source_ui_revision,62);if(st&&window_previous==UiPage::FactoryWait){st=draw_messages(out,"MENU.ASM:MESS_RESET");if(st)st=draw_messages(out,"MENU.ASM:MESS_FAC");}}
        if(!st)return st;
        i32 x,y;u16 w,h;opening.bounds(x,y,w,h);rect(out,x,y,w,h,0);return {};
    }
    case UiPage::Revision:return draw_revision(out,source_ui_revision,62);
    case UiPage::ResetWarning:st=draw_revision(out,source_ui_revision,62);if(!st)return st;return draw_messages(out,"MENU.ASM:MESS_RESET");
    case UiPage::FactoryNotice:case UiPage::FactoryWait:st=draw_revision(out,source_ui_revision,62);if(!st)return st;st=draw_messages(out,"MENU.ASM:MESS_RESET");if(!st)return st;return draw_messages(out,"MENU.ASM:MESS_FAC");
    case UiPage::HighScores:return draw_highscores(out,unsigned(ticks-page_started));
    case UiPage::Title:return draw_title(out,unsigned(ticks-page_started));
    case UiPage::Menu:case UiPage::Confirm:return draw_menu(out);
    case UiPage::Audit:return draw_audits(out);
    case UiPage::Adjustment:return draw_menu(out,true);
    case UiPage::SwitchTest:return ui_switch_screen(out,raw_closed,auxiliary);
    case UiPage::Pattern:return ui_monitor_pattern(out,pattern);
    case UiPage::BoardDiagram:return ui_diag_board(out,nullptr,0);
    case UiPage::Confirmation:{border(out,37,61,362,246,ui_color(4));const char*text=ui_literal(confirmation_text);if(text_equal(confirmation_text,"MENU.ASM:MESS_FFSCONF")){const char*keys[4]={"MENU.ASM:MESS_DEFCONF","MENU.ASM:MESS_CCNCONF","MENU.ASM:MESS_CAUDCONF","MENU.ASM:MESS_HSRCONF"};const unsigned cols[4]={2,5,4,14};for(unsigned i=0;i<4;++i){const char*p=ui_literal(keys[i]);if(!p)return Status::fail("Factory confirmation literal missing");st=fonts[0].draw(out,p,200,96+i32(i*16),ui_color(cols[i]),2,1);if(!st)return st;}return {};}
        if(!text)return Status::fail("Original confirmation text missing");
        return fonts[0].draw(out,text,200,115,ui_color(62),2,1);}
    case UiPage::Boundary:
        // Preserve the last translated screen. The Windows HOST header, not a
        // counterfeit arcade dialog, identifies the exact unfinished service.
        if(boundary_origin==UiPage::Menu)return draw_menu(out);
        if(boundary_origin==UiPage::Title)return draw_title(out,600);
        return {};
    case UiPage::Fault:return {};
    }return Status::fail("Unknown native UI page");
}
}
