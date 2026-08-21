#pragma once

#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl.h"
#include "storage_fatfs/storage_fatfs.hpp"

namespace storage_sdmmc {

inline constexpr char kMountPath[] = "/sdcard";
inline constexpr char kFileTransferRoot[] = "/sdcard/files";
inline constexpr char kStagingRoot[] = "/sdcard/.np2-staging";

inline constexpr storage_fatfs::RootConfig kSdmmcRootConfig{
    kFileTransferRoot,
    kStagingRoot,
    nullptr,
};

class SdmmcMountProvider final : public storage_fatfs::FatfsMountBackend {
public:
    ~SdmmcMountProvider() override;

    esp_err_t mount() override;
    esp_err_t unmount() override;
    bool mounted() const override { return mounted_; }

private:
    esp_err_t release_power();

    bool mounted_ = false;
    sd_pwr_ctrl_handle_t pwr_ctrl_handle_ = nullptr;
    sdmmc_card_t *card_ = nullptr;
    sdmmc_host_t host_{};
    sdmmc_slot_config_t slot_config_{};
};

} // namespace storage_sdmmc
