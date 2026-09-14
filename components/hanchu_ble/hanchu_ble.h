#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#ifdef USE_ESP32

#include <esp_gattc_api.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "hanchu_crypto.h"

namespace esphome::hanchu_ble {

namespace espbt = esphome::esp32_ble_tracker;

/// Group interval meaning "request once after boot, retried until answered". 0 means "every poll".
static constexpr uint32_t ONCE_AFTER_BOOT = UINT32_MAX;

class HanchuBlePollTrigger;

class HanchuBle : public PollingComponent, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void set_max_keys_per_request(uint8_t max_keys) { this->max_keys_per_request_ = max_keys; }
  void set_request_timeout(uint32_t timeout_ms) { this->request_timeout_ms_ = timeout_ms; }
  void add_poll_trigger(HanchuBlePollTrigger *trigger) { this->poll_triggers_.push_back(trigger); }

  void register_sensor(const std::string &key, const std::string &group, bool strict, uint32_t interval_ms,
                       float multiplier, sensor::Sensor *sens);
  void register_text_sensor(const std::string &key, const std::string &group, bool strict, uint32_t interval_ms,
                            text_sensor::TextSensor *sens);

 protected:
  enum class State : uint8_t { DISCONNECTED, SUBSCRIBING, HANDSHAKE, READY, WAITING_REPLY };

  struct Value {
    float number{NAN};
    std::string text;
    bool null{false};
  };
  using Values = std::map<std::string, Value>;

  /// Keys that are requested together. Strict groups (explicit `group:`) are never split.
  struct Group {
    std::string name;
    bool strict;
    uint32_t interval_ms;
    std::vector<std::string> keys;
    uint32_t last_success_ms{0};
    bool done{false};
  };
  struct SensorBinding {
    size_t group;
    std::string key;
    float multiplier;  // unit conversion for named sensors, e.g. SOC fraction to %
    sensor::Sensor *sensor;
  };
  struct TextSensorBinding {
    size_t group;
    std::string key;
    text_sensor::TextSensor *sensor;
  };
  /// One encrypted read request on the wire; its reply is a single snapshot for all groups in it.
  struct Request {
    std::vector<std::string> keys;
    std::vector<size_t> groups;
  };

  size_t find_or_create_group_(const std::string &name, bool strict, uint32_t interval_ms);
  void split_oversized_groups_();
  bool group_is_due_(const Group &group, uint32_t now) const;
  void build_cycle_();
  void append_request_(std::vector<std::string> keys, size_t group, bool allow_split);
  void send_next_request_();
  void finish_cycle_();
  void fail_request_(const char *reason);
  void reset_connection_state_();
  void end_connection_();
  void send_handshake_();
  void handle_notification_(const uint8_t *data, uint16_t length);
  void handle_frame_(const std::vector<uint8_t> &frame);
  void handle_reply_(const std::vector<uint8_t> &payload);
  bool parse_reply_(const std::vector<uint8_t> &payload, Values &values);
  void publish_request_(const Request &request, const Values &values);
  bool write_(const uint8_t *data, size_t length);
  size_t max_payload_() const { return this->mtu_ > 3 ? this->mtu_ - 3 : 0; }
  const char *address_() { return this->parent()->address_str(); }

  std::vector<Group> groups_;
  std::vector<SensorBinding> sensors_;
  std::vector<TextSensorBinding> text_sensors_;
  std::vector<HanchuBlePollTrigger *> poll_triggers_;

  std::vector<Request> cycle_;
  size_t cycle_index_{0};
  bool cycle_had_success_{false};
  std::map<uint16_t, std::vector<uint8_t>> fragments_;

  Aes128 aes_;
  State state_{State::DISCONNECTED};
  uint16_t notify_handle_{0};
  uint16_t write_handle_{0};
  uint16_t mtu_{23};
  uint8_t max_keys_per_request_{12};
  uint32_t connect_started_ms_{0};
  uint32_t request_timeout_ms_{5000};
  uint32_t state_started_ms_{0};
};

class HanchuBlePollTrigger : public Trigger<> {
 public:
  explicit HanchuBlePollTrigger(HanchuBle *parent) { parent->add_poll_trigger(this); }
};

}  // namespace esphome::hanchu_ble

#endif  // USE_ESP32
