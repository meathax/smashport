#pragma once
// Development observer: raw CPU-addressed RAM only, no Machine reads or writes.
#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace stv::content {
struct RamView {
    const std::vector<uint16_t>& words;
    bool read(uint32_t address, unsigned width, uint32_t& value) const {
        constexpr uint32_t base = 0x01000000u;
        if (address < base || width == 0 || width > 32) return false;
        const uint64_t offset = uint64_t(address) - base;
        if (offset + width > uint64_t(words.size()) * 16) return false;
        value = 0;
        // Word fields can start on a byte boundary (WPN_TYP is +0x198).
        for (unsigned i=0; i<width; ++i)
            value |= uint32_t((words[std::size_t((offset+i)/16)] >> ((offset+i)%16)) & 1) << i;
        return true;
    }
};

class Observer {
public:
    explicit Observer(const std::string& path) : output_(path, std::ios::binary) {
        output_ << "{\"schema\":\"stv.content.samples.v1\",\"cadence\":60,\"phase\":\"before_main_region_dump\",\"process_pdata198\":\"raw_unbound_field\"}\n";
    }
    bool good() const { return output_.good(); }
    bool sample(uint32_t frame, const std::vector<uint16_t>& ram) {
        if (frame % 60) return good();
        RamView view{ram};
        uint32_t gs=0,circuit=0,wave=0,world=0,p1lives=0,p2lives=0,objects=0,processes=0;
        // Retail addresses validated by existing native harness and C3 audits;
        // CIRCUIT/WAVE differ from the surviving source's RAM map by 16 bits.
        if (!view.read(0x010865a0,16,gs) || !view.read(0x010eb610,16,circuit) || !view.read(0x010eb630,16,wave)
            || !view.read(0x01000280,32,world) || !view.read(0x01086360,16,p1lives) || !view.read(0x01086520,16,p2lives)
            || !view.read(0x01000040,32,objects) || !view.read(0x01000000,32,processes)) return false;
        output_ << "{\"frame\":" << frame << ",\"gamstate\":" << gs << ",\"circuit\":" << circuit << ",\"wave\":" << wave
                << ",\"world_x\":" << int(int16_t(uint16_t(world))) << ",\"world_y\":" << int(int16_t(uint16_t(world>>16)))
                << ",\"p1_lives\":" << p1lives << ",\"p2_lives\":" << p2lives << ",\"objects\":[";
        const bool object_complete = list(view,objects,false);
        output_ << "],\"object_list_complete\":" << (object_complete ? "true" : "false") << ",\"processes\":[";
        const bool process_complete = list(view,processes,true);
        output_ << "],\"process_list_complete\":" << (process_complete ? "true" : "false") << "}\n";
        return good();
    }
private:
    bool list(const RamView& view, uint32_t node, bool process) {
        std::set<uint32_t> visited;
        bool first=true;
        while (node) {
            uint32_t next=0,a=0,b=0,c=0,d=0,e=0,f=0,g=0,h=0;
            const bool valid = (node & 15)==0 && visited.insert(node).second && view.read(node,32,next)
                && (process ? (view.read(node+0x20,16,a) && view.read(node+0xe0,32,b) && view.read(node+0x40,32,c)
                    && view.read(node+0x198,16,d) && view.read(node+0xc0,32,e) && view.read(node+0xa0,32,f)
                    && view.read(node+0x80,32,g) && view.read(node+0x60,32,h))
                  : (view.read(node+0x150,16,a) && view.read(node+0x130,32,b)
                    && view.read(node+0x70,16,c) && view.read(node+0x90,16,d)));
            if (!first) output_ << ',';
            first=false;
            if (!valid) {
                output_ << "{\"boundary_address\":" << node << ",\"reason\":\"invalid_or_cyclic_ram_node\"}";
                return false;
            }
            output_ << "{\"address\":" << node;
            if (process) output_ << ",\"pid\":" << a << ",\"wake\":" << b << ",\"saved_sp\":" << c
                << ",\"pdata198_raw\":" << d << ",\"saved_a8\":" << e << ",\"saved_a9\":" << f
                << ",\"saved_a10\":" << g << ",\"saved_a11\":" << h;
            else output_ << ",\"oid\":" << a << ",\"image\":" << b << ",\"x\":" << int(int16_t(uint16_t(c)))
                << ",\"y\":" << int(int16_t(uint16_t(d)));
            output_ << '}';
            node=next;
        }
        return true;
    }
    std::ofstream output_;
};
}
