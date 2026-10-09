#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <initializer_list>
#include <iterator>
#include <cstdlib>
#include <limits>
#include <string>

#include <unistd.h>

#include "bridge_random_signal.hpp"
#include "network_wifi_config.hpp"
#include "network_hostname_config.hpp"
#include "network_mdns_policy.hpp"
#include "network_wifi_state.hpp"
#include "ota_update_policy.hpp"
#include "platform_board.hpp"
#include "rf_automation_engine.hpp"
#include "rf_codec.hpp"
#include "rf_console_format.hpp"
#include "rf_console_linenoise.h"
#include "rf_console_parse.hpp"
#include "rf_activity_led_policy.hpp"
#include "rf_storage_format.hpp"
#include "rf_storage_recent_format.hpp"
#include "rf_signals_match.hpp"
#include "web_form.hpp"
#include "web_events_format.hpp"

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

uint32_t test_crc32(const uint8_t *data, std::size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

void rewrite_test_crc(uint8_t *record, std::size_t size)
{
    const uint32_t crc = test_crc32(record, size - 4U);
    for (std::size_t index = 0; index < 4; ++index) {
        record[size - 4U + index] = static_cast<uint8_t>(crc >> (index * 8U));
    }
}

struct LineEditorScript {
    std::string input;
    std::size_t offset = 0;
    std::size_t event_offset = std::numeric_limits<std::size_t>::max();
    bool event_emitted = false;
    bool nested_suspend = false;
    bool first_suspend = false;
    bool second_suspend = false;
};

LineEditorScript s_line_editor_script;
int s_line_editor_lock_depth = 0;
uint32_t s_line_editor_time = 0;
uint32_t s_line_editor_time_step = 100;

void line_editor_lock(void *)
{
    ++s_line_editor_lock_depth;
}

void line_editor_unlock(void *)
{
    require(s_line_editor_lock_depth > 0, "line editor synchronization remains balanced");
    --s_line_editor_lock_depth;
}

void emit_async_line()
{
    line_editor_lock(nullptr);
    s_line_editor_script.first_suspend = rf_linenoiseSuspendActiveLine();
    if (s_line_editor_script.nested_suspend) {
        s_line_editor_script.second_suspend = rf_linenoiseSuspendActiveLine();
    }
    std::fputs("[ASYNC] line\n", stdout);
    rf_linenoiseResumeActiveLine();
    line_editor_unlock(nullptr);
}

ssize_t scripted_line_read(int, void *buffer, std::size_t length)
{
    if (!s_line_editor_script.event_emitted &&
        s_line_editor_script.offset == s_line_editor_script.event_offset) {
        s_line_editor_script.event_emitted = true;
        emit_async_line();
    }
    if (length == 0 || s_line_editor_script.offset >= s_line_editor_script.input.size()) {
        return 0;
    }
    static_cast<char *>(buffer)[0] = s_line_editor_script.input[s_line_editor_script.offset++];
    return 1;
}

uint32_t scripted_line_time()
{
    const uint32_t current = s_line_editor_time;
    s_line_editor_time += s_line_editor_time_step;
    return current;
}

class StdoutCapture {
public:
    StdoutCapture()
    {
        std::fflush(stdout);
        saved_fd_ = dup(STDOUT_FILENO);
        file_ = std::tmpfile();
        require(saved_fd_ >= 0 && file_ != nullptr, "stdout capture setup succeeds");
        require(dup2(fileno(file_), STDOUT_FILENO) >= 0, "stdout capture redirects output");
    }

    ~StdoutCapture()
    {
        restore();
    }

    std::string finish()
    {
        std::fflush(stdout);
        require(std::fseek(file_, 0, SEEK_END) == 0, "stdout capture seeks to end");
        const long length = std::ftell(file_);
        require(length >= 0 && std::fseek(file_, 0, SEEK_SET) == 0,
                "stdout capture rewinds");
        std::string output(static_cast<std::size_t>(length), '\0');
        if (!output.empty()) {
            require(std::fread(output.data(), 1, output.size(), file_) == output.size(),
                    "stdout capture reads all output");
        }
        restore();
        return output;
    }

private:
    void restore()
    {
        if (saved_fd_ >= 0) {
            std::fflush(stdout);
            dup2(saved_fd_, STDOUT_FILENO);
            close(saved_fd_);
            saved_fd_ = -1;
        }
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    int saved_fd_ = -1;
    FILE *file_ = nullptr;
};

void configure_line_editor(std::string input, std::size_t event_offset,
                           bool dumb_mode = false, std::size_t columns = 80,
                           bool nested_suspend = false, uint32_t time_step = 100)
{
    s_line_editor_script = {};
    s_line_editor_script.input = std::move(input);
    s_line_editor_script.event_offset = event_offset;
    s_line_editor_script.nested_suspend = nested_suspend;
    s_line_editor_lock_depth = 0;
    s_line_editor_time = 0;
    s_line_editor_time_step = time_step;
    rf_linenoiseSetReadFunction(scripted_line_read);
    rf_linenoiseSetTimeFunction(scripted_line_time);
    rf_linenoiseSetSyncCallbacks(line_editor_lock, line_editor_unlock, nullptr);
    rf_linenoiseSetColumnsOverride(columns);
    rf_linenoiseSetDumbMode(dumb_mode ? 1 : 0);
    rf_linenoiseSetMultiLine(1);
    rf_linenoiseSetMaxLineLen(2048);
}

void reset_line_editor()
{
    rf_linenoiseSetCompletionCallback(nullptr);
    rf_linenoiseSetHintsCallback(nullptr);
    rf_linenoiseSetSyncCallbacks(nullptr, nullptr, nullptr);
    rf_linenoiseSetReadFunction(nullptr);
    rf_linenoiseSetTimeFunction(nullptr);
    rf_linenoiseSetColumnsOverride(0);
    rf_linenoiseSetDumbMode(0);
    rf_linenoiseHistoryFree();
    require(s_line_editor_lock_depth == 0, "line editor releases synchronization lock");
}

std::size_t substring_count(const std::string &value, const std::string &needle)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

void status_completion(const char *line, linenoiseCompletions *completions)
{
    if (std::strcmp(line, "sta") == 0) {
        rf_linenoiseAddCompletion(completions, "status");
    }
}

char *status_hint(const char *line, int *color, int *bold)
{
    static char hint[] = " <command>";
    if (std::strcmp(line, "sta") != 0) return nullptr;
    *color = 36;
    *bold = 0;
    return hint;
}

void test_prompt_safe_line_editor()
{
    rf_linenoiseHistoryFree();
    require(rf_linenoiseHistorySetMaxLen(32) == 1, "line editor history initializes");

    configure_line_editor("status\n", 3);
    StdoutCapture single_capture;
    char *single = rf_linenoise("rf> ");
    const std::string single_output = single_capture.finish();
    require(single != nullptr && std::strcmp(single, "status") == 0,
            "asynchronous output preserves single-line input");
    require(single_output.find("[ASYNC] line\n") != std::string::npos &&
                substring_count(single_output, "rf> sta") >= 2,
            "single-line interruption clears and redraws partial input");
    require(s_line_editor_script.first_suspend, "active line is suspended for external output");
    rf_linenoiseFree(single);
    reset_line_editor();

    configure_line_editor("sttus\x1b[D\x1b[D\x1b[Da\n", 14);
    StdoutCapture cursor_capture;
    char *cursor = rf_linenoise("rf> ");
    const std::string cursor_output = cursor_capture.finish();
    require(cursor != nullptr && std::strcmp(cursor, "status") == 0,
            "cursor-middle redraw preserves edit position");
    require(cursor_output.find("[ASYNC] line\n") != std::string::npos,
            "cursor-middle interruption emits external record");
    rf_linenoiseFree(cursor);
    reset_line_editor();

    const std::string multiline_command = "raw append 100 200 300 400 500 600";
    configure_line_editor(multiline_command + "\n", 18, false, 12);
    StdoutCapture multiline_capture;
    char *multiline = rf_linenoise("rf> ");
    const std::string multiline_output = multiline_capture.finish();
    require(multiline != nullptr && multiline_command == multiline,
            "multiline redraw preserves the full command");
    require(multiline_output.find("\x1b[1A") != std::string::npos &&
                multiline_output.find("[ASYNC] line\n") != std::string::npos,
            "multiline interruption clears prior rows before redraw");
    rf_linenoiseFree(multiline);
    reset_line_editor();

    configure_line_editor("sta\t\n", std::numeric_limits<std::size_t>::max());
    rf_linenoiseSetCompletionCallback(status_completion);
    rf_linenoiseSetHintsCallback(status_hint);
    StdoutCapture completion_capture;
    char *completed = rf_linenoise("rf> ");
    const std::string completion_output = completion_capture.finish();
    require(completed != nullptr && std::strcmp(completed, "status") == 0,
            "completion remains functional");
    require(completion_output.find("<command>") != std::string::npos,
            "command hints remain functional");
    rf_linenoiseFree(completed);
    reset_line_editor();

    configure_line_editor("status\n", std::numeric_limits<std::size_t>::max());
    StdoutCapture history_capture;
    char *history_seed = rf_linenoise("rf> ");
    require(history_seed != nullptr, "history seed command is read");
    rf_linenoiseHistoryAdd(history_seed);
    rf_linenoiseFree(history_seed);
    s_line_editor_script = {};
    s_line_editor_script.input = "\x1b[A\n";
    char *history_line = rf_linenoise("rf> ");
    const std::string history_output = history_capture.finish();
    require(history_line != nullptr && std::strcmp(history_line, "status") == 0,
            "history navigation survives custom editor");
    require(substring_count(history_output, "rf> ") >= 2, "history session presents both prompts");
    rf_linenoiseFree(history_line);
    reset_line_editor();

    configure_line_editor("statuu\x7fs\n", 3, false, 80, true);
    StdoutCapture nested_capture;
    char *nested = rf_linenoise("rf> ");
    const std::string nested_output = nested_capture.finish();
    require(nested != nullptr && std::strcmp(nested, "status") == 0,
            "backspace remains functional across interruption");
    require(s_line_editor_script.first_suspend && !s_line_editor_script.second_suspend &&
                substring_count(nested_output, "[ASYNC] line") == 1,
            "nested output suspends and redraws only once");
    rf_linenoiseFree(nested);
    reset_line_editor();

    configure_line_editor("status\n", 3, true);
    StdoutCapture dumb_capture;
    char *dumb = rf_linenoise("rf> ");
    const std::string dumb_output = dumb_capture.finish();
    require(dumb != nullptr && std::strcmp(dumb, "status") == 0,
            "dumb terminal interruption preserves input");
    require(dumb_output.find("\x1b[") == std::string::npos &&
                substring_count(dumb_output, "rf> sta") >= 2,
            "dumb terminal redraw uses no ANSI cursor control");
    rf_linenoiseFree(dumb);
    reset_line_editor();

    configure_line_editor("raw append 100 200 300\n", 8, false, 80, false, 0);
    StdoutCapture paste_capture;
    char *pasted = rf_linenoise("rf> ");
    paste_capture.finish();
    require(pasted != nullptr && std::strcmp(pasted, "raw append 100 200 300") == 0,
            "pasted input remains intact across asynchronous output");
    rf_linenoiseFree(pasted);
    reset_line_editor();

    configure_line_editor(std::string(70, 'a') + "\n",
                          std::numeric_limits<std::size_t>::max());
    require(rf_linenoiseSetMaxLineLen(64) == 0, "test command bound is accepted");
    StdoutCapture bounded_capture;
    char *bounded = rf_linenoise("rf> ");
    bounded_capture.finish();
    require(bounded != nullptr && std::strlen(bounded) == 63,
            "line editor enforces its configured maximum safely");
    rf_linenoiseFree(bounded);
    reset_line_editor();
}

void test_prompt_safe_masked_input()
{
    configure_line_editor("sec\x7f" "cret\n", 3);
    char secret[16]{};
    StdoutCapture capture;
    const int length = rf_linenoiseReadMasked("WiFi password: ", secret, sizeof(secret));
    const std::string output = capture.finish();
    require(length == 6 && std::strcmp(secret, "secret") == 0,
            "masked input preserves editing across asynchronous output");
    require(output.find("secret") == std::string::npos && output.find("sec") == std::string::npos,
            "masked input never renders entered bytes");
    require(output.find("[ASYNC] line\n") != std::string::npos &&
                substring_count(output, "WiFi password: ") >= 2,
            "masked prompt redraws after asynchronous output");
    std::memset(secret, 0, sizeof(secret));
    reset_line_editor();

    configure_line_editor(std::string(20, 'x') + "\n",
                          std::numeric_limits<std::size_t>::max());
    char bounded[8]{};
    StdoutCapture overflow_capture;
    const int overflow = rf_linenoiseReadMasked("WiFi password: ", bounded, sizeof(bounded));
    const std::string overflow_output = overflow_capture.finish();
    require(overflow == RF_LINENOISE_MASKED_TOO_LONG,
            "masked input reports bounded overflow");
    require(overflow_output.find('x') == std::string::npos,
            "overflowed masked input remains secret");
    std::memset(bounded, 0, sizeof(bounded));
    reset_line_editor();
}

void test_protocol_table()
{
    struct GoldenProtocol {
        uint16_t pulse;
        uint8_t sync_first;
        uint8_t sync_second;
        uint8_t zero_first;
        uint8_t zero_second;
        uint8_t one_first;
        uint8_t one_second;
        bool inverted;
    };
    constexpr GoldenProtocol golden[] = {
        {350, 1, 31, 1, 3, 3, 1, false},  {650, 1, 10, 1, 2, 2, 1, false},
        {100, 30, 71, 4, 11, 9, 6, false}, {380, 1, 6, 1, 3, 3, 1, false},
        {500, 6, 14, 1, 2, 2, 1, false},  {450, 23, 1, 1, 2, 2, 1, true},
        {150, 2, 62, 1, 6, 6, 1, false},  {200, 3, 130, 7, 16, 3, 16, false},
        {200, 130, 7, 16, 7, 16, 3, true}, {365, 18, 1, 3, 1, 1, 3, true},
        {270, 36, 1, 1, 2, 2, 1, true},   {320, 36, 1, 1, 2, 2, 1, true},
    };
    require(std::size(golden) == rfbridge::kRfProtocolCount, "golden protocol count");
    for (std::size_t index = 0; index < std::size(golden); ++index) {
        const rfbridge::RfProtocol *protocol = rfbridge::rf_protocol(static_cast<uint8_t>(index + 1U));
        const GoldenProtocol &expected = golden[index];
        require(protocol != nullptr && protocol->pulse_us == expected.pulse &&
                    protocol->sync.first == expected.sync_first && protocol->sync.second == expected.sync_second &&
                    protocol->zero.first == expected.zero_first && protocol->zero.second == expected.zero_second &&
                    protocol->one.first == expected.one_first && protocol->one.second == expected.one_second &&
                    protocol->inverted == expected.inverted,
                "rc-switch protocol table matches golden definitions");
    }
}

void test_protocol_round_trips()
{
    for (uint8_t protocol_number = 1; protocol_number <= rfbridge::kRfProtocolCount; ++protocol_number) {
        const rfbridge::RfProtocol *protocol = rfbridge::rf_protocol(protocol_number);
        require(protocol != nullptr, "protocol exists");
        rfbridge::DecodedSignal signal{};
        signal.code = 0xA5;
        signal.pulse_us = protocol->pulse_us;
        signal.bits = 8;
        signal.protocol = protocol_number;
        signal.inverted = protocol->inverted;
        rfbridge::RawSignal raw{};
        require(rfbridge::build_decoded_raw(signal, &raw), "protocol waveform builds");
        rfbridge::DecodedSignal identified{};
        const rfbridge::RawProtocolIdentity identity = rfbridge::identify_raw_protocol(raw, &identified);
        require(identity != rfbridge::RawProtocolIdentity::kUnknown, "protocol waveform is recognized");
        if (identity == rfbridge::RawProtocolIdentity::kDecoded) {
            require(rfbridge::decoded_signals_match(signal, identified), "unique protocol identity round trips");
        }
    }
}

void test_decoded_transport_boundaries()
{
    for (uint8_t protocol_number = 1; protocol_number <= rfbridge::kRfProtocolCount; ++protocol_number) {
        const rfbridge::RfProtocol *protocol = rfbridge::rf_protocol(protocol_number);
        const uint8_t minimum_factor = rfbridge::rf_protocol_min_factor(protocol_number);
        const uint8_t maximum_factor = rfbridge::rf_protocol_max_factor(protocol_number);
        require(protocol != nullptr && minimum_factor > 0 && maximum_factor > 0, "protocol factors exist");

        const uint16_t minimum_unit = static_cast<uint16_t>(
            (rfbridge::kMinimumRawPulseUs + minimum_factor - 1U) / minimum_factor);
        const uint16_t maximum_unit =
            static_cast<uint16_t>(rfbridge::kMaximumPulseDurationUs / maximum_factor);
        rfbridge::DecodedSignal signal{};
        signal.code = 0xA5;
        signal.bits = 8;
        signal.protocol = protocol_number;
        signal.inverted = protocol->inverted;

        signal.pulse_us = static_cast<uint16_t>(minimum_unit - 1U);
        require(!rfbridge::decoded_signal_is_valid(signal), "decoded unit below transport floor rejected");
        signal.pulse_us = minimum_unit;
        require(rfbridge::decoded_signal_is_valid(signal), "decoded unit at transport floor accepted");
        rfbridge::RawSignal raw{};
        require(rfbridge::build_decoded_raw(signal, &raw) && rfbridge::raw_signal_is_valid(raw),
                "every valid decoded signal builds a valid raw waveform");

        signal.pulse_us = maximum_unit;
        require(rfbridge::decoded_signal_is_valid(signal), "decoded unit at transport ceiling accepted");
        signal.pulse_us = static_cast<uint16_t>(maximum_unit + 1U);
        require(!rfbridge::decoded_signal_is_valid(signal), "decoded unit above transport ceiling rejected");
    }

    rfbridge::DecodedSignal normalized{};
    require(rfbridge::make_decoded_signal(13830801, 24, 1, 0, &normalized) &&
                normalized.code == 0xD30A91 && normalized.pulse_us == 350 &&
                normalized.inverted == 0,
            "decoded construction normalizes nominal pulse and protocol inversion");
    require(rfbridge::make_decoded_signal(0xD30A91, 24, 1, 199, &normalized) &&
                normalized.pulse_us == 199,
            "decoded construction preserves an explicit valid pulse");
    require(!rfbridge::make_decoded_signal(0x1000000, 24, 1, 199, &normalized) &&
                !rfbridge::make_decoded_signal(1, 24, 0, 199, &normalized) &&
                !rfbridge::make_decoded_signal(1, 24, 1, 9999, &normalized) &&
                !rfbridge::make_decoded_signal(1, 24, 1, 199, nullptr),
            "decoded construction rejects width protocol pulse and output errors");
}

void test_raw_identity_rules()
{
    rfbridge::DecodedSignal left{};
    left.code = 0;
    left.pulse_us = 350;
    left.bits = 24;
    left.protocol = 1;
    rfbridge::DecodedSignal right = left;
    right.code = 1;
    rfbridge::RawSignal left_raw{};
    rfbridge::RawSignal right_raw{};
    require(rfbridge::build_decoded_raw(left, &left_raw), "left raw builds");
    require(rfbridge::build_decoded_raw(right, &right_raw), "right raw builds");
    require(!rfbridge::raw_signals_match(left_raw, right_raw), "one changed bit remains distinct");

    rfbridge::RawSignal rotated{};
    rotated.count = left_raw.count;
    constexpr std::size_t shift = 7;
    rotated.start_level = static_cast<uint8_t>(left_raw.start_level ^ (shift & 1U));
    for (std::size_t index = 0; index < left_raw.count; ++index) {
        rotated.durations_us[index] = left_raw.durations_us[(index + shift) % left_raw.count];
    }
    require(rfbridge::raw_signals_match(left_raw, rotated), "cyclic phase remains equivalent");

    for (uint8_t bits = 1; bits < 4; ++bits) {
        rfbridge::DecodedSignal invalid = left;
        invalid.bits = bits;
        invalid.code = 1;
        require(!rfbridge::decoded_signal_is_valid(invalid), "short decoded code rejected");
    }
}

void test_protocol_alias_policy()
{
    rfbridge::DecodedSignal midpoint{};
    midpoint.code = 0x0A;
    midpoint.pulse_us = 297;
    midpoint.bits = 4;
    midpoint.protocol = 12;
    midpoint.inverted = 1;
    rfbridge::RawSignal raw{};
    require(rfbridge::build_decoded_raw(midpoint, &raw), "midpoint waveform builds");
    require(rfbridge::identify_raw_protocol(raw, nullptr) == rfbridge::RawProtocolIdentity::kAmbiguous,
            "protocol 11/12 midpoint fails closed");
}

void test_console_parser()
{
    uint64_t value = 0;
    require(rfbridge::parse_unsigned_value("11043138", UINT64_MAX, &value) && value == 11043138,
            "decimal parser");
    require(rfbridge::parse_unsigned_value("0xA88142", UINT64_MAX, &value) && value == 0xA88142,
            "hex parser");
    require(!rfbridge::parse_unsigned_value("-1", UINT64_MAX, &value), "negative rejected");
    require(!rfbridge::parse_unsigned_value("0x", UINT64_MAX, &value), "empty hex rejected");
    require(!rfbridge::parse_unsigned_value("12junk", UINT64_MAX, &value), "trailing junk rejected");
    require(!rfbridge::parse_unsigned_value("256", 255, &value), "bound enforced");

    const char *ram[] = {"replay"};
    const char *ram_repeats[] = {"replay", "10"};
    const char *named[] = {"replay", "gate", "7"};
    const char *missing_named_repeats[] = {"replay", "gate"};
    rfbridge::ReplayArguments replay{};
    require(rfbridge::parse_replay_arguments(1, ram, 8, &replay) &&
                replay.target == rfbridge::ReplayTarget::kRam && replay.repeats == 8,
            "default RAM replay preserved");
    require(rfbridge::parse_replay_arguments(2, ram_repeats, 8, &replay) &&
                replay.target == rfbridge::ReplayTarget::kRam && replay.repeats == 10,
            "explicit RAM replay preserved");
    require(rfbridge::parse_replay_arguments(3, named, 8, &replay) &&
                replay.target == rfbridge::ReplayTarget::kNamed && replay.repeats == 7,
            "named replay parsed");
    require(!rfbridge::parse_replay_arguments(2, missing_named_repeats, 8, &replay),
            "named replay requires repeats");

    const char *recent_list[] = {"recent", "list"};
    const char *recent_replay[] = {"recent", "replay", "18446744073709551614"};
    const char *recent_explicit[] = {"recent", "replay", "42", "20"};
    const char *recent_save[] = {"recent", "save", "42", "gate"};
    const char *recent_zero[] = {"recent", "replay", "0"};
    const char *recent_overflow[] = {"recent", "replay", "18446744073709551616"};
    rfbridge::RecentArguments recent{};
    require(rfbridge::parse_recent_arguments(2, recent_list, 8, &recent) &&
                recent.action == rfbridge::RecentAction::kList,
            "recent list parsed");
    require(rfbridge::parse_recent_arguments(3, recent_replay, 8, &recent) &&
                recent.action == rfbridge::RecentAction::kReplay &&
                recent.id == UINT64_MAX - 1U && recent.repeats == 8,
            "recent replay accepts a persistent 64-bit ID and default repeats");
    require(rfbridge::parse_recent_arguments(4, recent_explicit, 8, &recent) &&
                recent.repeats == 20,
            "recent replay accepts bounded explicit repeats");
    require(rfbridge::parse_recent_arguments(4, recent_save, 8, &recent) &&
                recent.action == rfbridge::RecentAction::kSave &&
                std::strcmp(recent.name, "gate") == 0,
            "recent save accepts a valid learned name");
    require(!rfbridge::parse_recent_arguments(3, recent_zero, 8, &recent) &&
                !rfbridge::parse_recent_arguments(3, recent_overflow, 8, &recent),
            "recent parser rejects zero and overflowing IDs");

    const char *save_nominal[] = {"save", "gate", "13830801", "24", "1"};
    const char *save_explicit[] = {"save", "gate_2", "0xD30A91", "24", "1", "199"};
    const char *save_overflow[] = {"save", "gate", "0x1000000", "24", "1"};
    const char *save_reserved[] = {"save", "list", "1", "24", "1"};
    const char *save_bad_pulse[] = {"save", "gate", "1", "24", "1", "9999"};
    rfbridge::SaveSignalArguments save{};
    require(rfbridge::parse_save_signal_arguments(5, save_nominal, &save) &&
                std::strcmp(save.name, "gate") == 0 && save.code == 0xD30A91 &&
                save.bits == 24 && save.protocol == 1 && save.pulse_us == 350,
            "manual save parser accepts decimal code and nominal pulse");
    require(rfbridge::parse_save_signal_arguments(6, save_explicit, &save) &&
                std::strcmp(save.name, "gate_2") == 0 && save.pulse_us == 199,
            "manual save parser accepts hexadecimal code and explicit pulse");
    require(!rfbridge::parse_save_signal_arguments(5, save_overflow, &save) &&
                !rfbridge::parse_save_signal_arguments(5, save_reserved, &save) &&
                !rfbridge::parse_save_signal_arguments(6, save_bad_pulse, &save),
            "manual save parser rejects code width reserved names and invalid pulses");

    const char *rule_default[] = {"rule", "add", "B", "A"};
    const char *rule_explicit[] = {"rule", "add", "B", "A", "12"};
    const char *rule_self[] = {"rule", "add", "A", "A"};
    uint8_t rule_repeats = 0;
    require(rfbridge::parse_rule_add_arguments(4, rule_default, 8, &rule_repeats) && rule_repeats == 8,
            "rule add default repeats parsed");
    require(rfbridge::parse_rule_add_arguments(5, rule_explicit, 8, &rule_repeats) && rule_repeats == 12,
            "rule add explicit repeats parsed");
    require(!rfbridge::parse_rule_add_arguments(4, rule_self, 8, &rule_repeats),
            "rule self mapping rejected");

    rfbridge::RfAutomationLogMode log_mode{};
    require(rfbridge::parse_rule_log_mode("actions", &log_mode) &&
                log_mode == rfbridge::RfAutomationLogMode::kActions,
            "rule action logging mode parsed");
    require(rfbridge::parse_rule_log_mode("verbose", &log_mode) &&
                log_mode == rfbridge::RfAutomationLogMode::kVerbose,
            "rule verbose logging mode parsed");
    require(!rfbridge::parse_rule_log_mode("debug", &log_mode), "unknown rule logging mode rejected");

    rfbridge::RfAutomationEvent event{};
    event.type = rfbridge::RfAutomationEventType::kTriggered;
    std::strcpy(event.trigger_name, "B");
    std::strcpy(event.target_name, "A");
    event.repeats = 8;
    event.action_id = 42;
    event.received_encoding = rfbridge::RfStoredEncoding::kRaw;
    event.target_encoding = rfbridge::RfStoredEncoding::kDecoded;
    char line[256]{};
    require(rfbridge::format_rf_automation_event(event, "ESP_OK", line, sizeof(line)) &&
                std::strcmp(line, "RULE TRIGGER id=42 trigger=B target=A repeats=8 rx_encoding=raw tx_encoding=decoded") == 0,
            "automation trigger event formatted");
    event.type = rfbridge::RfAutomationEventType::kActionCompleted;
    event.elapsed_ms = 742;
    require(rfbridge::format_rf_automation_event(event, "ESP_OK", line, sizeof(line)) &&
                std::strcmp(line, "RULE ACTION id=42 trigger=B target=A result=OK elapsed_ms=742") == 0,
            "automation completion event formatted");
}

void test_console_formatter()
{
    using rfbridge::ConsoleStyle;
    using rfbridge::ConsoleTone;
    ConsoleStyle style{};
    require(rfbridge::parse_console_style("pretty", &style) && style == ConsoleStyle::kPretty,
            "pretty console style parsed");
    require(rfbridge::parse_console_style("plain", &style) && style == ConsoleStyle::kPlain,
            "plain console style parsed");
    require(!rfbridge::parse_console_style("ansi", &style), "unknown console style rejected");
    require(rfbridge::ota_progress_percent(420, 1000) == 42, "OTA percentage computed");
    require(rfbridge::ota_progress_percent(UINT32_MAX, UINT32_MAX) == 100,
            "OTA percentage handles uint32 maximum");
    require(rfbridge::ota_progress_percent(UINT32_MAX, 1) == 100,
            "OTA percentage clamps oversized received count");
    std::size_t chunk_length = 0;
    std::size_t consumed_length = 0;
    require(rfbridge::console_wrap_chunk("alpha beta gamma", 10, &chunk_length,
                                         &consumed_length) &&
                chunk_length == 10 && consumed_length == 11,
            "console wrapping uses boundary delimiter");
    require(rfbridge::console_wrap_chunk("123,456,789", 7, &chunk_length,
                                         &consumed_length) &&
                chunk_length == 4 && consumed_length == 4,
            "console wrapping preserves comma-delimited values");

    char line[256]{};
    require(rfbridge::format_ota_progress_line(ConsoleStyle::kPlain, 420, 1000, line,
                                                sizeof(line)) &&
                std::strcmp(line, "OTA PROGRESS bytes=420 total=1000") == 0,
            "plain OTA progress remains exact");
    require(rfbridge::format_ota_progress_line(ConsoleStyle::kPretty, 420, 1000, line,
                                                sizeof(line)) &&
                std::strstr(line, " 42% [########------------]") != nullptr &&
                std::strcmp(line + std::strlen(line) - 4U, "\x1b[0m") == 0,
            "pretty OTA progress has fixed bar and reset");
    require(rfbridge::format_ota_progress_line(ConsoleStyle::kPretty, 957680, 957680, line,
                                                sizeof(line)) &&
                std::strstr(line, "100% [####################] 936 KiB / 936 KiB") != nullptr,
            "completed OTA progress uses consistent KiB rounding");
    require(rfbridge::format_console_tagged_line(
                ConsoleStyle::kPretty, ConsoleTone::kSuccess, " OK ", "OK rx on", "RX enabled",
                line, sizeof(line)) &&
                std::strstr(line, "\x1b[1;32m[ OK  ]\x1b[0m RX enabled") != nullptr &&
                std::strcmp(line + std::strlen(line) - 4U, "\x1b[0m") == 0,
            "pretty tag uses semantic color and reset");
    require(rfbridge::format_console_tagged_line(
                ConsoleStyle::kPretty, ConsoleTone::kInfo, "", "", "confidence=repeated", line,
                sizeof(line)) &&
                std::strcmp(line,
                            "      \x1b[1;36m|\x1b[0m confidence=repeated\x1b[0m") == 0,
            "pretty continuation uses aligned colored gutter");
    require(rfbridge::format_console_tagged_line(
                ConsoleStyle::kPretty, ConsoleTone::kInfo, "     ", "", "elapsed_ms=170", line,
                sizeof(line)) &&
                std::strcmp(line, "      \x1b[1;36m|\x1b[0m elapsed_ms=170\x1b[0m") == 0,
            "whitespace tag also uses continuation gutter");
    require(rfbridge::format_console_tagged_line(
                ConsoleStyle::kPlain, ConsoleTone::kInfo, "", "RULE ACTION elapsed_ms=170",
                "elapsed_ms=170", line, sizeof(line)) &&
                std::strcmp(line, "RULE ACTION elapsed_ms=170") == 0,
            "plain continuation input preserves stable line");
    std::strcpy(line, "RC code=7");
    require(rfbridge::decorate_console_message(ConsoleStyle::kPlain, ConsoleTone::kInfo,
                                                "RF RX", "RX ", line, sizeof(line)) &&
                std::strcmp(line, "RX RC code=7") == 0,
            "plain in-place message preserves legacy prefix");
    std::strcpy(line, "RC code=7");
    require(rfbridge::decorate_console_message(ConsoleStyle::kPretty, ConsoleTone::kInfo,
                                                "RF RX", "RX ", line, sizeof(line)) &&
                std::strstr(line, "[RF RX]\x1b[0m RC code=7") != nullptr &&
                std::strcmp(line + std::strlen(line) - 4U, "\x1b[0m") == 0,
            "pretty in-place message decorates one bounded buffer");
    require(rfbridge::format_console_section_header("Wi-Fi", line, sizeof(line)) &&
                std::strstr(line, "+-- Wi-Fi ") != nullptr,
            "dashboard section header formatted");
    require(rfbridge::format_console_dashboard_row(
                "Last frame", "available", ConsoleTone::kInfo, "Console drops", "0",
                ConsoleTone::kMuted, line, sizeof(line)) &&
                std::strstr(line, "Console drops") != nullptr,
            "system dashboard row accepts existing labels");
    require(rfbridge::format_console_dashboard_row(
                "Advertised", "32", ConsoleTone::kInfo, "Current", "32",
                ConsoleTone::kInfo, line, sizeof(line)),
            "MQTT discovery count row fits the dashboard");
    require(rfbridge::format_console_dashboard_value(
                "Heap bytes", "123456 / 123456 / 123456", ConsoleTone::kInfo, line,
                sizeof(line)),
            "MQTT heap telemetry fits the dashboard");
    require(rfbridge::format_console_dashboard_value(
                "Profile", "esp32s3-supermini-fh4r2", ConsoleTone::kInfo, line,
                sizeof(line)),
            "full-width board profile identifiers fit the dashboard");
    require(rfbridge::format_console_dashboard_row(
                "MQTT stack", "6144", ConsoleTone::kInfo, "Worker stack", "4096",
                ConsoleTone::kInfo, line, sizeof(line)),
            "MQTT task stack telemetry fits the dashboard");
    require(rfbridge::format_console_dashboard_row(
                "Internal free", "123456 / 196100", ConsoleTone::kInfo, "Min / largest",
                "123456 / 73728", ConsoleTone::kInfo, line, sizeof(line)),
            "memory dashboard labels fit the dashboard");
    require(rfbridge::format_console_dashboard_row(
                "1234567890123", "1234567890123456789012", ConsoleTone::kSuccess,
                "1234567890123", "1234567890123456789012", ConsoleTone::kInfo, line,
                sizeof(line)),
            "dashboard accepts exact label and value limits");
    require(!rfbridge::format_console_dashboard_row(
                "12345678901234", "value", ConsoleTone::kSuccess, "label", "value",
                ConsoleTone::kInfo, line, sizeof(line)),
            "dashboard rejects overlong labels");
    require(!rfbridge::format_console_dashboard_row(
                "label", "12345678901234567890123", ConsoleTone::kSuccess, "label", "value",
                ConsoleTone::kInfo, line, sizeof(line)),
            "dashboard rejects overlong two-column values");
    require(!rfbridge::format_console_dashboard_value(
                "Network", "this value is deliberately longer than the sixty character dashboard limit 123",
                ConsoleTone::kInfo, line, sizeof(line)),
            "dashboard refuses truncation");
    require(rfbridge::format_console_wifi_disconnected_message(
                ConsoleStyle::kPlain, "iliadbox-2.4ghz", 8, "online", line, sizeof(line)) &&
                std::strcmp(line,
                            "WIFI DISCONNECTED ssid=iliadbox-2.4ghz reason=8 state=online") == 0,
            "plain Wi-Fi disconnect remains exact");
    require(rfbridge::format_console_wifi_disconnected_message(
                ConsoleStyle::kPretty, "iliadbox-2.4ghz", 8, "online", line, sizeof(line)) &&
                std::strcmp(line, "Disconnected from iliadbox-2.4ghz | reason 8") == 0 &&
                std::strstr(line, "ONLINE") == nullptr,
            "pretty Wi-Fi disconnect omits stale state");
}

void test_rf_activity_led_policy()
{
    const int radio_gpios[] = {18, 19, 23, 27, 26, 25};
    rfbridge::RfActivityLedConfig config{
        .enabled = true,
        .gpio = 2,
        .active_high = true,
        .pulse_ms = 25,
    };
    require(rfbridge::rf_activity_led_config_is_valid(config, radio_gpios,
                                                       std::size(radio_gpios)),
            "GPIO2 activity LED defaults are valid");
    require(rfbridge::rf_activity_led_active_level(config) == 1 &&
                rfbridge::rf_activity_led_inactive_level(config) == 0 &&
                rfbridge::rf_activity_led_pulse_us(config) == 25000,
            "active-high LED levels and pulse conversion are exact");
    constexpr uint8_t active_high_startup[] = {1, 0, 1, 0, 1, 0};
    constexpr uint64_t startup_durations_us[] = {25000, 150000, 25000,
                                                 150000, 25000, 150000};
    for (std::size_t phase = 0; phase < std::size(active_high_startup); ++phase) {
        const rfbridge::RfActivityLedStartupStep step =
            rfbridge::rf_activity_led_startup_step(config, static_cast<uint8_t>(phase));
        require(step.level == active_high_startup[phase] &&
                    step.duration_us == startup_durations_us[phase] &&
                    step.complete == (phase == rfbridge::kRfActivityLedStartupPhaseCount),
                "active-high startup sequence uses distinct 150 ms gaps");
    }
    require(rfbridge::kRfActivityLedStartupPulseCount == 3 &&
                rfbridge::kRfActivityLedStartupPhaseCount == 5 &&
                rfbridge::kRfActivityLedStartupGapMs == 150,
            "startup sequence has three active phases and fixed 150 ms gaps");
    config.active_high = false;
    require(rfbridge::rf_activity_led_active_level(config) == 0 &&
                rfbridge::rf_activity_led_inactive_level(config) == 1,
            "active-low LED levels are exact");
    constexpr uint8_t active_low_startup[] = {0, 1, 0, 1, 0, 1};
    for (std::size_t phase = 0; phase < std::size(active_low_startup); ++phase) {
        const rfbridge::RfActivityLedStartupStep step =
            rfbridge::rf_activity_led_startup_step(config, static_cast<uint8_t>(phase));
        require(step.level == active_low_startup[phase] &&
                    step.duration_us == startup_durations_us[phase] &&
                    step.complete == (phase == rfbridge::kRfActivityLedStartupPhaseCount),
                "active-low startup sequence preserves polarity and timing");
    }
    config.active_high = true;
    config.gpio = 25;
    require(!rfbridge::rf_activity_led_config_is_valid(config, radio_gpios,
                                                        std::size(radio_gpios)),
            "activity LED rejects CC1101 pin conflicts");
    for (const int reserved_gpio : {0, 1, 3, 5, 6, 11, 12, 15, 16, 17, 20, 24, 28, 34}) {
        config.gpio = reserved_gpio;
        require(!rfbridge::rf_activity_led_config_is_valid(config, radio_gpios,
                                                            std::size(radio_gpios)),
                "activity LED rejects unavailable output GPIO");
    }
    config.gpio = 2;
    config.pulse_ms = rfbridge::kRfActivityLedMinimumPulseMs - 1U;
    require(!rfbridge::rf_activity_led_config_is_valid(config, radio_gpios,
                                                        std::size(radio_gpios)),
            "activity LED rejects short pulses");
    config.pulse_ms = rfbridge::kRfActivityLedMaximumPulseMs + 1U;
    require(!rfbridge::rf_activity_led_config_is_valid(config, radio_gpios,
                                                        std::size(radio_gpios)),
            "activity LED rejects long pulses");
    config.enabled = false;
    config.gpio = -1;
    require(rfbridge::rf_activity_led_config_is_valid(config, nullptr, 0),
            "disabled activity LED ignores hardware configuration");

    const rfbridge::RfActivityLedDeadlineDecision pending =
        rfbridge::rf_activity_led_deadline_decision(1000, 1001);
    const rfbridge::RfActivityLedDeadlineDecision expired =
        rfbridge::rf_activity_led_deadline_decision(1000, 1000);
    require(!pending.turn_off && pending.rearm_us == 1,
            "activity LED rearms until the latest deadline");
    require(expired.turn_off && expired.rearm_us == 0,
            "activity LED turns off at the latest deadline");
}

void test_platform_board_policy()
{
    using rfbridge::BoardProfile;
    const rfbridge::BoardInfo *classic = rfbridge::board_info(BoardProfile::kEsp32Devkit);
    const rfbridge::BoardInfo *n16r8 =
        rfbridge::board_info(BoardProfile::kEsp32s3DevkitcN16r8);
    const rfbridge::BoardInfo *xiao = rfbridge::board_info(BoardProfile::kXiaoEsp32s3);
    const rfbridge::BoardInfo *supermini =
        rfbridge::board_info(BoardProfile::kEsp32s3SuperminiFh4r2);
    require(classic != nullptr && n16r8 != nullptr && xiao != nullptr && supermini != nullptr &&
                classic->flash_mib == 4 && classic->psram_mib == 0 &&
                !classic->combined_services && n16r8->flash_mib == 16 &&
                n16r8->psram_mib == 8 && n16r8->combined_services &&
                xiao->flash_mib == 8 && xiao->psram_mib == 8 &&
                xiao->combined_services && supermini->flash_mib == 4 &&
                supermini->psram_mib == 2 && supermini->combined_services &&
                supermini->console == rfbridge::ConsoleTransport::kUsbSerialJtag,
            "all supported board profiles expose their memory and service capabilities");
    require(rfbridge::board_info(static_cast<BoardProfile>(0)) == nullptr &&
                !rfbridge::board_profile_supports_combined_services(
                    static_cast<BoardProfile>(0)),
            "unknown board profiles are rejected");

    for (const rfbridge::BoardInfo *board : {classic, n16r8, xiao, supermini}) {
        require(rfbridge::board_cc1101_gpio_map_is_valid(board->profile, board->cc1101),
                "each profile's default CC1101 wiring is valid");
        require(rfbridge::board_generic_gpio_map_is_valid(
                    board->profile, board->cc1101, board->cc1101.generic_tx,
                    board->cc1101.generic_rx,
                    board->activity_led_enabled ? board->activity_led_gpio : -1),
                "each profile's generic RF wiring satisfies its overlap policy");
        rfbridge::BoardGpioMap duplicate = board->cc1101;
        duplicate.gdo2 = duplicate.gdo0;
        require(!rfbridge::board_cc1101_gpio_map_is_valid(board->profile, duplicate),
                "CC1101 wiring rejects duplicate GPIO ownership");
    }

    rfbridge::BoardGpioMap invalid = classic->cc1101;
    invalid.gdo0 = 34;
    require(!rfbridge::board_cc1101_gpio_map_is_valid(classic->profile, invalid),
            "classic profile rejects input-only GPIO for TX");
    invalid = n16r8->cc1101;
    invalid.sclk = 43;
    require(!rfbridge::board_cc1101_gpio_map_is_valid(n16r8->profile, invalid),
            "N16R8 profile reserves its UART0 console pins");
    invalid = n16r8->cc1101;
    invalid.miso = 26;
    require(!rfbridge::board_cc1101_gpio_map_is_valid(n16r8->profile, invalid),
            "S3 profiles reserve flash and PSRAM GPIOs");
    invalid = xiao->cc1101;
    invalid.sclk = 10;
    require(!rfbridge::board_cc1101_gpio_map_is_valid(xiao->profile, invalid),
            "XIAO profile rejects GPIOs that are not exposed on its headers");
    invalid = xiao->cc1101;
    invalid.miso = 21;
    require(!rfbridge::board_cc1101_gpio_map_is_valid(xiao->profile, invalid),
            "XIAO profile reserves its onboard activity LED");
    invalid = supermini->cc1101;
    invalid.gdo0 = 48;
    require(!rfbridge::board_cc1101_gpio_map_is_valid(supermini->profile, invalid),
            "FH4R2 profile reserves its onboard LED GPIO");
    invalid = supermini->cc1101;
    invalid.gdo2 = 3;
    require(!rfbridge::board_cc1101_gpio_map_is_valid(supermini->profile, invalid),
            "FH4R2 profile rejects its strapping GPIO");
    require(rfbridge::board_generic_gpio_map_is_valid(
                classic->profile, classic->cc1101, classic->cc1101.gdo0,
                classic->cc1101.generic_rx, classic->activity_led_gpio),
            "generic RF validation permits a CC1101 TX overlap");
    require(n16r8->cc1101.generic_tx == 13 && n16r8->cc1101.generic_rx == 4,
            "N16R8 exposes its temporary TX13 and RX4 generic mapping");
    require(rfbridge::board_generic_gpio_map_is_valid(
                n16r8->profile, n16r8->cc1101, n16r8->cc1101.gdo0,
                n16r8->cc1101.miso),
            "N16R8 permits valid overlaps in either direction");
    require(rfbridge::board_generic_gpio_map_is_valid(
                n16r8->profile, n16r8->cc1101, n16r8->cc1101.miso,
                n16r8->cc1101.gdo2),
            "N16R8 permits overlaps beyond its temporary aliases");
    invalid = n16r8->cc1101;
    invalid.miso = 6;
    require(rfbridge::board_generic_gpio_map_is_valid(
        n16r8->profile, invalid, invalid.miso, invalid.gdo0),
            "N16R8 permits an overlap with a remapped CC1101 GPIO");
    invalid = n16r8->cc1101;
    invalid.gdo0 = 7;
    require(rfbridge::board_generic_gpio_map_is_valid(
        n16r8->profile, invalid, invalid.miso, invalid.gdo0),
            "N16R8 permits an overlap with a remapped CC1101 GPIO");
    for (const rfbridge::BoardInfo *board : {classic, xiao, supermini}) {
        require(rfbridge::board_generic_gpio_map_is_valid(
                    board->profile, board->cc1101, board->cc1101.miso,
                    board->cc1101.generic_rx,
                    board->activity_led_enabled ? board->activity_led_gpio : -1),
                "all profiles permit generic TX overlap with CC1101 MISO");
        require(rfbridge::board_generic_gpio_map_is_valid(
                    board->profile, board->cc1101, board->cc1101.generic_tx,
                    board->cc1101.gdo0,
                    board->activity_led_enabled ? board->activity_led_gpio : -1),
                "all profiles permit generic RX overlap with CC1101 GDO0");
    }
    require(!rfbridge::board_generic_gpio_map_is_valid(
                classic->profile, classic->cc1101, classic->cc1101.generic_tx,
                classic->cc1101.generic_tx, classic->activity_led_gpio),
            "generic TX and RX cannot share a GPIO");
    require(!rfbridge::board_generic_gpio_map_is_valid(
                classic->profile, classic->cc1101, classic->cc1101.generic_tx, 34,
                classic->activity_led_gpio),
            "classic Generic RX rejects input-only GPIOs without pull-down support");
    int tx_options[rfbridge::kBoardGenericGpioOptionCapacity]{};
    std::size_t tx_count = 0;
    require(rfbridge::board_generic_gpio_options(
                classic->profile, true, classic->activity_led_gpio, tx_options,
                std::size(tx_options), &tx_count) && tx_count > 0,
            "generic TX options enumerate valid exposed GPIOs");
    int rx_options[rfbridge::kBoardGenericGpioOptionCapacity]{};
    std::size_t rx_count = 0;
    require(rfbridge::board_generic_gpio_options(
                classic->profile, false, classic->activity_led_gpio, rx_options,
                std::size(rx_options), &rx_count) &&
                std::find(rx_options, rx_options + rx_count, 34) == rx_options + rx_count,
            "classic Generic RX options omit input-only GPIOs");

    rfbridge::RfActivityLedConfig led{.enabled = true, .gpio = 21,
                                      .active_high = false, .pulse_ms = 25};
    const int xiao_radio[] = {7, 8, 9, 4, 2, 1};
    require(rfbridge::rf_activity_led_config_is_valid(
                BoardProfile::kXiaoEsp32s3, led, xiao_radio, std::size(xiao_radio)),
            "XIAO onboard active-low LED is valid beside the default radio map");
    led.gpio = 6;
    require(rfbridge::rf_activity_led_config_is_valid(
                BoardProfile::kXiaoEsp32s3, led, xiao_radio, std::size(xiao_radio)),
            "XIAO permits an exposed non-radio GPIO as an LED override");
    led.gpio = 2;
    require(!rfbridge::rf_activity_led_config_is_valid(
                BoardProfile::kXiaoEsp32s3, led, xiao_radio, std::size(xiao_radio)),
            "XIAO LED validation rejects a CC1101 pin");
    led.gpio = 48;
    require(!rfbridge::rf_activity_led_config_is_valid(
                BoardProfile::kEsp32s3DevkitcN16r8, led, nullptr, 0),
            "N16R8 profile reserves the board RGB LED GPIO");
    require(!rfbridge::rf_activity_led_config_is_valid(
                BoardProfile::kEsp32s3SuperminiFh4r2, led, nullptr, 0),
            "FH4R2 profile reserves the onboard LED GPIO");

    const auto descriptor_for = [](const rfbridge::BoardInfo &board) {
        return rfbridge::RfBoardImageDescriptor{
            .magic = {'R', 'F', 'B', 'D'},
            .version = rfbridge::kBoardImageDescriptorVersion,
            .size = sizeof(rfbridge::RfBoardImageDescriptor),
            .board_id = static_cast<uint8_t>(board.profile),
            .target_id = static_cast<uint8_t>(board.target),
            .flash_mib = board.flash_mib,
            .partition_layout_id = static_cast<uint8_t>(board.partition_layout),
            .reserved = {},
        };
    };
    const rfbridge::RfBoardImageDescriptor classic_descriptor = descriptor_for(*classic);
    const rfbridge::RfBoardImageDescriptor xiao_descriptor = descriptor_for(*xiao);
    const rfbridge::RfBoardImageDescriptor supermini_descriptor = descriptor_for(*supermini);
    require(rfbridge::board_image_descriptor_is_valid(classic_descriptor) &&
                rfbridge::board_image_descriptor_is_valid(xiao_descriptor) &&
                rfbridge::board_image_descriptor_is_valid(supermini_descriptor) &&
                rfbridge::board_image_descriptor_is_compatible(classic_descriptor,
                                                                classic_descriptor) &&
                !rfbridge::board_image_descriptor_is_compatible(classic_descriptor,
                                                                 xiao_descriptor) &&
                !rfbridge::board_image_descriptor_is_compatible(supermini_descriptor,
                                                                 xiao_descriptor),
            "OTA descriptors accept exact profiles and reject cross-board images");
    rfbridge::RfBoardImageDescriptor corrupted = classic_descriptor;
    corrupted.reserved[0] = 1;
    require(!rfbridge::board_image_descriptor_is_valid(corrupted),
            "OTA descriptors reject nonzero reserved bytes");
    corrupted = classic_descriptor;
    corrupted.flash_mib = 8;
    require(!rfbridge::board_image_descriptor_is_valid(corrupted),
            "OTA descriptors reject board metadata mismatches");
}

void test_learned_signal_matching()
{
    using rfbridge::LearnedMatchKind;
    using rfbridge::RfLearnFrameDisposition;
    require(rfbridge::classify_learn_frame_window(1000, 31001000, 1000, 1000) ==
                RfLearnFrameDisposition::kCapture,
            "frame at arm boundary captured");
    require(rfbridge::classify_learn_frame_window(1000, 31001000, 31000999, 2000) ==
                RfLearnFrameDisposition::kCapture,
            "frame before learn deadline captured");
    require(rfbridge::classify_learn_frame_window(1000, 31001000, 31001000, 2000) ==
                RfLearnFrameDisposition::kTimeout,
            "frame at learn deadline rejected");
    require(rfbridge::classify_learn_frame_window(1000, 31001000, 999, 999) ==
                RfLearnFrameDisposition::kIgnore,
            "frame accepted before arm ignored");
    require(rfbridge::classify_learn_frame_window(1000, 31001000, 2000, 999) ==
                RfLearnFrameDisposition::kIgnore,
            "capture that started before learning is ignored");

    rfbridge::RfStoredSignal incoming{};
    incoming.decoded.code = 0xA88142;
    incoming.decoded.pulse_us = 386;
    incoming.decoded.bits = 24;
    incoming.decoded.protocol = 1;
    rfbridge::LearnedSignalEntry entries[2]{};
    std::strcpy(entries[0].name.value, "gate");
    entries[0].signal = incoming;
    std::strcpy(entries[1].name.value, "porch");
    entries[1].signal = incoming;

    rfbridge::LearnedMatch match =
        rfbridge::find_learned_signal_match(incoming, entries, 1, true);
    require(match.kind == LearnedMatchKind::kUnique && match.count == 1 &&
                std::strcmp(match.name, "gate") == 0,
            "unique learned signal match retains its name");
    match = rfbridge::find_learned_signal_match(incoming, entries, 2, true);
    require(match.kind == LearnedMatchKind::kAmbiguous && match.count == 2 && match.name[0] == '\0',
            "ambiguous learned signal match does not choose a name");
    entries[0].signal.decoded.code ^= 1U;
    match = rfbridge::find_learned_signal_match(incoming, entries, 1, true);
    require(match.kind == LearnedMatchKind::kNone && match.count == 0,
            "unmatched learned signal reports none");
    match = rfbridge::find_learned_signal_match(incoming, nullptr, 0, false);
    require(match.kind == LearnedMatchKind::kUnavailable,
            "unavailable learned catalog remains distinguishable");
}

void test_storage_format()
{
    require(rfbridge::rf_storage_name_is_valid("Remote_1"), "valid storage name");
    require(!rfbridge::rf_storage_name_is_valid("list"), "list name reserved");
    require(!rfbridge::rf_storage_name_is_valid("1remote"), "numeric-leading name rejected");
    require(!rfbridge::rf_storage_name_is_valid("a-23456789012345"), "overlength name rejected");

    rfbridge::RfStoredSignal decoded{};
    decoded.encoding = rfbridge::RfStoredEncoding::kDecoded;
    decoded.decoded.code = 0xA88142;
    decoded.decoded.pulse_us = 386;
    decoded.decoded.bits = 24;
    decoded.decoded.protocol = 1;
    uint8_t record[rfbridge::kRfStorageMaxRecordSize]{};
    std::size_t record_size = 0;
    require(rfbridge::encode_rf_storage_record(decoded, record, sizeof(record), &record_size) ==
                rfbridge::RfStorageFormatResult::kOk &&
                record_size == 25,
            "decoded storage record encodes");
    constexpr uint8_t golden_record[] = {0x52, 0x46, 0x53, 0x52, 0x01, 0x01, 0x0D, 0x00, 0x42,
                                         0x81, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x01,
                                         0x18, 0x01, 0x00, 0xFA, 0x83, 0x81, 0x07};
    require(record_size == sizeof(golden_record) && std::memcmp(record, golden_record, sizeof(golden_record)) == 0,
            "version-one decoded storage record matches golden bytes");
    rfbridge::RfStoredSignal loaded{};
    require(rfbridge::decode_rf_storage_record(record, record_size, &loaded) ==
                rfbridge::RfStorageFormatResult::kOk &&
                loaded.encoding == rfbridge::RfStoredEncoding::kDecoded &&
                rfbridge::decoded_signals_match(decoded.decoded, loaded.decoded),
            "decoded storage record round trips");
    record[8] ^= 1U;
    require(rfbridge::decode_rf_storage_record(record, record_size, &loaded) ==
                rfbridge::RfStorageFormatResult::kInvalidCrc,
            "storage record CRC corruption rejected");

    rfbridge::RfStoredSignal raw{};
    raw.encoding = rfbridge::RfStoredEncoding::kRaw;
    raw.raw.count = rfbridge::kMaxRawPulses;
    raw.raw.start_level = 1;
    for (std::size_t index = 0; index < raw.raw.count; ++index) {
        raw.raw.durations_us[index] = static_cast<uint16_t>(100U + index);
    }
    require(rfbridge::encode_rf_storage_record(raw, record, sizeof(record), &record_size) ==
                rfbridge::RfStorageFormatResult::kOk &&
                record_size == rfbridge::kRfStorageMaxRecordSize,
            "maximum raw storage record encodes");
    require(rfbridge::decode_rf_storage_record(record, record_size, &loaded) ==
                rfbridge::RfStorageFormatResult::kOk &&
                loaded.encoding == rfbridge::RfStoredEncoding::kRaw &&
                rfbridge::raw_signals_match(raw.raw, loaded.raw),
            "maximum raw storage record round trips");

    uint8_t hardware_record[32]{};
    std::size_t hardware_size = 0;
    require(rfbridge::encode_rf_hardware_record(rfbridge::RfHardware::kGeneric,
                                                 hardware_record, sizeof(hardware_record),
                                                 &hardware_size) ==
                rfbridge::RfHardwareFormatResult::kOk && hardware_size == 13,
            "RF hardware record encodes with a versioned CRC envelope");
    rfbridge::RfHardware hardware = rfbridge::RfHardware::kCc1101;
    require(rfbridge::decode_rf_hardware_record(hardware_record, hardware_size, &hardware) ==
                rfbridge::RfHardwareFormatResult::kOk &&
                hardware == rfbridge::RfHardware::kGeneric,
            "RF hardware record decodes generic selection");
    hardware_record[hardware_size - 1U] ^= 0x01U;
    require(rfbridge::decode_rf_hardware_record(hardware_record, hardware_size, &hardware) ==
                rfbridge::RfHardwareFormatResult::kInvalidCrc,
            "RF hardware record rejects CRC corruption");

    uint8_t gpio_record[32]{};
    std::size_t gpio_size = 0;
    require(rfbridge::encode_rf_generic_gpio_record(
                static_cast<uint8_t>(rfbridge::BoardProfile::kEsp32s3DevkitcN16r8), 13, 4,
                gpio_record, sizeof(gpio_record), &gpio_size) ==
                rfbridge::RfGenericGpioFormatResult::kOk && gpio_size == 15,
            "generic GPIO record encodes with profile identity and CRC");
    uint8_t profile_id = 0;
    uint8_t tx_gpio = 0;
    uint8_t rx_gpio = 0;
    require(rfbridge::decode_rf_generic_gpio_record(
                gpio_record, gpio_size, &profile_id, &tx_gpio, &rx_gpio) ==
                rfbridge::RfGenericGpioFormatResult::kOk &&
                profile_id == static_cast<uint8_t>(rfbridge::BoardProfile::kEsp32s3DevkitcN16r8) &&
                tx_gpio == 13 && rx_gpio == 4,
            "generic GPIO record round trips");
    gpio_record[gpio_size - 1U] ^= 0x01U;
    require(rfbridge::decode_rf_generic_gpio_record(
                gpio_record, gpio_size, &profile_id, &tx_gpio, &rx_gpio) ==
                rfbridge::RfGenericGpioFormatResult::kInvalidCrc,
            "generic GPIO record rejects CRC corruption");
}

void test_recent_storage_format()
{
    rfbridge::RfRecentHistory history{};
    uint8_t record[rfbridge::kRfRecentMaxRecordSize]{};
    std::size_t record_size = 0;
    require(rfbridge::encode_rf_recent_record(history, record, sizeof(record), &record_size) ==
                rfbridge::RfRecentFormatResult::kOk && record_size == 20,
            "empty recent history record encodes");
    rfbridge::RfRecentHistory loaded{};
    require(rfbridge::decode_rf_recent_record(record, record_size, &loaded) ==
                rfbridge::RfRecentFormatResult::kOk && loaded.count == 0 && loaded.next_id == 1,
            "empty recent history record round trips");

    rfbridge::DecodedSignal decoded{};
    decoded.pulse_us = 386;
    decoded.bits = 24;
    decoded.protocol = 1;
    decoded.code = 0xA88142;
    rfbridge::RfRecentHistory single{};
    require(rfbridge::append_rf_recent_history(&single, decoded) ==
                rfbridge::RfRecentFormatResult::kOk &&
                rfbridge::encode_rf_recent_record(single, record, sizeof(record), &record_size) ==
                    rfbridge::RfRecentFormatResult::kOk &&
                rfbridge::decode_rf_recent_record(record, record_size, &loaded) ==
                    rfbridge::RfRecentFormatResult::kOk &&
                loaded.count == 1 && loaded.next_id == 2 && loaded.entries[0].id == 1,
            "one-entry recent history round trips with its next persistent ID");

    for (uint64_t code = 1; code <= 6; ++code) {
        decoded.code = code == 2 ? 1 : code;
        require(rfbridge::append_rf_recent_history(&history, decoded) ==
                    rfbridge::RfRecentFormatResult::kOk,
                "recent history accepts each decoded reception");
    }
    require(history.count == rfbridge::kRfRecentSignalCapacity && history.next_id == 7 &&
                history.entries[0].id == 6 && history.entries[4].id == 2 &&
                history.entries[4].decoded.code == 1,
            "recent history keeps duplicates and evicts only the oldest reception");
    require(rfbridge::encode_rf_recent_record(history, record, sizeof(record), &record_size) ==
                rfbridge::RfRecentFormatResult::kOk &&
                record_size == rfbridge::kRfRecentMaxRecordSize,
            "full recent history record reaches its fixed bound");
    require(rfbridge::decode_rf_recent_record(record, record_size, &loaded) ==
                rfbridge::RfRecentFormatResult::kOk && loaded.count == history.count &&
                loaded.next_id == history.next_id &&
                loaded.entries[0].id == 6 && loaded.entries[4].id == 2,
            "full recent history record round trips newest first");

    record[12] ^= 1U;
    require(rfbridge::decode_rf_recent_record(record, record_size, &loaded) ==
                rfbridge::RfRecentFormatResult::kInvalidCrc,
            "recent history rejects CRC corruption");
    record[12] ^= 1U;
    record[5] = static_cast<uint8_t>(rfbridge::kRfRecentSignalCapacity + 1U);
    rewrite_test_crc(record, record_size);
    require(rfbridge::decode_rf_recent_record(record, record_size, &loaded) ==
                rfbridge::RfRecentFormatResult::kInvalidRecord,
            "recent history rejects count metadata above its fixed capacity");
    require(rfbridge::encode_rf_recent_record(history, record, sizeof(record), &record_size) ==
                rfbridge::RfRecentFormatResult::kOk,
            "recent history re-encodes after count corruption test");
    record[4] = 2;
    rewrite_test_crc(record, record_size);
    require(rfbridge::decode_rf_recent_record(record, record_size, &loaded) ==
                rfbridge::RfRecentFormatResult::kInvalidVersion,
            "recent history rejects unsupported versions");

    rfbridge::DecodedSignal invalid = decoded;
    invalid.protocol = 0;
    require(rfbridge::append_rf_recent_history(&history, invalid) ==
                rfbridge::RfRecentFormatResult::kInvalidArgument,
            "recent history rejects invalid decoded payloads");
    history.next_id = UINT64_MAX;
    require(rfbridge::append_rf_recent_history(&history, decoded) ==
                rfbridge::RfRecentFormatResult::kInvalidArgument,
            "recent history fails closed before ID overflow");
    history.next_id = 7;
    history.entries[1].id = history.entries[0].id;
    require(!rfbridge::rf_recent_history_is_valid(history),
            "recent history rejects duplicate or unordered IDs");
}


rfbridge::RfStorageRuleEntry make_rule(const char *trigger, const char *target)
{
    rfbridge::RfStorageRuleEntry entry{};
    std::strncpy(entry.trigger_name, trigger, sizeof(entry.trigger_name) - 1U);
    std::strncpy(entry.rule.target_name, target, sizeof(entry.rule.target_name) - 1U);
    entry.rule.repeats = 8;
    entry.rule.cooldown_ms = rfbridge::kRfAutomationCooldownMs;
    return entry;
}

void test_wifi_config()
{
    rfbridge::WifiCredentials credentials{};
    std::strcpy(credentials.ssid, "Workshop WiFi");
    std::strcpy(credentials.password, "personal-password");
    uint8_t record[rfbridge::kWifiConfigMaxRecordSize]{};
    std::size_t size = 0;
    require(rfbridge::wifi_credentials_are_valid(credentials) &&
                rfbridge::encode_wifi_config_record(credentials, record, sizeof(record), &size) ==
                    rfbridge::WifiConfigFormatResult::kOk,
            "Wi-Fi credentials encode");
    rfbridge::WifiCredentials decoded{};
    require(rfbridge::decode_wifi_config_record(record, size, &decoded) ==
                    rfbridge::WifiConfigFormatResult::kOk &&
                std::strcmp(decoded.ssid, credentials.ssid) == 0 &&
                std::strcmp(decoded.password, credentials.password) == 0,
            "Wi-Fi credentials round trip");
    record[8] ^= 1U;
    require(rfbridge::decode_wifi_config_record(record, size, &decoded) ==
                rfbridge::WifiConfigFormatResult::kInvalidCrc,
            "Wi-Fi credential CRC rejects corruption");
    std::strcpy(credentials.password, "short");
    require(!rfbridge::wifi_credentials_are_valid(credentials), "short Wi-Fi passphrase rejected");
    credentials.password[0] = '\0';
    require(rfbridge::wifi_credentials_are_valid(credentials), "open Wi-Fi credentials accepted");
    require(rfbridge::network_wifi_retry_delay_ms(0) == 0 &&
                rfbridge::network_wifi_retry_delay_ms(1) == 1000 &&
                rfbridge::network_wifi_retry_delay_ms(5) == 16000 &&
                rfbridge::network_wifi_retry_delay_ms(6) == 30000 &&
                rfbridge::network_wifi_retry_delay_ms(20) == 30000,
            "Wi-Fi reconnect backoff is bounded");
    char connected_line[192]{};
    require(rfbridge::format_network_wifi_connected_line(
                "Workshop WiFi", 0x2a01a8c0U, 0x00ffffffU, 0x0101a8c0U,
                connected_line, sizeof(connected_line)) &&
                std::strcmp(connected_line,
                            "WIFI CONNECTED ssid=Workshop WiFi ip=192.168.1.42 "
                            "netmask=255.255.255.0 gateway=192.168.1.1") == 0,
            "Wi-Fi DHCP success line is stable");
}

void test_network_hostname_config()
{
    char hostname[rfbridge::kNetworkHostnameCapacity]{};
    require(rfbridge::canonicalize_network_hostname("Bridge-A1B2", hostname,
                                                     sizeof(hostname)) &&
                std::strcmp(hostname, "bridge-a1b2") == 0,
            "network hostname canonicalizes ASCII case");
    for (const char *invalid : {"", "-bridge", "bridge-", "bridge.local", "bridge_name",
                                "bridge name", "bridge/one", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
        require(!rfbridge::canonicalize_network_hostname(invalid, hostname, sizeof(hostname)),
                "invalid network hostname rejected");
    }
    require(rfbridge::canonicalize_network_hostname(
                "a2345678901234567890123456789012", hostname, sizeof(hostname)) &&
                std::strlen(hostname) == rfbridge::kNetworkHostnameMaxLength,
            "maximum network hostname accepted");

    constexpr uint8_t mac[] = {0x24, 0x6f, 0x28, 0xa1, 0xb2, 0xc3};
    char suffix[rfbridge::kNetworkHostnameMacSuffixCapacity]{};
    require(rfbridge::derive_default_network_hostname(mac, hostname, sizeof(hostname), suffix,
                                                       sizeof(suffix)) &&
                std::strcmp(hostname, "esp32-cc1101-a1b2c3") == 0 &&
                std::strcmp(suffix, "a1b2c3") == 0,
            "default network hostname uses the station MAC suffix");

    uint8_t record[rfbridge::kNetworkHostnameRecordMaxSize]{};
    std::size_t size = 0;
    require(rfbridge::encode_network_hostname_record(hostname, record, sizeof(record), &size) ==
                    rfbridge::NetworkHostnameFormatResult::kOk &&
                size == 8U + std::strlen(hostname) + 4U && record[6] == 0 && record[7] == 0,
            "network hostname record has the bounded v1 layout");
    char decoded[rfbridge::kNetworkHostnameCapacity]{};
    require(rfbridge::decode_network_hostname_record(record, size, decoded, sizeof(decoded)) ==
                    rfbridge::NetworkHostnameFormatResult::kOk &&
                std::strcmp(decoded, hostname) == 0,
            "network hostname record round trips");
    record[8] ^= 1U;
    require(rfbridge::decode_network_hostname_record(record, size, decoded, sizeof(decoded)) ==
                rfbridge::NetworkHostnameFormatResult::kInvalidCrc,
            "network hostname CRC rejects corruption");
    record[8] ^= 1U;
    record[4] = 2;
    require(rfbridge::decode_network_hostname_record(record, size, decoded, sizeof(decoded)) ==
                rfbridge::NetworkHostnameFormatResult::kInvalidVersion,
            "network hostname record rejects unknown versions");
    record[4] = 1;
    record[6] = 1;
    require(rfbridge::decode_network_hostname_record(record, size, decoded, sizeof(decoded)) ==
                rfbridge::NetworkHostnameFormatResult::kInvalidRecord,
            "network hostname record rejects reserved bytes");
    require(rfbridge::decode_network_hostname_record(record, size - 1U, decoded,
                                                       sizeof(decoded)) ==
                rfbridge::NetworkHostnameFormatResult::kInvalidRecord,
            "network hostname record rejects inconsistent sizes");
    require(rfbridge::encode_network_hostname_record("Bridge", record, sizeof(record), &size) ==
                rfbridge::NetworkHostnameFormatResult::kInvalidArgument,
            "network hostname records require canonical storage");
}

void test_network_mdns_policy()
{
    using rfbridge::NetworkMdnsState;
    require(!rfbridge::network_mdns_hostname_apply_needed(4, 4, false, 0, false),
            "mDNS skips an already applied hostname generation");
    require(rfbridge::network_mdns_hostname_apply_needed(3, 4, false, 0, false),
            "mDNS applies a new hostname generation");
    require(!rfbridge::network_mdns_hostname_apply_needed(3, 4, true, 4, false) &&
                rfbridge::network_mdns_hostname_apply_needed(3, 5, true, 4, false) &&
                rfbridge::network_mdns_hostname_apply_needed(3, 4, true, 4, true),
            "mDNS hostname failures retry only for a new generation or Web lifecycle");
    require(rfbridge::network_mdns_registration_is_complete(true, true, true) &&
                !rfbridge::network_mdns_registration_is_complete(true, true, false) &&
                !rfbridge::network_mdns_registration_is_complete(false, true, true),
            "mDNS readiness requires initialization and both services");
    require(rfbridge::network_mdns_reported_state(NetworkMdnsState::kReady, true, 5000, 5000) ==
                NetworkMdnsState::kReady &&
                rfbridge::network_mdns_reported_state(NetworkMdnsState::kReady, true, 5001, 5000) ==
                    NetworkMdnsState::kStalled &&
                rfbridge::network_mdns_reported_state(NetworkMdnsState::kFaulted, true, 6000, 5000) ==
                    NetworkMdnsState::kFaulted &&
                rfbridge::network_mdns_reported_state(NetworkMdnsState::kStarting, false, 0, 5000) ==
                    NetworkMdnsState::kStarting,
            "mDNS stall reporting is bounded to live owner states with a heartbeat");
}

void test_ota_policy()
{
    require(rfbridge::ota_http_upload_request_is_valid("application/octet-stream", 4096, 8192, 512) &&
                !rfbridge::ota_http_upload_request_is_valid("application/json", 4096, 8192, 512) &&
                !rfbridge::ota_http_upload_request_is_valid("application/octet-stream", 511, 8192, 512) &&
                !rfbridge::ota_http_upload_request_is_valid("application/octet-stream", 8193, 8192, 512),
            "OTA HTTP upload policy is strict");
    require(rfbridge::ota_project_name_is_compatible("esp32-cc1101", "esp32-cc1101") &&
                !rfbridge::ota_project_name_is_compatible("other", "esp32-cc1101"),
            "OTA project identity is exact");
    uint8_t sha256[rfbridge::kOtaSha256Size]{};
    for (std::size_t index = 0; index < sizeof(sha256); ++index) {
        sha256[index] = static_cast<uint8_t>(index);
    }
    char digest[rfbridge::kOtaSha256HexCapacity]{};
    require(rfbridge::format_ota_sha256(sha256, sizeof(sha256), digest, sizeof(digest)) &&
                std::strcmp(digest,
                            "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f") == 0 &&
                !rfbridge::format_ota_sha256(sha256, sizeof(sha256) - 1U, digest,
                                             sizeof(digest)) &&
                !rfbridge::format_ota_sha256(sha256, sizeof(sha256), digest,
                                             sizeof(digest) - 1U),
            "OTA ELF SHA-256 formatting is exact and bounded");
    require(rfbridge::ota_image_state_from_raw(0) == rfbridge::OtaImageState::kNew &&
                rfbridge::ota_image_state_from_raw(1) ==
                    rfbridge::OtaImageState::kPendingVerify &&
                rfbridge::ota_image_state_from_raw(2) == rfbridge::OtaImageState::kValid &&
                rfbridge::ota_image_state_from_raw(3) == rfbridge::OtaImageState::kInvalid &&
                rfbridge::ota_image_state_from_raw(4) == rfbridge::OtaImageState::kAborted &&
                rfbridge::ota_image_state_from_raw(UINT32_MAX) ==
                    rfbridge::OtaImageState::kUndefined &&
                rfbridge::ota_image_state_from_raw(5) == rfbridge::OtaImageState::kUnknown &&
                std::strcmp(rfbridge::ota_image_state_name(rfbridge::OtaImageState::kValid),
                            "valid") == 0,
            "OTA image states map to stable public names");
}

void test_automation_rules()
{
    rfbridge::RfStoredRule rule{};
    std::strcpy(rule.target_name, "A");
    rule.repeats = 8;
    rule.cooldown_ms = rfbridge::kRfAutomationCooldownMs;
    uint8_t record[rfbridge::kRfStorageMaxRuleRecordSize]{};
    std::size_t size = 0;
    require(rfbridge::encode_rf_rule_record(rule, record, sizeof(record), &size) ==
                rfbridge::RfRuleFormatResult::kOk,
            "rule record encodes");
    constexpr uint8_t golden[] = {0x52, 0x46, 0x52, 0x4C, 0x01, 0x00, 0x07, 0x00, 0x01, 0x08,
                                  0xE8, 0x03, 0x00, 0x00, 0x41, 0x6E, 0xFB, 0x09, 0xF0};
    require(size == sizeof(golden) && std::memcmp(record, golden, sizeof(golden)) == 0,
            "rule record matches golden bytes");
    rfbridge::RfStoredRule loaded{};
    require(rfbridge::decode_rf_rule_record(record, size, &loaded) == rfbridge::RfRuleFormatResult::kOk &&
                std::strcmp(loaded.target_name, "A") == 0 && loaded.repeats == 8 &&
                loaded.cooldown_ms == rfbridge::kRfAutomationCooldownMs,
            "rule record round trips");
    record[10] ^= 1U;
    require(rfbridge::decode_rf_rule_record(record, size, &loaded) == rfbridge::RfRuleFormatResult::kInvalidCrc,
            "rule CRC corruption rejected");

    rfbridge::RfStorageRuleEntry graph[] = {make_rule("B", "A"), make_rule("C", "B")};
    require(!rfbridge::rf_rule_graph_has_cycle(graph, 2), "acyclic rule graph accepted");
    require(rfbridge::rf_rule_would_create_cycle(graph, 2, "A", "C"), "directed cycle rejected");
    require(!rfbridge::rf_rule_would_create_cycle(graph, 2, "D", "A"), "shared target remains valid");
    require(!rfbridge::rf_automation_cooldown_allows(1000000, 1999999, 1000),
            "cooldown suppresses before boundary");
    require(rfbridge::rf_automation_cooldown_allows(1000000, 2000000, 1000),
            "cooldown allows exact boundary");

    rfbridge::RfStoredSignal decoded{};
    decoded.encoding = rfbridge::RfStoredEncoding::kDecoded;
    decoded.decoded.code = 0xA88142;
    decoded.decoded.pulse_us = 350;
    decoded.decoded.bits = 24;
    decoded.decoded.protocol = 1;
    rfbridge::RfStoredSignal raw{};
    raw.encoding = rfbridge::RfStoredEncoding::kRaw;
    require(rfbridge::build_decoded_raw(decoded.decoded, &raw.raw) &&
                rfbridge::rf_stored_signals_equivalent(decoded, raw),
            "decoded and raw stored signals match canonically");

    auto uniform_raw = [](uint16_t duration_us) {
        rfbridge::RfStoredSignal signal{};
        signal.encoding = rfbridge::RfStoredEncoding::kRaw;
        signal.raw.start_level = 1;
        signal.raw.count = 8;
        for (std::size_t index = 0; index < signal.raw.count; ++index) {
            signal.raw.durations_us[index] = duration_us;
        }
        return signal;
    };
    const rfbridge::RfStoredSignal low = uniform_raw(100);
    const rfbridge::RfStoredSignal middle = uniform_raw(108);
    const rfbridge::RfStoredSignal high = uniform_raw(116);
    const rfbridge::RfStoredSignal *triggers[] = {&low, &high};
    std::size_t matched_index = 0;
    require(rfbridge::rf_stored_signals_equivalent(low, middle) &&
                rfbridge::rf_stored_signals_equivalent(middle, high) &&
                !rfbridge::rf_stored_signals_equivalent(low, high),
            "raw matching has the expected non-transitive boundary");
    require(rfbridge::rf_find_unique_stored_signal_match(middle, triggers, 2, &matched_index) ==
                rfbridge::RfAutomationMatchResult::kAmbiguous,
            "overlapping raw trigger match is suppressed as ambiguous");
    require(rfbridge::rf_automation_event_is_current(4, 4, 2001, 2000) &&
                !rfbridge::rf_automation_event_is_current(3, 4, 3000, 2000) &&
                !rfbridge::rf_automation_event_is_current(4, 4, 2000, 2000),
            "stale rule-generation events are rejected");
    require(rfbridge::rf_automation_log_mode_allows(rfbridge::RfAutomationLogMode::kActions,
                                                     rfbridge::RfAutomationEventType::kTriggered) &&
                !rfbridge::rf_automation_log_mode_allows(rfbridge::RfAutomationLogMode::kActions,
                                                          rfbridge::RfAutomationEventType::kCooldownSuppressed) &&
                rfbridge::rf_automation_log_mode_allows(rfbridge::RfAutomationLogMode::kVerbose,
                                                         rfbridge::RfAutomationEventType::kCooldownSuppressed),
            "automation log mode filtering is exact");
    require(rfbridge::rf_automation_cooldown_remaining_ms(1000000, 1499001, 1000) == 501 &&
                rfbridge::rf_automation_cooldown_remaining_ms(1000000, 2000000, 1000) == 0,
            "automation cooldown logging remainder is rounded up");
}

void test_web_forms()
{
    char name[16]{};
    require(rfbridge::parse_web_learn_form("name=gate_1", 11, name, sizeof(name)) &&
                std::strcmp(name, "gate_1") == 0,
            "Web learn form accepts bounded names");
    require(!rfbridge::parse_web_learn_form("name=gate%201", 13, name, sizeof(name)),
            "Web learn form rejects encoded names");
    require(!rfbridge::parse_web_learn_form("name=gate&x=1", 13, name, sizeof(name)),
            "Web learn form rejects extra fields");

    rfbridge::WebReplayForm replay{};
    require(rfbridge::parse_web_replay_form("name=gate_1&repeats=7", 21, &replay) &&
                !replay.latest && std::strcmp(replay.name, "gate_1") == 0 && replay.repeats == 7,
            "Web named replay form accepts bounded values");
    require(rfbridge::parse_web_replay_form("name=&repeats=8", 15, &replay) && replay.latest,
            "Web latest replay form accepts empty name");
    require(!rfbridge::parse_web_replay_form("name=gate_1&repeats=7&extra=1", 29, &replay),
            "Web replay form rejects extra fields");

    rfbridge::WebSignalSaveForm signal_save{};
    constexpr char signal_save_nominal[] =
        "name=gate&code=13830801&bits=24&protocol=1";
    constexpr char signal_save_explicit[] =
        "name=gate_2&code=0xD30A91&bits=24&protocol=1&pulse_us=199";
    require(rfbridge::parse_web_signal_save_form(
                signal_save_nominal, sizeof(signal_save_nominal) - 1U, &signal_save) &&
                std::strcmp(signal_save.name, "gate") == 0 &&
                signal_save.code == 0xD30A91 && signal_save.pulse_us == 350,
            "Web manual save accepts required fields and normalizes nominal pulse");
    require(rfbridge::parse_web_signal_save_form(
                signal_save_explicit, sizeof(signal_save_explicit) - 1U, &signal_save) &&
                std::strcmp(signal_save.name, "gate_2") == 0 &&
                signal_save.pulse_us == 199,
            "Web manual save accepts an explicit pulse");
    constexpr char signal_save_overflow[] =
        "name=gate&code=0x1000000&bits=24&protocol=1";
    constexpr char signal_save_reserved[] =
        "name=list&code=1&bits=24&protocol=1";
    constexpr char signal_save_extra[] =
        "name=gate&code=1&bits=24&protocol=1&extra=1";
    constexpr char signal_save_duplicate[] =
        "name=gate&code=1&code=2&bits=24&protocol=1";
    require(!rfbridge::parse_web_signal_save_form(
                signal_save_overflow, sizeof(signal_save_overflow) - 1U, &signal_save) &&
                !rfbridge::parse_web_signal_save_form(
                    signal_save_reserved, sizeof(signal_save_reserved) - 1U, &signal_save) &&
                !rfbridge::parse_web_signal_save_form(
                    signal_save_extra, sizeof(signal_save_extra) - 1U, &signal_save) &&
                !rfbridge::parse_web_signal_save_form(
                    signal_save_duplicate, sizeof(signal_save_duplicate) - 1U, &signal_save),
            "Web manual save rejects overflow reserved names extra and duplicate fields");

    rfbridge::WebRecentForm recent{};
    constexpr char recent_replay[] = "action=replay&id=18446744073709551614&repeats=20";
    constexpr char recent_save[] = "action=save&id=42&name=gate_1";
    require(rfbridge::parse_web_recent_form(recent_replay, sizeof(recent_replay) - 1U,
                                            &recent) &&
                recent.action == rfbridge::WebRecentAction::kReplay &&
                recent.id == UINT64_MAX - 1U && recent.repeats == 20,
            "Web recent replay accepts string-safe 64-bit IDs");
    require(rfbridge::parse_web_recent_form(recent_save, sizeof(recent_save) - 1U, &recent) &&
                recent.action == rfbridge::WebRecentAction::kSave &&
                std::strcmp(recent.name, "gate_1") == 0,
            "Web recent save accepts exact fields");
    require(rfbridge::parse_web_recent_form("action=clear", 12, &recent) &&
                recent.action == rfbridge::WebRecentAction::kClear,
            "Web recent clear accepts its exact action");
    constexpr char recent_zero[] = "action=replay&id=0&repeats=8";
    constexpr char recent_reserved[] = "action=save&id=42&name=list";
    constexpr char recent_extra[] = "action=clear&extra=1";
    constexpr char recent_embedded_nul[] = "action=replay&id=42\0&repeats=8";
    require(!rfbridge::parse_web_recent_form(
                    recent_zero, sizeof(recent_zero) - 1U, &recent) &&
                !rfbridge::parse_web_recent_form(
                    recent_reserved, sizeof(recent_reserved) - 1U, &recent) &&
                !rfbridge::parse_web_recent_form(
                    recent_extra, sizeof(recent_extra) - 1U, &recent) &&
                !rfbridge::parse_web_recent_form(
                    recent_embedded_nul, sizeof(recent_embedded_nul) - 1U, &recent),
            "Web recent forms reject zero IDs reserved names extra fields and embedded NULs");

    rfbridge::WebDecodedForm decoded{};
    require(rfbridge::parse_web_decoded_form(
                "code=0xA88142&bits=24&protocol=1&pulse_us=0&repeats=8", 53, &decoded) &&
                decoded.code == 0xA88142 && decoded.bits == 24 && decoded.protocol == 1,
            "Web decoded transmit form accepts hexadecimal code");
    require(!rfbridge::parse_web_decoded_form(
                "code=0xA88142&bits=24&protocol=1&pulse_us=0&repeats=21", 54, &decoded),
            "Web decoded transmit form rejects excessive repeats");

    rfbridge::WebRawForm raw{};
    require(rfbridge::parse_web_raw_form(
                "start_level=0&durations=100,200,300,400,500,600,700,800&repeats=2", 65, &raw) &&
                raw.count == 8 && raw.repeats == 2,
            "Web raw transmit form accepts bounded alternating pulses");
    require(!rfbridge::parse_web_raw_form("start_level=0&durations=100,200,300&repeats=2", 45, &raw),
            "Web raw transmit form rejects short signals");
    for (const char *start_level : {"0", "1", "0x0", "0x1"}) {
        char body[128]{};
        std::snprintf(body, sizeof(body),
                      "start_level=%s&durations=100,200,300,400,500,600,700,800&repeats=2",
                      start_level);
        require(rfbridge::parse_web_raw_form(body, std::strlen(body), &raw) &&
                    raw.start_level <= 1,
                "Web raw transmit accepts both bounded start levels in decimal and hex");
    }
    for (const char *start_level : {"2", "9", "256", "257", "0x2", "0xf", "0x100"}) {
        char body[128]{};
        std::snprintf(body, sizeof(body),
                      "start_level=%s&durations=100,200,300,400,500,600,700,800&repeats=2",
                      start_level);
        require(!rfbridge::parse_web_raw_form(body, std::strlen(body), &raw),
                "Web raw transmit rejects out-of-range start levels before narrowing");
    }

    rfbridge::WebRuleAddForm rule{};
    require(rfbridge::parse_web_rule_add_form("trigger=gate&target=lamp&repeats=3", 34, &rule) &&
                std::strcmp(rule.trigger, "gate") == 0 && rule.repeats == 3,
            "Web rule add form accepts bounded names");
    require(!rfbridge::parse_web_rule_add_form("trigger=gate&target=gate&repeats=3", 34, &rule),
            "Web rule add form rejects self references");
    rfbridge::WebRulePatchForm patch{};
    require(rfbridge::parse_web_rule_patch_form("enabled=1", 9, &patch) && patch.enabled,
            "Web rule enabled patch accepts boolean");
    require(rfbridge::parse_web_rule_patch_form("enabled=0", 9, &patch) && !patch.enabled,
            "Web rule enabled patch accepts false");
    require(rfbridge::parse_web_rule_patch_form("enabled=0x1", 11, &patch) && patch.enabled,
            "Web rule enabled patch accepts bounded hexadecimal boolean");
    for (const char *body : {"enabled=2", "enabled=9", "enabled=256", "enabled=0x2",
                             "enabled=0xf", "enabled=0x100"}) {
        require(!rfbridge::parse_web_rule_patch_form(body, std::strlen(body), &patch),
                "Web rule enabled patch rejects numeric values above one");
    }
    require(rfbridge::parse_web_rule_patch_form("log_mode=verbose", 16, &patch) &&
                patch.patch == rfbridge::WebRulePatch::kLogMode,
            "Web rule log patch accepts exact mode");

    rfbridge::WebHardwareForm hardware{};
    constexpr char cc1101_hardware_form[] = "hardware=cc1101";
    constexpr char generic_hardware_form[] = "hardware=generic";
    require(rfbridge::parse_web_hardware_form(
                cc1101_hardware_form, sizeof(cc1101_hardware_form) - 1U, &hardware) &&
                hardware.hardware == rfbridge::RfHardware::kCc1101,
            "Web RF hardware form accepts the exact CC1101 value");
    require(rfbridge::parse_web_hardware_form(
                generic_hardware_form, sizeof(generic_hardware_form) - 1U, &hardware) &&
                hardware.hardware == rfbridge::RfHardware::kGeneric,
            "Web RF hardware form accepts the exact generic value");
    constexpr char embedded_hardware[] = "hardware=cc1101\0junk";
    require(!rfbridge::parse_web_hardware_form(
                embedded_hardware, sizeof(embedded_hardware) - 1U, &hardware) &&
                !rfbridge::parse_web_hardware_form("hardware=GENERIC", 16, &hardware) &&
                !rfbridge::parse_web_hardware_form("hardware=generic&extra=1", 24, &hardware),
            "Web RF hardware form rejects NUL suffixes, case changes, and extra fields");

    rfbridge::WebGenericGpioForm generic_gpio{};
    constexpr char generic_gpio_body[] = "tx_gpio=13&rx_gpio=4";
    require(rfbridge::parse_web_generic_gpio_form(
                generic_gpio_body, sizeof(generic_gpio_body) - 1U, &generic_gpio) &&
                generic_gpio.tx_gpio == 13 && generic_gpio.rx_gpio == 4,
            "Web generic GPIO form accepts bounded decimal values");
    constexpr char generic_gpio_hex[] = "tx_gpio=0x20&rx_gpio=33";
    require(rfbridge::parse_web_generic_gpio_form(generic_gpio_hex,
                                                   sizeof(generic_gpio_hex) - 1U,
                                                   &generic_gpio) &&
                generic_gpio.tx_gpio == 32 && generic_gpio.rx_gpio == 33,
            "Web generic GPIO form accepts hexadecimal values");
    constexpr char generic_gpio_overflow[] = "tx_gpio=49&rx_gpio=4";
    constexpr char generic_gpio_empty[] = "tx_gpio=13&rx_gpio=";
    constexpr char generic_gpio_extra[] = "tx_gpio=13&rx_gpio=4&extra=1";
    require(!rfbridge::parse_web_generic_gpio_form(
                generic_gpio_overflow, sizeof(generic_gpio_overflow) - 1U, &generic_gpio) &&
                !rfbridge::parse_web_generic_gpio_form(
                    generic_gpio_empty, sizeof(generic_gpio_empty) - 1U, &generic_gpio) &&
                !rfbridge::parse_web_generic_gpio_form(
                    generic_gpio_extra, sizeof(generic_gpio_extra) - 1U, &generic_gpio),
            "Web generic GPIO form rejects out-of-range and extra fields");

    require(rfbridge::web_form_content_type_is_valid("application/x-www-form-urlencoded"),
            "Web form content type accepted");
    require(!rfbridge::web_form_content_type_is_valid("application/json"),
            "Web JSON action rejected");
    require(rfbridge::web_octet_stream_content_type_is_valid("application/octet-stream"),
            "Web OTA content type accepted");
    constexpr uint32_t device_ip = 0x1101A8C0;
    require(rfbridge::web_host_matches_ipv4("192.168.1.17", device_ip, 80),
            "Web default-port Host accepted");
    require(rfbridge::web_host_matches_ipv4("192.168.1.17:80", device_ip, 80),
            "Web explicit default-port Host accepted");
    require(!rfbridge::web_host_matches_ipv4("rfbridge.local", device_ip, 80),
            "Web DNS rebinding Host rejected");
    require(!rfbridge::web_host_matches_ipv4("192.168.1.17:8032", device_ip, 80),
            "Web alternate port rejected");
    require(rfbridge::web_origin_matches_ipv4("http://192.168.1.17", device_ip, 80),
            "Web browser Origin accepted");
    require(rfbridge::web_origin_matches_ipv4("http://192.168.1.17:80", device_ip, 80),
            "Web explicit Origin port accepted");
    require(!rfbridge::web_origin_matches_ipv4("null", device_ip, 80),
            "Opaque browser Origin rejected");
    require(rfbridge::web_origin_matches_host("http://192.168.1.17", "192.168.1.17:80"),
            "Web normalized same origin accepted");

    rfbridge::WebDeviceHost device_host{};
    require(rfbridge::parse_web_device_host("ESP32-CC1101-A1B2C3.local:80", device_ip,
                                            "esp32-cc1101-a1b2c3",
                                            "esp32-cc1101-a1b2c3-2", 80, &device_host) &&
                std::strcmp(device_host.normalized, "esp32-cc1101-a1b2c3.local") == 0,
            "Web configured mDNS Host accepted and normalized");
    require(rfbridge::web_origin_matches_device_host(
                "http://esp32-cc1101-a1b2c3.local", device_host, 80) &&
                !rfbridge::web_origin_matches_device_host(
                    "http://esp32-cc1101-a1b2c3-2.local", device_host, 80),
            "Web mutation origin must match the actual configured Host");
    require(rfbridge::parse_web_device_host("esp32-cc1101-a1b2c3-2.local", device_ip,
                                            "esp32-cc1101-a1b2c3",
                                            "esp32-cc1101-a1b2c3-2", 80, &device_host) &&
                rfbridge::web_origin_matches_device_host(
                    "http://ESP32-CC1101-A1B2C3-2.local:80", device_host, 80),
            "Web effective conflict hostname and same origin accepted");
    require(rfbridge::parse_web_device_host("192.168.1.17", device_ip,
                                            "esp32-cc1101-a1b2c3", nullptr, 80,
                                            &device_host) &&
                rfbridge::web_origin_matches_device_host("http://192.168.1.17:80",
                                                         device_host, 80),
            "Web IPv4 normalized device Host remains accepted");
    for (const char *invalid_host : {"bridge.local.", "bridge.local:81", "bridge.example.local",
                                     "user@bridge.local", "https://bridge.local", "192.168.1.017"}) {
        require(!rfbridge::parse_web_device_host(invalid_host, device_ip, "bridge", nullptr,
                                                 80, &device_host),
                "Web malformed or unrelated Host rejected");
    }
    require(!rfbridge::web_origin_matches_device_host("https://192.168.1.17", device_host, 80) &&
                !rfbridge::web_origin_matches_device_host("null", device_host, 80),
            "Web HTTPS and opaque origins rejected");

    char escaped[64]{};
    require(rfbridge::escape_web_html("<&>\"'", escaped, sizeof(escaped)) &&
                std::strcmp(escaped, "&lt;&amp;&gt;&quot;&#39;") == 0,
            "Web HTML escaping covers active characters");
    char too_small[4]{};
    require(!rfbridge::escape_web_html("<", too_small, sizeof(too_small)),
            "Web HTML escaping rejects truncation");

    constexpr char json_active[] = {'s', 's', 'i', 'd', '"', '\\', '\b', '\f', '\n', '\r',
                                    '\t', '\x01', '\0'};
    char json_escaped[64]{};
    std::size_t json_length = 0;
    require(rfbridge::escape_web_json_string(json_active, json_escaped,
                                             sizeof(json_escaped), &json_length) &&
                std::strcmp(json_escaped, "ssid\\\"\\\\\\b\\f\\n\\r\\t\\u0001") == 0 &&
                json_length == std::strlen(json_escaped),
            "Web JSON escaping covers active and control characters");
    char json_too_small[2] = {'x', '\0'};
    json_length = 99;
    require(!rfbridge::escape_web_json_string("\"", json_too_small,
                                              sizeof(json_too_small), &json_length) &&
                json_too_small[0] == '\0' && json_length == 0,
            "Web JSON escaping rejects truncation atomically");

    char max_ssid[33]{};
    char expected_max_ssid[65]{};
    for (std::size_t index = 0; index < sizeof(max_ssid) - 1; ++index) {
        max_ssid[index] = (index % 2 == 0) ? '"' : '\\';
        expected_max_ssid[index * 2] = '\\';
        expected_max_ssid[index * 2 + 1] = max_ssid[index];
    }
    char escaped_max_ssid[65]{};
    json_length = 0;
    require(rfbridge::escape_web_json_string(max_ssid, escaped_max_ssid,
                                             sizeof(escaped_max_ssid), &json_length) &&
                json_length == sizeof(expected_max_ssid) - 1 &&
                std::strcmp(escaped_max_ssid, expected_max_ssid) == 0,
            "Web JSON escaping fits a worst-case 32-byte printable SSID exactly");
    char escaped_max_ssid_short[64] = {'x', '\0'};
    json_length = 99;
    require(!rfbridge::escape_web_json_string(max_ssid, escaped_max_ssid_short,
                                              sizeof(escaped_max_ssid_short), &json_length) &&
                escaped_max_ssid_short[0] == '\0' && json_length == 0,
            "Web JSON escaping rejects a worst-case SSID buffer one byte short");

    char ipv4[16]{};
    require(rfbridge::format_web_ipv4(0x1101A8C0, ipv4, sizeof(ipv4)) &&
                std::strcmp(ipv4, "192.168.1.17") == 0,
            "Web IPv4 formatting preserves network byte order");
    char ipv4_max[16]{};
    require(rfbridge::format_web_ipv4(0xFFFFFFFF, ipv4_max, sizeof(ipv4_max)) &&
                std::strcmp(ipv4_max, "255.255.255.255") == 0,
            "Web IPv4 formatting fits the longest address at exact capacity");
    char ipv4_too_small[15] = {'x', '\0'};
    require(!rfbridge::format_web_ipv4(0xFFFFFFFF, ipv4_too_small,
                                      sizeof(ipv4_too_small)) &&
                ipv4_too_small[0] == '\0',
            "Web IPv4 formatting rejects an exact-capacity buffer one byte short");
    char ipv4_zero[8]{};
    require(rfbridge::format_web_ipv4(0, ipv4_zero, sizeof(ipv4_zero)) &&
                std::strcmp(ipv4_zero, "0.0.0.0") == 0,
            "Web IPv4 formatting handles the minimum address at exact capacity");
    char ipv4_zero_capacity = 'x';
    require(!rfbridge::format_web_ipv4(0, nullptr, sizeof(ipv4_max)) &&
                !rfbridge::format_web_ipv4(0, &ipv4_zero_capacity, 0) &&
                ipv4_zero_capacity == 'x',
            "Web IPv4 formatting rejects null and zero-capacity outputs");
}

void test_random_signal_candidates()
{
    rfbridge::RandomSignalSaveResult candidate{};
    require(rfbridge::make_random_signal_candidate(0xA5D30A91U, &candidate) &&
                std::strcmp(candidate.name.value, "random_D30A91") == 0 &&
                candidate.decoded.code == 0xD30A91 &&
                candidate.decoded.bits == rfbridge::kRandomSignalBits &&
                candidate.decoded.protocol == rfbridge::kRandomSignalProtocol &&
                candidate.decoded.pulse_us == 350 &&
                rfbridge::rf_storage_name_is_valid(candidate.name.value),
            "random signal masks to a valid canonical 24-bit protocol 1 candidate");
    require(rfbridge::make_random_signal_candidate(0, &candidate) &&
                std::strcmp(candidate.name.value, "random_000000") == 0 &&
                candidate.decoded.code == 0,
            "random signal keeps the unbiased all-zero boundary");
    require(rfbridge::make_random_signal_candidate(UINT32_MAX, &candidate) &&
                std::strcmp(candidate.name.value, "random_FFFFFF") == 0 &&
                candidate.decoded.code == 0xFFFFFF,
            "random signal keeps the unbiased all-one boundary");
    require(!rfbridge::make_random_signal_candidate(1, nullptr) &&
                rfbridge::kRandomSignalMaximumAttempts == 8,
            "random signal rejects a missing result and keeps bounded retries");
}

void test_web_sse_format()
{
    char output[128]{};
    std::size_t length = 0;
    require(rfbridge::format_web_sse_event(481, "automation_config",
                                           "{\"revision\":9}", output, sizeof(output),
                                           &length) &&
                std::strcmp(output,
                            "id: 481\nevent: automation_config\ndata:{\"revision\":9}\n\n") == 0 &&
                length == std::strlen(output),
            "SSE event framing is exact and length-delimited");
    char exact[55]{};
    require(rfbridge::format_web_sse_event(481, "automation_config",
                                           "{\"revision\":9}", exact, sizeof(exact), nullptr),
            "SSE event framing fits the exact output capacity");
    char short_output[54] = {'x', '\0'};
    require(!rfbridge::format_web_sse_event(481, "automation_config",
                                            "{\"revision\":9}", short_output,
                                            sizeof(short_output), nullptr),
            "SSE event framing rejects truncation");
    require(!rfbridge::format_web_sse_event(1, "bad-name", "{}", output,
                                            sizeof(output), nullptr) &&
                !rfbridge::format_web_sse_event(1, "rx", "{\n}", output,
                                                sizeof(output), nullptr),
            "SSE framing rejects event-name and data-line injection");
}

}  // namespace

int main()
{
    test_protocol_table();
    test_protocol_round_trips();
    test_decoded_transport_boundaries();
    test_raw_identity_rules();
    test_protocol_alias_policy();
    test_console_parser();
    test_console_formatter();
    test_prompt_safe_line_editor();
    test_prompt_safe_masked_input();
    test_rf_activity_led_policy();
    test_platform_board_policy();
    test_learned_signal_matching();
    test_storage_format();
    test_recent_storage_format();
    test_wifi_config();
    test_network_hostname_config();
    test_network_mdns_policy();
    test_ota_policy();
    test_automation_rules();
    test_web_forms();
    test_web_sse_format();
    test_random_signal_candidates();
    std::puts("All host RF tests passed");
    return 0;
}
