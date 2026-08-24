#include <cassert>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string read_file(const char *path)
{
    std::ifstream stream(path);
    assert(stream.good());
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

std::size_t find_required(const std::string &source, const char *needle)
{
    const auto position = source.find(needle);
    assert(position != std::string::npos);
    return position;
}

} // namespace

int main()
{
    const auto runtime = read_file(
        "../firmware/components/p4_nano_pc98_runtime/"
        "p4_nano_pc98_runtime.cpp");
    const auto header = read_file(
        "../firmware/components/p4_nano_usb_keyboard/include/"
        "p4_nano_usb_keyboard/producer.hpp");
    const auto producer = read_file(
        "../firmware/components/p4_nano_usb_keyboard/producer.cpp");

    assert(runtime.find("p4_nano_usb_keyboard::Producer s_usb_keyboard{}") !=
           std::string::npos);
    assert(runtime.find("composition->usb_keyboard->request_stop()") !=
           std::string::npos);
    assert(runtime.find("composition->usb_keyboard.stop()") ==
           std::string::npos);
    assert(runtime.find("composition.usb_keyboard->stop()") !=
           std::string::npos);

    assert(header.find("TeardownFailed") != std::string::npos);
    assert(header.find("enum class StopResult") != std::string::npos);
    const auto request_stop = find_required(producer,
                                            "void Producer::request_stop()");
    const auto request_stop_end = find_required(
        producer.substr(request_stop), "\n}\n\nStopResult Producer::stop") +
                                  request_stop;
    const auto request_body =
        producer.substr(request_stop, request_stop_end - request_stop);
    const auto bridge_gate = find_required(
        request_body, "bridge_calls_allowed_.store(false,");
    const auto stop_latch =
        find_required(request_body, "stop_requested_.store(true,");
    assert(bridge_gate < stop_latch);
    assert(request_body.find("xSemaphoreTake") == std::string::npos);

    const auto hid_wait = find_required(producer, "xSemaphoreTake(hid_done_");
    const auto hid_uninstall = find_required(producer, "hid_host_uninstall()");
    assert(hid_wait < hid_uninstall);
    assert(producer.find(
               "mark_teardown_failed(\"HID_TASK_STOP_TIMEOUT\")") !=
           std::string::npos);
    const auto failed_return = find_required(producer,
                                             "return StopResult::Failed;");
    const auto handle_clear = find_required(producer, "producer_task_ = nullptr;");
    assert(failed_return < handle_clear);
    assert(producer.find("P4_NANO_USB_KEYBOARD_STATE=TEARDOWN_FAILED") !=
           std::string::npos);
    return 0;
}
