#include "hanchu_ble.h"

#include "esphome/components/json/json_util.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome::hanchu_ble {

static const char *const TAG = "hanchu_ble";

static constexpr uint16_t SERVICE_UUID = 0xFFFF;
static constexpr uint16_t SERVICE_UUID_FALLBACK = 0xFF00;
static constexpr uint16_t NOTIFY_CHAR_UUID = 0xFF01;
static constexpr uint16_t WRITE_CHAR_UUID = 0xFF02;

static constexpr uint8_t HANDSHAKE_PREFIX = 0x05;
static constexpr uint8_t FRAME_TYPE_DATA = 0x03;
static constexpr uint8_t PACKET_TYPE_FINAL = 0x00;
static constexpr size_t FRAME_HEADER_LENGTH = 6;
static constexpr uint32_t HANDSHAKE_TIMEOUT_MS = 5000;
static constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;
static constexpr uint16_t DEFAULT_MTU = 23;
static constexpr const char *TOKEN_CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

// The logger answers malformed requests with plaintext errors, so stick to the exact format known to work.
static std::string build_read_request(const std::vector<std::string> &keys) {
  std::string json = R"({"act":"1","cmd":"local","data":[)";
  for (size_t i = 0; i < keys.size(); i++) {
    if (i > 0)
      json += ',';
    json += R"({"k":")";
    json += keys[i];
    json += R"("})";
  }
  json += R"(],"tid":"10001"})";
  return json;
}

static std::string join_keys(const std::vector<std::string> &keys) {
  std::string joined;
  for (const auto &key : keys) {
    if (!joined.empty())
      joined += ',';
    joined += key;
  }
  return joined;
}

// BLEClientBase::register_for_notify() (ESPHome 2026.8+) keeps the GATT cache alive until the registration
// completes; older ESPHome versions only offer the raw IDF call.
template<typename Client>
static auto register_for_notify(Client *client, uint16_t handle, int /*prefer*/)
    -> decltype(client->register_for_notify(handle)) {
  return client->register_for_notify(handle);
}
template<typename Client> static esp_err_t register_for_notify(Client *client, uint16_t handle, long /*fallback*/) {
  return esp_ble_gattc_register_for_notify(client->get_gattc_if(), client->get_remote_bda(), handle);
}

static bool contains(const std::vector<std::string> &keys, const std::string &key) {
  return std::find(keys.begin(), keys.end(), key) != keys.end();
}

void HanchuBle::setup() {
  if (!crypto_self_test()) {
    ESP_LOGE(TAG, "AES self-test failed, refusing to talk to the device");
    this->mark_failed();
    return;
  }
  this->split_oversized_groups_();
  // The client starts enabled, so the first poll connects right away
  this->connect_started_ms_ = millis();
}

void HanchuBle::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Hanchu BLE:\n"
                "  Address: %s\n"
                "  Max keys per request: %u\n"
                "  Request timeout: %u ms",
                this->address_(), this->max_keys_per_request_, (unsigned) this->request_timeout_ms_);
  LOG_UPDATE_INTERVAL(this);
  for (const auto &group : this->groups_) {
    char interval[24];
    if (group.interval_ms == ONCE_AFTER_BOOT) {
      snprintf(interval, sizeof(interval), "once after boot");
    } else if (group.interval_ms == 0) {
      snprintf(interval, sizeof(interval), "every poll");
    } else {
      snprintf(interval, sizeof(interval), "%u ms", (unsigned) group.interval_ms);
    }
    ESP_LOGCONFIG(TAG, "  Group '%s' (%s, %s): %s", group.name.c_str(), interval, group.strict ? "fixed" : "auto",
                  join_keys(group.keys).c_str());
    if (group.strict && group.keys.size() > this->max_keys_per_request_) {
      ESP_LOGE(TAG, "    Group has more keys than max_keys_per_request and will never be requested");
    }
  }
  for (const auto &binding : this->sensors_) {
    LOG_SENSOR("  ", "Sensor", binding.sensor);
    ESP_LOGCONFIG(TAG, "    Key: %s", binding.key.c_str());
  }
  for (const auto &binding : this->text_sensors_) {
    LOG_TEXT_SENSOR("  ", "Text sensor", binding.sensor);
    ESP_LOGCONFIG(TAG, "    Key: %s", binding.key.c_str());
  }
}

size_t HanchuBle::find_or_create_group_(const std::string &name, bool strict, uint32_t interval_ms) {
  for (size_t i = 0; i < this->groups_.size(); i++) {
    if (this->groups_[i].name == name && this->groups_[i].interval_ms == interval_ms)
      return i;
  }
  Group group;
  group.name = name;
  group.strict = strict;
  group.interval_ms = interval_ms;
  this->groups_.push_back(std::move(group));
  return this->groups_.size() - 1;
}

void HanchuBle::register_sensor(const std::string &key, const std::string &group, bool strict, uint32_t interval_ms,
                                float multiplier, sensor::Sensor *sens) {
  size_t index = this->find_or_create_group_(group, strict, interval_ms);
  if (!contains(this->groups_[index].keys, key))
    this->groups_[index].keys.push_back(key);
  this->sensors_.push_back({index, key, multiplier, sens});
}

void HanchuBle::register_text_sensor(const std::string &key, const std::string &group, bool strict,
                                     uint32_t interval_ms, text_sensor::TextSensor *sens) {
  size_t index = this->find_or_create_group_(group, strict, interval_ms);
  if (!contains(this->groups_[index].keys, key))
    this->groups_[index].keys.push_back(key);
  this->text_sensors_.push_back({index, key, sens});
}

void HanchuBle::split_oversized_groups_() {
  const size_t original_count = this->groups_.size();
  for (size_t i = 0; i < original_count; i++) {
    if (this->groups_[i].strict)
      continue;
    while (this->groups_[i].keys.size() > this->max_keys_per_request_) {
      auto &keys = this->groups_[i].keys;
      size_t take = std::min<size_t>(keys.size() - this->max_keys_per_request_, this->max_keys_per_request_);
      Group part;
      part.name = this->groups_[i].name + "_" + std::to_string(this->groups_.size());
      part.strict = false;
      part.interval_ms = this->groups_[i].interval_ms;
      part.keys.assign(keys.end() - take, keys.end());
      keys.resize(keys.size() - take);

      const size_t new_index = this->groups_.size();
      for (auto &binding : this->sensors_) {
        if (binding.group == i && contains(part.keys, binding.key))
          binding.group = new_index;
      }
      for (auto &binding : this->text_sensors_) {
        if (binding.group == i && contains(part.keys, binding.key))
          binding.group = new_index;
      }
      this->groups_.push_back(std::move(part));
    }
  }
}

bool HanchuBle::group_is_due_(const Group &group, uint32_t now) const {
  if (group.interval_ms == ONCE_AFTER_BOOT)
    return !group.done;
  if (group.interval_ms == 0 || group.last_success_ms == 0)
    return true;
  // Half an update interval of slack, so a 60 s group isn't pushed one tick late every time
  return now - group.last_success_ms + this->get_update_interval() / 2 >= group.interval_ms;
}

void HanchuBle::append_request_(std::vector<std::string> keys, size_t group, bool allow_split) {
  if (keys.size() <= this->max_keys_per_request_ && build_read_request(keys).size() <= this->max_payload_()) {
    Request request;
    request.keys = std::move(keys);
    request.groups.push_back(group);
    this->cycle_.push_back(std::move(request));
    return;
  }
  if (!allow_split || keys.size() <= 1) {
    ESP_LOGE(TAG, "[%s] Keys %s don't fit in one request (max %u keys, MTU %u); skipping", this->address_(),
             join_keys(keys).c_str(), this->max_keys_per_request_, this->mtu_);
    return;
  }
  const auto middle = keys.begin() + keys.size() / 2;
  this->append_request_(std::vector<std::string>(keys.begin(), middle), group, true);
  this->append_request_(std::vector<std::string>(middle, keys.end()), group, true);
}

void HanchuBle::build_cycle_() {
  this->cycle_.clear();
  this->cycle_index_ = 0;
  this->cycle_had_success_ = false;

  const uint32_t now = millis();
  std::vector<size_t> due;
  for (size_t i = 0; i < this->groups_.size(); i++) {
    if (this->group_is_due_(this->groups_[i], now))
      due.push_back(i);
  }
  // Explicit groups exist because their timing matters, so they go first; bigger groups first packs requests tighter
  auto rank = [this](size_t index) { return this->groups_[index].strict ? 0 : 1; };
  std::stable_sort(due.begin(), due.end(), [this, &rank](size_t a, size_t b) {
    if (rank(a) != rank(b))
      return rank(a) < rank(b);
    return this->groups_[a].keys.size() > this->groups_[b].keys.size();
  });

  for (size_t index : due) {
    const auto &group = this->groups_[index];
    bool placed = false;
    // Whole groups share a request when they fit, which saves round trips
    for (size_t r = 0; r < this->cycle_.size() && !placed; r++) {
      auto &request = this->cycle_[r];
      std::vector<std::string> merged = request.keys;
      for (const auto &key : group.keys) {
        if (!contains(merged, key))
          merged.push_back(key);
      }
      if (merged.size() <= this->max_keys_per_request_ &&
          build_read_request(merged).size() <= this->max_payload_()) {
        request.keys = std::move(merged);
        request.groups.push_back(index);
        placed = true;
      }
    }
    if (!placed)
      this->append_request_(group.keys, index, !group.strict);
  }
}

void HanchuBle::update() {
  if (!this->parent()->enabled) {
    // Connect for this poll; it starts as soon as the handshake is acknowledged
    ESP_LOGD(TAG, "[%s] Connecting for poll", this->address_());
    this->connect_started_ms_ = millis();
    this->parent()->set_enabled(true);
    return;
  }
  if (this->state_ != State::READY) {
    ESP_LOGD(TAG, "[%s] Previous poll still running, skipping this one", this->address_());
    return;
  }
  if (this->mtu_ <= DEFAULT_MTU) {
    ESP_LOGD(TAG, "[%s] Waiting for MTU negotiation", this->address_());
    return;
  }
  this->build_cycle_();
  this->send_next_request_();
}

void HanchuBle::loop() {
  const uint32_t now = millis();
  if (this->state_ == State::HANDSHAKE && now - this->state_started_ms_ > HANDSHAKE_TIMEOUT_MS) {
    ESP_LOGW(TAG, "[%s] No handshake acknowledgement", this->address_());
    this->end_connection_();
  } else if (this->state_ == State::WAITING_REPLY && now - this->state_started_ms_ > this->request_timeout_ms_) {
    this->fail_request_("timeout");
  } else if (this->parent()->enabled && this->state_ != State::READY && this->state_ != State::WAITING_REPLY &&
             now - this->connect_started_ms_ > CONNECT_TIMEOUT_MS) {
    // Device not found, connection or service discovery stuck: give up, the next poll tries again
    ESP_LOGW(TAG, "[%s] Not connected within %u s", this->address_(), (unsigned) (CONNECT_TIMEOUT_MS / 1000));
    this->end_connection_();
  }
}

void HanchuBle::send_next_request_() {
  if (this->cycle_index_ >= this->cycle_.size()) {
    this->finish_cycle_();
    return;
  }
  const auto &request = this->cycle_[this->cycle_index_];
  const std::string json = build_read_request(request.keys);
  std::vector<uint8_t> encrypted(json.size());
  cfb8_encrypt(this->aes_, hanchu_state_vector(), reinterpret_cast<const uint8_t *>(json.data()), encrypted.data(),
               json.size());
  ESP_LOGV(TAG, "[%s] Request: %s", this->address_(), json.c_str());

  this->fragments_.clear();
  this->state_ = State::WAITING_REPLY;
  this->state_started_ms_ = millis();
  if (!this->write_(encrypted.data(), encrypted.size()))
    this->fail_request_("write failed");
}

void HanchuBle::finish_cycle_() {
  this->state_ = State::READY;
  const bool had_success = this->cycle_had_success_;
  this->cycle_.clear();
  this->cycle_index_ = 0;
  if (had_success) {
    for (auto *trigger : this->poll_triggers_)
      trigger->trigger();
  }
  this->end_connection_();
}

void HanchuBle::end_connection_() {
  // Disabling the client disconnects and stops automatic reconnects until the next poll enables it again
  this->state_ = State::DISCONNECTED;
  this->parent()->set_enabled(false);
}

void HanchuBle::fail_request_(const char *reason) {
  this->fragments_.clear();
  this->state_ = State::READY;

  ESP_LOGW(TAG, "[%s] Request %s failed (%s)", this->address_(),
           join_keys(this->cycle_[this->cycle_index_].keys).c_str(), reason);
  this->status_set_warning();
  // Carry on with the rest of the poll: a device may simply not answer some keys
  this->cycle_index_++;
  this->send_next_request_();
}

void HanchuBle::reset_connection_state_() {
  this->state_ = State::DISCONNECTED;
  this->cycle_.clear();
  this->cycle_index_ = 0;
  this->fragments_.clear();
  this->notify_handle_ = 0;
  this->write_handle_ = 0;
}

void HanchuBle::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                    esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status == ESP_GATT_OK)
        ESP_LOGI(TAG, "[%s] Connected", this->address_());
      break;

    case ESP_GATTC_CFG_MTU_EVT:
      if (param->cfg_mtu.status == ESP_GATT_OK) {
        this->mtu_ = param->cfg_mtu.mtu;
        ESP_LOGD(TAG, "[%s] MTU %u (max %u bytes per request)", this->address_(), this->mtu_,
                 (unsigned) this->max_payload_());
      }
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      this->reset_connection_state_();
      auto *notify_chr = this->parent()->get_characteristic(espbt::ESPBTUUID::from_uint16(SERVICE_UUID),
                                                            espbt::ESPBTUUID::from_uint16(NOTIFY_CHAR_UUID));
      auto *write_chr = this->parent()->get_characteristic(espbt::ESPBTUUID::from_uint16(SERVICE_UUID),
                                                           espbt::ESPBTUUID::from_uint16(WRITE_CHAR_UUID));
      if (notify_chr == nullptr || write_chr == nullptr) {
        notify_chr = this->parent()->get_characteristic(espbt::ESPBTUUID::from_uint16(SERVICE_UUID_FALLBACK),
                                                        espbt::ESPBTUUID::from_uint16(NOTIFY_CHAR_UUID));
        write_chr = this->parent()->get_characteristic(espbt::ESPBTUUID::from_uint16(SERVICE_UUID_FALLBACK),
                                                       espbt::ESPBTUUID::from_uint16(WRITE_CHAR_UUID));
      }
      if (notify_chr == nullptr || write_chr == nullptr) {
        ESP_LOGE(TAG, "[%s] Hanchu service (0xFF01 notify / 0xFF02 write) not found", this->address_());
        this->status_set_warning();
        break;
      }
      // Handles only: the parent frees its characteristic cache once this node is established
      this->notify_handle_ = notify_chr->handle;
      this->write_handle_ = write_chr->handle;
      this->state_ = State::SUBSCRIBING;
      auto status = register_for_notify(this->parent(), this->notify_handle_, 0);
      if (status != ESP_OK)
        ESP_LOGW(TAG, "[%s] register_for_notify failed, status=%d", this->address_(), status);
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      if (param->reg_for_notify.handle != this->notify_handle_)
        break;
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[%s] Enabling notifications failed, status=%d", this->address_(),
                 param->reg_for_notify.status);
        break;
      }
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->send_handshake_();
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle == this->notify_handle_)
        this->handle_notification_(param->notify.value, param->notify.value_len);
      break;

    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      // Our own disconnects set DISCONNECTED first, so only unexpected ones are reported
      if (this->state_ != State::DISCONNECTED)
        ESP_LOGW(TAG, "[%s] Disconnected unexpectedly", this->address_());
      this->reset_connection_state_();
      this->mtu_ = DEFAULT_MTU;
      break;

    default:
      break;
  }
}

void HanchuBle::send_handshake_() {
  // Six distinct characters: the token format the logger is known to accept
  char token[HANCHU_TOKEN_LENGTH + 1]{};
  size_t count = 0;
  while (count < HANCHU_TOKEN_LENGTH) {
    char c = TOKEN_CHARSET[random_uint32() % 36];
    if (std::memchr(token, c, count) == nullptr)
      token[count++] = c;
  }

  uint8_t session_key[HANCHU_KEY_LENGTH];
  derive_session_key(token, session_key);
  this->aes_.set_key(session_key);

  uint8_t packet[1 + HANCHU_TOKEN_LENGTH];
  packet[0] = HANDSHAKE_PREFIX;
  std::memcpy(packet + 1, token, HANCHU_TOKEN_LENGTH);

  ESP_LOGD(TAG, "[%s] Sending handshake (token %s)", this->address_(), token);
  this->state_ = State::HANDSHAKE;
  this->state_started_ms_ = millis();
  if (!this->write_(packet, sizeof(packet)))
    this->end_connection_();
}

bool HanchuBle::write_(const uint8_t *data, size_t length) {
  if (length > this->max_payload_()) {
    ESP_LOGE(TAG, "[%s] %u byte write exceeds MTU %u", this->address_(), (unsigned) length, this->mtu_);
    return false;
  }
  std::vector<uint8_t> buffer(data, data + length);  // the IDF API takes a non-const pointer
  auto status = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                         this->write_handle_, buffer.size(), buffer.data(),
                                         ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "[%s] esp_ble_gattc_write_char failed, status=%d", this->address_(), status);
    return false;
  }
  return true;
}

void HanchuBle::handle_notification_(const uint8_t *data, uint16_t length) {
  // Handshake acknowledgements arrive unencrypted
  if (length >= 2 && data[0] == HANDSHAKE_PREFIX && data[1] == 0x00) {
    if (this->state_ == State::HANDSHAKE) {
      ESP_LOGI(TAG, "[%s] Handshake acknowledged", this->address_());
      this->state_ = State::READY;
      this->update();
    }
    return;
  }
  if (this->state_ != State::WAITING_REPLY) {
    ESP_LOGV(TAG, "[%s] Ignoring unexpected notification (%u bytes)", this->address_(), length);
    return;
  }
  // Every notification is decrypted on its own, restarting from the fixed IV
  std::vector<uint8_t> frame(length);
  cfb8_decrypt(this->aes_, hanchu_state_vector(), data, frame.data(), length);
  this->handle_frame_(frame);
}

void HanchuBle::handle_frame_(const std::vector<uint8_t> &frame) {
  auto log_frame = [this, &frame](const char *what) {
    char hex[format_hex_pretty_size(24)];
    format_hex_pretty_to(hex, frame.data(), std::min<size_t>(frame.size(), 24));
    ESP_LOGW(TAG, "[%s] %s (%u bytes): %s", this->address_(), what, (unsigned) frame.size(), hex);
  };

  // Acknowledgement-shaped frames carry no data
  if (frame.size() <= FRAME_HEADER_LENGTH && frame.size() >= 2 && frame[0] == HANDSHAKE_PREFIX && frame[1] == 0x00)
    return;
  if (frame.size() < FRAME_HEADER_LENGTH) {
    log_frame("Ignoring undersized frame");
    return;
  }

  uint16_t index;
  uint8_t packet_type;
  if (frame[0] == '{' || frame[0] == '[') {
    // Unframed JSON: a complete reply in a single notification
    index = 0;
    packet_type = PACKET_TYPE_FINAL;
    this->fragments_[index] = frame;
  } else if (frame[0] == FRAME_TYPE_DATA) {
    const uint16_t payload_length = frame[4] | (frame[5] << 8);
    if (FRAME_HEADER_LENGTH + payload_length > frame.size()) {
      log_frame("Ignoring frame with length mismatch");
      return;
    }
    packet_type = frame[1];
    index = frame[2] | (frame[3] << 8);
    this->fragments_[index].assign(frame.begin() + FRAME_HEADER_LENGTH,
                                   frame.begin() + FRAME_HEADER_LENGTH + payload_length);
  } else {
    log_frame("Ignoring frame with unsupported type");
    return;
  }

  if (packet_type != PACKET_TYPE_FINAL)
    return;

  const uint16_t first = this->fragments_.begin()->first;
  for (uint32_t i = first; i <= index; i++) {
    if (this->fragments_.count(i) == 0) {
      this->fail_request_("missing reply fragment");
      return;
    }
  }
  std::vector<uint8_t> payload;
  for (const auto &fragment : this->fragments_)
    payload.insert(payload.end(), fragment.second.begin(), fragment.second.end());
  this->fragments_.clear();
  this->handle_reply_(payload);
}

bool HanchuBle::parse_reply_(const std::vector<uint8_t> &payload, Values &values) {
  size_t end = payload.size();
  while (end > 0 && payload[end - 1] == 0x00)
    end--;
  ESP_LOGV(TAG, "[%s] Reply: %.*s", this->address_(), (int) end, reinterpret_cast<const char *>(payload.data()));

  // Skip any transport bytes before the JSON object
  for (size_t start = 0; start < end; start++) {
    if (payload[start] != '{')
      continue;
    bool parsed = json::parse_json(payload.data() + start, end - start, [this, &values](JsonObject root) -> bool {
      if (!root["code"].isNull()) {
        std::string code;
        serializeJson(root["code"], code);
        ESP_LOGV(TAG, "[%s] Reply code %s", this->address_(), code.c_str());
      }
      JsonArray data = root["data"].as<JsonArray>();
      if (data.isNull())
        return true;  // no values, e.g. an error reply
      for (JsonObject item : data) {
        const char *key = item["k"].as<const char *>();
        if (key == nullptr)
          continue;
        JsonVariant raw = item["v"];
        Value value;
        if (raw.isNull()) {
          value.null = true;
          value.text = "null";
        } else if (raw.is<const char *>()) {
          // Some firmware sends numbers as strings
          value.text = raw.as<const char *>();
          char *parse_end = nullptr;
          float number = strtof(value.text.c_str(), &parse_end);
          if (parse_end != value.text.c_str() && *parse_end == '\0')
            value.number = number;
        } else if (raw.is<bool>()) {
          value.number = raw.as<bool>() ? 1.0f : 0.0f;
          value.text = raw.as<bool>() ? "true" : "false";
        } else {
          if (raw.is<double>() || raw.is<long long>())
            value.number = raw.as<double>();
          serializeJson(raw, value.text);
        }
        values[key] = std::move(value);
      }
      return true;
    });
    if (parsed)
      return true;
  }
  return false;
}

void HanchuBle::handle_reply_(const std::vector<uint8_t> &payload) {
  Values values;
  if (!this->parse_reply_(payload, values)) {
    this->fail_request_("reply is not valid JSON");
    return;
  }

  const auto &request = this->cycle_[this->cycle_index_];
  ESP_LOGD(TAG, "[%s] Reply with %u of %u keys in %u ms", this->address_(), (unsigned) values.size(),
           (unsigned) request.keys.size(), (unsigned) (millis() - this->state_started_ms_));

  for (const auto &key : request.keys) {
    auto it = values.find(key);
    ESP_LOGV(TAG, "[%s]   %s = %s", this->address_(), key.c_str(),
             it == values.end() ? "(not returned)" : it->second.text.c_str());
  }
  this->cycle_had_success_ = true;
  this->status_clear_warning();
  this->publish_request_(request, values);

  this->state_ = State::READY;
  this->cycle_index_++;
  this->send_next_request_();
}

void HanchuBle::publish_request_(const Request &request, const Values &values) {
  const uint32_t now = millis();
  for (size_t index : request.groups) {
    this->groups_[index].last_success_ms = now;
    this->groups_[index].done = true;
  }

  // Missing or null values are skipped, so a sensor keeps its last state instead of publishing garbage
  for (const auto &binding : this->sensors_) {
    if (!contains(request.keys, binding.key) ||
        std::find(request.groups.begin(), request.groups.end(), binding.group) == request.groups.end())
      continue;
    auto it = values.find(binding.key);
    if (it != values.end() && !std::isnan(it->second.number))
      binding.sensor->publish_state(it->second.number * binding.multiplier);
  }
  for (const auto &binding : this->text_sensors_) {
    if (!contains(request.keys, binding.key) ||
        std::find(request.groups.begin(), request.groups.end(), binding.group) == request.groups.end())
      continue;
    auto it = values.find(binding.key);
    if (it != values.end() && !it->second.null)
      binding.sensor->publish_state(it->second.text);
  }
}

}  // namespace esphome::hanchu_ble

#endif  // USE_ESP32
