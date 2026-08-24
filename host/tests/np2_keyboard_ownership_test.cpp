#include <array>
#include <cassert>
#include <cstdint>

#include "np2_keyboard_input/ownership.hpp"

namespace {

using namespace np2_keyboard_input;
using mapping::FrontendKeyId;

struct Emitted final {
    bool press = false;
    FrontendKeyId key = FrontendKeyId::Escape;
};

struct FakeSink final {
    std::array<Emitted, 32> emitted{};
    std::size_t count = 0;
    std::size_t all_release_count = 0;

    static void press(void *context, const FrontendKeyId key) noexcept
    {
        auto *sink = static_cast<FakeSink *>(context);
        assert(sink->count < sink->emitted.size());
        sink->emitted[sink->count++] = {true, key};
    }

    static void release(void *context, const FrontendKeyId key) noexcept
    {
        auto *sink = static_cast<FakeSink *>(context);
        assert(sink->count < sink->emitted.size());
        sink->emitted[sink->count++] = {false, key};
    }

    static void all_release(void *context) noexcept
    {
        auto *sink = static_cast<FakeSink *>(context);
        ++sink->all_release_count;
    }

    Sink contract() noexcept
    {
        return {this, &FakeSink::press, &FakeSink::release,
                &FakeSink::all_release};
    }
};

constexpr Event event(const SourceId source, const Key key,
                      const Action action) noexcept
{
    return {source, key, action};
}

void test_invalid_events()
{
    FakeSink fake;
    OwnershipEngine engine(fake.contract());
    assert(engine.process(Event{}) == EventResult::Invalid);
    assert(engine.process(event(SourceId{}, Key::A, Action::Press)) ==
           EventResult::Invalid);
    assert(engine.process(event(kSyntheticSource, Key::Unknown,
                                Action::Press)) == EventResult::Invalid);
    assert(engine.process(event(kSyntheticSource, Key::A,
                                static_cast<Action>(0xff))) ==
           EventResult::Invalid);
    assert(fake.count == 0);
    assert(engine.counters().invalid_rejected == 4);
}

void test_basic_and_duplicate_edges()
{
    FakeSink fake;
    OwnershipEngine engine(fake.contract());
    assert(engine.process(event(kSyntheticSource, Key::A, Action::Press)) ==
           EventResult::Injected);
    assert(engine.process(event(kSyntheticSource, Key::A, Action::Press)) ==
           EventResult::Duplicate);
    assert(engine.process(event(kSyntheticSource, Key::A, Action::Release)) ==
           EventResult::Accepted);
    assert(engine.process(event(kSyntheticSource, Key::A, Action::Release)) ==
           EventResult::Duplicate);
    assert(fake.count == 2);
    assert(fake.emitted[0].press && fake.emitted[0].key == FrontendKeyId::A);
    assert(!fake.emitted[1].press && fake.emitted[1].key == FrontendKeyId::A);
    assert(engine.counters().duplicate_suppressed == 2);
}

void test_multi_source_and_disconnect()
{
    const SourceId source_a{10};
    const SourceId source_b{11};
    FakeSink fake;
    OwnershipEngine engine(fake.contract());
    assert(engine.process(event(source_a, Key::A, Action::Press)) ==
           EventResult::Injected);
    assert(engine.process(event(source_b, Key::A, Action::Press)) ==
           EventResult::Accepted);
    assert(engine.process(event(source_a, Key::A, Action::Release)) ==
           EventResult::Accepted);
    assert(fake.count == 1);
    assert(engine.process(event(source_b, Key::A, Action::Release)) ==
           EventResult::Accepted);
    assert(fake.count == 2);

    assert(engine.process(event(source_a, Key::A, Action::Press)) ==
           EventResult::Injected);
    assert(engine.process(event(source_b, Key::B, Action::Press)) ==
           EventResult::Injected);
    assert(engine.disconnect_source(source_a));
    assert(fake.count == 5);
    assert(!fake.emitted[4].press && fake.emitted[4].key == FrontendKeyId::A);
    assert(engine.disconnect_source(source_b));
    assert(fake.count == 6);
    assert(!fake.emitted[5].press && fake.emitted[5].key == FrontendKeyId::B);
}

void test_shift_alias_and_mode_transition()
{
    FakeSink fake;
    OwnershipEngine engine(fake.contract());
    const SourceId left_source{20};
    const SourceId right_source{21};
    assert(engine.process(event(left_source, Key::LeftShift, Action::Press)) ==
           EventResult::Injected);
    assert(engine.process(event(right_source, Key::RightShift, Action::Press)) ==
           EventResult::Accepted);
    assert(engine.process(event(left_source, Key::LeftShift, Action::Release)) ==
           EventResult::Accepted);
    assert(fake.count == 1);
    assert(engine.process(event(right_source, Key::RightShift, Action::Release)) ==
           EventResult::Accepted);
    assert(fake.count == 2);
    assert(fake.emitted[0].key == FrontendKeyId::LeftShift);
    assert(fake.emitted[1].key == FrontendKeyId::LeftShift);

    FakeSink reverse_fake;
    OwnershipEngine reverse(reverse_fake.contract());
    assert(reverse.process(event(right_source, Key::RightShift,
                                 Action::Press)) == EventResult::Injected);
    assert(reverse.process(event(left_source, Key::LeftShift,
                                 Action::Press)) == EventResult::Accepted);
    assert(reverse.process(event(right_source, Key::RightShift,
                                 Action::Release)) == EventResult::Accepted);
    assert(reverse_fake.count == 1);
    assert(reverse.process(event(left_source, Key::LeftShift,
                                 Action::Release)) == EventResult::Accepted);
    assert(reverse_fake.count == 2);
    assert(reverse_fake.emitted[0].key == FrontendKeyId::LeftShift);
    assert(reverse_fake.emitted[1].key == FrontendKeyId::LeftShift);

    FakeSink transition_fake;
    OwnershipEngine transition(transition_fake.contract());
    assert(transition.process(event(left_source, Key::RightShift,
                                    Action::Press)) == EventResult::Injected);
    transition.set_shift_mode(ShiftMode::Separate);
    assert(transition.process(event(left_source, Key::RightShift,
                                    Action::Release)) == EventResult::Accepted);
    assert(transition_fake.count == 2);
    assert(transition_fake.emitted[0].key == FrontendKeyId::LeftShift);
    assert(transition_fake.emitted[1].key == FrontendKeyId::LeftShift);

    FakeSink separate_fake;
    OwnershipEngine separate(separate_fake.contract(), ShiftMode::Separate);
    assert(separate.process(event(left_source, Key::LeftShift,
                                  Action::Press)) == EventResult::Injected);
    assert(separate.process(event(right_source, Key::RightShift,
                                  Action::Press)) == EventResult::Injected);
    assert(separate.process(event(left_source, Key::LeftShift,
                                  Action::Release)) == EventResult::Accepted);
    assert(separate.process(event(right_source, Key::RightShift,
                                  Action::Release)) == EventResult::Accepted);
    assert(separate_fake.count == 4);
    assert(separate_fake.emitted[1].key == FrontendKeyId::RightShift);
    assert(separate_fake.emitted[3].key == FrontendKeyId::RightShift);
}

void test_caps_and_kana_toggle_edges()
{
    FakeSink fake;
    OwnershipEngine engine(fake.contract());
    const SourceId source{30};
    assert(engine.process(event(source, Key::CapsLock, Action::Press)) ==
           EventResult::Injected);
    assert(engine.process(event(source, Key::CapsLock, Action::Press)) ==
           EventResult::Duplicate);
    assert(engine.process(event(source, Key::CapsLock, Action::Release)) ==
           EventResult::Accepted);
    assert(engine.process(event(source, Key::CapsLock, Action::Press)) ==
           EventResult::Injected);
    assert(engine.disconnect_source(source));
    assert(fake.count == 2);
    assert(fake.emitted[0].press && fake.emitted[0].key == FrontendKeyId::CapsLock);
    assert(fake.emitted[1].press && fake.emitted[1].key == FrontendKeyId::CapsLock);

    assert(engine.process(event(source, Key::Kana, Action::Press)) ==
           EventResult::Injected);
    assert(engine.process(event(source, Key::Kana, Action::Press)) ==
           EventResult::Duplicate);
    assert(engine.process(event(source, Key::Kana, Action::Release)) ==
           EventResult::Accepted);
    assert(fake.count == 3);
    assert(fake.emitted[2].press && fake.emitted[2].key == FrontendKeyId::Kana);
}

void test_capacity_and_recovery()
{
    FakeSink fake;
    OwnershipEngine engine(fake.contract());
    for (std::uint8_t value = 1; value <= kMaxActiveSources; ++value) {
        assert(engine.process(event(SourceId{value}, Key::A,
                                    Action::Press)) != EventResult::Invalid);
    }
    assert(engine.process(event(SourceId{99}, Key::B, Action::Press)) ==
           EventResult::SourceCapacityExceeded);
    assert(engine.desynchronized());
    assert(engine.process(event(SourceId{1}, Key::C, Action::Press)) ==
           EventResult::Quarantined);
    assert(engine.emergency_recover());
    assert(!engine.emergency_recover());
    assert(fake.all_release_count == 1);
    assert(engine.rearm());
    assert(engine.process(event(SourceId{99}, Key::B, Action::Press)) ==
           EventResult::Injected);
}

} // namespace

int main()
{
    test_invalid_events();
    test_basic_and_duplicate_edges();
    test_multi_source_and_disconnect();
    test_shift_alias_and_mode_transition();
    test_caps_and_kana_toggle_edges();
    test_capacity_and_recovery();
    return 0;
}
