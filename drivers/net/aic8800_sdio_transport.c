/* Native bounded worker. Published register and protocol references are pinned
 * in docs/radio-transport.md; no Linux driver code or allocation is used. */
#include "aic8800_sdio_transport.h"
#include "aic8800_firmware.h"
#include "aic8800_runtime.h"
#include "aic8800_wifi.h"
#include "aic8800_data.h"
#include "aic8800_security.h"
#include "aic8800_supplicant.h"
#include <string.h>

enum { BLOCK_LO_READ, BLOCK_HI_READ, BLOCK_LO_WRITE, BLOCK_HI_WRITE,
    BLOCK_LO_VERIFY, BLOCK_HI_VERIFY, ENABLE_READ, ENABLE_WRITE, ENABLE_READY,
    PAD_F2, PAD_F1, SETTLE, BYTE_MODE, IRQ_READ, IRQ_WRITE,
    FUNCTION_IRQ_READ, FUNCTION_IRQ_WRITE, CREDIT, TX, RX_STATUS, RX_LENGTH,
    RX, PARSE, RESTORE_PAD, RESTORE_SETTLE, FIRMWARE_STEP, RUNTIME_STEP };
#define SETUP_OPERATION 2u
/* The configured command timeout covers this many receive bytes; a larger
 * aggregate read gets one timeout per started unit. */
#define RX_TIMEOUT_BYTES 4096u
static int span(const void *p, size_t n) { return p && n <= UINTPTR_MAX - (uintptr_t)p; }
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an;
}
static int independent(const struct aic8800_sdio_transport *t, const void *p, size_t n)
{
    if (!span(p, n) || overlap(t, sizeof(*t), p, n) ||
        overlap(t->card, sizeof(*t->card), p, n) ||
        overlap(t->card->host, sizeof(*t->card->host), p, n)) return 0;
    if (t->receive_owner && overlap(t->receive_owner,t->receive_owner_bytes,p,n)) return 0;
    if (t->security && overlap(t->security, sizeof(*t->security), p, n)) return 0;
    if (t->supplicant && overlap(t->supplicant, sizeof(*t->supplicant), p, n)) return 0;
    if (t->data && overlap(t->data, sizeof(*t->data), p, n)) return 0;
    if (t->wifi && overlap(t->wifi, sizeof(*t->wifi), p, n)) return 0;
    if (t->runtime && overlap(t->runtime, sizeof(*t->runtime), p, n)) return 0;
    if (t->firmware) {
        if (overlap(t->firmware, sizeof(*t->firmware), p, n)) return 0;
        for (unsigned i = 0; i < AIC8800_FIRMWARE_BLOBS; ++i)
            if (overlap(t->firmware->config.blobs[i].data, t->firmware->config.blobs[i].bytes, p, n)) return 0;
    }
    return 1;
}
static void fail(struct aic8800_sdio_transport *t, enum aic8800_transport_error e)
{
    if (t->state == AIC8800_TRANSPORT_FAULT) return;
    t->error = e; t->state = AIC8800_TRANSPORT_FAULT;
    if(t->security) aic8800_security_stop(t->security,t);
#ifdef SUNSTONE_AIC8800_RSN
    if(t->supplicant) aic8800_supplicant_stop(t->supplicant);
#endif
    if (t->data) aic8800_data_stop(t->data, t,
        e == AIC8800_TRANSPORT_TIMEOUT ? AIC8800_DATA_TIME_ERROR : AIC8800_DATA_TRANSPORT_ERROR);
    if (t->wifi) aic8800_wifi_stop(t->wifi, t,
        e == AIC8800_TRANSPORT_TIMEOUT ? AIC8800_WIFI_TIME_ERROR : AIC8800_WIFI_TRANSPORT_ERROR);
    if (t->runtime) aic8800_runtime_stop(t->runtime, t,
        e == AIC8800_TRANSPORT_TIMEOUT ? AIC8800_RUNTIME_TIME_ERROR : AIC8800_RUNTIME_TRANSPORT_ERROR);
    if (t->firmware) aic8800_firmware_stop(t->firmware, t,
        e == AIC8800_TRANSPORT_TIMEOUT ? AIC8800_FW_TIME_ERROR : AIC8800_FW_TRANSPORT_ERROR);
    sunxi_sdio_stop(t->card->host, e == AIC8800_TRANSPORT_TIMEOUT ?
                    SUNXI_SDIO_TIMEOUT : SUNXI_SDIO_CARD_ERROR);
    t->last_result = t->card->host->result;
}
static int observe(struct aic8800_sdio_transport *t, uint64_t *now)
{
    if (!sunxi_sdio_time(t->card->host, now)) { fail(t, AIC8800_TRANSPORT_HOST_ERROR); return 0; }
    if (*now >= t->deadline || (t->enable_deadline && *now >= t->enable_deadline)) {
        fail(t, AIC8800_TRANSPORT_TIMEOUT); return 0;
    }
    if (t->runtime) {
        aic8800_runtime_observe(t->runtime, t, *now);
        if (t->runtime->state == AIC8800_RUNTIME_FAULT) {
            fail(t, AIC8800_TRANSPORT_RUNTIME_ERROR); return 0;
        }
    }
    if (t->wifi) {
        aic8800_wifi_observe(t->wifi, t, *now);
        if (t->wifi->state == AIC8800_WIFI_FAULT) { fail(t, AIC8800_TRANSPORT_WIFI_ERROR); return 0; }
    }
    if (t->data) {
        aic8800_data_observe(t->data, t, *now);
        if (t->data->state == AIC8800_DATA_FAULT) { fail(t, AIC8800_TRANSPORT_DATA_ERROR); return 0; }
    }
    if(t->security) {
        aic8800_security_observe(t->security,t,*now);
        if(t->security->state==AIC8800_SECURITY_FAULT) {fail(t,AIC8800_TRANSPORT_DATA_ERROR);return 0;}
    }
#ifdef SUNSTONE_AIC8800_RSN
    if(t->supplicant) {
        aic8800_supplicant_observe(t->supplicant,*now);
        if(t->supplicant->state==AIC8800_SUPPLICANT_FAULT) {fail(t,AIC8800_TRANSPORT_DATA_ERROR);return 0;}
    }
#endif
    return 1;
}
static int later(uint64_t now, uint64_t delta, uint64_t *out)
{
    if (delta > UINT64_MAX - now) return 0;
    *out = now + delta; return 1;
}
int aic8800_transport_init(struct aic8800_sdio_transport *t, struct sdio_card *c,
                          const struct aic8800_transport_config *cfg)
{
    if (!span(t, sizeof(*t)) || !span(c, sizeof(*c)) || !span(cfg, sizeof(*cfg)) ||
        !span(c->host, sizeof(*c->host)) || overlap(t, sizeof(*t), c, sizeof(*c)) ||
        overlap(t, sizeof(*t), cfg, sizeof(*cfg)) || overlap(t, sizeof(*t), c->host, sizeof(*c->host)) ||
        overlap(c, sizeof(*c), cfg, sizeof(*cfg)) || overlap(c->host, sizeof(*c->host), cfg, sizeof(*cfg)) ||
        t->initialized || !c->initialized || !c->host->initialized || c->state != SDIO_CARD_READY ||
        c->host->state != SUNXI_SDIO_READY || c->host->entered || c->entered ||
        cfg->variant != AIC8800_TRANSPORT_D80_BOOTSTRAP ||
        (cfg->pad != AIC8800_PAD_KEEP && cfg->pad != AIC8800_PAD_WRITE_80) ||
        !cfg->ticks_per_ms || !cfg->command_timeout || !cfg->poll_interval ||
        c->function_count < 2 || !(c->capabilities & 2) ||
        !c->functions[1].has_manufacturer || c->functions[1].manufacturer != 0xc8a1 ||
        c->functions[1].device != 0x0182 || c->functions[1].max_block_bytes < 512 ||
        !c->functions[1].enable_timeout_ms ||
        cfg->ticks_per_ms > UINT64_MAX / c->functions[1].enable_timeout_ms) return 0;
    struct aic8800_sdio_transport initial = {0};
    initial.card = c; initial.config = *cfg; initial.initialized = 1; *t = initial; return 1;
}
int aic8800_transport_setup(struct aic8800_sdio_transport *t, uint64_t deadline)
{
    if (!t || !t->initialized || t->entered || t->state != AIC8800_TRANSPORT_NEW ||
        t->card->host->state != SUNXI_SDIO_READY || deadline <= t->card->host->last_now) return 0;
    t->deadline = deadline; t->phase = BLOCK_LO_READ; t->state = AIC8800_TRANSPORT_SETUP; return 1;
}
static void encode(struct aic8800_sdio_transport *t)
{
    t->tx_bytes = 512;
    memset(t->tx, 0, t->tx_bytes);
    if (t->expected == SETUP_OPERATION)
        t->codec_result = aic8800_sdio_encode_bootstrap_setup(t->tx, sizeof(t->tx));
    else t->codec_result = aic8800_sdio_encode_identity_read(
        (enum aic8800_identity_register)t->expected, t->tx, sizeof(t->tx));
    if (t->codec_result != AIC8800_CODEC_OK) fail(t, AIC8800_TRANSPORT_MESSAGE_ERROR);
}
int aic8800_transport_read_identity(struct aic8800_sdio_transport *t,
                                  const struct aic8800_identity_admission *a, uint64_t deadline)
{
    if (!t || !t->initialized || t->entered || t->state != AIC8800_TRANSPORT_READY ||
        !independent(t, a, sizeof(*a)) || !a->record || a->address != AIC8800_SDIO_SETUP_ADDRESS || a->value != AIC8800_SDIO_SETUP_VALUE ||
        a->awake != 1 || a->no_outstanding_debug_operations != 1 ||
        t->card->host->state != SUNXI_SDIO_READY || deadline <= t->card->host->last_now) return 0;
    t->admission = *a; t->deadline = deadline; t->wait_until = 0;
    t->expected = SETUP_OPERATION; t->state = AIC8800_TRANSPORT_IDENTITY;
    t->phase = CREDIT; encode(t); return 1;
}
int aic8800_transport_load_firmware(struct aic8800_sdio_transport *t,
                                  struct aic8800_firmware *f, uint64_t deadline)
{
    if (!t || !t->initialized || t->entered || t->state != AIC8800_TRANSPORT_COMPLETE ||
        t->held || t->pending || t->firmware || t->card->host->state != SUNXI_SDIO_READY ||
        !independent(t, f, sizeof(*f)) || !f->initialized || f->state != AIC8800_FW_NEW ||
        deadline <= t->card->host->last_now) return 0;
    for (unsigned i = 0; i < AIC8800_FIRMWARE_BLOBS; ++i)
        if (!independent(t, f->config.blobs[i].data, f->config.blobs[i].bytes)) return 0;
    uint64_t delay = t->config.ticks_per_ms / 2 + t->config.ticks_per_ms % 2;
    if (!aic8800_firmware_claim(f, &t->identity, t, t->card->host->last_now, deadline, delay)) return 0;
    t->firmware = f; t->deadline = deadline; t->wait_until = 0;
    t->phase = FIRMWARE_STEP; t->state = AIC8800_TRANSPORT_FIRMWARE; return 1;
}
int aic8800_transport_start_runtime(struct aic8800_sdio_transport *t,
                                  struct aic8800_runtime *r, uint64_t deadline)
{
    if (!t || !t->initialized || t->entered || t->state != AIC8800_TRANSPORT_START_CONFIRMED ||
        t->held || t->pending || t->runtime || !t->firmware ||
        t->firmware->state != AIC8800_FW_START_CONFIRMED || t->card->host->state != SUNXI_SDIO_READY ||
        !independent(t, r, sizeof(*r)) || !r->initialized || r->state != AIC8800_RUNTIME_NEW ||
        deadline <= t->card->host->last_now) return 0;
    if (!aic8800_runtime_claim(r, t, t->card->host->last_now,
            t->firmware->binding_required ? t->firmware->binding_challenge : NULL)) return 0;
    t->runtime = r; t->deadline = deadline; t->wait_until = 0;
    t->phase = RUNTIME_STEP; t->state = AIC8800_TRANSPORT_RUNTIME; return 1;
}
static int data_busy(const struct aic8800_sdio_transport *t)
{ return t->data && (t->data->state == AIC8800_DATA_REQUEST || t->data->state == AIC8800_DATA_WAITING); }
int aic8800_transport_renew_runtime(struct aic8800_sdio_transport *t, uint64_t deadline)
{
    if (!t || !t->initialized || t->entered || t->state != AIC8800_TRANSPORT_RUNTIME ||
        !t->runtime || t->runtime->state != AIC8800_RUNTIME_RECEIVING || t->held || t->pending ||
        data_busy(t) || t->cursor != t->rx_bytes || t->card->host->state != SUNXI_SDIO_READY ||
        t->card->host->last_now >= t->deadline || deadline <= t->deadline ||
        (t->wifi && t->wifi->state != AIC8800_WIFI_READY && t->wifi->state != AIC8800_WIFI_SCAN_DONE) ||
        (t->phase != RUNTIME_STEP && t->phase != RX_STATUS)) return 0;
    uint64_t now; t->entered = 1;
    int ok = observe(t, &now);
    if (ok) t->deadline = deadline;
    t->entered = 0; return ok;
}
static int runtime_idle(const struct aic8800_sdio_transport *t)
{
    return t && t->initialized && !t->entered && t->state == AIC8800_TRANSPORT_RUNTIME &&
        t->runtime && t->runtime->state == AIC8800_RUNTIME_RECEIVING && !t->held && !t->pending &&
        t->cursor == t->rx_bytes && t->card->host->state == SUNXI_SDIO_READY &&
        (t->phase == RUNTIME_STEP || t->phase == RX_STATUS);
}
int aic8800_transport_attachment_ready(const struct aic8800_sdio_transport *t)
{
    return runtime_idle(t)&&t->data&&t->data->state==AIC8800_DATA_IDLE&&
        !t->data_tx&&!t->security_tx;
}
int aic8800_transport_start_wifi(struct aic8800_sdio_transport *t, struct aic8800_wifi *w, uint64_t deadline)
{
    if (!runtime_idle(t) || t->wifi || !independent(t, w, sizeof(*w)) || !w->initialized ||
        w->state != AIC8800_WIFI_NEW || deadline > t->deadline || deadline <= t->card->host->last_now) return 0;
    uint64_t now; t->entered = 1; int ok = observe(t, &now);
    if (ok) ok = aic8800_wifi_claim(w, t, now, deadline);
    if (ok) { t->wifi = w; t->phase = RUNTIME_STEP; t->wait_until = 0; }
    t->entered = 0; return ok;
}
int aic8800_transport_scan(struct aic8800_sdio_transport *t, uint64_t deadline)
{
    if (!runtime_idle(t) || !t->wifi || t->wifi->state != AIC8800_WIFI_READY || deadline > t->deadline) return 0;
    uint64_t now; t->entered = 1; int ok = observe(t, &now);
    if (ok) ok = aic8800_wifi_scan(t->wifi, t, now, deadline);
    if (ok) { t->phase = RUNTIME_STEP; t->wait_until = 0; }
    t->entered = 0; return ok;
}
int aic8800_transport_connect(struct aic8800_sdio_transport *t,const struct aic8800_wifi_target *target,uint64_t deadline)
{
    if(!runtime_idle(t)||!t->wifi||(data_busy(t)||t->security)||!independent(t,target,sizeof(*target))||deadline>t->deadline)return 0;
    uint64_t now;t->entered=1;int ok=observe(t,&now);
    if(ok)ok=aic8800_wifi_connect(t->wifi,t,target,now,deadline);
    if(ok){t->phase=RUNTIME_STEP;t->wait_until=0;}
    t->entered=0;return ok;
}
int aic8800_transport_disconnect(struct aic8800_sdio_transport *t,uint16_t reason,uint64_t deadline)
{
    if(!runtime_idle(t)||!t->wifi||data_busy(t)||deadline>t->deadline)return 0;
    uint64_t now;t->entered=1;int ok=observe(t,&now);
    if(ok)ok=aic8800_wifi_disconnect(t->wifi,t,reason,now,deadline);
    if(ok){t->phase=RUNTIME_STEP;t->wait_until=0;}
    t->entered=0;return ok;
}
int aic8800_transport_peek_station(const struct aic8800_sdio_transport *t,struct aic8800_wifi_station_event *out)
{
    if(!t||!t->initialized||t->entered||!t->wifi||!t->held||!t->wifi->station_held||
        t->unrelated.record!=t->wifi->station_record||!independent(t,out,sizeof(*out)))return 0;
    *out=t->wifi->station_event;return 1;
}
int aic8800_transport_peek_scan(const struct aic8800_sdio_transport *t, struct aic8800_wifi_scan_result *out)
{
    if (!t || !t->initialized || t->entered || !t->wifi || !t->held || !t->wifi->held ||
        t->unrelated.record != t->wifi->held_record || !independent(t, out, sizeof(*out))) return 0;
    *out = t->wifi->result; return 1;
}
int aic8800_transport_take_scan(struct aic8800_sdio_transport *t, struct aic8800_wifi_scan_summary *out)
{
    if (!runtime_idle(t) || !t->wifi || t->wifi->state != AIC8800_WIFI_SCAN_DONE ||
        t->wifi->held || !independent(t, out, sizeof(*out))) return 0;
    *out = t->wifi->summary; t->wifi->state = AIC8800_WIFI_READY; return 1;
}
int aic8800_transport_start_data(struct aic8800_sdio_transport *t,struct aic8800_data *d)
{
    if(!runtime_idle(t)||!t->wifi||t->wifi->state!=AIC8800_WIFI_READY||t->data||
        !independent(t,d,sizeof(*d))||!d->initialized||d->owner||d->state!=AIC8800_DATA_IDLE)return 0;
    uint64_t now;t->entered=1;int ok=observe(t,&now);
    if(ok)ok=aic8800_data_claim(d,t,t->wifi,now);
    if(ok)t->data=d;
    t->entered=0;return ok;
}
int aic8800_transport_authorize_open_data(struct aic8800_sdio_transport *t,uint64_t record)
{
    if(!runtime_idle(t)||!t->data)return 0;
    uint64_t now;t->entered=1;int ok=observe(t,&now);
    if(ok)ok=aic8800_data_authorize_open(t->data,t,record);
    t->entered=0;return ok;
}
int aic8800_transport_send_data(struct aic8800_sdio_transport *t,const uint8_t *p,size_t n,uint64_t deadline)
{
    if(!runtime_idle(t)||!t->data||(t->supplicant&&t->supplicant->state!=AIC8800_SUPPLICANT_AUTHORIZED)||
        !independent(t,p,n)||deadline>t->deadline||(t->supplicant&&n>=14&&p[12]==0x88&&p[13]==0x8e))return 0;
    uint64_t now;t->entered=1;int ok=observe(t,&now);
    if(ok)ok=aic8800_data_send(t->data,t,p,n,now,deadline);
    if(ok){t->phase=RUNTIME_STEP;t->wait_until=0;}
    t->entered=0;return ok;
}
int aic8800_transport_take_data_result(struct aic8800_sdio_transport *t,struct aic8800_data_result *out)
{
    if(!t||!t->initialized||t->entered||t->state!=AIC8800_TRANSPORT_RUNTIME||!t->data||
        t->data->state!=AIC8800_DATA_DONE||(t->supplicant&&t->supplicant->action_active)||!independent(t,out,sizeof(*out)))return 0;
    *out=t->data->result;t->data->state=AIC8800_DATA_IDLE;return 1;
}
int aic8800_transport_peek_data(const struct aic8800_sdio_transport *t,struct aic8800_data_frame *out)
{
    if(!t||!t->initialized||t->entered||!t->data||!t->held||!t->data->held||
        t->unrelated.record!=t->data->held_record||!independent(t,out,sizeof(*out)))return 0;
    *out=t->data->frame;return 1;
}
#ifdef SUNSTONE_AIC8800_RSN
int aic8800_transport_start_security(struct aic8800_sdio_transport *t,struct aic8800_security *sec,
    struct aic8800_supplicant *s,uint64_t deadline)
{
    if(!runtime_idle(t)||!t->data||t->data->state!=AIC8800_DATA_IDLE||t->security||t->supplicant||
        !independent(t,sec,sizeof(*sec))||!independent(t,s,sizeof(*s))||
        overlap(sec,sizeof(*sec),s,sizeof(*s))||!sec->initialized||sec->owner||!s->initialized||s->owner||
        deadline>t->deadline||deadline<=t->card->host->last_now)return 0;
    uint64_t now;t->entered=1;int ok=observe(t,&now);
    if(ok) {
        ok=aic8800_supplicant_claim(s,t,sec,now,deadline);
        /* A claim that retained either owner is irrevocable even on failure. */
        if(sec->owner==t){t->security=sec;t->supplicant=s;t->data->security=sec;}
        if(!ok&&t->security)fail(t,AIC8800_TRANSPORT_DATA_ERROR);
        if(ok){t->phase=RUNTIME_STEP;t->wait_until=0;}
    }
    t->entered=0;return ok;
}
int aic8800_transport_retire_security(struct aic8800_sdio_transport *t,uint64_t deadline)
{
    if(!runtime_idle(t)||!t->supplicant)return 0;
    uint64_t now;t->entered=1;int ok=observe(t,&now);
    if(ok)ok=aic8800_supplicant_retire(t->supplicant,now,deadline);
    if(ok){t->phase=RUNTIME_STEP;t->wait_until=0;}t->entered=0;return ok;
}
int aic8800_transport_finish_security(struct aic8800_sdio_transport *t)
{
    if(!runtime_idle(t)||!t->supplicant||t->supplicant->state!=AIC8800_SUPPLICANT_RETIRED||
        !t->security||!t->security->retired||t->data->state!=AIC8800_DATA_IDLE)return 0;
    t->data->security=0;t->security=0;t->supplicant=0;return 1;
}
#endif
static int cmd52(struct aic8800_sdio_transport *t, unsigned fn, uint32_t addr, int write, uint8_t value,
                 struct sunxi_sdio_request *r)
{
    return sdio_card_cmd52(t->card, fn, addr, write, 0, value, r);
}
static int admit(struct aic8800_sdio_transport *t, uint64_t now)
{
    struct sunxi_sdio_request r = {0};
    uint64_t deadline;
    if (now < t->wait_until) return 1;
    uint64_t timeout = t->config.command_timeout;
    if (t->phase == RX) {
        uint64_t units = (t->rx_bytes + RX_TIMEOUT_BYTES - 1) / RX_TIMEOUT_BYTES;
        if (timeout > UINT64_MAX / units) return 0;
        timeout *= units;
    }
    if (!later(now, timeout, &deadline)) return 0;
    if (deadline > t->deadline) deadline = t->deadline;
    if (t->enable_deadline && deadline > t->enable_deadline) deadline = t->enable_deadline;
    int ok;
    switch (t->phase) {
    case BLOCK_LO_READ: case BLOCK_LO_VERIFY: ok = cmd52(t, 0, 0x110, 0, 0, &r); break;
    case BLOCK_HI_READ: case BLOCK_HI_VERIFY: ok = cmd52(t, 0, 0x111, 0, 0, &r); break;
    case BLOCK_LO_WRITE: ok = cmd52(t, 0, 0x110, 1, 0, &r); break;
    case BLOCK_HI_WRITE: ok = cmd52(t, 0, 0x111, 1, 2, &r); break;
    case ENABLE_READ: ok = cmd52(t, 0, 2, 0, 0, &r); break;
    case ENABLE_WRITE: ok = cmd52(t, 0, 2, 1, t->prior_enable | 2, &r); break;
    case ENABLE_READY: ok = cmd52(t, 0, 3, 0, 0, &r); break;
    case PAD_F2: ok = cmd52(t, 0, 0xf2, 1, 0x7f, &r); break;
    case PAD_F1: ok = cmd52(t, 0, 0xf1, 1, 0x80, &r); break;
    case SETTLE: t->phase = BYTE_MODE; return 1;
    case RESTORE_PAD: ok = cmd52(t, 0, 0xf1, 1, 0, &r); break;
    case RESTORE_SETTLE: t->expected = 0; t->phase = CREDIT; encode(t); return 1;
    case BYTE_MODE: ok = cmd52(t, 1, 7, 1, 1, &r); break;
    case IRQ_READ: ok = cmd52(t, 0, 4, 0, 0, &r); break;
    case IRQ_WRITE: ok = cmd52(t, 0, 4, 1, t->prior_irq | 7, &r); break;
    case FUNCTION_IRQ_READ: ok = cmd52(t, 1, 0, 0, 0, &r); break;
    case FUNCTION_IRQ_WRITE: ok = cmd52(t, 1, 0, 1, t->prior_function_irq | 7, &r); break;
    case FIRMWARE_STEP:
        aic8800_firmware_step(t->firmware, t, now);
        if (t->firmware->state == AIC8800_FW_FAULT) { fail(t, AIC8800_TRANSPORT_FIRMWARE_ERROR); return 1; }
        if (t->firmware->state == AIC8800_FW_REQUEST) {
            t->tx_bytes = t->firmware->request_bytes;
            if (!t->tx_bytes || t->tx_bytes > sizeof(t->tx) || t->tx_bytes % 512) return 0;
            memcpy(t->tx, t->firmware->request, t->tx_bytes);
            t->phase = CREDIT;
        }
        return 1;
    case RUNTIME_STEP:
        /* Do not transmit binding while a Wi-Fi command awaits confirmation. */
        if (t->wifi && t->wifi->state == AIC8800_WIFI_WAITING) { t->phase = RX_STATUS; return 1; }
#ifdef SUNSTONE_AIC8800_RSN
        if(t->supplicant) {
            aic8800_supplicant_step(t->supplicant,now);
            if(t->supplicant->state==AIC8800_SUPPLICANT_FAULT) {fail(t,AIC8800_TRANSPORT_DATA_ERROR);return 1;}
            if(t->security->state==AIC8800_SECURITY_WAITING){t->phase=RX_STATUS;return 1;}
        }
#endif
        aic8800_runtime_step(t->runtime, t, now);
        if (t->runtime->state == AIC8800_RUNTIME_FAULT) { fail(t, AIC8800_TRANSPORT_RUNTIME_ERROR); return 1; }
        if (t->runtime->state == AIC8800_RUNTIME_REQUEST) {
            t->tx_bytes = sizeof(t->runtime->request);
            memcpy(t->tx, t->runtime->request, t->tx_bytes);
            t->phase = CREDIT;
        } else if (t->runtime->state == AIC8800_RUNTIME_RECEIVING && t->wifi) {
            aic8800_wifi_step(t->wifi, t, now);
            if (t->wifi->state == AIC8800_WIFI_FAULT) { fail(t, AIC8800_TRANSPORT_WIFI_ERROR); return 1; }
            if (t->wifi->state == AIC8800_WIFI_REQUEST) {
                t->tx_bytes = sizeof(t->wifi->request);
                memcpy(t->tx, t->wifi->request, t->tx_bytes);
                t->phase = CREDIT;
            } else if(t->security && t->security->state == AIC8800_SECURITY_REQUEST) {
                t->tx_bytes=sizeof(t->security->request);
                memcpy(t->tx,t->security->request,t->tx_bytes);
                t->security_tx=1;t->phase=CREDIT;
            } else if(t->data && t->data->state == AIC8800_DATA_REQUEST) {
                /* The data worker's request stays unchanged until aic8800_data_sent,
                 * so CMD53 borrows it directly instead of copying it into tx. */
                t->tx_bytes=t->data->request_bytes;
                if(!t->tx_bytes||t->tx_bytes>sizeof(t->data->request)||t->tx_bytes%512)return 0;
                t->data_tx=1;t->phase=CREDIT;
            } else t->phase = RX_STATUS;
        } else if (t->runtime->state != AIC8800_RUNTIME_VERIFYING) t->phase = RX_STATUS;
        return 1;
    case CREDIT: ok = cmd52(t, 1, 3, 0, 0, &r); break;
    case RX_STATUS: ok = cmd52(t, 1, 4, 0, 0, &r); break;
    case RX_LENGTH: ok = cmd52(t, 1, 5, 0, 0, &r); break;
    case TX: ok = sdio_card_cmd53(t->card, 1, 0x10, 1, 0, 512, t->data_tx ? t->data->request : t->tx,
                     t->tx_bytes, &r); break;
    case RX: ok = sdio_card_cmd53(t->card, 1, 0x0f, 0, 0,
                     t->rx_bytes > 512 ? 512 : 0, t->rx, t->rx_bytes, &r); break;
    default: return 0;
    }
    if (!ok || !sunxi_sdio_submit(t->card->host, &r, deadline)) return 0;
    t->pending = 1; return 1;
}
static void advanced(struct aic8800_sdio_transport *t)
{ if (t->progress != UINT64_MAX) ++t->progress; }
static void delay(struct aic8800_sdio_transport *t, uint64_t now, uint64_t ticks)
{
    if (!later(now, ticks, &t->wait_until)) fail(t, AIC8800_TRANSPORT_TIMEOUT);
}
static void rx_size(struct aic8800_sdio_transport *t, size_t bytes)
{
    t->rx_bytes = bytes; t->cursor = 0;
    if (!bytes || bytes > sizeof(t->rx) || (bytes & 3) || (bytes > 512 && bytes % 512)) {
        fail(t, AIC8800_TRANSPORT_RX_BOUNDS); return;
    }
    t->phase = RX;
}
static void completed(struct aic8800_sdio_transport *t, uint64_t now)
{
    uint8_t value = (uint8_t)t->last_result.response;
    switch (t->phase) {
    case BLOCK_LO_READ: t->prior_block_bytes = value; ++t->phase; break;
    case BLOCK_HI_READ: t->prior_block_bytes |= (uint16_t)value << 8; ++t->phase; break;
    case BLOCK_LO_VERIFY: if (value) { fail(t, AIC8800_TRANSPORT_SETUP_ERROR); break; } ++t->phase; break;
    case BLOCK_HI_VERIFY: if (value != 2) { fail(t, AIC8800_TRANSPORT_SETUP_ERROR); break; } ++t->phase; break;
    case ENABLE_READ: t->prior_enable = value; ++t->phase; break;
    case ENABLE_WRITE:
        if (!later(now, t->config.ticks_per_ms * t->card->functions[1].enable_timeout_ms,
                   &t->enable_deadline)) { fail(t, AIC8800_TRANSPORT_TIMEOUT); break; }
        ++t->phase; break;
    case ENABLE_READY:
        if (value & 2) { t->enable_deadline = 0; ++t->phase; }
        else delay(t, now, t->config.poll_interval);
        break;
    case PAD_F2:
        if (t->config.pad == AIC8800_PAD_WRITE_80) ++t->phase;
        else { t->phase = SETTLE; delay(t, now, t->config.ticks_per_ms); }
        break;
    case PAD_F1: t->phase = SETTLE; delay(t, now, t->config.ticks_per_ms); break;
    case RESTORE_PAD: t->phase = RESTORE_SETTLE; delay(t, now, t->config.ticks_per_ms); break;
    case IRQ_READ: t->prior_irq = value; ++t->phase; break;
    case FUNCTION_IRQ_READ: t->prior_function_irq = value; ++t->phase; break;
    case FUNCTION_IRQ_WRITE: t->state = AIC8800_TRANSPORT_READY; break;
    case CREDIT:
        if (t->data_tx ? value >= 3 : (value && t->tx_bytes < (size_t)value * 1536)) t->phase = TX;
        else delay(t, now, t->config.poll_interval);
        break;
    case TX:
        if(t->security_tx) {
            if(!aic8800_security_sent(t->security,t)) {fail(t,AIC8800_TRANSPORT_DATA_ERROR);break;}
            t->security_tx=0;aic8800_security_zero(t->tx,sizeof(t->tx));
        } else if(t->data_tx) {
            if(!aic8800_data_sent(t->data,t)) {fail(t,AIC8800_TRANSPORT_DATA_ERROR);break;}
            t->data_tx=0;
        } else if (t->wifi && t->wifi->state == AIC8800_WIFI_REQUEST) {
            if (!aic8800_wifi_sent(t->wifi, t)) { fail(t, AIC8800_TRANSPORT_WIFI_ERROR); break; }
        } else if (t->runtime) {
            if (!aic8800_runtime_sent(t->runtime, t)) { fail(t, AIC8800_TRANSPORT_RUNTIME_ERROR); break; }
        } else if (t->firmware && !aic8800_firmware_sent(t->firmware, t)) { fail(t, AIC8800_TRANSPORT_FIRMWARE_ERROR); break; }
        t->phase = RX_STATUS; break;
    case RX_STATUS:
        t->raw_rx_status = value;
        if (value & 0x80) { fail(t, AIC8800_TRANSPORT_UNSUPPORTED_IRQ); break; }
        if (!value) {
            delay(t, now, t->config.poll_interval);
            if (t->runtime) t->phase = RUNTIME_STEP;
            break;
        }
        if ((value | 8u) > 120u) {
            if ((value | 8u) == 127u) t->phase = RX_LENGTH;
            else rx_size(t, (value & 7u) * 512u);
        } else if (value == 120) t->phase = RX_LENGTH;
        else rx_size(t, (value & 0x7fu) * 512u);
        break;
    case RX_LENGTH: t->byte_length = value; rx_size(t, (size_t)value * 4); break;
    case RX: t->phase = PARSE; break;
    default: ++t->phase; break;
    }
}
static void aggregate_done(struct aic8800_sdio_transport *t)
{
    t->cursor = t->rx_bytes;
    if (t->runtime) { t->phase = RUNTIME_STEP; return; }
    if (t->firmware) {
        if (t->firmware->state == AIC8800_FW_WAITING) t->phase = RX_STATUS;
        else if (t->firmware->state == AIC8800_FW_START_CONFIRMED) t->state = AIC8800_TRANSPORT_START_CONFIRMED;
        else t->phase = FIRMWARE_STEP;
        return;
    }
    if (t->expected == SETUP_OPERATION) {
        if (!t->setup_confirmed) t->phase = RX_STATUS;
        else if (t->config.pad == AIC8800_PAD_WRITE_80) t->phase = RESTORE_PAD;
        else { t->expected = 0; t->phase = CREDIT; encode(t); }
        return;
    }
    if (!t->have[t->expected]) { t->phase = RX_STATUS; return; }
    if (!t->expected) { t->expected = 1; t->phase = CREDIT; encode(t); return; }
    t->codec_result = aic8800_sdio_identity(&t->confirmations[0], &t->confirmations[1], &t->identity);
    if (t->codec_result != AIC8800_CODEC_OK) fail(t, AIC8800_TRANSPORT_MESSAGE_ERROR);
    else t->state = AIC8800_TRANSPORT_COMPLETE;
}
static void parse(struct aic8800_sdio_transport *t, uint64_t now)
{
    if (t->held) return;
    if (t->cursor == t->rx_bytes) { aggregate_done(t); return; }
    struct aic8800_sdio_response r;
    if (t->runtime)
        t->codec_result = aic8800_runtime_response(t->runtime, t, t->rx + t->cursor,
            t->rx_bytes - t->cursor, now, &r);
    else if (t->firmware)
        t->codec_result = aic8800_firmware_response(t->firmware, t, t->rx + t->cursor,
            t->rx_bytes - t->cursor, now, &r);
    else if (t->expected == SETUP_OPERATION)
        t->codec_result = aic8800_sdio_decode_bootstrap_setup_response(t->rx + t->cursor,
            t->rx_bytes - t->cursor, &r);
    else t->codec_result = aic8800_sdio_decode_identity_response(t->rx + t->cursor,
        t->rx_bytes - t->cursor, (enum aic8800_identity_register)t->expected, &r);
    if (t->codec_result == AIC8800_CODEC_END) { aggregate_done(t); return; }
    if (t->runtime) {
        if (t->runtime->state == AIC8800_RUNTIME_FAULT) { fail(t, AIC8800_TRANSPORT_RUNTIME_ERROR); return; }
        if (t->wifi && t->codec_result == AIC8800_CODEC_UNRELATED) {
            t->codec_result = aic8800_wifi_response(t->wifi, t, &r, now);
            if (t->wifi->state == AIC8800_WIFI_FAULT) { fail(t, AIC8800_TRANSPORT_WIFI_ERROR); return; }
        }
        if(t->security && t->codec_result == AIC8800_CODEC_UNRELATED) {
            t->codec_result=aic8800_security_response(t->security,t,&r,now);
            if(t->security->state==AIC8800_SECURITY_FAULT) {
                t->unrelated=r;t->held=1;fail(t,AIC8800_TRANSPORT_DATA_ERROR);return;
            }
        }
        if(t->data && t->codec_result == AIC8800_CODEC_UNRELATED) {
            t->codec_result=aic8800_data_response(t->data,t,&r,now);
            if(t->data->state==AIC8800_DATA_FAULT) {
                t->unrelated=r;t->held=1;fail(t,AIC8800_TRANSPORT_DATA_ERROR);return;
            }
        }
#ifdef SUNSTONE_AIC8800_RSN
        if(t->supplicant&&t->data->held&&t->data->held_record==r.record&&t->data->frame.eapol&&
            aic8800_supplicant_receive(t->supplicant,&t->data->frame,now)) {
            aic8800_data_consumed(t->data,t,r.record);t->codec_result=AIC8800_CODEC_OK;
        }
        if(t->supplicant&&t->supplicant->state==AIC8800_SUPPLICANT_FAULT) {
            t->unrelated=r;t->held=1;fail(t,AIC8800_TRANSPORT_DATA_ERROR);return;
        }
#endif
        if (t->codec_result == AIC8800_CODEC_OK) t->cursor += r.consumed;
        else if (t->codec_result == AIC8800_CODEC_UNRELATED) { t->unrelated = r; t->held = 1; }
        else fail(t, AIC8800_TRANSPORT_MESSAGE_ERROR);
        return;
    }
    if (t->firmware) {
        if (t->firmware->state == AIC8800_FW_FAULT) { fail(t, AIC8800_TRANSPORT_FIRMWARE_ERROR); return; }
        if (t->codec_result == AIC8800_CODEC_OK) t->cursor += r.consumed;
        else if (t->codec_result == AIC8800_CODEC_UNRELATED) { t->unrelated = r; t->held = 1; }
        else fail(t, AIC8800_TRANSPORT_MESSAGE_ERROR);
        return;
    }
    int have = t->expected == SETUP_OPERATION ? t->setup_confirmed : t->have[t->expected];
    if (t->codec_result == AIC8800_CODEC_OK && !have) {
        if (t->expected == SETUP_OPERATION) {
            t->setup_confirmation = (struct aic8800_sdio_response){ .is_memory_write = 1,
                .message_id = r.message_id, .parameter_bytes = r.parameter_bytes,
                .address = r.address, .value = r.value };
            t->setup_confirmed = 1;
        } else {
            t->confirmations[t->expected] = (struct aic8800_sdio_response){ .is_memory_read = 1,
                .address = r.address, .value = r.value };
            t->have[t->expected] = 1;
        }
        t->cursor += r.consumed;
    } else if (t->codec_result == AIC8800_CODEC_OK || t->codec_result == AIC8800_CODEC_UNRELATED) {
        t->unrelated = r; t->held = 1;
    } else fail(t, AIC8800_TRANSPORT_MESSAGE_ERROR);
}
static void poll_receive(struct aic8800_sdio_transport *t,const void *owner)
{
    uint64_t now;
    if (!t || !t->initialized || t->entered || t->receive_owner!=owner ||
        (t->state != AIC8800_TRANSPORT_SETUP && t->state != AIC8800_TRANSPORT_IDENTITY &&
         t->state != AIC8800_TRANSPORT_FIRMWARE && t->state != AIC8800_TRANSPORT_RUNTIME)) return;
    t->entered = 1;
    enum aic8800_transport_state state = t->state;
    uint8_t phase = t->phase, pending = t->pending, held = t->held;
    size_t cursor = t->cursor;
    uint64_t host_progress = t->card->host->progress;
    if (!observe(t, &now)) goto out;
    if (t->pending) {
        sunxi_sdio_poll(t->card->host);
        if (t->card->host->state == SUNXI_SDIO_FAULT) { fail(t, AIC8800_TRANSPORT_HOST_ERROR); goto out; }
        if (t->card->host->state != SUNXI_SDIO_DONE) goto post;
        if (!sunxi_sdio_take_result(t->card->host, &t->last_result)) { fail(t, AIC8800_TRANSPORT_HOST_ERROR); goto out; }
        t->pending = 0;
        if (!observe(t, &now)) goto out;
        completed(t, now);
        if(t->state!=AIC8800_TRANSPORT_FAULT&&t->card->host->request.command==53&&
            t->last_result.error==SUNXI_SDIO_NO_ERROR&&t->last_result.transferred&&
            t->completed_transfers!=UINT64_MAX)++t->completed_transfers;
        /* Parsing a newly received aggregate starts on the next worker call. */
        if (t->phase == PARSE) goto post;
    }
    if (t->state == AIC8800_TRANSPORT_SETUP || t->state == AIC8800_TRANSPORT_IDENTITY ||
        t->state == AIC8800_TRANSPORT_FIRMWARE || t->state == AIC8800_TRANSPORT_RUNTIME) {
        if (t->phase == PARSE) parse(t, now);
        else if (!admit(t, now)) fail(t, AIC8800_TRANSPORT_HOST_ERROR);
    }
post:
    if (t->state != AIC8800_TRANSPORT_FAULT) (void)observe(t, &now);
out:
    if (t->state != state || t->phase != phase || t->pending != pending || t->held != held ||
        t->cursor != cursor || t->card->host->progress != host_progress) advanced(t);
    t->entered = 0;
}
int aic8800_transport_peek(const struct aic8800_sdio_transport *t, struct aic8800_sdio_response *out)
{
    if (!t || !t->initialized || t->entered || !t->held || !independent(t, out, sizeof(*out))) return 0;
    *out = t->unrelated; return 1;
}
static int consume_receive(struct aic8800_sdio_transport *t,const void *owner)
{
    if (!t || !t->initialized || t->entered || t->receive_owner!=owner || !t->held || t->state == AIC8800_TRANSPORT_FAULT) return 0;
    uint64_t now;
    if(owner){t->entered=1;if(!observe(t,&now)){t->entered=0;return 0;}}
    if (t->data) aic8800_data_consumed(t->data, t, t->unrelated.record);
    if (t->wifi) aic8800_wifi_consumed(t->wifi, t, t->unrelated.record);
    /* A remaining A-MSDU subframe keeps the same record held. */
    if (!t->data || !t->data->held || t->data->held_record != t->unrelated.record) {
        t->cursor += t->unrelated.consumed; t->held = 0;
    }
    advanced(t);
    if(owner){int ok=observe(t,&now);t->entered=0;return ok;}return 1;
}

void aic8800_transport_poll(struct aic8800_sdio_transport *t){poll_receive(t,NULL);}
int aic8800_transport_consume(struct aic8800_sdio_transport *t){return consume_receive(t,NULL);}
void aic8800_transport_poll_receive(struct aic8800_sdio_transport *t,const void *owner)
{if(owner)poll_receive(t,owner);}
int aic8800_transport_consume_receive(struct aic8800_sdio_transport *t,const void *owner)
{return owner&&consume_receive(t,owner);}
int aic8800_transport_claim_receive(struct aic8800_sdio_transport *t,const void *owner,size_t bytes)
{
    if(!t||!t->initialized||t->entered||t->receive_owner||t->state!=AIC8800_TRANSPORT_RUNTIME||
        !bytes||!independent(t,owner,bytes)||t->pending||t->card->host->state!=SUNXI_SDIO_READY)return 0;
    uint64_t now;t->entered=1;int ok=observe(t,&now);
    if(ok){t->receive_owner=owner;t->receive_owner_bytes=bytes;}
    t->entered=0;return ok;
}
int aic8800_transport_receive_view(const struct aic8800_sdio_transport *t,const void *owner,
    const uint8_t **record,size_t *available)
{
    if(!t||!owner||t->receive_owner!=owner||t->entered||t->pending||t->held||
        t->state!=AIC8800_TRANSPORT_RUNTIME||t->phase!=PARSE||t->cursor>=t->rx_bytes||
        !independent(t,record,sizeof(*record))||!independent(t,available,sizeof(*available))||
        overlap(record,sizeof(*record),available,sizeof(*available)))return 0;
    *record=t->rx+t->cursor;*available=t->rx_bytes-t->cursor;return 1;
}
int aic8800_transport_retire_record(struct aic8800_sdio_transport *t,const void *owner,
    const uint8_t *record,size_t consumed)
{
    const uint8_t *current;size_t available;
    if(!aic8800_transport_receive_view(t,owner,&current,&available)||record!=current||available<4)return 0;
    size_t length=(size_t)record[0]|(size_t)record[1]<<8;
    if(!length)return 0;
    size_t expected=(record[2]&0x10)?4+((length+3)&~(size_t)3):((length+60+3)&~(size_t)3);
    if(consumed!=expected||consumed>available)return 0;
    uint64_t now;t->entered=1;int ok=observe(t,&now);
    if(ok){t->cursor+=consumed;advanced(t);ok=observe(t,&now);}
    t->entered=0;return ok;
}
void aic8800_transport_fault_receive(struct aic8800_sdio_transport *t,const void *owner)
{
    if(t&&owner&&t->receive_owner==owner&&!t->entered&&t->state==AIC8800_TRANSPORT_RUNTIME)
        fail(t,AIC8800_TRANSPORT_MESSAGE_ERROR);
}
