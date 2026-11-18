/* The iPhone link over BLE (NimBLE), with nothing to install on the phone.
   Pip advertises asking for Apple's notification and media services; the phone pairs once (numeric comparison: the
   same 6 digits on both screens) and bonds. Then, as a GATT client of the phone:
     ANCS  notifications and incoming calls -> phone_note / phone_call; dismiss, answer, decline go back
     AMS   now playing (title, artist, duration, position, volume) -> phone_track; play/pause, tracks, volume go back
     CTS   the phone's clock and time zone -> the system clock (Pip's day and the clock app)
   Pip is also a tiny Bluetooth keyboard with two media keys (HID): iOS lists keyboards in Settings -> Bluetooth
   (a plain BLE device it would not show), and "volume up" is the Camera app's shutter.
   To a computer Pip is a mouse, page keys and media keys (a second HID service, last in the table so the iPhone's
   cached handles stay put): the remote app and the music app. Its report map has the keyboard only for the computer's
   connection; the phone reads a mouse alone, or iOS would hide its on-screen keyboard. Connections: the phone, the debug
   page and a computer; a second one that asks for encryption is the computer, a first one that turns out to have
   neither notifications nor music is a computer too, and a computer once seen is known by its address.
   Everything the UI sees goes through phone.h, like the simulator's fake phone. */
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "phone.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "shell.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "ble";
void ble_store_config_init(void);

/* ---------- Apple's UUIDs ---------- */
static ble_uuid128_t ANCS, ANCS_NS, ANCS_CP, ANCS_DS, AMS, AMS_RC, AMS_EU;
/* Generic (non-Apple) services: kept for the old discovery code; the Android/Windows path now uses the companion app */
static ble_uuid128_t GATT_NOTIFY, GATT_NOTIFY_CHR, GATT_MEDIA, GATT_MEDIA_CTRL, GATT_MEDIA_TRACK;
/* Generic call service for Android/Windows */
static ble_uuid128_t GATT_CALL, GATT_CALL_STATE, GATT_CALL_CTRL;
/* Pip Link: the companion app's service (Android/Windows): lines in, lines out (docs/companion.md) */
static ble_uuid128_t COMP, COMP_RX, COMP_TX, COMP_INFO;
static const ble_uuid16_t CTS = BLE_UUID16_INIT(0x1805), CTS_TIME = BLE_UUID16_INIT(0x2A2B), CTS_LOCAL = BLE_UUID16_INIT(0x2A0F),
                          CCCD = BLE_UUID16_INIT(0x2902);

static void trim_utf8(char *s) /* a string cut short must not end inside a character */
{
    int n = (int)strlen(s), i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i > 0) {
        unsigned c = (unsigned char)s[i - 1];
        int need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (n - (i - 1) < need) s[i - 1] = 0;
    }
}

static void uuid(ble_uuid128_t *u, const char *s) /* "7905F431-B5CE-..." -> little-endian bytes */
{
    u->u.type = BLE_UUID_TYPE_128;
    int n = 15;
    for (; *s && n >= 0; s++) {
        if (*s == '-') continue;
        unsigned v;
        sscanf(s, "%2x", &v);
        u->value[n--] = (uint8_t)v;
        s++;
    }
}

/* ---------- connection state ---------- */
static struct {
    uint8_t own_addr;
    uint16_t conn;
    bool connected, numcmp, ready; /* ready: discovery done, ANCS requests may go */
    uint16_t svc_start, svc_end; /* the service being looked at */
    uint16_t ns, cp, ds, rc, eu, ct, lt; /* characteristic value handles, 0 = not there */
    uint16_t cs, cc;                      /* generic call state / call control (Android/Windows) */
    uint16_t cccd;
    int step, retries;
    bool is_phone; /* it said so (Appearance): never taken for a computer, however slow its services are */
    uint32_t call_uid;
    int type; /* PHONE_TYPE_*: what this connected phone speaks (set from NVS or appearance) */
} b = {.conn = BLE_HS_CONN_HANDLE_NONE, .type = PHONE_TYPE_IPHONE};

/* ---------- Pip Link: the companion app on Android/Windows ---------- */
#define COMP_LINE 640
static uint16_t comp_conn = BLE_HS_CONN_HANDLE_NONE, comp_tx_handle, comp_info_handle;
static bool comp_ready; /* it said HELLO: the link is up */
static char comp_buf[COMP_LINE];
static int comp_len;
static bool comp_command(int cmd, uint32_t uid); /* defined with the Pip Link handlers below */

/* Set the phone type (iPhone/Android/Windows): called from main.c after loading NVS. Changing it drops a live
   phone link so the next connection is asked the right way. */
void ble_set_phone_type(int type)
{
    if (type < 0 || type >= PHONE_TYPE_COUNT || b.type == type) return;
    b.type = type;
    if (b.connected) ble_gap_terminate(b.conn, BLE_ERR_REM_USER_CONN_TERM);
}

static void advertise(void);
static void step(void);
static void step_generic(void); /* the old generic GATT path: kept compiled but no longer selected */
static void key(uint8_t bits);
static void host_take(uint16_t conn);
static uint16_t pair_conn = BLE_HS_CONN_HANDLE_NONE; /* the connection the numeric comparison is for */

static void ui(void (*fn)(void *), void *arg) /* run UI code under the LVGL lock */
{
    ui_lock();
    fn(arg);
    ui_unlock();
}
static void ui_link_off(void *a) { (void)a; phone_link(PHONE_OFF, 0); }
static void ui_link_on(void *a) { (void)a; phone_link(PHONE_ON, 0); }
static void ui_link_pair(void *a) { phone_link(PHONE_PAIRING, *(uint32_t *)a); }
/* One "phone connected" state for two independent links: the iPhone over ANCS and the companion app. */
static void link_sync(void) { ui(b.ready || comp_ready ? ui_link_on : ui_link_off, NULL); }

/* ---------- ANCS: notifications ---------- */
#define Q_MAX 24
static struct { uint32_t uid; uint8_t category; bool quiet; } q[Q_MAX]; /* waiting for their attributes */
static int q_n;
static bool fetching;
static uint8_t ds_buf[800];
static int ds_len;

static const char *app_name(const char *bundle) /* the apps people have; anything else by its last word */
{
    static const struct { const char *id, *name; } known[] = {
        {"ph.telegra.Telegraph", "Telegram"}, {"com.burbn.instagram", "Instagram"}, {"net.whatsapp.WhatsApp", "WhatsApp"},
        {"com.apple.MobileSMS", "Сообщения"}, {"com.apple.mobilephone", "Телефон"}, {"com.apple.mobilemail", "Почта"},
        {"com.apple.mobilecal", "Календарь"}, {"com.apple.reminders", "Напоминания"}, {"com.google.Gmail", "Gmail"},
        {"com.vk.vkclient", "VK"}, {"com.apple.facetime", "FaceTime"}, {"com.spotify.client", "Spotify"},
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
        if (!strcmp(bundle, known[i].id)) return known[i].name;
    const char *dot = strrchr(bundle, '.');
    return dot ? dot + 1 : bundle;
}

static void ancs_fetch(void)
{
    if (fetching || !q_n || !b.cp || !b.ready) return;
    /* Get Notification Attributes: app id, title (64), message (480), date */
    uint8_t cmd[] = {0, 0, 0, 0, 0, 0, 1, 64, 0, 3, 480 & 255, 480 >> 8, 5};
    memcpy(&cmd[1], &q[0].uid, 4);
    fetching = true, ds_len = 0;
    if (ble_gattc_write_flat(b.conn, b.cp, cmd, sizeof cmd, NULL, NULL)) fetching = false;
}

typedef struct { uint32_t uid; uint8_t category; bool quiet; char app[64], title[66], text[482], date[20]; } note_t;

static void ui_note(void *a)
{
    note_t *n = a;
    if (n->category == 1) { /* an incoming call: the title is who is calling */
        b.call_uid = n->uid;
        phone_call(n->uid, n->title[0] ? n->title : "Звонок", "входящий");
        return;
    }
    int h = 0, m = 0;
    if (strlen(n->date) >= 13) sscanf(n->date + 9, "%2d%2d", &h, &m); /* yyyyMMdd'T'HHmmSS */
    phone_note(n->uid, app_name(n->app), n->title[0] ? n->title : app_name(n->app), n->text, h, m, n->quiet);
}

static void ancs_data(const uint8_t *d, int len) /* Data Source: replies may span notifications */
{
    if (ds_len + len > (int)sizeof ds_buf) ds_len = 0;
    memcpy(ds_buf + ds_len, d, len);
    ds_len += len;
    if (ds_len < 5 || ds_buf[0] != 0) return;
    static note_t n;
    memset(&n, 0, sizeof n);
    memcpy(&n.uid, ds_buf + 1, 4);
    int pos = 5, got = 0;
    while (got < 4 && pos + 3 <= ds_len) {
        int id = ds_buf[pos], alen = ds_buf[pos + 1] | ds_buf[pos + 2] << 8;
        if (pos + 3 + alen > ds_len) return; /* the rest is still coming */
        char *dst = id == 0 ? n.app : id == 1 ? n.title : id == 3 ? n.text : id == 5 ? n.date : NULL;
        size_t cap = id == 0 ? sizeof n.app : id == 1 ? sizeof n.title : id == 3 ? sizeof n.text : sizeof n.date;
        if (dst) {
            int c = alen < (int)cap - 1 ? alen : (int)cap - 1;
            memcpy(dst, ds_buf + pos + 3, c);
            dst[c] = 0;
            trim_utf8(dst);
        }
        pos += 3 + alen, got++;
    }
    if (got < 4) return;
    ds_len = 0, fetching = false;
    if (q_n && q[0].uid == n.uid) {
        n.category = q[0].category, n.quiet = q[0].quiet;
        memmove(&q[0], &q[1], sizeof q[0] * --q_n);
        ui(ui_note, &n);
    }
    ancs_fetch();
}

static void ui_gone(void *a)
{
    uint32_t uid = *(uint32_t *)a;
    if (uid == b.call_uid) phone_call(0, NULL, NULL), b.call_uid = 0;
    phone_note_gone(uid);
}

static void ancs_source(const uint8_t *d, int len) /* Notification Source: event, flags, category, count, uid */
{
    if (len < 8) return;
    uint32_t uid;
    memcpy(&uid, d + 4, 4);
    if (d[0] == 2) { ui(ui_gone, &uid); return; } /* removed */
    if (d[0] != 0 || q_n >= Q_MAX) return;        /* only new ones */
    if (d[2] != 1 && phone_privacy() == PHONE_PRIV_OFF) return; /* notifications off: their contents never leave the phone; calls (category 1) still ring */
    q[q_n++] = (typeof(q[0])){uid, d[2], (d[1] & 0x04) != 0}; /* pre-existing: into the list without a banner */
    ancs_fetch();
}

static void ancs_action(uint32_t uid, uint8_t action) /* 0 positive (answer), 1 negative (clear, decline) */
{
    if (!b.cp) return;
    uint8_t cmd[6] = {2, 0, 0, 0, 0, action};
    memcpy(&cmd[1], &uid, 4);
    ble_gattc_write_flat(b.conn, b.cp, cmd, sizeof cmd, NULL, NULL);
}

/* ---------- AMS: now playing ---------- */
static struct { char title[64], artist[48]; float dur, pos; bool playing; float vol; } np;

static void ui_track(void *a)
{
    (void)a;
    phone_track(np.title[0] ? np.title : NULL, np.artist, (int)np.dur, np.pos, np.playing);
}
static void ui_volume(void *a) { (void)a; phone_volume(np.vol); }

static void ams_update(const uint8_t *d, int len) /* entity, attribute, flags, value */
{
    if (len < 3) return;
    char v[80];
    int n = len - 3 < (int)sizeof v - 1 ? len - 3 : (int)sizeof v - 1;
    memcpy(v, d + 3, n);
    v[n] = 0;
    if (d[0] == 0 && d[1] == 1) { /* player: "state,rate,elapsed" */
        int state = 0;
        float rate = 0, elapsed = 0;
        sscanf(v, "%d,%f,%f", &state, &rate, &elapsed);
        np.playing = state == 1, np.pos = elapsed;
    } else if (d[0] == 0 && d[1] == 2) {
        np.vol = strtof(v, NULL);
        ui(ui_volume, NULL);
        return;
    } else if (d[0] == 2 && d[1] == 0) snprintf(np.artist, sizeof np.artist, "%.47s", v), trim_utf8(np.artist);
    else if (d[0] == 2 && d[1] == 2) snprintf(np.title, sizeof np.title, "%.63s", v), trim_utf8(np.title);
    else if (d[0] == 2 && d[1] == 3) np.dur = strtof(v, NULL);
    ui(ui_track, NULL);
}

/* ---------- generic notifications for Android/Windows ---------- */
/* Data comes as a pipe-separated UTF-8 line: "app|title|text|hour|min|quiet"
   quiet = 0: show banner, 1: silent (added to list only). Calls are not supported here. */
static char gnote_buf[512];
static int gnote_len;

static void generic_notify_flush(void)
{
    if (gnote_len < 3) { gnote_len = 0; return; }
    gnote_buf[gnote_len] = 0;
    char app[24] = "", title[48] = "", text[480] = "";
    int hour = 0, min = 0, quiet = 0;
    int n = sscanf(gnote_buf, "%23[^|]|%47[^|]|%479[^|]|%d|%d|%d", app, title, text, &hour, &min, &quiet);
    if (n >= 3) {
        uint32_t uid = esp_random();
        phone_note(uid, app[0] ? app : "Уведомления", title[0] ? title : "", text, hour, min, quiet != 0);
    }
    gnote_len = 0;
}

static void generic_notify(const uint8_t *d, int len)
{
    if (gnote_len + len >= (int)sizeof gnote_buf) gnote_len = 0; /* overflow: drop */
    memcpy(gnote_buf + gnote_len, d, len);
    gnote_len += len;
    /* a complete message ends with newline */
    if (len > 0 && d[len - 1] == '\n') generic_notify_flush();
}

/* ---------- generic call state for Android/Windows ---------- */
/* "INCOMING|Имя|мобильный\n"  -> incoming call
   "ENDED\n"                   -> call ended / declined / missed */
static char call_buf[128];
static int call_len;

static void generic_call_state_flush(void)
{
    if (call_len < 3) { call_len = 0; return; }
    call_buf[call_len] = 0;
    char action[16] = "", name[48] = "", kind[24] = "";
    int n = sscanf(call_buf, "%15[^|]|%47[^|]|%23[^\n]", action, name, kind);
    if (n >= 1) {
        if (strcmp(action, "INCOMING") == 0) {
            uint32_t uid = esp_random();
            phone_call(uid, name[0] ? name : NULL, kind[0] ? kind : NULL);
        } else if (strcmp(action, "ENDED") == 0) {
            phone_call(0, NULL, NULL);
        }
    }
    call_len = 0;
}

static void generic_call_state(const uint8_t *d, int len)
{
    if (call_len + len >= (int)sizeof call_buf) call_len = 0;
    memcpy(call_buf + call_len, d, len);
    call_len += len;
    if (len > 0 && d[len - 1] == '\n') generic_call_state_flush();
}

/* ---------- generic media track for Android/Windows ---------- */
/* "Название|Исполнитель|258|102.5|1\n" -> title|artist|duration|position|playing(0/1) */
static char track_buf[256];
static int track_len;

static void generic_media_track_flush(void)
{
    if (track_len < 3) { track_len = 0; return; }
    track_buf[track_len] = 0;
    char title[64] = "", artist[48] = "";
    int dur = 0;
    float pos = 0;
    int playing = 0;
    int n = sscanf(track_buf, "%63[^|]|%47[^|]|%d|%f|%d", title, artist, &dur, &pos, &playing);
    if (n >= 2) {
        phone_track(title[0] ? title : NULL, artist[0] ? artist : NULL, dur, pos, playing != 0);
    }
    track_len = 0;
}

static void generic_media_track(const uint8_t *d, int len)
{
    if (track_len + len >= (int)sizeof track_buf) track_len = 0;
    memcpy(track_buf + track_len, d, len);
    track_len += len;
    if (len > 0 && d[len - 1] == '\n') generic_media_track_flush();
}


/* ---------- commands from the UI (called in the LVGL task) ---------- */
static void command(int cmd, uint32_t uid)
{
    static const uint8_t remote[] = {[PHONE_PLAY_PAUSE] = 2, [PHONE_NEXT] = 3, [PHONE_PREV] = 4, [PHONE_VOL_UP] = 5, [PHONE_VOL_DOWN] = 6};
    /* Android/Windows: the companion app executes the command. Pairing stays NimBLE's own (numeric comparison). */
    if (comp_ready && cmd != PHONE_PAIR_OK && cmd != PHONE_PAIR_CANCEL && comp_command(cmd, uid)) return;
    switch (cmd) {
    case PHONE_NOTE_DISMISS:
        if (b.type == PHONE_TYPE_IPHONE) ancs_action(uid, 1);
        else phone_note_gone(uid); /* generic: just drop it locally */
        break;
    case PHONE_CALL_ANSWER:
        if (b.type == PHONE_TYPE_IPHONE) ancs_action(uid, 0);
        else if (b.cc) { uint8_t v = 1; ble_gattc_write_flat(b.conn, b.cc, &v, 1, NULL, NULL); }
        break;
    case PHONE_CALL_DECLINE:
        if (b.type == PHONE_TYPE_IPHONE) ancs_action(uid, 1);
        else if (b.cc) { uint8_t v = 0; ble_gattc_write_flat(b.conn, b.cc, &v, 1, NULL, NULL); }
        break;
    case PHONE_PLAY_PAUSE:
    case PHONE_NEXT:
    case PHONE_PREV:
    case PHONE_VOL_UP:
    case PHONE_VOL_DOWN:
        if (b.rc) ble_gattc_write_flat(b.conn, b.rc, &remote[cmd], 1, NULL, NULL);
        break;
    case PHONE_PAIR_OK:
    case PHONE_PAIR_CANCEL:
        if (b.numcmp) {
            struct ble_sm_io io = {.action = BLE_SM_IOACT_NUMCMP, .numcmp_accept = cmd == PHONE_PAIR_OK};
            ble_sm_inject_io(pair_conn, &io);
            b.numcmp = false;
        }
        break;
    case PHONE_SHUTTER: key(0x01); break; /* volume up */
    }
}

/* ---------- CTS: the clock ---------- */
static int cts_tz = -10000;
static int on_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    uint8_t v[10] = {0};
    uint16_t n = 0;
    if (!err->status && attr) ble_hs_mbuf_to_flat(attr->om, v, sizeof v, &n);
    if (!err->status && arg == &b.lt && n >= 2) cts_tz = ((int8_t)v[0] + v[1]) * 15; /* zone and DST, in 15 min units */
    if (!err->status && arg == &b.ct && n >= 7) {
        struct tm t = {.tm_year = (v[0] | v[1] << 8) - 1900, .tm_mon = v[2] - 1, .tm_mday = v[3], .tm_hour = v[4], .tm_min = v[5], .tm_sec = v[6]};
        int zone = cts_tz > -10000 ? cts_tz : tz_min;
        struct timeval tv = {.tv_sec = mktime(&t) - zone * 60}; /* the phone tells local time; the system keeps UTC */
        settimeofday(&tv, NULL);
        tz_min = zone;
        ESP_LOGI(TAG, "time from the iPhone: %04d-%02d-%02d %02d:%02d, UTC%+d min", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, zone);
    }
    if (err->status != BLE_HS_EDONE) (b.type == PHONE_TYPE_IPHONE ? step : step_generic)();
    return 0;
}

/* ---------- who we have met: computers and phones, by identity address, kept in NVS ---------- */
#define HOSTS_MAX 4
static ble_addr_t hosts[HOSTS_MAX]; /* computers seen before (identity addresses), newest first, kept in NVS */
static int hosts_n;
static ble_addr_t phones[HOSTS_MAX]; /* and phones: the ones with notifications or music */
static int phones_n;

static bool addr_in(uint16_t conn, const ble_addr_t *list, int n)
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d)) return false;
    for (int i = 0; i < n; i++)
        if (!ble_addr_cmp(&list[i], &d.peer_id_addr)) return true;
    return false;
}

static bool addr_add(uint16_t conn, ble_addr_t *list, int *n, const char *key) /* true if it is new */
{
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(conn, &d) || addr_in(conn, list, *n)) return false;
    memmove(&list[1], &list[0], sizeof list[0] * (HOSTS_MAX - 1));
    list[0] = d.peer_id_addr;
    if (*n < HOSTS_MAX) (*n)++;
    nvs_handle_t h;
    if (nvs_open("keypet", NVS_READWRITE, &h) != ESP_OK) return true;
    if (nvs_set_blob(h, key, list, sizeof list[0] * (size_t)*n) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return true;
}

static bool phone_known(uint16_t conn) { return addr_in(conn, phones, phones_n); }

/* ---------- discovery, one GATT procedure after another ---------- */
static int on_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    step();
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (!err->status && svc) b.svc_start = svc->start_handle, b.svc_end = svc->end_handle;
    if (err->status) step(); /* done (or not found) */
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *c, void *arg)
{
    if (!err->status && c) {
        const ble_uuid_t *u = &c->uuid.u;
        if (!ble_uuid_cmp(u, &ANCS_NS.u)) b.ns = c->val_handle;
        else if (!ble_uuid_cmp(u, &ANCS_CP.u)) b.cp = c->val_handle;
        else if (!ble_uuid_cmp(u, &ANCS_DS.u)) b.ds = c->val_handle;
        else if (!ble_uuid_cmp(u, &AMS_RC.u)) b.rc = c->val_handle;
        else if (!ble_uuid_cmp(u, &AMS_EU.u)) b.eu = c->val_handle;
        else if (!ble_uuid_cmp(u, &CTS_TIME.u)) b.ct = c->val_handle;
        else if (!ble_uuid_cmp(u, &CTS_LOCAL.u)) b.lt = c->val_handle;
        /* generic (non-Apple) services: Android/Windows */
        else if (!ble_uuid_cmp(u, &GATT_NOTIFY_CHR.u)) b.ns = c->val_handle;
        else if (!ble_uuid_cmp(u, &GATT_MEDIA_CTRL.u)) b.rc = c->val_handle;
        else if (!ble_uuid_cmp(u, &GATT_MEDIA_TRACK.u)) b.eu = c->val_handle;
        else if (!ble_uuid_cmp(u, &GATT_CALL_STATE.u)) b.cs = c->val_handle;
        else if (!ble_uuid_cmp(u, &GATT_CALL_CTRL.u)) b.cc = c->val_handle;
    }
    if (err->status) step();
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr, const struct ble_gatt_dsc *d, void *arg)
{
    if (!err->status && d && !b.cccd && !ble_uuid_cmp(&d->uuid.u, &CCCD.u)) b.cccd = d->handle;
    if (err->status) step();
    return 0;
}

static bool find_svc(const ble_uuid_t *u)
{
    b.svc_start = b.svc_end = 0;
    return !ble_gattc_disc_svc_by_uuid(b.conn, u, on_svc, NULL);
}
static bool find_chrs(void) { return b.svc_start && !ble_gattc_disc_all_chrs(b.conn, b.svc_start, b.svc_end, on_chr, NULL); }
static bool find_cccd(uint16_t val, uint16_t end)
{
    b.cccd = 0;
    return val && !ble_gattc_disc_all_dscs(b.conn, val, end, on_dsc, NULL);
}
static bool subscribe(void)
{
    static const uint8_t on[2] = {1, 0};
    return b.cccd && !ble_gattc_write_flat(b.conn, b.cccd, on, 2, on_written, NULL);
}

static uint16_t ancs_end, ams_end;
static struct ble_npl_callout again; /* runs in the host task, like the GATT callbacks */
static void step(void);
static void step_generic(void);   /* Android/Windows: generic GATT notification + media services */
static void look_again(struct ble_npl_event *ev) { (void)ev; if (b.connected) b.step = 0, (b.type == PHONE_TYPE_IPHONE ? step : step_generic)(); }

static void step(void)
{
    for (;;) {
        bool started = false;
        switch (b.step++) {
        case 0: started = find_svc(&ANCS.u); break;
        case 1: ancs_end = b.svc_end, started = find_chrs(); break;
        case 2: started = find_svc(&AMS.u); break;
        case 3: ams_end = b.svc_end, started = find_chrs(); break;
        case 4: started = find_svc(&CTS.u); break;
        case 5: started = find_chrs(); break;
        /* Data Source before Notification Source, as Apple asks: no reply may be missed */
        case 6: started = find_cccd(b.ds, ancs_end); break;
        case 7: started = subscribe(); break;
        case 8: started = find_cccd(b.ns, ancs_end); break;
        case 9: started = subscribe(); break;
        case 10: started = find_cccd(b.eu, ams_end); break;
        case 11: started = subscribe(); break;
        case 12: { /* the player's state and volume */
            static const uint8_t player[] = {0, 1, 2};
            started = b.eu && !ble_gattc_write_flat(b.conn, b.eu, player, sizeof player, on_written, NULL);
            break;
        }
        case 13: { /* the track's artist, title and duration */
            static const uint8_t track[] = {2, 0, 2, 3};
            started = b.eu && !ble_gattc_write_flat(b.conn, b.eu, track, sizeof track, on_written, NULL);
            break;
        }
        case 14: started = b.lt && !ble_gattc_read(b.conn, b.lt, on_read, &b.lt); break;
        case 15: started = b.ct && !ble_gattc_read(b.conn, b.ct, on_read, &b.ct); break;
        case 16:
            if (!b.ns && !b.eu && !b.ct && b.retries++ < 3) { /* nothing at all: the phone was not ready to be asked yet */
                ESP_LOGW(TAG, "no services found, looking again in 2 s");
                ble_npl_callout_reset(&again, ble_npl_time_ms_to_ticks32(2000));
                return;
            }
            if (!b.ns && !b.eu && !b.is_phone) { /* no notifications and no music: a computer, for the remote; the phone's place is free */
                uint16_t c = b.conn;
                uint8_t own = b.own_addr;
                int type = b.type;
                b = (typeof(b)){.own_addr = own, .conn = BLE_HS_CONN_HANDLE_NONE, .type = type};
                link_sync();
                host_take(c);
                return;
            }
            ESP_LOGI(TAG, "iPhone ready: ANCS %s, AMS %s, CTS %s", b.ns && b.ds && b.cp ? "yes" : "no", b.eu && b.rc ? "yes" : "no", b.ct ? "yes" : "no");
            if (addr_add(b.conn, phones, &phones_n, "phones")) /* a new phone: it may have read the computer's keys */
                ble_svc_gatt_changed(0x0001, 0xFFFF);
            b.ready = true;
            link_sync();
            ancs_fetch(); /* what queued up while we were looking around */
            return;
        default: return;
        }
        if (started) return; /* its callback comes back here */
    }
}

/* Android/Windows: discover generic notification, call and media services, then CTS time */
static void step_generic(void)
{
    for (;;) {
        bool started = false;
        switch (b.step++) {
        case 0: started = find_svc(&GATT_NOTIFY.u); break;
        case 1: started = find_chrs(); break;
        case 2: started = find_svc(&GATT_CALL.u); break;
        case 3: started = find_chrs(); break;
        case 4: started = find_svc(&GATT_MEDIA.u); break;
        case 5: started = find_chrs(); break;
        case 6: started = find_svc(&CTS.u); break;
        case 7: started = find_chrs(); break;
        case 8: started = find_cccd(b.ns, 0xFFFF); break; /* subscribe to notifications */
        case 9: started = subscribe(); break;
        case 10: started = find_cccd(b.cs, 0xFFFF); break; /* subscribe to call state */
        case 11: started = subscribe(); break;
        case 12:
            if (!b.ns && !b.eu && !b.cs && !b.ct && b.retries++ < 3) {
                ESP_LOGW(TAG, "no generic services found, looking again in 2 s");
                ble_npl_callout_reset(&again, ble_npl_time_ms_to_ticks32(2000));
                return;
            }
            ESP_LOGI(TAG, "generic phone ready: notify %s, call %s, media %s, CTS %s",
                     b.ns ? "yes" : "no", b.cs ? "yes" : "no", b.eu ? "yes" : "no", b.ct ? "yes" : "no");
            if (addr_add(b.conn, phones, &phones_n, "phones"))
                ble_svc_gatt_changed(0x0001, 0xFFFF);
            b.ready = true;
            link_sync();
            return;
        default: return;
        }
        if (started) return;
    }
}

/* ---------- our own services: a two-key HID keyboard, the battery, who we are ---------- */
static const uint8_t report_map[] = {
    0x05, 0x0C,       /* usage page: consumer */
    0x09, 0x01,       /* usage: consumer control */
    0xA1, 0x01,       /* collection: application */
    0x85, 0x01,       /*   report id 1 */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, /* two 1-bit buttons */
    0x09, 0xE9,       /*   volume up: the Camera app's shutter */
    0x09, 0xEA,       /*   volume down */
    0x81, 0x02,       /*   input */
    0x95, 0x06, 0x81, 0x03, /* padding to a byte */
    0xC0,
};
/* The computer's, in its own service: a mouse with three buttons, x, y and a wheel (relative, report id 1), a keyboard
   (id 2: modifiers, reserved, six keys) and media keys (id 3: one consumer usage). The phone gets the mouse alone. */
#define MOUSE_MAP                                                                                                        \
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, /* generic desktop: mouse, application */                                      \
        0x85, 0x01,                     /*   report id 1 */                                                            \
        0x09, 0x01, 0xA1, 0x00,         /*   pointer, physical */                                                      \
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, /* buttons 1..3 */                                                         \
        0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03, /* + padding */ \
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, /* x, y, wheel: -127..127, relative */                         \
        0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xC0, 0xC0
static const uint8_t mouse_map[] = {MOUSE_MAP};
static const uint8_t host_map[] = {
    MOUSE_MAP,
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, /* generic desktop: keyboard, application */
    0x85, 0x02,                         /*   report id 2 */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, /* modifiers */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, /*   reserved byte */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, /* six keys */
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, /* consumer control, application */
    0x85, 0x03,                         /*   report id 3 */
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, /* one usage */
    0xC0,
};
#define HOST_REPORT(id) ((void *)(intptr_t)(id)) /* the access callback's arg in the computer's service: its report id */
static uint16_t report_handle, battery_handle, mouse_handle, keys_handle, media_handle;
static uint16_t host_conn = BLE_HS_CONN_HANDLE_NONE; /* the computer */

static int read_flat(struct ble_gatt_access_ctxt *ctxt, const void *v, int n)
{
    return os_mbuf_append(ctxt->om, v, n) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static int hid_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t id = ble_uuid_u16(ctxt->chr ? ctxt->chr->uuid : ctxt->dsc->uuid);
    static const uint8_t info[4] = {0x11, 0x01, 0x00, 0x02}, ref[2] = {1, 1}, zero = 0, protocol = 1;
    static const uint8_t pnp[7] = {0x02, 0x5E, 0x04, 0x01, 0x00, 0x01, 0x00}; /* USB vendor id space, product 1, v1 */
    static const uint8_t still[8] = {0}, host_ref[3][2] = {{1, 1}, {2, 1}, {3, 1}};
    static const int still_n[3] = {4, 8, 2}; /* mouse, keyboard, media */
    int rid = (int)(intptr_t)arg; /* 0: the phone's service */
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR || ctxt->op == BLE_GATT_ACCESS_OP_WRITE_DSC) return 0; /* control point */
    switch (id) {
    case 0x2A4A: return read_flat(ctxt, info, 4);                            /* HID information */
    case 0x2A4B:                                                             /* report map */
        if (!rid) return read_flat(ctxt, report_map, sizeof report_map);
        /* the keys go to everyone but a phone we know (a keyboard on an iPhone hides its on-screen one). macOS reads this
           once, at pairing, and keeps it even when told the services changed: a computer must not get the short one */
        return phone_known(conn) ? read_flat(ctxt, mouse_map, sizeof mouse_map) : read_flat(ctxt, host_map, sizeof host_map);
    case 0x2A4D: return rid ? read_flat(ctxt, still, still_n[rid - 1]) : read_flat(ctxt, &zero, 1); /* the report: nothing pressed */
    case 0x2908: return read_flat(ctxt, rid ? host_ref[rid - 1] : ref, 2);   /* report reference: id, input */
    case 0x2A4E: return read_flat(ctxt, &protocol, 1);                        /* protocol mode: report */
    case 0x2A50: return read_flat(ctxt, pnp, 7);                              /* PnP id */
    case 0x2A29: return read_flat(ctxt, "KeyPet", 6);                         /* manufacturer */
    case 0x2A19: {                                                            /* battery level */
        int pct = shell_battery_level(NULL);
        uint8_t v = pct < 0 ? 100 : (uint8_t)pct;
        return read_flat(ctxt, &v, 1);
    }
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* ---------- Pip Link: the companion app (docs/companion.md) ---------- */
static void companion_claim(uint16_t conn); /* cancel the "is it the phone?" grace when the app writes to us */

static void comp_send(const char *fmt, ...)
{
    if (!comp_ready || comp_conn == BLE_HS_CONN_HANDLE_NONE || !comp_tx_handle) return;
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof b) n = (int)sizeof b - 1;
    ble_gatts_notify_custom(comp_conn, comp_tx_handle, ble_hs_mbuf_from_flat(b, (uint16_t)n));
}

static int comp_split(char *s, char **f, int max)
{
    int n = 0;
    f[n++] = s;
    for (; *s && n < max; s++)
        if (*s == '|') *s = 0, f[n++] = s + 1;
    return n;
}

static void comp_time(const char *s)
{
    char *e;
    long long t = strtoll(s, &e, 10);
    int tz = (int)strtol(e, NULL, 10);
    if (t > 1735689600) { /* after 2025-01-01: a real clock, not 1970 */
        struct timeval tv = {.tv_sec = (time_t)t};
        settimeofday(&tv, NULL);
        tz_min = tz;
        ESP_LOGI(TAG, "time from the companion: UTC%+d min", tz);
    }
}

static void comp_handle_line(char *line)
{
    if (!strncmp(line, "HELLO ", 6)) {
        comp_ready = true;
        link_sync();
        comp_send("OK HELLO 1\n");
        comp_send("PRIVACY %d\n", phone_privacy());
        bool ch = false;
        int bat = shell_battery_level(&ch);
        comp_send("STATE %d %d\n", bat, ch ? 1 : 0);
        ESP_LOGI(TAG, "companion connected: %s", line + 6);
        return;
    }
    if (!comp_ready) return;
    if (!strncmp(line, "TIME ", 5)) { comp_time(line + 5); return; }
    if (!strncmp(line, "PING", 4)) { comp_send("PONG\n"); return; }
    if (!strncmp(line, "VOL ", 4)) {
        ui_lock();
        phone_volume((float)atoi(line + 4) / 100.0f);
        ui_unlock();
        return;
    }
    char *f[8];
    if (!strncmp(line, "NOTE ", 5)) {
        int n = comp_split(line + 5, f, 7);
        if (n < 4) return;
        uint32_t uid = (uint32_t)strtoul(f[0], NULL, 10);
        ui_lock();
        phone_note(uid, f[1], f[2], f[3], n > 4 ? atoi(f[4]) : 0, n > 5 ? atoi(f[5]) : 0, n > 6 && atoi(f[6]));
        ui_unlock();
    } else if (!strncmp(line, "NOTE_GONE ", 10)) {
        ui_lock();
        phone_note_gone((uint32_t)strtoul(line + 10, NULL, 10));
        ui_unlock();
    } else if (!strncmp(line, "CALL_END", 8)) {
        ui_lock();
        phone_call(0, NULL, NULL);
        ui_unlock();
    } else if (!strncmp(line, "CALL ", 5)) {
        int n = comp_split(line + 5, f, 3);
        if (n < 1) return;
        uint32_t uid = (uint32_t)strtoul(f[0], NULL, 10);
        ui_lock();
        phone_call(uid, n > 1 && f[1][0] ? f[1] : "Телефон", n > 2 && f[2][0] ? f[2] : "мобильный");
        ui_unlock();
    } else if (!strncmp(line, "TRACK ", 6)) {
        int n = comp_split(line + 6, f, 5);
        if (n < 2) return;
        ui_lock();
        phone_track(f[0][0] ? f[0] : NULL, f[1], n > 2 ? atoi(f[2]) : 0, n > 3 ? (float)atof(f[3]) : 0, n > 4 && atoi(f[4]));
        ui_unlock();
    }
}

static int comp_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        static const char info[] = "Pip companion 1\n";
        return read_flat(ctxt, info, sizeof info - 1);
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (comp_conn != BLE_HS_CONN_HANDLE_NONE && comp_conn != conn) return BLE_ATT_ERR_INSUFFICIENT_RES;
    if (comp_conn != conn) comp_conn = conn, companion_claim(conn);
    uint8_t d[200];
    uint16_t n = 0;
    ble_hs_mbuf_to_flat(ctxt->om, d, sizeof d, &n);
    for (int i = 0; i < n; i++) {
        char c = (char)d[i];
        if (c == '\n') {
            comp_buf[comp_len] = 0;
            comp_handle_line(comp_buf);
            comp_len = 0;
        } else if (c != '\r' && comp_len < COMP_LINE - 1) {
            comp_buf[comp_len++] = c;
        }
    }
    return 0;
}

static bool comp_command(int cmd, uint32_t uid)
{
    switch (cmd) {
    case PHONE_NOTE_DISMISS: comp_send("CMD DISMISS %u\n", (unsigned)uid); return true;
    case PHONE_CALL_ANSWER: comp_send("CMD ANSWER %u\n", (unsigned)uid); return true;
    case PHONE_CALL_DECLINE: comp_send("CMD DECLINE %u\n", (unsigned)uid); return true;
    case PHONE_PLAY_PAUSE: comp_send("CMD PLAY_PAUSE\n"); return true;
    case PHONE_NEXT: comp_send("CMD NEXT\n"); return true;
    case PHONE_PREV: comp_send("CMD PREV\n"); return true;
    case PHONE_VOL_UP: comp_send("CMD VOL_UP\n"); return true;
    case PHONE_VOL_DOWN: comp_send("CMD VOL_DOWN\n"); return true;
    }
    return false;
}

void ble_companion_privacy(int mode) { if (comp_ready) comp_send("PRIVACY %d\n", mode); }

static int dbg_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);
static ble_uuid128_t DBG, DBG_RX, DBG_TX;
static uint16_t dbg_tx;

#define HOST_INPUT(id, handle) /* an input report of the computer's service, with its reference (id, input) */             \
    {.uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access, .arg = HOST_REPORT(id), .val_handle = &handle,            \
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,                                        \
     .descriptors = (struct ble_gatt_dsc_def[]){{.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, \
                                                 .access_cb = hid_access, .arg = HOST_REPORT(id)}, {0}}}
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x1812),
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
         {.uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
         {.uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access, .val_handle = &report_handle,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
          .descriptors = (struct ble_gatt_dsc_def[]){{.uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                                                      .access_cb = hid_access}, {0}}},
         {.uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
         {.uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP},
         {0}}},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180F),
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = hid_access, .val_handle = &battery_handle,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY}, {0}}},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &DBG.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &DBG_RX.u, .access_cb = dbg_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
         {.uuid = &DBG_TX.u, .access_cb = dbg_access, .val_handle = &dbg_tx, .flags = BLE_GATT_CHR_F_NOTIFY}, {0}}},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x180A),
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ},
         {.uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = hid_access, .flags = BLE_GATT_CHR_F_READ}, {0}}},
    /* the computer's, then the companion app's: both added after the rest so the iPhone's cached handles do not move */
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x1812),
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = hid_access, .arg = HOST_REPORT(1), .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
         {.uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = hid_access, .arg = HOST_REPORT(1), .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
         HOST_INPUT(1, mouse_handle),
         HOST_INPUT(2, keys_handle),
         HOST_INPUT(3, media_handle),
         {.uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = hid_access, .arg = HOST_REPORT(1), .flags = BLE_GATT_CHR_F_WRITE_NO_RSP},
         {.uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = hid_access, .arg = HOST_REPORT(1), .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP},
         {0}}},
    /* Pip Link: the Android/Windows companion app (docs/companion.md) */
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &COMP.u,
     .characteristics = (struct ble_gatt_chr_def[]){
         {.uuid = &COMP_RX.u, .access_cb = comp_access,
          .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
         {.uuid = &COMP_TX.u, .access_cb = comp_access, .val_handle = &comp_tx_handle, .flags = BLE_GATT_CHR_F_NOTIFY},
         {.uuid = &COMP_INFO.u, .access_cb = comp_access, .val_handle = &comp_info_handle,
          .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
         {0}}},
    {0},
};

static void key(uint8_t bits) /* press, then release */
{
    if (!b.connected || !report_handle) return;
    uint8_t up = 0;
    ble_gatts_notify_custom(b.conn, report_handle, ble_hs_mbuf_from_flat(&bits, 1));
    ble_gatts_notify_custom(b.conn, report_handle, ble_hs_mbuf_from_flat(&up, 1));
}

/* ---------- the computer: the remote's pages, pointer and scroll, the music app's media keys ---------- */
static void ui_host_on(void *a) { (void)a; phone_host(HOST_ON, 0); }
static void ui_host_off(void *a) { (void)a; phone_host(HOST_OFF, 0); }
static void ui_host_pair(void *a) { phone_host(HOST_PAIRING, *(uint32_t *)a); }


static void hosts_load(void)
{
    nvs_handle_t h;
    size_t n = sizeof hosts;
    if (nvs_open("keypet", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "hosts", hosts, &n) == ESP_OK) hosts_n = (int)(n / sizeof hosts[0]);
    n = sizeof phones;
    if (nvs_get_blob(h, "phones", phones, &n) == ESP_OK) phones_n = (int)(n / sizeof phones[0]);
    nvs_close(h);
}

static void host_remember(uint16_t conn) { addr_add(conn, hosts, &hosts_n, "hosts"); }

static void host_take(uint16_t conn)
{
    host_conn = conn;
    ESP_LOGI(TAG, "a computer connected: Pip is its remote");
    host_remember(conn); /* next time it is known at once, before it reads the report map */
    /* 7.5..15 ms between packets: a pointer that keeps up with the hand */
    struct ble_gap_upd_params p = {.itvl_min = 6, .itvl_max = 12, .latency = 0, .supervision_timeout = 400};
    ble_gap_update_params(conn, &p);
    ui(ui_host_on, NULL);
    advertise(); /* the debug page may still come */
}

static void host_report(uint16_t handle, const uint8_t *r, int n)
{
    ble_gatts_notify_custom(host_conn, handle, ble_hs_mbuf_from_flat(r, (uint16_t)n));
}

static void mouse(int buttons, int dx, int dy, int wheel)
{
    uint8_t r[4] = {(uint8_t)buttons, (uint8_t)(int8_t)dx, (uint8_t)(int8_t)dy, (uint8_t)(int8_t)wheel};
    host_report(mouse_handle, r, 4);
}

static void press_key(uint8_t usage) /* a keyboard key, down and up */
{
    uint8_t r[8] = {0, 0, usage};
    host_report(keys_handle, r, 8);
    r[2] = 0;
    host_report(keys_handle, r, 8);
}

static void press_media(uint16_t usage) /* a media key, down and up */
{
    uint8_t r[2] = {usage & 255, usage >> 8};
    host_report(media_handle, r, 2);
    r[0] = r[1] = 0;
    host_report(media_handle, r, 2);
}

static int clamp127(int v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

/* HOST_SCROLL > 0 is the page going down. A notch "up" (+1) does that under macOS's natural scrolling, its default;
   with natural scrolling off (and on Windows) make this -1. */
#define WHEEL_SIGN 1

static int held; /* the mouse buttons held for a drag */

static void host_cmd(int what, int dx, int dy) /* from the UI: the remote and the music apps */
{
    if (host_conn == BLE_HS_CONN_HANDLE_NONE || !mouse_handle) return;
    switch (what) {
    case HOST_NEXT: press_key(0x4E); break; /* Page Down: the next slide in Keynote, PowerPoint, Google Slides, a PDF, a page */
    case HOST_PREV: press_key(0x4B); break; /* Page Up */
    case HOST_CLICK: mouse(1, 0, 0, 0), mouse(0, 0, 0, 0); break;
    case HOST_DOWN: held = 1, mouse(held, 0, 0, 0); break; /* a drag: every move below carries the button */
    case HOST_UP: held = 0, mouse(held, 0, 0, 0); break;
    case HOST_SCROLL:
        for (int w = dy * WHEEL_SIGN; w; w -= clamp127(w)) mouse(held, 0, 0, clamp127(w));
        break;
    case HOST_MOVE:
        while (dx || dy) { /* a report moves at most 127 px each way */
            int sx = clamp127(dx), sy = clamp127(dy);
            mouse(held, sx, sy, 0);
            dx -= sx, dy -= sy;
        }
        break;
    case HOST_PLAY_PAUSE: press_media(0xCD); break;
    case HOST_TRACK_NEXT: press_media(0xB5); break;
    case HOST_TRACK_PREV: press_media(0xB6); break;
    case HOST_VOL_UP: press_media(0xE9); break;
    case HOST_VOL_DOWN: press_media(0xEA); break;
    }
}

/* ---------- the debug link: tools/pipdebug.html in Chrome (Web Bluetooth), next to the iPhone ----------
   A second connection. Who is who: one that writes to the debug service within 4 s of connecting is the debug
   page; otherwise it is the phone (or the phone is already here, so it is the page). */
static uint16_t dbg_conn = BLE_HS_CONN_HANDLE_NONE, pending = BLE_HS_CONN_HANDLE_NONE;
static uint16_t pending2 = BLE_HS_CONN_HANDLE_NONE; /* came while the phone was here: the debug page or a computer */
static int64_t since[8]; /* when each connection came (handles are small numbers), for the Bluetooth page */
static struct ble_npl_callout claim;
static void advertise(void);

/* The app writes to Pip Link at once: it is neither the phone nor a computer, so do not let the 4 s "phone?" timer
   claim it (the same idea as the debug page's first write). */
static void companion_claim(uint16_t conn)
{
    if (pending == conn) ble_npl_callout_stop(&claim), pending = BLE_HS_CONN_HANDLE_NONE;
    if (pending2 == conn) pending2 = BLE_HS_CONN_HANDLE_NONE;
}

static void phone_take(uint16_t conn)
{
    ble_npl_callout_stop(&claim);
    pending = BLE_HS_CONN_HANDLE_NONE;
    if (b.connected && b.conn != conn) { /* the phone's place is taken: ask this one to encrypt, then it says who it is */
        pending2 = conn;
        ble_gap_security_initiate(conn);
        return;
    }
    uint8_t own = b.own_addr;
    int type = b.type;
    b = (typeof(b)){.own_addr = own, .conn = conn, .connected = true, .type = type};
    q_n = 0, fetching = false;
    ESP_LOGI(TAG, "connected");
    ble_gattc_exchange_mtu(b.conn, NULL, NULL);
    ble_gap_security_initiate(b.conn); /* bonded: just encrypts; new: the pairing dialog on the phone */
    advertise();                       /* the debug page may still come */
}
static void claim_phone(struct ble_npl_event *ev) { (void)ev; if (pending != BLE_HS_CONN_HANDLE_NONE) phone_take(pending); }

/* Who is it: every Apple device states its kind in GAP's Appearance (category = value >> 6: 1 phone, 2 computer).
   Guessing from the order they connect in mixed them up when the iPhone and the Mac came back within a second, and
   a phone taken for a computer is never asked for its notifications. Without an answer: the phone's place if it is
   free (the services found then decide), a computer if not. */
static void decide(uint16_t c, int cat)
{
    bool slot = c == b.conn || !b.connected;
    if (cat > 1 || (cat != 1 && !slot)) { /* anything that is not a phone: a Mac calls itself a "media player" (0x0280) */
        if (c == b.conn) {
            uint8_t own = b.own_addr;
            int type = b.type;
            b = (typeof(b)){.own_addr = own, .conn = BLE_HS_CONN_HANDLE_NONE, .type = type};
            link_sync();
        }
        host_take(c);
        return;
    }
    if (!slot) { ESP_LOGW(TAG, "a second phone: left alone"); return; }
    if (c != b.conn) {
        uint8_t own = b.own_addr;
        int type = b.type;
        b = (typeof(b)){.own_addr = own, .conn = c, .connected = true, .type = type};
        q_n = 0, fetching = false;
        ble_gattc_exchange_mtu(c, NULL, NULL);
        advertise();
    }
    if (b.type != PHONE_TYPE_IPHONE) {
        /* Android/Windows: no ANCS to discover. Wait for the companion app's HELLO over Pip Link; without the app
           this connection stays a plain Bluetooth host (remote/media keys only). */
        b.is_phone = true;
        b.ready = false;
        link_sync();
        return;
    }
    b.is_phone = cat == 1;
    b.step = 0, b.retries = 0;
    step();
}

static uint16_t who_done = BLE_HS_CONN_HANDLE_NONE;
static int on_who(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (!err->status && attr) {
        uint16_t v = 0;
        ble_hs_mbuf_to_flat(attr->om, &v, sizeof v, NULL);
        ESP_LOGI(TAG, "it says it is %s (appearance 0x%04x)", v >> 6 == 1 ? "a phone" : v >> 6 ? "not a phone: the computer" : "nothing", v);
        who_done = conn;
        decide(conn, v >> 6);
    } else if (who_done != conn) decide(conn, -1); /* no answer */
    if (err->status) who_done = BLE_HS_CONN_HANDLE_NONE;
    return 0;
}

static void who_is(uint16_t c)
{
    if (ble_gattc_read_by_uuid(c, 1, 0xFFFF, BLE_UUID16_DECLARE(0x2A01), on_who, NULL)) decide(c, -1);
}

/* The same question asked the moment it connects, before any pairing (Appearance needs no encryption): a new
   computer then pairs as a computer, instead of being shown the iPhone's pairing screen. */
static int8_t kind[8]; /* what each connection said it is: Appearance category, 0 not yet */
static int kind_of(uint16_t c) { return c < 8 ? kind[c] : 0; }

static void as_computer(uint16_t c) /* not the phone after all: out of the phone's place, then it pairs or encrypts */
{
    if (c != pending) return;
    ble_npl_callout_stop(&claim);
    pending = BLE_HS_CONN_HANDLE_NONE, pending2 = c;
    ble_gap_security_initiate(c);
}

static int on_who_early(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status || !attr || conn >= 8) return 0;
    uint16_t v = 0;
    ble_hs_mbuf_to_flat(attr->om, &v, sizeof v, NULL);
    kind[conn] = (int8_t)(v >> 6 ? v >> 6 : -1);
    ESP_LOGI(TAG, "connecting: %s (appearance 0x%04x)", v >> 6 == 1 ? "a phone" : v >> 6 ? "not a phone: the computer" : "it does not say", v);
    if (v >> 6 > 1) as_computer(conn);
    return 0;
}

static int dbg_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    if (conn == pending) ble_npl_callout_stop(&claim), pending = BLE_HS_CONN_HANDLE_NONE;
    if (conn == pending2) pending2 = BLE_HS_CONN_HANDLE_NONE;
    if (dbg_conn != conn) {
        dbg_conn = conn;
        ESP_LOGI(TAG, "debug page connected");
        advertise();
    }
    static char line[200];
    static int len;
    uint8_t d[200];
    uint16_t n = 0;
    ble_hs_mbuf_to_flat(ctxt->om, d, sizeof d, &n);
    for (int i = 0; i < n; i++) {
        if (d[i] == '\n') line[len] = 0, debug_line(line), len = 0;
        else if (len < (int)sizeof line - 1) line[len++] = (char)d[i];
    }
    return 0;
}

bool ble_debug_on(void) { return dbg_conn != BLE_HS_CONN_HANDLE_NONE; }

void ble_debug_send(const char *s, int n) /* notifications as big as the link allows; waits when the buffers are full */
{
    uint16_t c = dbg_conn;
    if (c == BLE_HS_CONN_HANDLE_NONE) return;
    int mtu = ble_att_mtu(c) - 3;
    if (mtu < 20) mtu = 20;
    for (int at = 0; at < n;) {
        int k = n - at < mtu ? n - at : mtu;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s + at, (uint16_t)k);
        int rc = om ? ble_gatts_notify_custom(c, dbg_tx, om) : BLE_HS_ENOMEM;
        if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        if (rc) return; /* gone */
        at += k;
    }
}

/* ---------- GAP ---------- */
static int gap_event(struct ble_gap_event *e, void *arg)
{
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status) { advertise(); break; }
        if (e->connect.conn_handle < 8) since[e->connect.conn_handle] = esp_timer_get_time(), kind[e->connect.conn_handle] = 0;
        ble_gattc_read_by_uuid(e->connect.conn_handle, 1, 0xFFFF, BLE_UUID16_DECLARE(0x2A01), on_who_early, NULL);
        if (b.connected || pending != BLE_HS_CONN_HANDLE_NONE) { /* the debug page (it writes to us) or a computer */
            pending2 = e->connect.conn_handle;
            ble_gap_security_initiate(pending2); /* bonded: it just encrypts, then says who it is */
            advertise();
            break;
        }
        pending = e->connect.conn_handle; /* the phone, unless it turns out to be the debug page */
        ble_npl_callout_reset(&claim, ble_npl_time_ms_to_ticks32(4000));
        advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if (e->disconnect.conn.conn_handle == dbg_conn) {
            dbg_conn = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "debug page left");
            advertise();
            break;
        }
        if (e->disconnect.conn.conn_handle == pending) { ble_npl_callout_stop(&claim), pending = BLE_HS_CONN_HANDLE_NONE; advertise(); break; }
        if (e->disconnect.conn.conn_handle == pending2) { pending2 = BLE_HS_CONN_HANDLE_NONE; advertise(); break; }
        if (e->disconnect.conn.conn_handle == comp_conn) {
            comp_conn = BLE_HS_CONN_HANDLE_NONE, comp_ready = false, comp_len = 0;
            ESP_LOGI(TAG, "companion left");
            link_sync();
            /* fall through: the same link may also be the phone slot or the computer */
        }
        if (e->disconnect.conn.conn_handle == host_conn) {
            host_conn = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "the computer left");
            ui(ui_host_off, NULL);
            advertise();
            break;
        }
        ESP_LOGI(TAG, "disconnected (%d)", e->disconnect.reason);
        b.connected = false, b.conn = BLE_HS_CONN_HANDLE_NONE;
        link_sync();
        advertise();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        bool computer = e->passkey.conn_handle == pending2 || kind_of(e->passkey.conn_handle) > 1;
        if (computer) as_computer(e->passkey.conn_handle);
        else if (e->passkey.conn_handle == pending) phone_take(pending);
        pair_conn = e->passkey.conn_handle;
        if (e->passkey.params.action == BLE_SM_IOACT_NUMCMP) { /* both screens show it; the top button confirms */
            b.numcmp = true;
            uint32_t code = e->passkey.params.numcmp;
            ui(computer ? ui_host_pair : ui_link_pair, &code);
        } else if (e->passkey.params.action == BLE_SM_IOACT_DISP) { /* the other side asks to type ours */
            struct ble_sm_io io = {.action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000};
            ble_sm_inject_io(e->passkey.conn_handle, &io);
            uint32_t code = io.passkey;
            ui(computer ? ui_host_pair : ui_link_pair, &code);
        }
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        uint16_t c = e->enc_change.conn_handle;
        if (c == host_conn) break; /* the computer again */
        if (e->enc_change.status) {
            ESP_LOGI(TAG, "encryption failed");
            if (c == b.conn) link_sync();
            if (c == pending2) pending2 = BLE_HS_CONN_HANDLE_NONE, ui(ui_host_off, NULL);
            break;
        }
        if (c == pending) phone_take(pending); /* the debug page never asks for encryption */
        if (c == pending2) pending2 = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "encryption on");
        if (kind_of(c) > 0) decide(c, kind_of(c)); /* it said so when it connected */
        else who_is(c);                         /* a phone or a computer: it says so itself */
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: { /* the phone forgot us: forget it too and pair again */
        struct ble_gap_conn_desc d;
        if (!ble_gap_conn_find(e->repeat_pairing.conn_handle, &d)) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t d[256];
        uint16_t n = 0;
        ble_hs_mbuf_to_flat(e->notify_rx.om, d, sizeof d, &n);
        uint16_t h = e->notify_rx.attr_handle;
        if (b.type == PHONE_TYPE_IPHONE) {
            if (h == b.ns) ancs_source(d, n);
            else if (h == b.ds) ancs_data(d, n);
            else if (h == b.eu) ams_update(d, n);
        } else {
            if (h == b.ns) generic_notify(d, n);
            else if (h == b.cs) generic_call_state(d, n);
            else if (h == b.eu) ams_update(d, n); /* media keys still work via AMS-like attribute */
        }
        break;
    }
    }
    return 0;
}

/* As a keyboard (the HID service and the keyboard appearance) named Pip, which iOS lists in Settings -> Bluetooth;
   the scan response asks for ANCS. */
/* ---------- the Bluetooth page (phone.c phone_bt_*): who is on which link, a reconnect, forgetting everyone ---------- */
static void bt_peer(phone_bt_t *s, uint16_t c, int role)
{
    struct ble_gap_conn_desc d;
    if (c == BLE_HS_CONN_HANDLE_NONE || s->n >= 5 || ble_gap_conn_find(c, &d)) return;
    int8_t rssi = 0;
    ble_gap_conn_rssi(c, &rssi);
    phone_peer_t *p = &s->peer[s->n++];
    *p = (phone_peer_t){.role = role, .rssi = rssi, .secs = c < 8 ? (int)((esp_timer_get_time() - since[c]) / 1000000) : 0,
                        .encrypted = d.sec_state.encrypted, .bonded = d.sec_state.bonded};
    if (role == PEER_PHONE) p->ready = b.ready, p->ancs = b.ns && b.ds && b.cp, p->ams = b.eu && b.rc, p->cts = b.ct;
}

static void bt_query(phone_bt_t *s)
{
    memset(s, 0, sizeof *s);
    bt_peer(s, b.conn, PEER_PHONE);
    if (comp_conn != BLE_HS_CONN_HANDLE_NONE && comp_conn != b.conn && comp_conn != host_conn && comp_conn != dbg_conn) {
        int before = s->n;
        bt_peer(s, comp_conn, PEER_PHONE);
        if (s->n > before) { /* the companion app's own link: notifications, music and time */
            phone_peer_t *p = &s->peer[s->n - 1];
            p->ready = comp_ready, p->ancs = comp_ready, p->ams = comp_ready, p->cts = comp_ready, p->type = b.type;
        }
    }
    bt_peer(s, host_conn, PEER_COMPUTER);
    bt_peer(s, dbg_conn, PEER_DEBUG); /* may be the computer's own link: Chrome on the same Mac */
    bt_peer(s, pending, PEER_UNKNOWN);
    bt_peer(s, pending2, PEER_UNKNOWN);
    s->advertising = ble_gap_adv_active();
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &s->bonds);
}

static void bt_act(int what)
{
    if (what == BT_RECONNECT_PHONE) { /* drop the phone's link: iOS comes straight back and everything is looked up anew */
        if (b.connected) ble_gap_terminate(b.conn, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    /* forget everyone: every link down, every bond, and who was a phone or a computer */
    const uint16_t cs[] = {b.conn, host_conn, dbg_conn, comp_conn, pending, pending2};
    for (unsigned i = 0; i < sizeof cs / sizeof cs[0]; i++)
        if (cs[i] != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(cs[i], BLE_ERR_REM_USER_CONN_TERM);
    comp_conn = BLE_HS_CONN_HANDLE_NONE, comp_ready = false, comp_len = 0;
    ble_store_clear();
    hosts_n = phones_n = 0;
    nvs_handle_t h;
    if (nvs_open("keypet", NVS_READWRITE, &h) == ESP_OK) nvs_erase_key(h, "hosts"), nvs_erase_key(h, "phones"), nvs_commit(h), nvs_close(h);
    ESP_LOGI(TAG, "forgot every bonded device");
}

static bool up, quiet; /* the host started; asleep in energy saving: off the air */

void ble_quiet(bool q)
{
    if (!up) return;
    quiet = q;
    if (!q) { advertise(); ESP_LOGI(TAG, "awake: advertising again"); return; }
    ble_gap_adv_stop();
    const uint16_t cs[] = {b.conn, host_conn, dbg_conn, comp_conn, pending, pending2}; /* the iPhone reconnects by itself when we are back */
    for (unsigned i = 0; i < sizeof cs / sizeof cs[0]; i++)
        if (cs[i] != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(cs[i], BLE_ERR_REM_USER_CONN_TERM);
    comp_conn = BLE_HS_CONN_HANDLE_NONE, comp_ready = false, comp_len = 0;
    ESP_LOGI(TAG, "asleep: Bluetooth off the air");
}

static void advertise(void)
{
    if (quiet) return; /* a link that just went down would call us back on air */
    static const uint8_t adv[] = {2, 0x01, 0x06,          /* flags: general discoverable, LE only */
                                  3, 0x03, 0x12, 0x18,    /* services: HID */
                                  3, 0x19, 0xC1, 0x03,    /* appearance: keyboard */
                                  4, 0x09, 'P', 'i', 'p'}; /* name */
    uint8_t rsp[18] = {17, 0x15};
    memcpy(rsp + 2, ANCS.value, 16);
    ble_gap_adv_set_data(adv, sizeof adv);
    ble_gap_adv_rsp_set_data(rsp, sizeof rsp);
    if (ble_gap_adv_active()) return;
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int rc = ble_gap_adv_start(b.own_addr, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) ESP_LOGW(TAG, "advertising failed: %d", rc);
}

/* Bump when the services table changes: phones and computers bonded to the old one are told once (Service Changed, on
   their next connection) to look at our services again instead of trusting the handles they cached. */
#define GATT_LAYOUT 3 /* 3: the Pip Link companion service */

static void gatt_layout_check(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("keypet", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_u8(h, "gatt", &v);
    if (v != GATT_LAYOUT) {
        ble_svc_gatt_changed(0x0001, 0xFFFF);
        if (nvs_set_u8(h, "gatt", GATT_LAYOUT) == ESP_OK) nvs_commit(h);
        ESP_LOGI(TAG, "services changed (%d -> %d): bonded peers will look again", v, GATT_LAYOUT);
    }
    nvs_close(h);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &b.own_addr);
    gatt_layout_check();
    advertise();
    ESP_LOGI(TAG, "advertising as Pip");
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_start(void)
{
    uuid(&ANCS, "7905F431-B5CE-4E99-A40F-4B1E122D00D0");
    uuid(&ANCS_NS, "9FBF120D-6301-42D9-8C58-25E699A21DBD");
    uuid(&ANCS_CP, "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
    uuid(&ANCS_DS, "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
    uuid(&AMS, "89D3502B-0F36-433A-8EF4-C502AD55F8DC");
    uuid(&AMS_RC, "9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
    uuid(&AMS_EU, "2F7CABCE-808D-411F-9A0C-BB92BA96C102");
    /* generic (non-Apple) services: any Android/Windows host may expose these */
    uuid(&GATT_NOTIFY,   "C6A2F3E1-8B4D-421A-9F7C-3D5E6F7A8B9C");
    uuid(&GATT_NOTIFY_CHR,"D1B4C2E3-5F6A-4B8C-9D0E-1F2A3B4C5D6E");
    uuid(&GATT_MEDIA,    "E2F3A4B5-C6D7-4E8F-9A0B-1C2D3E4F5A6B");
    uuid(&GATT_MEDIA_CTRL,"F3A4B5C6-D7E8-4F9A-0B1C-2D3E4F5A6B7C");
    uuid(&GATT_MEDIA_TRACK,"A4B5C6D7-E8F9-4A0B-1C2D-3E4F5A6B7C8D");
    /* generic call service for Android/Windows */
    uuid(&GATT_CALL,      "B1A2C3D4-E5F6-4A7B-8C9D-0E1F2A3B4C5D");
    uuid(&GATT_CALL_STATE,"C2D3E4F5-A6B7-4C8D-9E0F-1A2B3C4D5E6F");
    uuid(&GATT_CALL_CTRL, "D3E4F5A6-B7C8-4D9E-0F1A-2B3C4D5E6F7A");
    uuid(&DBG, "50495044-4247-4B45-5950-455400000001");
    uuid(&DBG_RX, "50495044-4247-4B45-5950-455400000002");
    uuid(&DBG_TX, "50495044-4247-4B45-5950-455400000003");
    /* Pip Link: the companion app's service (docs/companion.md) */
    uuid(&COMP, "50495044-4247-4B45-5950-455400000010");
    uuid(&COMP_RX, "50495044-4247-4B45-5950-455400000011");
    uuid(&COMP_TX, "50495044-4247-4B45-5950-455400000012");
    uuid(&COMP_INFO, "50495044-4247-4B45-5950-455400000013");
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "NimBLE did not start"); return; }
    ble_npl_callout_init(&again, nimble_port_get_dflt_eventq(), look_again, NULL);
    ble_npl_callout_init(&claim, nimble_port_get_dflt_eventq(), claim_phone, NULL);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO; /* numeric comparison: secure, and one button press */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("Pip");
    ble_svc_gap_device_appearance_set(0x03C1); /* keyboard */
    if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) ESP_LOGE(TAG, "HID services did not register");
    ble_store_config_init(); /* bonds in NVS: pair once */
    phone_on_command(command);
    phone_on_bt(bt_query, bt_act);
    phone_on_host(host_cmd);
    hosts_load(); /* the computers we know */
    nimble_port_freertos_init(host_task);
    up = true;
}
