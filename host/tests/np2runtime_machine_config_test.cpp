#include <cassert>
#include <cstdint>

#include "np2runtime/np2runtime.hpp"

int main()
{
    const auto config = np2runtime::production_machine_config();
    assert(config.model == "VX");
    assert(config.baseclock == 2457600U);
    assert(config.multiple == 20U);
    assert((config.dipsw == std::array<std::uint8_t, 3>{0x3e, 0xe3, 0x7b}));
    assert((config.memsw == std::array<std::uint8_t, 8>{
        0x48, 0x05, 0x04, 0x08, 0x01, 0x00, 0x00, 0x6e}));
    assert(config.extmem_mb == 8U);
    assert(!config.fddequip_override.has_value());
    assert(config.memcheckspeed == 8U);
    assert(config.itf_work == 1U);
    assert(config.emuspeed == 100U);
    assert(config.dispsync == 1U);
    assert((config.wait == std::array<std::uint8_t, 6>{1U, 1U, 6U, 1U, 8U, 1U}));
    assert(!config.usebios);
    assert(config.disable_sound);
    assert(config.disable_midi);
    assert(config.disable_optional_devices);
    assert(config.clear_disk_paths);
    return 0;
}
