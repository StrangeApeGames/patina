// No LightUSD in this build (the third_party/LightUSD submodule was not checked out, or -DPATINA_USD=OFF).
#include "usd.h"

namespace pt {

std::vector<std::string> usd_formats() { return {"usda", "usdc", "usdz"}; }
bool usd_available() { return false; }

Json write_usd(const std::string&, const std::string&, const std::string&, const Mesh&, const std::vector<UsdMaterial>&) {
  fail("this build has no USD export");  // export checks usd_available() first and says how to get it
}

}  // namespace pt
