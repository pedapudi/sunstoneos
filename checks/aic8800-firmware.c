#define AIC8800_TRANSPORT_FIXTURE_MAIN radio_transport_component_unused_main
#include "aic8800-sdio-transport.c"
#undef AIC8800_TRANSPORT_FIXTURE_MAIN
#include "aic8800_firmware.h"
#include <stdlib.h>

static struct aic8800_firmware firmware, saved_firmware;
static struct aic8800_sdio_transport saved_transport, identified_transport;
static struct sunxi_sdio saved_host, identified_host;
static struct sdio_card saved_card;
static struct model saved_model, identified_model;
static uint8_t *artifact[5], *transcript;
static size_t artifact_size[5], transcript_size, transcript_at, saved_transcript_at;
static unsigned firmware_commands, fw_block_commands, held_events;
static int drop_firmware_cfm, fail_firmware_tx;
static uint64_t power_record_time;
/* Radio memory written by firmware commands, kept per run to compare the
 * per-word and block-write table modes. Writes outside the RAM window, the
 * 0x40000000 registers, are logged in order. */
#define RAM_BASE 0x100000u
#define RAM_WORDS (0x200000u / 4)
static uint32_t ram[2][RAM_WORDS], other[2][64][2];
static uint8_t written[2][RAM_WORDS];
static unsigned other_count[2], table_commands;
static int memory_model, model_run;

static void store(uint32_t address, uint32_t value)
{
    if (address >= RAM_BASE && address - RAM_BASE < RAM_WORDS * 4) {
        assert(!(address & 3));
        ram[model_run][(address - RAM_BASE) / 4] = value; written[model_run][(address - RAM_BASE) / 4] = 1;
        return;
    }
    assert(other_count[model_run] < 64);
    other[model_run][other_count[model_run]][0] = address; other[model_run][other_count[model_run]++][1] = value;
}
static void model_tx_done(void)
{
    const uint8_t *p = m.transmitted;
    unsigned id = p[8] | (unsigned)p[9] << 8, n = 4;
    uint32_t address = get32(p + 16), value = get32(p + 20);
    uint8_t parameters[8] = {0};
    ++firmware_commands;
    if (firmware.phase == 2) ++table_commands; /* APPLY_BT_TABLE */
    if (id == 0x0402) { store(address, value); put32(parameters, address); put32(parameters + 4, value); n = 8; }
    else if (id == 0x040b) {
        assert(m.bytes == 1536 && value <= 1024 && !(value & 3)); ++fw_block_commands;
        for (uint32_t i = 0; i < value / 4; ++i) store(address + 4 * i, get32(p + 24 + 4 * i));
    } else if (id == 0x0400) {
        assert(address >= RAM_BASE && written[model_run][(address - RAM_BASE) / 4]);
        put32(parameters, address); put32(parameters + 4, ram[model_run][(address - RAM_BASE) / 4]); n = 8;
    } else { assert(id == 0x040d); put32(parameters, 0xa501); }
    memset(receive, 0, sizeof(receive));
    receive[0] = (uint8_t)(12 + n); receive[2] = 0x11;
    receive[4] = (uint8_t)(id + 1); receive[5] = (uint8_t)((id + 1) >> 8); receive[10] = (uint8_t)n;
    memcpy(receive + 16, parameters, n); m.card[1][4] = 1;
}

static void fw_tx_done(void)
{
    if (memory_model) { model_tx_done(); return; }
    assert(transcript_at + 20 <= transcript_size);
    const uint8_t *entry = transcript + transcript_at;
    uint32_t bytes = get32(entry), response_id = get32(entry + 4), n = get32(entry + 8);
    uint32_t delay = get32(entry + 12);
    assert(bytes == m.bytes && bytes <= 1536 && n <= 8);
    assert(transcript_at + 20 + bytes + n <= transcript_size);
    assert(!memcmp(m.transmitted, entry + 20, bytes));
    if (power_record_time) { assert(m.now - power_record_time >= 5); power_record_time = 0; }
    if (delay) power_record_time = m.now;
    ++firmware_commands;
    if (response_id == 0x040c) { ++fw_block_commands; assert(bytes == 1536); }
    memset(receive, 0, sizeof(receive));
    unsigned pos = 0;
    if (firmware_commands == 1) {
        confirmation(receive, 0x76543210, 0x98765432); pos = 24;
    }
    receive[pos] = (uint8_t)(12 + n); receive[pos + 2] = 0x11;
    receive[pos + 4] = (uint8_t)response_id; receive[pos + 5] = (uint8_t)(response_id >> 8);
    receive[pos + 10] = (uint8_t)n;
    memcpy(receive + pos + 16, entry + 20 + bytes, n); pos += 16 + n;
    if (response_id == 0x040e) {
        receive[pos] = 28; receive[pos + 2] = 0x11;
        receive[pos + 4] = 0x17; receive[pos + 5] = 4; receive[pos + 10] = 16;
        for (unsigned i = 0; i < 16; ++i) receive[pos + 16 + i] = (uint8_t)(0xa0 + i);
    }
    m.card[1][4] = drop_firmware_cfm ? 0 : 1;
    transcript_at += 20 + bytes + n;
}
static int firmware_write(void *context, uint32_t offset, uint32_t value)
{
    if (!transport.firmware) return transport_write(context, offset, value);
    if (offset == 0x18 && !(value & (1u << 21))) {
        uint32_t arg = m.regs[0x1c / 4]; unsigned cmd = value & 63;
        if (cmd == 52 && !(arg >> 31) && ((arg >> 28) & 7) == 1 && ((arg >> 9) & 0x1ffff) == 3)
            m.card[1][3] = 2; /* Strict source credit rule: 1536 < 2 * 1536. */
        if (cmd == 53) {
            assert(!(arg & (1u << 26)) && ((arg >> 28) & 7) == 1);
            assert(((arg >> 9) & 0x1ffff) == (arg >> 31 ? 0x10u : 0x0fu));
        }
    }
    if (fail_firmware_tx && offset == 0x200 && m.write && m.position == 4) { ++m.accesses; return 0; }
    int result = write_cb(context, offset, value);
    if (offset == 0x200 && m.write && m.position == m.bytes) fw_tx_done();
    return result;
}
static uint8_t *file_bytes(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb"); assert(f && !fseek(f, 0, SEEK_END));
    long n = ftell(f); assert(n > 0 && n < 8000000 && !fseek(f, 0, SEEK_SET));
    uint8_t *p = malloc((size_t)n); assert(p && fread(p, 1, (size_t)n, f) == (size_t)n);
    assert(!fclose(f)); *bytes = (size_t)n; return p;
}
static struct aic8800_firmware_config config(void)
{
    struct aic8800_firmware_config c = { .identity = transport.identity, .profile = 1 };
    for (unsigned i = 0; i < 4; ++i) c.blobs[i] = (struct aic8800_firmware_blob){artifact[i],artifact_size[i]};
    return c;
}
static void checkpoint(void)
{
    saved_model = m; saved_host = host; saved_card = card;
    saved_transport = transport; saved_firmware = firmware; saved_transcript_at = transcript_at;
}
static void restore(void)
{
    /* Fork an entirely simulated device from a verified immutable checkpoint. */
    m = saved_model; host = saved_host; card = saved_card;
    transport = saved_transport; firmware = saved_firmware; transcript_at = saved_transcript_at;
    firmware_commands = fw_block_commands = held_events = 0;
    drop_firmware_cfm = fail_firmware_tx = 0; power_record_time = 0;
}
static void run_firmware(void)
{
    for (unsigned i = 0; i < 120000 && transport.state == AIC8800_TRANSPORT_FIRMWARE; ++i) {
        step();
        if (firmware.state == AIC8800_FW_DELAY) {
            uint64_t until = firmware.delay_until;
            unsigned before = m.command_count;
            m.now = until - 1; aic8800_transport_poll(&transport);
            assert(firmware.state == AIC8800_FW_DELAY && m.command_count == before);
        }
        if (transport.held) {
            struct aic8800_sdio_response event;
            assert(aic8800_transport_peek(&transport, &event));
            unsigned before = m.command_count; uint8_t bytes[4096]; memcpy(bytes, transport.rx, sizeof(bytes));
            aic8800_transport_poll(&transport);
            assert(m.command_count == before && !memcmp(bytes, transport.rx, sizeof(bytes)));
            if (event.message_id == 0x0417) assert(firmware.binding_required && event.parameter_bytes == 16);
            else assert(event.address == 0x76543210);
            ++held_events; assert(aic8800_transport_consume(&transport));
        }
    }
}
/* Run the complete upload with each table mode from the same identified device
 * and compare the resulting radio memory and register write order. */
static void table_bursts(void)
{
    unsigned commands[2], table[2], blocks[2];
    for (model_run = 0; model_run < 2; ++model_run) {
        m = identified_model; host = identified_host; transport = identified_transport;
        memset(&firmware, 0, sizeof(firmware)); struct aic8800_firmware_config c = config();
        c.table_bursts = 2; assert(!aic8800_firmware_init(&firmware, &c));
        c.table_bursts = (uint8_t)model_run; assert(aic8800_firmware_init(&firmware, &c));
        memory_model = 1; firmware_commands = fw_block_commands = table_commands = held_events = 0;
        assert(aic8800_transport_load_firmware(&transport, &firmware, m.now + 200000));
        run_firmware();
        assert(transport.state == AIC8800_TRANSPORT_START_CONFIRMED && firmware.bt_patch_applied);
        assert(firmware.bootstatus == 0xa501 && !held_events && !firmware.table_burst);
        commands[model_run] = firmware_commands; table[model_run] = table_commands; blocks[model_run] = fw_block_commands;
    }
    printf("AIC firmware: table commands %u per word, %u with bursts; all commands %u and %u\n",
           table[0], table[1], commands[0], commands[1]);
    assert(commands[0] == 3256 && table[0] == 2880 && blocks[0] == 361);
    /* The 2,766-word TRAP run becomes ten 256-word and one 206-word block write. */
    assert(table[1] == 125 && commands[1] == 3256 - 2766 + 11 && blocks[1] == 361 + 11);
    assert(!memcmp(ram[0], ram[1], sizeof(ram[0])) && !memcmp(written[0], written[1], sizeof(written[0])));
    assert(other_count[0] == other_count[1] && !memcmp(other[0], other[1], sizeof(other[0])));
    memory_model = 0;
}
int main(int argc, char **argv)
{
    assert(argc == 7 || (argc == 8 && (!strcmp(argv[7], "mode-alias") || !strcmp(argv[7], "table-bursts"))));
    for (unsigned i = 0; i < 5; ++i) artifact[i] = file_bytes(argv[i + 1], &artifact_size[i]);
    transcript = file_bytes(argv[6], &transcript_size);
    begin(AIC8800_PAD_KEEP); setup(); identity(2000);
    while (transport.state == AIC8800_TRANSPORT_IDENTITY) step();
    assert(transport.state == AIC8800_TRANSPORT_COMPLETE);
    host.config.ops.write32 = firmware_write; /* Model binding before ownership handoff. */
    identified_transport = transport; identified_host = host; identified_model = m;
    if (argc == 8 && !strcmp(argv[7], "table-bursts")) {
        table_bursts();
        for (unsigned i = 0; i < 5; ++i) free(artifact[i]);
        free(transcript);
        puts("AIC firmware: table block writes leave the same radio memory and register order with 125 table commands");
        return 0;
    }
    struct aic8800_firmware_config c = config();
    assert(aic8800_firmware_init(&firmware, &c));
    assert(!aic8800_firmware_init(&firmware, &c));
    assert(aic8800_transport_load_firmware(&transport, &firmware, m.now + 200000));
    assert(!aic8800_transport_load_firmware(&transport, &firmware, m.now + 200000));
    unsigned initial_commands = m.command_count, verifying_steps = 0;
    for (unsigned i = 0; i < 7000 && firmware.state == AIC8800_FW_VERIFYING; ++i) { step(); ++verifying_steps; }
    printf("AIC firmware: %u verification steps\n", verifying_steps);
    assert(verifying_steps <= 100); /* Bounded 4 KiB digest steps, not one block per step. */
    for (unsigned i = 0; i < 7000 && firmware.state == AIC8800_FW_VALIDATING; ++i) step();
    assert(firmware.state == AIC8800_FW_PREPARING && m.command_count == initial_commands);
    checkpoint();
    if (argc == 8) {
        /* Select the source-defined UART mode pair in an already verified plan. */
        firmware.phase = 2; firmware.table_offset = 16;
        const uint8_t *table = artifact[2];
        while (get32(table + firmware.table_offset + 16) != 3)
            firmware.table_offset += 24 + (size_t)get32(table + firmware.table_offset + 20) * 8;
        firmware.pair_index = 4;
        aic8800_firmware_step(&firmware, &transport, m.now);
        assert(firmware.state == AIC8800_FW_REQUEST && get32(firmware.request + 16) == 0x201fc4);
        /* The independent transcript derives UART=2 from the pinned enum. */
        size_t offset = 0; int matched = 0;
        while (offset < transcript_size) {
            const uint8_t *entry = transcript + offset; size_t bytes = get32(entry), n = get32(entry + 8);
            if (entry[28] == 2 && get32(entry + 36) == 0x201fc4) {
                assert(get32(firmware.request + 20) == get32(entry + 40)); matched = 1; break;
            }
            offset += 20 + bytes + n;
        }
        assert(matched && get32(firmware.request + 20) == 2);
        restore();
        while (!transport.held && transport.state == AIC8800_TRANSPORT_FIRMWARE) step();
        assert(transport.held && firmware_commands == 1);
        uint8_t saved[sizeof(struct aic8800_sdio_response)];
        memcpy(saved, firmware.request, sizeof(saved));
        assert(!aic8800_transport_peek(&transport, (struct aic8800_sdio_response *)firmware.request));
        assert(!memcmp(saved, firmware.request, sizeof(saved)));
        memcpy(saved, artifact[0], sizeof(saved));
        assert(!aic8800_transport_peek(&transport, (struct aic8800_sdio_response *)artifact[0]));
        assert(!memcmp(saved, artifact[0], sizeof(saved)));
        struct aic8800_sdio_response event;
        assert(aic8800_transport_peek(&transport, &event) && event.address == 0x76543210);
        m.now = transport.deadline; step();
        assert(transport.state == AIC8800_TRANSPORT_FAULT);
        for (unsigned i = 0; i < 5; ++i) free(artifact[i]);
        free(transcript);
        puts("AIC firmware: source UART mode and immutable firmware peek alias refusal passed");
        return 0;
    }
    run_firmware();
    assert(transport.state == AIC8800_TRANSPORT_START_CONFIRMED && firmware.state == AIC8800_FW_START_CONFIRMED);
    assert(transcript_at == transcript_size && firmware_commands == 3256 && fw_block_commands == 361);
    assert(firmware.bt_patch_applied && firmware.binding_required && held_events == 2);
    assert(firmware.binding_challenge[0] == 0xa0 && firmware.binding_challenge[15] == 0xaf);
    assert(firmware.bootstatus == 0xa501 && firmware.first_nonzero_block_status == 0x80000003);

    restore(); drop_firmware_cfm = 1;
    while (!firmware_commands && transport.state == AIC8800_TRANSPORT_FIRMWARE) step();
    assert(firmware_commands == 1); m.now = transport.deadline; step();
    assert(transport.state == AIC8800_TRANSPORT_FAULT && firmware.state == AIC8800_FW_FAULT);
    assert(firmware.error == AIC8800_FW_TIME_ERROR && firmware.request_bytes == 1536);
    unsigned before = m.accesses; aic8800_transport_poll(&transport); assert(m.accesses == before);

    restore(); fail_firmware_tx = 1;
    while (transport.state == AIC8800_TRANSPORT_FIRMWARE) step();
    assert(transport.state == AIC8800_TRANSPORT_FAULT && firmware.state == AIC8800_FW_FAULT);
    assert(transport.last_result.transferred == 4 && host.request.data == transport.tx);
    assert(firmware.request_bytes == 1536 && firmware_commands == 0);

    /* Corrupt the smallest artifact: refuse before the first firmware command. */
    m = identified_model; host = identified_host; transport = identified_transport;
    memset(&firmware, 0, sizeof(firmware)); c = config();
    uint8_t corrupt[1708]; memcpy(corrupt, artifact[0], sizeof(corrupt)); corrupt[0] ^= 1;
    c.blobs[0].data = corrupt;
    assert(aic8800_firmware_init(&firmware, &c));
    assert(aic8800_transport_load_firmware(&transport, &firmware, m.now + 200000));
    before = m.command_count;
    for (unsigned i = 0; i < 40 && transport.state == AIC8800_TRANSPORT_FIRMWARE; ++i) step();
    assert(firmware.state == AIC8800_FW_FAULT && firmware.error == AIC8800_FW_HASH_ERROR);
    assert(m.command_count == before);

    /* A changed final byte of the largest artifact is refused after its last step. */
    m = identified_model; host = identified_host; transport = identified_transport;
    memset(&firmware, 0, sizeof(firmware)); c = config();
    size_t late_bytes = c.blobs[3].bytes; uint8_t *late = malloc(late_bytes); assert(late);
    memcpy(late, c.blobs[3].data, late_bytes); late[late_bytes - 1] ^= 0x80; c.blobs[3].data = late;
    assert(aic8800_firmware_init(&firmware, &c));
    assert(aic8800_transport_load_firmware(&transport, &firmware, m.now + 200000));
    before = m.command_count;
    for (unsigned i = 0; i < 200 && transport.state == AIC8800_TRANSPORT_FIRMWARE; ++i) step();
    assert(firmware.state == AIC8800_FW_FAULT && firmware.error == AIC8800_FW_HASH_ERROR);
    assert(firmware.hash_role == AIC8800_WIFI_IMAGE && m.command_count == before);
    free(late);

    memset(&firmware, 0, sizeof(firmware)); c = config(); c.identity.chip_word = 0x00010000;
    assert(!aic8800_firmware_init(&firmware, &c)); /* Missing non-H U01 artifacts. */
    c.identity.chip_word = 0x00030000;
    assert(!aic8800_firmware_init(&firmware, &c)); /* H image cannot stand in for non-H. */
    c.blobs[3] = (struct aic8800_firmware_blob){artifact[4],artifact_size[4]};
    assert(aic8800_firmware_init(&firmware, &c) && !firmware.h_variant);
    for (unsigned i = 0; i < 5; ++i) free(artifact[i]);
    free(transcript);
    puts("AIC firmware: pinned artifacts, source transcript, 1536-byte uploads, BT/Wi-Fi ordering, binding retention and faults passed");
    return 0;
}
