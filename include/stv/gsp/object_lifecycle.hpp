#pragma once
#include <cstdint>

namespace stv::gsp {
struct VisibleMuzzle { int x{},y{}; float dx{},dy{}; unsigned weapon{}; };

// Native bit addresses, NDSP1 foreground object pool.
inline constexpr uint32_t object_pool_base=0x010003a0, object_stride=0x1f0, object_slots=420;
enum class ObjectEvent { Insert, Retag, Retire, Contact, Blast, SmartBombStart, SmartBombVictim, MachineWound };
}
