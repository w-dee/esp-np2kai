#include "np2runtime/np2runtime.hpp"

#include <algorithm>
#include <cstddef>

extern "C" {
#include <compiler.h>
#include <dosio.h>
#include <pccore.h>
}

namespace np2runtime {
namespace {

void clear_disk_paths() noexcept
{
    for (std::size_t index = 0; index < 4U; ++index) {
        np2cfg.fddfile[index][0] = '\0';
    }
    for (std::size_t index = 0; index < 2U; ++index) {
        np2cfg.sasihdd[index][0] = '\0';
    }
}

} // namespace

void apply_production_machine_config() noexcept
{
    const ProductionMachineConfig config = production_machine_config();

    /* This gives a deterministic baseline for fields not used by Slice A;
     * only the values below are production policy. */
    pccore_setdefault();

    file_cpyname(np2cfg.model, config.model.data(),
                 static_cast<int>(sizeof(np2cfg.model)));
    np2cfg.baseclock = config.baseclock;
    np2cfg.multiple = config.multiple;
    std::copy(config.dipsw.begin(), config.dipsw.end(), np2cfg.dipsw);
    std::copy(config.memsw.begin(), config.memsw.end(), np2cfg.memsw);
    np2cfg.EXTMEM = config.extmem_mb;
    np2cfg.memcheckspeed = config.memcheckspeed;
    np2cfg.ITF_WORK = config.itf_work;
    np2cfg.emuspeed = config.emuspeed;
    np2cfg.DISPSYNC = config.dispsync;
    std::copy(config.wait.begin(), config.wait.end(), np2cfg.wait);

    /* No external BIOS/font asset is part of this runtime slice. */
    np2cfg.usebios = config.usebios ? 1U : 0U;
    np2cfg.biospath[0] = '\0';
    np2cfg.fontfile[0] = '\0';
    np2cfg.fontface[0] = '\0';

    /* Keep Slice A independent of sound, MIDI, optional boards and media. */
    if (config.disable_sound) {
        np2cfg.SOUND_SW = 0U;
        np2cfg.MOTOR = 0U;
        np2cfg.MOTORVOL = 0U;
    }
    if (config.disable_midi) {
        np2cfg.mpuenable = 0U;
    }
    if (config.disable_optional_devices) {
        np2cfg.pc9861enable = 0U;
        np2cfg.hdrvenable = 0U;
        np2cfg.hdrvntenable = 0U;
        np2cfg.usefd144 = 0U;
    }
    if (config.clear_disk_paths) {
        clear_disk_paths();
    }

    /* fddequip intentionally remains the pccore default.  Slice C will set
     * and validate the FDD0-only equipment mask when FDD is composed. */
    if (config.fddequip_override.has_value()) {
        np2cfg.fddequip = *config.fddequip_override;
    }
}

} // namespace np2runtime
