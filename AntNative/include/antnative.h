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

/* Bumped whenever a struct layout or function signature changes */
#define ANT_API_VERSION 1

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

#ifdef __cplusplus
}
#endif

#endif /* ANTNATIVE_H */
