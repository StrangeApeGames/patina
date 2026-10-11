// No GPU backend on this platform (or the kernels could not be compiled at build time): the baker uses the CPU.
#include "gpu.h"

namespace pt {

Gpu* Gpu::get() { return nullptr; }
bool Gpu::enabled() { return false; }
void Gpu::warm_up() {}
std::string Gpu::unavailable_reason() { return "this build has no GPU backend"; }

}  // namespace pt
