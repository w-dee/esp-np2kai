#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace p4_nano_board {

/*
 * Host-testable ownership ledger for the P4-NANO's one physical I2C bus.
 *
 * Callers own a Lease object, while this ledger owns the bus lifetime.  The
 * callback layer is deliberately expressed in integer tokens so this header
 * remains independent of ESP-IDF and can be compiled by the host tests.
 */
enum class SharedI2cLeaseResult {
    kOk,
    kInvalidArgument,
    kInvalidOperations,
    kDuplicateAddress,
    kNoLeaseSlots,
    kBusCreateFailed,
    kDeviceCreateFailed,
    kDeviceDeleteFailed,
    kShutdownBusy,
    kBusDeleteFailed,
};

struct SharedI2cLeaseOps {
    using CreateBus = int (*)(void *context, std::uintptr_t *bus_token);
    using DeleteBus = int (*)(void *context, std::uintptr_t bus_token);
    using CreateDevice = int (*)(void *context, std::uintptr_t bus_token,
                                 std::uint8_t address,
                                 std::uintptr_t *device_token);
    using DeleteDevice = int (*)(void *context,
                                 std::uintptr_t device_token);

    CreateBus create_bus = nullptr;
    DeleteBus delete_bus = nullptr;
    CreateDevice create_device = nullptr;
    DeleteDevice delete_device = nullptr;
    void *context = nullptr;
};

class SharedI2cLeaseModel {
public:
    static constexpr std::size_t kMaxLeases = 4;

    class Lease {
    public:
        Lease() = default;
        Lease(const Lease &) = delete;
        Lease &operator=(const Lease &) = delete;
        Lease(Lease &&) = delete;
        Lease &operator=(Lease &&) = delete;

        bool is_active() const noexcept { return active_; }
        std::uint8_t address() const noexcept { return address_; }
        std::uintptr_t token() const noexcept { return device_token_; }

    private:
        friend class SharedI2cLeaseModel;

        void clear() noexcept
        {
            owner_ = nullptr;
            address_ = 0;
            device_token_ = 0;
            active_ = false;
        }

        SharedI2cLeaseModel *owner_ = nullptr;
        std::uint8_t address_ = 0;
        std::uintptr_t device_token_ = 0;
        bool active_ = false;
    };

    explicit SharedI2cLeaseModel(SharedI2cLeaseOps ops) noexcept : ops_(ops) {}

    SharedI2cLeaseResult acquire(std::uint8_t address,
                                 Lease *out) noexcept
    {
        if (out == nullptr || out->active_ || out->owner_ != nullptr) {
            return SharedI2cLeaseResult::kInvalidArgument;
        }
        if (!ops_complete()) {
            return SharedI2cLeaseResult::kInvalidOperations;
        }
        if (has_address(address)) {
            return SharedI2cLeaseResult::kDuplicateAddress;
        }

        std::size_t slot = kMaxLeases;
        for (std::size_t index = 0; index < kMaxLeases; ++index) {
            if (leases_[index] == nullptr) {
                slot = index;
                break;
            }
        }
        if (slot == kMaxLeases) {
            return SharedI2cLeaseResult::kNoLeaseSlots;
        }

        bool created_bus = false;
        if (!bus_alive_) {
            std::uintptr_t bus_token = 0;
            if (ops_.create_bus(ops_.context, &bus_token) != 0 ||
                bus_token == 0) {
                return SharedI2cLeaseResult::kBusCreateFailed;
            }
            bus_token_ = bus_token;
            bus_alive_ = true;
            created_bus = true;
        }

        std::uintptr_t device_token = 0;
        if (ops_.create_device(ops_.context, bus_token_, address,
                               &device_token) != 0 || device_token == 0) {
            if (device_token != 0) {
                (void)ops_.delete_device(ops_.context, device_token);
            }
            if (created_bus) {
                if (ops_.delete_bus(ops_.context, bus_token_) == 0) {
                    bus_token_ = 0;
                    bus_alive_ = false;
                } else {
                    return SharedI2cLeaseResult::kBusDeleteFailed;
                }
            }
            return SharedI2cLeaseResult::kDeviceCreateFailed;
        }

        out->owner_ = this;
        out->address_ = address;
        out->device_token_ = device_token;
        out->active_ = true;
        leases_[slot] = out;
        ++lease_count_;
        return SharedI2cLeaseResult::kOk;
    }

    SharedI2cLeaseResult release(Lease *lease) noexcept
    {
        const std::size_t slot = find_slot(lease);
        if (slot == kMaxLeases) {
            return SharedI2cLeaseResult::kInvalidArgument;
        }
        if (ops_.delete_device(ops_.context, lease->device_token_) != 0) {
            return SharedI2cLeaseResult::kDeviceDeleteFailed;
        }
        leases_[slot] = nullptr;
        --lease_count_;
        lease->clear();
        return SharedI2cLeaseResult::kOk;
    }

    SharedI2cLeaseResult shutdown() noexcept
    {
        if (lease_count_ != 0) {
            return SharedI2cLeaseResult::kShutdownBusy;
        }
        if (!bus_alive_) {
            return SharedI2cLeaseResult::kOk;
        }
        if (ops_.delete_bus(ops_.context, bus_token_) != 0) {
            return SharedI2cLeaseResult::kBusDeleteFailed;
        }
        bus_token_ = 0;
        bus_alive_ = false;
        return SharedI2cLeaseResult::kOk;
    }

    bool bus_alive() const noexcept { return bus_alive_; }
    std::size_t lease_count() const noexcept { return lease_count_; }

    bool owns(const Lease *lease) const noexcept
    {
        return find_slot(lease) != kMaxLeases;
    }

private:
    bool ops_complete() const noexcept
    {
        return ops_.create_bus != nullptr && ops_.delete_bus != nullptr &&
               ops_.create_device != nullptr &&
               ops_.delete_device != nullptr;
    }

    bool has_address(std::uint8_t address) const noexcept
    {
        for (const Lease *lease : leases_) {
            if (lease != nullptr && lease->address_ == address) {
                return true;
            }
        }
        return false;
    }

    std::size_t find_slot(const Lease *lease) const noexcept
    {
        if (lease == nullptr || lease->owner_ != this || !lease->active_) {
            return kMaxLeases;
        }
        for (std::size_t index = 0; index < kMaxLeases; ++index) {
            if (leases_[index] == lease) {
                return index;
            }
        }
        return kMaxLeases;
    }

    SharedI2cLeaseOps ops_{};
    std::array<Lease *, kMaxLeases> leases_{};
    std::uintptr_t bus_token_ = 0;
    std::size_t lease_count_ = 0;
    bool bus_alive_ = false;
};

} // namespace p4_nano_board
