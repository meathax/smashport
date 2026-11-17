#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace stv::host {
// Optional display-only D3D12/Slang path. It never reads machine state.
class GpuFilter {
public:
    GpuFilter(); ~GpuFilter();
    bool render(const std::uint32_t* source, int sw, int sh, int dw, int dh,
                int preset, int glow, int sharpen, int curvature,
                std::vector<std::uint32_t>& output, std::string& error);
    void reset();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
// Latest-frame worker: bounded to one pending source and one completed picture.
// Shader compilation and readback never block the simulation/audio owner.
class GpuFilterWorker {
public:
    explicit GpuFilterWorker(std::function<void()> ready = {}); ~GpuFilterWorker();
    void submit(const std::uint32_t* source,int sw,int sh,int dw,int dh,int preset,int glow,int sharpen,int curvature);
    bool poll(std::vector<std::uint32_t>& output,std::string& error);
    void clear();
private:
    struct Impl; std::unique_ptr<Impl> impl_; std::function<void()> ready_;
};
}
