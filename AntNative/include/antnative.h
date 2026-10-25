/*
 * AntNative C API: ANT USB stick access for any language that can call a C
 * DLL (C, C++, C# P/Invoke, LuaJIT FFI, Rust, GDExtension, ...).
 *
 * Link antnative.lib (the import library) and ship antnative.dll beside the
 * program. C++ programs can use antnative.hpp instead, a header-only RAII
 * wrapper over this API.
 *
 * Usage:
 *   ant_device* dev = ant_create();
 *   ant_open(dev, 57600, 0);                 stick 0 (see ant_list_devices)
 *   ant_reset(dev, 1000);
 *   ant_set_network_key(dev, 0, key, 1000);  then assign/configure/open channels
 *   ant_start_messages(dev);                 messages queue up from here
 *   ... each frame: n = ant_poll(dev, msgs, 32); handle msgs[0..n) ...
 *   ant_destroy(dev);                        stops, closes and frees
 *
 * Return values: functions returning int give ANT_OK on success, ANT_FAILED
 * when the stick refused or didn't answer in time, and a negative ANT_ERROR*
 * code when the call itself was wrong or failed; ant_last_error() then
 * describes it (per thread). ant_list_devices and ant_poll return a count
 * instead, or a negative error.
 *
 * Threading: one ant_device may be used from several threads; blocking calls
 * on it run one at a time. Messages arrive on an internal thread and wait in
 * a queue for ant_poll - nothing ever calls back into your code.
 */
#ifndef ANTNATIVE_H
#define ANTNATIVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef ANTNATIVE_BUILDING
#    define ANT_API __declspec(dllexport)
#  else
#    define ANT_API __declspec(dllimport)
#  endif
#else
#  define ANT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever a struct layout or function signature changes.
 * 2: ant_message.page_data, ant_page, the ant_manager API (sensors, profiles,
 *    watchdog) and the control page builders */
#define ANT_API_VERSION 2

/* Return codes */
#define ANT_OK 0
#define ANT_FAILED 1
#define ANT_ERROR (-1)
#define ANT_ERROR_INVALID_ARGUMENT (-2)
#define ANT_ERROR_NOT_OPEN (-3)

/* ant_send_acknowledged's transfer results (the ANT SDK's ANTFRAMER_*) */
#define ANT_TRANSFER_FAIL 0
#define ANT_TRANSFER_PASS 1
#define ANT_TRANSFER_TIMEOUT 2
#define ANT_TRANSFER_CANCELLED 3
#define ANT_TRANSFER_INVALID_PARAM 4

/* Channel types for ant_assign_channel */
#define ANT_CHANNEL_TYPE_SLAVE 0x00
#define ANT_CHANNEL_TYPE_MASTER 0x10

/* ANT+ device types decoded into ant_message's profile fields */
#define ANT_DEVICE_TYPE_POWER 11
#define ANT_DEVICE_TYPE_FITNESS_EQUIPMENT 17
#define ANT_DEVICE_TYPE_HEART_RATE 120
#define ANT_DEVICE_TYPE_SPEED_CADENCE 121
#define ANT_DEVICE_TYPE_CADENCE 122
#define ANT_DEVICE_TYPE_SPEED 123

#define ANT_USB_STRING_MAX 256
#define ANT_MESSAGE_DATA_MAX 41

typedef struct ant_device ant_device;

typedef struct ant_usb_device {
    uint32_t index; /* the device_number ant_open takes */
    uint32_t vid;
    uint32_t pid;
    uint32_t serial_number;
    uint32_t device_type;
    int has_product_description;
    int has_serial_string;
    char product_description[ANT_USB_STRING_MAX];
    char serial_string[ANT_USB_STRING_MAX];
} ant_usb_device;

typedef struct ant_channel_id {
    uint16_t device_number;
    uint8_t device_type;
    uint8_t transmission_type;
} ant_channel_id;

/* One ANT+ data page decoded field by field (see ant_decode_page). Field
 * names and units are listed in src/core/ant_pages.cpp and python/ant/pages.py;
 * they're the same in every language. */
#define ANT_PAGE_FIELDS_MAX 16
#define ANT_VALUE_INT 0
#define ANT_VALUE_DOUBLE 1
#define ANT_VALUE_BOOL 2

typedef struct ant_page_field {
    const char* name;  /* static string, e.g. "speed_mps" */
    int type;          /* ANT_VALUE_* */
    double value;      /* every type, as a double (bools 0/1) */
    int64_t int_value; /* ANT_VALUE_INT and ANT_VALUE_BOOL */
} ant_page_field;

typedef struct ant_page {
    int page;          /* the page number, or -1 when there's none */
    const char* name;  /* static string, e.g. "general_fe_data"; "unknown" when not decoded */
    int field_count;
    ant_page_field fields[ANT_PAGE_FIELDS_MAX];
} ant_page;

/* ant_message.fields: which of ant_message's optional field groups are set */
#define ANT_FIELD_CHANNEL               (1ull << 0)  /* channel */
#define ANT_FIELD_PAYLOAD               (1ull << 1)  /* payload, payload_size */
#define ANT_FIELD_EXTENDED              (1ull << 2)  /* extended_bitfield, extended_data(_size) */
#define ANT_FIELD_RSSI                  (1ull << 3)  /* rssi_data(_size) */
#define ANT_FIELD_DEVICE_ID             (1ull << 4)  /* device_number, device_type, transmission_type */
#define ANT_FIELD_RESPONSE_EVENT        (1ull << 5)  /* response_to_message_id, event_code, event_name */
#define ANT_FIELD_ERROR_CODE            (1ull << 6)  /* error_code */
#define ANT_FIELD_PAGE                  (1ull << 7)  /* page, page_change_toggle */
#define ANT_FIELD_HEART_RATE            (1ull << 8)  /* heart_beat_event_time/_ticks, heart_beat_count, computed_heart_rate */
#define ANT_FIELD_HEART_RATE_PREVIOUS   (1ull << 9)  /* previous_heart_beat_event_time, rr_interval */
#define ANT_FIELD_POWER                 (1ull << 10) /* instantaneous_power_watts */
#define ANT_FIELD_POWER_CADENCE         (1ull << 11) /* power_meter_cadence_rpm */
#define ANT_FIELD_SPEED_CADENCE         (1ull << 12) /* cadence_* and speed_* (combined sensor, 121) */
#define ANT_FIELD_SPEED_OR_CADENCE      (1ull << 13) /* event_time, event_ticks, revolution_count (122/123) */
#define ANT_FIELD_PAGE_DATA             (1ull << 14) /* page_data: the payload's page, every field decoded */

/* extended_bitfield flags */
#define ANT_EXT_DEVICE_ID 0x80
#define ANT_EXT_RSSI 0x40
#define ANT_EXT_TIMESTAMP 0x20
#define ANT_EXT_EXTENSION 0x01

/* One message from the stick, decoded. Check `fields` before reading any
 * optional group; unset groups are zero. */
typedef struct ant_message {
    uint64_t fields;
    uint8_t message_id;
    uint8_t is_error;
    uint8_t error_code;
    uint8_t channel;
    const char* message_name; /* static string, e.g. "broadcast_data" */

    /* The message exactly as received (data[0] is the channel for data messages) */
    uint8_t data_size;
    uint8_t data[ANT_MESSAGE_DATA_MAX];

    /* Data messages: the 8-byte ANT payload */
    uint8_t payload_size;
    uint8_t payload[8];

    /* Flagged extended data */
    uint8_t extended_bitfield;
    uint8_t extended_data_size;
    uint8_t extended_data[ANT_MESSAGE_DATA_MAX];
    uint8_t rssi_data_size;
    uint8_t rssi_data[ANT_MESSAGE_DATA_MAX];

    /* Device identity (extended data, or a Channel ID response) */
    uint16_t device_number;
    uint8_t device_type;
    uint8_t transmission_type;

    /* RESPONSE_EVENT (0x40) */
    uint8_t response_to_message_id;
    uint8_t event_code;
    const char* event_name; /* static string, e.g. "EVENT_RX_FAIL" */

    /* ANT+ pages */
    uint8_t page;
    uint8_t page_change_toggle;
    /* Heart rate (120) */
    double heart_beat_event_time;  /* seconds */
    uint16_t heart_beat_event_ticks; /* 1/1024 s */
    uint8_t heart_beat_count;
    uint8_t computed_heart_rate;   /* bpm */
    double previous_heart_beat_event_time;
    double rr_interval;            /* seconds */
    /* Bicycle power (11) */
    uint16_t instantaneous_power_watts;
    uint8_t power_meter_cadence_rpm;
    /* Combined speed/cadence (121) */
    double cadence_event_time;
    uint16_t cadence_event_ticks;
    uint16_t cadence_revolution_count;
    double speed_event_time;
    uint16_t speed_event_ticks;
    uint16_t speed_revolution_count;
    /* Speed-only (123) / cadence-only (122) */
    double event_time;
    uint16_t event_ticks;
    uint16_t revolution_count;

    /* The payload's page, every field decoded (ANT_FIELD_PAGE_DATA) */
    ant_page page_data;
} ant_message;

ANT_API int ant_api_version(void);

/* The message describing the last error on this thread ("" if none) */
ANT_API const char* ant_last_error(void);

/* Fills up to `max` entries of `out` (which may be NULL when max is 0) and
 * returns how many sticks are attached, or a negative error */
ANT_API int ant_list_devices(ant_usb_device* out, int max);

ANT_API ant_device* ant_create(void);
/* Stops messages, closes the stick and frees `dev` (NULL is ignored) */
ANT_API void ant_destroy(ant_device* dev);

ANT_API int ant_open(ant_device* dev, uint32_t baudrate, uint8_t device_number);
ANT_API void ant_close(ant_device* dev);
ANT_API int ant_is_open(ant_device* dev);

ANT_API int ant_reset(ant_device* dev, uint32_t timeout_ms);
ANT_API int ant_set_network_key(ant_device* dev, uint8_t network_number, const uint8_t key[8], uint32_t timeout_ms);
ANT_API int ant_assign_channel(ant_device* dev, uint8_t channel, uint8_t channel_type, uint8_t network_number, uint32_t timeout_ms);
ANT_API int ant_unassign_channel(ant_device* dev, uint8_t channel, uint32_t timeout_ms);
ANT_API int ant_set_channel_id(ant_device* dev, uint8_t channel, uint16_t device_number, uint8_t device_type, uint8_t transmission_type, uint32_t timeout_ms);
ANT_API int ant_set_channel_period(ant_device* dev, uint8_t channel, uint16_t period, uint32_t timeout_ms);
ANT_API int ant_set_channel_rf_frequency(ant_device* dev, uint8_t channel, uint8_t rf_frequency, uint32_t timeout_ms);
ANT_API int ant_set_search_priority(ant_device* dev, uint8_t channel, uint8_t priority, uint32_t timeout_ms);
/* Search timeouts in 2.5 s units, 0xFF = search forever */
ANT_API int ant_set_search_timeout(ant_device* dev, uint8_t channel, uint8_t timeout, uint32_t timeout_ms);
ANT_API int ant_set_low_priority_search_timeout(ant_device* dev, uint8_t channel, uint8_t timeout, uint32_t timeout_ms);
ANT_API int ant_set_rx_extended_messages_enabled(ant_device* dev, int enabled, uint32_t timeout_ms);
ANT_API int ant_open_rx_scan_mode(ant_device* dev, uint32_t timeout_ms);
ANT_API int ant_open_channel(ant_device* dev, uint8_t channel, uint32_t timeout_ms);
ANT_API int ant_close_channel(ant_device* dev, uint8_t channel, uint32_t timeout_ms);
ANT_API int ant_request_message(ant_device* dev, uint8_t channel, uint8_t message_id);
ANT_API int ant_get_channel_id(ant_device* dev, uint8_t channel, uint32_t timeout_ms, ant_channel_id* out);

ANT_API int ant_send_broadcast(ant_device* dev, uint8_t channel, const uint8_t payload[8]);
/* `transfer_result` (may be NULL) gets the ANT_TRANSFER_* result; returns
 * ANT_OK only for ANT_TRANSFER_PASS */
ANT_API int ant_send_acknowledged(ant_device* dev, uint8_t channel, const uint8_t payload[8], uint32_t timeout_ms, int* transfer_result);
ANT_API int ant_send_burst_packet(ant_device* dev, uint8_t channel_sequence, const uint8_t payload[8]);

/* Starts queuing received messages for ant_poll (the device must be open).
 * The queue holds the newest 4096; older ones are dropped and counted. */
ANT_API int ant_start_messages(ant_device* dev);
ANT_API void ant_stop_messages(ant_device* dev);
/* Moves up to `max` queued messages, oldest first, into `out`; returns how
 * many, or a negative error */
ANT_API int ant_poll(ant_device* dev, ant_message* out, int max);
/* Messages dropped from a full queue since ant_start_messages */
ANT_API uint64_t ant_dropped_messages(ant_device* dev);

/* Decodes one raw message the way ant_poll does (for messages captured
 * elsewhere, or tests). `channel` < 0 means unknown. */
ANT_API int ant_decode_message(uint8_t message_id, const uint8_t* data, size_t data_size,
                               int channel, ant_message* out);

/* ---- Pages ------------------------------------------------------------- */

/* Decodes one 8-byte payload from a `device_type` sensor. ANT_FAILED for a
 * combined speed & cadence sensor (121), which has no pages. */
ANT_API int ant_decode_page(int device_type, const uint8_t payload[8], ant_page* out);
/* A field's value by name: ANT_OK, or ANT_FAILED when the page hasn't got it */
ANT_API int ant_page_get_field(const ant_page* page, const char* name, double* value);

/* Control pages a display sends (each fills `out` with 8 bytes). A NAN
 * argument sends that field's "use your default" value. */
ANT_API void ant_page_basic_resistance(double percent, uint8_t out[8]);    /* FE-C 48: 0-100% */
ANT_API void ant_page_target_power(double watts, uint8_t out[8]);          /* FE-C 49: 0-4000 W */
ANT_API void ant_page_wind_resistance(double coefficient_kg_per_m, double wind_speed_kmh, double drafting_factor,
                                      uint8_t out[8]);                     /* FE-C 50 */
ANT_API void ant_page_track_resistance(double grade_percent, double rolling_resistance,
                                       uint8_t out[8]);                    /* FE-C 51: -200 to 200% */
ANT_API void ant_page_user_configuration(double user_weight_kg, double bicycle_weight_kg, double wheel_diameter_m,
                                         double gear_ratio, uint8_t out[8]);   /* FE-C 55 */
/* Common page 70: ask for `page` `times` times (1-127), or until acknowledged */
ANT_API void ant_page_request(uint8_t page, uint8_t times, int acknowledged, uint8_t out[8]);
ANT_API void ant_page_power_manual_zero(uint8_t out[8]);                   /* power page 1 */

/* ---- Sensors -------------------------------------------------------------
 *
 * ant_manager does what the Python ant package's ANTManager and
 * RecoveryManager do: scans for ANT+ sensors, moves chosen ones onto their own
 * channels (the only way to send to them, e.g. a trainer's target power),
 * keeps each sensor's profile (heart rate, power, speed, cadence, fitness
 * equipment) and, if started, the watchdog that reopens channels and resets
 * the stick when sensors drop out. Messages arrive on an internal thread;
 * ant_manager_sensors gives a snapshot whenever it's called.
 *
 *   ant_manager* m = ant_manager_create();
 *   ant_manager_open(m, -1);
 *   ant_manager_configure_scan(m, key, 0, 0, 57);
 *   ant_manager_start(m);
 *   ... ant_manager_sensors(m, sensors, 16) each frame ...
 *   ant_manager_open_device_channels(m, chosen, n);   (to send to them)
 *   ant_manager_destroy(m);
 */

typedef struct ant_manager ant_manager;

typedef struct ant_sensor_id {
    uint8_t device_type;
    uint16_t device_number;
    uint8_t transmission_type;  /* 0 matches any */
} ant_sensor_id;

#define ANT_SENSOR_ACTIVE 0
#define ANT_SENSOR_STALE 1
#define ANT_SENSOR_LOST 2

/* ant_sensor.fields: which readings are set */
#define ANT_READING_HEART_RATE   (1u << 0)  /* heart_rate, heart_beat_count */
#define ANT_READING_RR_INTERVAL  (1u << 1)  /* rr_interval */
#define ANT_READING_POWER        (1u << 2)  /* power_watts */
#define ANT_READING_CADENCE      (1u << 3)  /* cadence_rpm (cadence sensor, power meter or trainer) */
#define ANT_READING_SPEED        (1u << 4)  /* speed_mps */
#define ANT_READING_TRAINER      (1u << 5)  /* trainer_status */
#define ANT_READING_CAPABILITIES (1u << 6)  /* supports_*, maximum_resistance_n */
#define ANT_READING_BATTERY      (1u << 7)  /* battery_status, battery_level_percent, battery_voltage */

typedef struct ant_sensor {
    uint16_t device_number;
    uint8_t device_type;
    uint8_t transmission_type;
    int channel;               /* its dedicated channel, or -1 */
    int state;                 /* ANT_SENSOR_* (watchdog running: otherwise ACTIVE) */
    double seconds_since_heard;
    uint64_t packet_count;
    int data_stalled;          /* still heard but the reading has frozen */
    uint32_t fields;           /* ANT_READING_* */

    int heart_rate;            /* bpm */
    int heart_beat_count;
    double rr_interval;        /* seconds */
    int power_watts;
    double cadence_rpm;
    double speed_mps;          /* uses the wheel circumference set for it */
    int trainer_status;        /* FE-C trainer status bits */
    int supports_basic_resistance, supports_target_power, supports_simulation;
    int maximum_resistance_n;  /* -1 when not reported */
    int battery_status;        /* 1 new ... 5 critical; 0/6/7 unknown */
    int battery_level_percent; /* -1 when not reported */
    double battery_voltage;    /* NAN when not reported */
} ant_sensor;

typedef struct ant_recovery_options {
    double stale_timeout, lost_timeout, interval;
    int channel_retry_limit;
    double channel_retry_cooldown, device_reset_timeout, device_reset_cooldown, channel_reopen_wait;
    double channel_stick_reset_cooldown, channel_open_retry_interval, hr_frozen_timeout, value_frozen_timeout;
} ant_recovery_options;

ANT_API ant_manager* ant_manager_create(void);
/* Stops everything, closes the stick and frees `m` (NULL is ignored) */
ANT_API void ant_manager_destroy(ant_manager* m);

/* `device_number` < 0: the first stick that opens */
ANT_API int ant_manager_open(ant_manager* m, int device_number);
ANT_API void ant_manager_close(ant_manager* m);
/* Resets the stick and opens the scan channel (ANT+: rf_frequency 57) */
ANT_API int ant_manager_configure_scan(ant_manager* m, const uint8_t key[8], uint8_t network_number,
                                       uint8_t channel, uint8_t rf_frequency);
/* Starts and stops the message pump */
ANT_API int ant_manager_start(ant_manager* m);
ANT_API void ant_manager_stop(ant_manager* m);

/* One dedicated channel per sensor (channels 0, 1, ... in order). On
 * failure the stick is left scanning. */
ANT_API int ant_manager_open_device_channels(ant_manager* m, const ant_sensor_id* sensors, int count);
ANT_API int ant_manager_return_to_scan(ant_manager* m);
/* The sensor's dedicated channel, or -1 */
ANT_API int ant_manager_channel_for(ant_manager* m, uint8_t device_type, uint16_t device_number);
/* Sends an 8-byte page to a sensor on its dedicated channel, acknowledged.
 * `transfer_result` (may be NULL) gets the ANT_TRANSFER_* result. */
ANT_API int ant_manager_send(ant_manager* m, uint8_t device_type, uint16_t device_number, const uint8_t payload[8],
                             uint32_t timeout_ms, int* transfer_result);

/* Every sensor heard so far; returns how many there are (fills up to `max`) */
ANT_API int ant_manager_sensors(ant_manager* m, ant_sensor* out, int max);
/* The last page `page` received from a sensor: ANT_OK, or ANT_FAILED if none yet */
ANT_API int ant_manager_sensor_page(ant_manager* m, uint8_t device_type, uint16_t device_number, int page,
                                    ant_page* out);
/* Wheel circumference for a speed (123) or speed & cadence (121) sensor's speed */
ANT_API int ant_manager_set_wheel_circumference(ant_manager* m, uint16_t device_number, double meters);

/* Every message, decoded, as ant_poll gives them (queue of the newest 4096) */
ANT_API int ant_manager_poll(ant_manager* m, ant_message* out, int max);

/* The watchdog. `options` may be NULL for the defaults (as RecoveryManager's). */
ANT_API void ant_recovery_default_options(ant_recovery_options* out);
ANT_API int ant_manager_start_recovery(ant_manager* m, const ant_recovery_options* options);
ANT_API void ant_manager_stop_recovery(ant_manager* m);
/* Only these sensors drive the watchdog; count < 0 watches every sensor */
ANT_API int ant_manager_watch(ant_manager* m, const ant_sensor_id* sensors, int count);

/* The manager's and watchdog's log, a line at a time: copies the oldest line
 * into `buffer` (up to size - 1 characters) and returns its length, or 0 when
 * there's none (the newest 256 are kept) */
ANT_API int ant_manager_next_log(ant_manager* m, char* buffer, int size);

#ifdef __cplusplus
}
#endif

#endif /* ANTNATIVE_H */
