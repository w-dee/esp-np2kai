#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>

#include "p4_nano_board/pa_service_model.hpp"
#include "p4_nano_board/shared_i2c_lease_model.hpp"

namespace {

struct FakeI2c {
    int bus_creates = 0;
    int bus_deletes = 0;
    int device_creates = 0;
    int device_deletes = 0;
    int fail_bus_create = 0;
    int fail_device_create_call = 0;
    bool fail_device_delete = false;
    bool fail_bus_delete = false;
    std::array<std::uintptr_t, 4> live_devices{};

    static int create_bus(void *context, std::uintptr_t *token)
    {
        auto *fake = static_cast<FakeI2c *>(context);
        ++fake->bus_creates;
        if (fake->fail_bus_create != 0 || token == nullptr) {
            return 1;
        }
        *token = 0x1000U;
        return 0;
    }

    static int delete_bus(void *context, std::uintptr_t token)
    {
        auto *fake = static_cast<FakeI2c *>(context);
        ++fake->bus_deletes;
        if (fake->fail_bus_delete || token == 0) {
            return 1;
        }
        return 0;
    }

    static int create_device(void *context, std::uintptr_t bus_token,
                             std::uint8_t, std::uintptr_t *token)
    {
        auto *fake = static_cast<FakeI2c *>(context);
        ++fake->device_creates;
        if (bus_token == 0 || token == nullptr ||
            fake->fail_device_create_call == fake->device_creates) {
            return 1;
        }
        *token = 0x2000U + static_cast<std::uintptr_t>(fake->device_creates);
        fake->live_devices[fake->device_creates - 1] = *token;
        return 0;
    }

    static int delete_device(void *context, std::uintptr_t token)
    {
        auto *fake = static_cast<FakeI2c *>(context);
        ++fake->device_deletes;
        if (fake->fail_device_delete || token == 0) {
            return 1;
        }
        for (auto &live : fake->live_devices) {
            if (live == token) {
                live = 0;
                return 0;
            }
        }
        return 1;
    }

    p4_nano_board::SharedI2cLeaseOps ops() noexcept
    {
        return {
            .create_bus = create_bus,
            .delete_bus = delete_bus,
            .create_device = create_device,
            .delete_device = delete_device,
            .context = this,
        };
    }
};

void assert_clean(const FakeI2c &fake)
{
    assert(fake.bus_creates == 1);
    assert(fake.bus_deletes == 1);
    assert(fake.device_creates == 2);
    assert(fake.device_deletes == 2);
    for (const auto token : fake.live_devices) {
        assert(token == 0);
    }
}

void run_acquisition_and_release_order(std::uint8_t first_address,
                                        std::uint8_t second_address,
                                        bool release_first_lease)
{
    FakeI2c fake;
    p4_nano_board::SharedI2cLeaseModel model(fake.ops());
    p4_nano_board::SharedI2cLeaseModel::Lease first;
    p4_nano_board::SharedI2cLeaseModel::Lease second;
    assert(model.acquire(first_address, &first) ==
           p4_nano_board::SharedI2cLeaseResult::kOk);
    assert(model.acquire(second_address, &second) ==
           p4_nano_board::SharedI2cLeaseResult::kOk);
    assert(model.bus_alive());
    assert(model.lease_count() == 2);
    assert(fake.bus_creates == 1);
    assert(fake.device_creates == 2);

    auto *release_first = release_first_lease ? &first : &second;
    auto *release_second = release_first_lease ? &second : &first;
    assert(model.release(release_first) ==
           p4_nano_board::SharedI2cLeaseResult::kOk);
    assert(model.bus_alive());
    assert(model.lease_count() == 1);
    assert(release_second->is_active());
    assert(model.shutdown() ==
           p4_nano_board::SharedI2cLeaseResult::kShutdownBusy);
    assert(model.release(release_second) ==
           p4_nano_board::SharedI2cLeaseResult::kOk);
    assert(model.bus_alive());
    assert(model.shutdown() == p4_nano_board::SharedI2cLeaseResult::kOk);
    assert(!model.bus_alive());
    assert_clean(fake);
}

void test_failure_paths()
{
    {
        FakeI2c fake;
        fake.fail_bus_create = 1;
        p4_nano_board::SharedI2cLeaseModel model(fake.ops());
        p4_nano_board::SharedI2cLeaseModel::Lease lease;
        assert(model.acquire(0x45, &lease) ==
               p4_nano_board::SharedI2cLeaseResult::kBusCreateFailed);
        assert(!model.bus_alive());
        assert(model.release(&lease) ==
               p4_nano_board::SharedI2cLeaseResult::kInvalidArgument);
    }
    {
        FakeI2c fake;
        fake.fail_device_create_call = 1;
        p4_nano_board::SharedI2cLeaseModel model(fake.ops());
        p4_nano_board::SharedI2cLeaseModel::Lease lease;
        assert(model.acquire(0x45, &lease) ==
               p4_nano_board::SharedI2cLeaseResult::kDeviceCreateFailed);
        assert(!model.bus_alive());
        assert(fake.bus_deletes == 1);
    }
    {
        FakeI2c fake;
        fake.fail_device_create_call = 2;
        p4_nano_board::SharedI2cLeaseModel model(fake.ops());
        p4_nano_board::SharedI2cLeaseModel::Lease first;
        p4_nano_board::SharedI2cLeaseModel::Lease second;
        assert(model.acquire(0x45, &first) ==
               p4_nano_board::SharedI2cLeaseResult::kOk);
        assert(model.acquire(0x18, &second) ==
               p4_nano_board::SharedI2cLeaseResult::kDeviceCreateFailed);
        assert(model.bus_alive());
        assert(model.lease_count() == 1);
        assert(model.release(&first) ==
               p4_nano_board::SharedI2cLeaseResult::kOk);
        assert(model.shutdown() == p4_nano_board::SharedI2cLeaseResult::kOk);
    }
    {
        FakeI2c fake;
        p4_nano_board::SharedI2cLeaseModel model(fake.ops());
        p4_nano_board::SharedI2cLeaseModel::Lease first;
        p4_nano_board::SharedI2cLeaseModel::Lease duplicate;
        assert(model.acquire(0x45, &first) ==
               p4_nano_board::SharedI2cLeaseResult::kOk);
        assert(model.acquire(0x45, &duplicate) ==
               p4_nano_board::SharedI2cLeaseResult::kDuplicateAddress);
        assert(model.shutdown() ==
               p4_nano_board::SharedI2cLeaseResult::kShutdownBusy);
        assert(model.release(&first) ==
               p4_nano_board::SharedI2cLeaseResult::kOk);
        assert(model.shutdown() == p4_nano_board::SharedI2cLeaseResult::kOk);
    }
    {
        FakeI2c fake;
        p4_nano_board::SharedI2cLeaseModel model(fake.ops());
        assert(model.shutdown() == p4_nano_board::SharedI2cLeaseResult::kOk);
        p4_nano_board::SharedI2cLeaseModel::Lease lease;
        assert(model.release(&lease) ==
               p4_nano_board::SharedI2cLeaseResult::kInvalidArgument);
    }
}

void test_pa_service()
{
    p4_nano_board::PaServiceModel pa;
    assert(!pa.enable());
    assert(!pa.disable());
    assert(!pa.init(false));
    assert(!pa.is_initialized());
    assert(pa.init(true));
    assert(pa.is_initialized());
    assert(!pa.is_enabled());
    assert(pa.is_safe_low());
    assert(pa.enable());
    assert(pa.is_enabled());
    assert(!pa.is_safe_low());
    assert(pa.disable());
    assert(!pa.is_enabled());
    assert(pa.is_safe_low());
    assert(pa.enable());
    assert(pa.shutdown());
    assert(!pa.is_initialized());
    assert(!pa.is_enabled());
    assert(pa.is_safe_low());
}

} // namespace

int main()
{
    run_acquisition_and_release_order(0x45, 0x18, true);
    run_acquisition_and_release_order(0x18, 0x45, true);
    std::printf("P4_NANO_SHARED_I2C_LEASE_TEST=PASS\n");

    run_acquisition_and_release_order(0x45, 0x18, false);
    run_acquisition_and_release_order(0x18, 0x45, false);
    std::printf("P4_NANO_SHARED_I2C_RELEASE_ORDER_TEST=PASS\n");

    test_failure_paths();
    test_pa_service();
    std::printf("P4_NANO_PA_SERVICE_TEST=PASS\n");
    return 0;
}
