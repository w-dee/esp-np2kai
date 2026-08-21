#include "storage_sdmmc/storage_sdmmc.hpp"

#include "driver/sdmmc_default_configs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

namespace storage_sdmmc {
namespace {

constexpr char kLogTag[] = "storage_sdmmc";
constexpr int kLdoChannel = 4;
constexpr int kBusWidth = 4;

} // namespace

SdmmcMountProvider::~SdmmcMountProvider()
{
    if (mounted_) {
        const esp_err_t result = unmount();
        if (result != ESP_OK) {
            ESP_LOGE(kLogTag, "destructor unmount failed: %s", esp_err_to_name(result));
        }
    }

    if (!mounted_ && pwr_ctrl_handle_ != nullptr) {
        const esp_err_t result = release_power();
        if (result != ESP_OK) {
            ESP_LOGE(kLogTag, "destructor LDO cleanup failed: %s", esp_err_to_name(result));
        }
    }
}

esp_err_t SdmmcMountProvider::release_power()
{
    if (pwr_ctrl_handle_ == nullptr) return ESP_OK;

    const esp_err_t result = sd_pwr_ctrl_del_on_chip_ldo(pwr_ctrl_handle_);
    if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "LDO channel %d cleanup failed: %s", kLdoChannel,
                 esp_err_to_name(result));
        return result;
    }

    pwr_ctrl_handle_ = nullptr;
    return ESP_OK;
}

esp_err_t SdmmcMountProvider::mount()
{
    if (mounted_) return ESP_ERR_INVALID_STATE;

    // A previous cleanup failure retains the handle so the next lifecycle
    // attempt can retry cleanup before acquiring another LDO channel.
    const esp_err_t previous_cleanup = release_power();
    if (previous_cleanup != ESP_OK) return previous_cleanup;

    const sd_pwr_ctrl_ldo_config_t ldo_config{
        .ldo_chan_id = kLdoChannel,
    };
    esp_err_t result = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle_);
    if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "LDO channel %d setup failed: %s", kLdoChannel,
                 esp_err_to_name(result));
        return result;
    }

    host_ = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
    host_.slot = SDMMC_HOST_SLOT_1;
    host_.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host_.pwr_ctrl_handle = pwr_ctrl_handle_;

    slot_config_ = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config_.width = kBusWidth;
    slot_config_.d4 = GPIO_NUM_NC;
    slot_config_.d5 = GPIO_NUM_NC;
    slot_config_.d6 = GPIO_NUM_NC;
    slot_config_.d7 = GPIO_NUM_NC;
    slot_config_.cd = SDMMC_SLOT_NO_CD;
    slot_config_.wp = SDMMC_SLOT_NO_WP;
    slot_config_.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 8;

    card_ = nullptr;
    result = esp_vfs_fat_sdmmc_mount(kMountPath, &host_, &slot_config_,
                                     &mount_config, &card_);
    if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "mount failed at %s (slot=%d width=%d freq=%d kHz): %s",
                 kMountPath, host_.slot, slot_config_.width, host_.max_freq_khz,
                 esp_err_to_name(result));

        // The high-level IDF API has already rolled back card, FAT/VFS,
        // slot, and host resources before returning the error. The provider
        // still owns the external LDO handle and releases it here.
        const esp_err_t cleanup = release_power();
        if (cleanup != ESP_OK) return cleanup;
        return result;
    }

    mounted_ = true;
    ESP_LOGI(kLogTag, "mounted %s (slot=%d width=%d configured_freq=%d kHz)",
             kMountPath, host_.slot, slot_config_.width, host_.max_freq_khz);
    return ESP_OK;
}

esp_err_t SdmmcMountProvider::unmount()
{
    if (!mounted_) return ESP_ERR_INVALID_STATE;

    const esp_err_t result = esp_vfs_fat_sdcard_unmount(kMountPath, card_);
    // With SDMMC_HOST_DEFAULT(), the high-level IDF unmount path calls
    // sdmmc_host_deinit_slot(host.slot) through DEINIT_ARG. Do not repeat
    // host or slot deinitialization here. The IDF path also consumes its
    // internal context before returning, so do not retain card_ for retry
    // when it reports an error.
    card_ = nullptr;
    mounted_ = false;

    const esp_err_t cleanup = release_power();
    if (result != ESP_OK) {
        ESP_LOGE(kLogTag, "unmount failed at %s: %s", kMountPath,
                 esp_err_to_name(result));
        if (cleanup != ESP_OK) {
            ESP_LOGE(kLogTag, "unmount LDO cleanup also failed: %s",
                     esp_err_to_name(cleanup));
        }
        return result;
    }

    if (cleanup != ESP_OK) return cleanup;

    ESP_LOGI(kLogTag, "unmounted %s (slot=%d)", kMountPath, host_.slot);
    return ESP_OK;
}

} // namespace storage_sdmmc
